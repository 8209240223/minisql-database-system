// X24 页式权限目录的字节布局与损坏拒绝契约。
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import {
  AccessStoreError, DATA_CAPACITY, DATA_HEADER_BYTES, FORMAT_VERSION, PAGE_SIZE,
  decodePages, encodePages, fnv1a32, openStore, pagesPathFor, readHeader, readStore, writeStore,
} from '../scripts/access-store.mjs';
import { defaultAccess, normalizeAccess } from '../scripts/access-catalog.mjs';

let checks = 0;
const ok = (value, message) => { assert.ok(value, message); ++checks; };
const equal = (actual, expected, message) => { assert.deepEqual(actual, expected, message); ++checks; };
const rejects = (fn, pattern, message) => {
  assert.throws(fn, error => error instanceof AccessStoreError && pattern.test(error.message), message);
  ++checks;
};

const directory = mkdtempSync(join(tmpdir(), 'minisql-access-store-'));
const sample = normalizeAccess(defaultAccess(), undefined);

// --- 布局 ---------------------------------------------------------------
const pages = encodePages(sample, { permissionVersion: 7, updatedAtMs: 1_700_000_000_000 });
equal(pages.length % PAGE_SIZE, 0, 'file is a whole number of pages');
ok(pages.length >= PAGE_SIZE * 2, 'at least one META page and one DATA page');
equal(pages.subarray(0, 8).toString('latin1'), 'MSQLACL\0', 'META magic');
equal(pages.readUInt32LE(8), FORMAT_VERSION, 'format version');
equal(pages.readUInt32LE(12), PAGE_SIZE, 'page size recorded in META');
equal(pages.readUInt32LE(16), pages.length / PAGE_SIZE, 'page count matches file size');
equal(pages.readUInt32LE(24), 7, 'permission version stored in META');
equal(pages.readUInt32LE(PAGE_SIZE), 0x444c4341, 'DATA page magic is ACLD');
equal(pages.readUInt32LE(PAGE_SIZE + 4), 1, 'first DATA page id is 1');

// --- 往返 ---------------------------------------------------------------
const decoded = decodePages(pages);
equal(decoded.catalog, sample, 'catalog survives an encode/decode round trip');
equal(decoded.permissionVersion, 7, 'permission version survives the round trip');
equal(decoded.updatedAtMs, 1_700_000_000_000, 'timestamp survives the round trip');

// 多页负载：制造一个大到需要跨页的目录。
const wide = normalizeAccess({
  users: Object.fromEntries(Array.from({ length: 120 }, (_, i) => [`user_${i}`, { roles: ['readers'], grants: [{ object: `table_${i}`, permissions: ['SELECT', 'INSERT'] }] }])),
  roles: { readers: { inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ'] }] } },
}, undefined);
const widePages = encodePages(wide, { permissionVersion: 2 });
ok(widePages.length / PAGE_SIZE > 2, 'wide catalog spans multiple DATA pages');
equal(decodePages(widePages).catalog, wide, 'multi-page catalog round trips');
ok(decodePages(widePages).payloadBytes > DATA_CAPACITY, 'payload really exceeds a single page');

// --- 损坏拒绝 -----------------------------------------------------------
const corrupt = mutate => { const copy = Buffer.from(pages); mutate(copy); return copy; };
rejects(() => decodePages(corrupt(buffer => buffer.write('XXXXXXX\0', 0, 'latin1'))), /magic mismatch/, 'bad META magic rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(99, 8))), /format version/, 'unknown format version rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(512, 12))), /page size/, 'unexpected page size rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(99, 16))), /page count/, 'page count mismatch rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(0, PAGE_SIZE))), /magic mismatch/, 'bad DATA magic rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(9, PAGE_SIZE + 4))), /page id mismatch/, 'wrong page id rejected');
rejects(() => decodePages(corrupt(buffer => buffer.writeUInt32LE(0xdeadbeef, PAGE_SIZE + 12))), /checksum mismatch/, 'page checksum mismatch rejected');
rejects(() => decodePages(corrupt(buffer => buffer.write('[', PAGE_SIZE + DATA_HEADER_BYTES))), /checksum mismatch/, 'payload edit caught by the page checksum');
// 同时修好页校验值，只留整体 sha256 不符 —— 必须仍然被拒。
rejects(() => decodePages(corrupt(buffer => {
  const byteCount = buffer.readUInt32LE(PAGE_SIZE + 8);
  buffer.write('[', PAGE_SIZE + DATA_HEADER_BYTES);
  const slice = buffer.subarray(PAGE_SIZE + DATA_HEADER_BYTES, PAGE_SIZE + DATA_HEADER_BYTES + byteCount);
  buffer.writeUInt32LE(fnv1a32(slice), PAGE_SIZE + 12);
})), /digest mismatch/, 'repaired page checksum still fails the payload digest');
rejects(() => decodePages(pages.subarray(0, PAGE_SIZE)), /whole number of pages|not a whole/, 'META-only file rejected');
rejects(() => decodePages(pages.subarray(0, PAGE_SIZE * 2 - 5)), /whole number of pages/, 'truncated page rejected');
rejects(() => decodePages(Buffer.alloc(0)), /whole number of pages/, 'empty file rejected');
assert.throws(() => encodePages(sample, { permissionVersion: 0 }), /UINT32/);
++checks;

// --- 落盘、原子写与 META 读取 -------------------------------------------
const pagesFile = join(directory, 'access.catalog.pages');
writeStore(pagesFile, sample, { permissionVersion: 11 });
equal(readStore(pagesFile).permissionVersion, 11, 'writeStore/readStore round trip');
equal(readHeader(pagesFile).permissionVersion, 11, 'readHeader sees the version without a full parse');
equal(readHeader(pagesFile).pageSize, PAGE_SIZE, 'readHeader reports the page size');
ok(!existsSync(pagesFile + '.tmp'), 'temporary file is renamed away, not left behind');
writeStore(pagesFile, sample, { permissionVersion: 12 });
equal(readHeader(pagesFile).permissionVersion, 12, 'rewrite bumps the on-disk version');

// --- 路径推导 -----------------------------------------------------------
equal(pagesPathFor('/tmp/a/access.catalog.json'), '/tmp/a/access.catalog.pages', 'json suffix replaced');
equal(pagesPathFor('/tmp/a/access.catalog'), '/tmp/a/access.catalog.pages', 'suffixless path gets .pages');

// --- 从旧 JSON 迁移 -----------------------------------------------------
const migrateDirectory = mkdtempSync(join(tmpdir(), 'minisql-access-migrate-'));
const legacyJson = join(migrateDirectory, 'access.catalog.json');
writeFileSync(legacyJson, JSON.stringify({
  version: 1,
  users: { admin: { hash: null, roles: ['administrators'], grants: [] }, legacy: { hash: null, roles: ['readers'], grants: [] } },
  roles: {
    administrators: { inherits: [], grants: [{ object: '*', permissions: ['*'] }] },
    readers: { inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ'] }] },
  },
}), 'utf8');
const migrated = openStore(legacyJson, { defaults: defaultAccess, normalize: normalizeAccess });
equal(migrated.migrated, true, 'legacy JSON triggers a migration');
equal(migrated.source, 'json', 'migration source is reported');
ok(Object.keys(migrated.catalog.users).includes('legacy'), 'legacy user survives the migration');
ok(existsSync(pagesPathFor(legacyJson)), 'page file written during migration');
ok(existsSync(legacyJson + '.migrated'), 'original JSON kept as a rollback copy');
ok(!existsSync(legacyJson), 'original JSON path no longer shadows the page file');
// 第二次打开必须直接走页式文件，不再迁移。
const reopened = openStore(legacyJson, { defaults: defaultAccess, normalize: normalizeAccess });
equal(reopened.migrated, false, 'reopening does not migrate again');
equal(reopened.source, 'pages', 'reopen reads the page file');
ok(Object.keys(reopened.catalog.users).includes('legacy'), 'migrated user is readable from pages');

// --- 全新初始化 ---------------------------------------------------------
const freshDirectory = mkdtempSync(join(tmpdir(), 'minisql-access-fresh-'));
const fresh = openStore(join(freshDirectory, 'access.catalog.json'), { defaults: defaultAccess, normalize: normalizeAccess });
equal(fresh.source, 'defaults', 'missing files fall back to the built-in defaults');
equal(fresh.permissionVersion, 1, 'fresh catalog starts at permission version 1');
ok(Object.keys(fresh.catalog.users).sort().join(',') === 'admin,reader,writer', 'default users created');
ok(readFileSync(pagesPathFor(join(freshDirectory, 'access.catalog.json'))).length >= PAGE_SIZE * 2, 'default catalog is persisted as pages');

console.log(`${checks} paged access-catalog checks passed: layout, multi-page payload, corruption rejection, atomic write, JSON migration`);

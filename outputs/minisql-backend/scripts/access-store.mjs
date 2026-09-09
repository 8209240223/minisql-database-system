// X24 权限目录的页式持久化。
//
// 权限数据不再以裸 JSON 文件形式保存，而是写入固定 4096 字节页的
// `access.catalog.pages` 文件，与堆表/索引页使用同一页大小，便于后续搬进 C++ 页管理器。
//
// 文件布局：
//   页 0        META 页
//     0..7      magic "MSQLACL\0"
//     8..11     uint32 formatVersion
//     12..15    uint32 pageSize
//     16..19    uint32 pageCount（含 META 页）
//     20..23    uint32 payloadBytes
//     24..27    uint32 permissionVersion（单调递增，永不回退）
//     28..31    uint32 catalogVersion（结构版本，用于迁移）
//     32..39    uint64 updatedAtMs
//     40..71    sha256(payload)
//   页 1..n     DATA 页
//     0..3      magic "ACLD"
//     4..7      uint32 pageId（从 1 开始）
//     8..11     uint32 byteCount（本页有效负载字节数）
//     12..15    uint32 fnv1a32(本页有效负载)
//     16..4095  负载（尾部零填充）
//
// 写入始终先落临时文件再 rename，保证崩溃后要么是旧页面要么是新页面。
// 读取时逐页校验 magic、pageId、byteCount 和页内校验值，再校验整体 sha256；
// 任何一项不符都明确拒绝，不静默返回空权限（空权限会被误当成"全部拒绝"或"默认放行"）。

import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';

export const PAGE_SIZE = 4096;
export const META_MAGIC = 'MSQLACL\0';
export const DATA_MAGIC = 0x444c4341; // "ACLD" little-endian
export const FORMAT_VERSION = 1;
export const META_HEADER_BYTES = 72;
export const DATA_HEADER_BYTES = 16;
export const DATA_CAPACITY = PAGE_SIZE - DATA_HEADER_BYTES;

export class AccessStoreError extends Error {
  constructor(message, detail = {}) {
    super(message);
    this.name = 'AccessStoreError';
    this.detail = detail;
  }
}

export function fnv1a32(buffer) {
  let hash = 0x811c9dc5;
  for (const byte of buffer) {
    hash ^= byte;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash >>> 0;
}

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest();
}

/** 把权限目录编码成页式缓冲区。纯函数，便于契约测试直接断言字节布局。 */
export function encodePages(catalog, { permissionVersion, catalogVersion = 1, updatedAtMs = Date.now() } = {}) {
  if (!Number.isInteger(permissionVersion) || permissionVersion < 1 || permissionVersion > 0xffffffff)
    throw new AccessStoreError('permissionVersion must be a UINT32 >= 1', { permissionVersion });
  const payload = Buffer.from(JSON.stringify(catalog), 'utf8');
  const dataPages = Math.max(1, Math.ceil(payload.length / DATA_CAPACITY));
  const file = Buffer.alloc((dataPages + 1) * PAGE_SIZE);
  file.write(META_MAGIC, 0, 'latin1');
  file.writeUInt32LE(FORMAT_VERSION, 8);
  file.writeUInt32LE(PAGE_SIZE, 12);
  file.writeUInt32LE(dataPages + 1, 16);
  file.writeUInt32LE(payload.length, 20);
  file.writeUInt32LE(permissionVersion, 24);
  file.writeUInt32LE(catalogVersion, 28);
  file.writeBigUInt64LE(BigInt(updatedAtMs), 32);
  sha256(payload).copy(file, 40);
  for (let index = 0; index < dataPages; ++index) {
    const start = index * DATA_CAPACITY;
    const slice = payload.subarray(start, Math.min(start + DATA_CAPACITY, payload.length));
    const base = (index + 1) * PAGE_SIZE;
    file.writeUInt32LE(DATA_MAGIC, base);
    file.writeUInt32LE(index + 1, base + 4);
    file.writeUInt32LE(slice.length, base + 8);
    file.writeUInt32LE(fnv1a32(slice), base + 12);
    slice.copy(file, base + DATA_HEADER_BYTES);
  }
  return file;
}

/** 解码并逐页校验。任何结构问题都抛 AccessStoreError，绝不返回"看起来能用"的部分结果。 */
export function decodePages(file) {
  if (!Buffer.isBuffer(file)) throw new AccessStoreError('Access catalog pages must be a Buffer');
  if (file.length < PAGE_SIZE * 2 || file.length % PAGE_SIZE !== 0)
    throw new AccessStoreError('Access catalog file is not a whole number of pages', { bytes: file.length });
  if (file.subarray(0, 8).toString('latin1') !== META_MAGIC)
    throw new AccessStoreError('Access catalog magic mismatch');
  const formatVersion = file.readUInt32LE(8);
  if (formatVersion !== FORMAT_VERSION)
    throw new AccessStoreError(`Unsupported access catalog format version ${formatVersion}`, { formatVersion });
  const pageSize = file.readUInt32LE(12);
  if (pageSize !== PAGE_SIZE) throw new AccessStoreError(`Unsupported access catalog page size ${pageSize}`, { pageSize });
  const pageCount = file.readUInt32LE(16);
  if (pageCount !== file.length / PAGE_SIZE)
    throw new AccessStoreError('Access catalog page count disagrees with file size', { pageCount, bytes: file.length });
  const payloadBytes = file.readUInt32LE(20);
  const permissionVersion = file.readUInt32LE(24);
  const catalogVersion = file.readUInt32LE(28);
  const updatedAtMs = Number(file.readBigUInt64LE(32));
  const expectedDigest = file.subarray(40, 72);
  const dataPages = pageCount - 1;
  if (payloadBytes > dataPages * DATA_CAPACITY)
    throw new AccessStoreError('Access catalog payload does not fit the recorded pages', { payloadBytes, dataPages });
  const chunks = [];
  let seen = 0;
  for (let index = 0; index < dataPages; ++index) {
    const base = (index + 1) * PAGE_SIZE;
    if (file.readUInt32LE(base) !== DATA_MAGIC)
      throw new AccessStoreError(`Access catalog data page ${index + 1} magic mismatch`, { page: index + 1 });
    const pageId = file.readUInt32LE(base + 4);
    if (pageId !== index + 1)
      throw new AccessStoreError(`Access catalog data page id mismatch at page ${index + 1}`, { page: index + 1, pageId });
    const byteCount = file.readUInt32LE(base + 8);
    if (byteCount > DATA_CAPACITY)
      throw new AccessStoreError(`Access catalog data page ${pageId} declares ${byteCount} bytes`, { page: pageId, byteCount });
    const slice = file.subarray(base + DATA_HEADER_BYTES, base + DATA_HEADER_BYTES + byteCount);
    if (file.readUInt32LE(base + 12) !== fnv1a32(slice))
      throw new AccessStoreError(`Access catalog data page ${pageId} checksum mismatch`, { page: pageId });
    chunks.push(Buffer.from(slice));
    seen += byteCount;
  }
  if (seen !== payloadBytes)
    throw new AccessStoreError('Access catalog page bytes disagree with the META payload length', { seen, payloadBytes });
  const payload = Buffer.concat(chunks);
  if (!sha256(payload).equals(expectedDigest))
    throw new AccessStoreError('Access catalog payload digest mismatch');
  let catalog;
  try {
    catalog = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(payload));
  } catch (error) {
    throw new AccessStoreError('Access catalog payload is not valid UTF-8 JSON', { cause: String(error) });
  }
  return { catalog, permissionVersion, catalogVersion, updatedAtMs, pageCount, payloadBytes, formatVersion };
}

/** 只读 META 页，用于在不解析全部权限数据的情况下比较权限版本。 */
export function readHeader(pagesFile) {
  const meta = Buffer.alloc(META_HEADER_BYTES);
  const handle = readFileSync(pagesFile);
  handle.copy(meta, 0, 0, Math.min(META_HEADER_BYTES, handle.length));
  if (meta.subarray(0, 8).toString('latin1') !== META_MAGIC) throw new AccessStoreError('Access catalog magic mismatch');
  return {
    formatVersion: meta.readUInt32LE(8),
    pageSize: meta.readUInt32LE(12),
    pageCount: meta.readUInt32LE(16),
    payloadBytes: meta.readUInt32LE(20),
    permissionVersion: meta.readUInt32LE(24),
    catalogVersion: meta.readUInt32LE(28),
    updatedAtMs: Number(meta.readBigUInt64LE(32)),
  };
}

/** 由 access 文件路径推导页式文件路径；`.json` 后缀被替换为 `.pages`。 */
export function pagesPathFor(accessPath) {
  return String(accessPath).replace(/\.json$/i, '') + '.pages';
}

export function writeStore(pagesFile, catalog, options) {
  mkdirSync(dirname(pagesFile), { recursive: true });
  const buffer = encodePages(catalog, options);
  const temporary = pagesFile + '.tmp';
  writeFileSync(temporary, buffer);
  renameSync(temporary, pagesFile);
  return buffer.length;
}

export function readStore(pagesFile) {
  return decodePages(readFileSync(pagesFile));
}

/**
 * 打开权限存储，必要时从旧的 JSON 文件迁移。
 * 迁移只发生一次：写出页式文件后保留 `.json` 作为 `.json.migrated` 备份，方便回滚。
 */
export function openStore(accessPath, { defaults, normalize }) {
  const pagesFile = pagesPathFor(accessPath);
  if (existsSync(pagesFile)) {
    const state = readStore(pagesFile);
    return { pagesFile, migrated: false, source: 'pages', ...state, catalog: normalize(state.catalog, state.catalog) };
  }
  if (existsSync(accessPath) && statSync(accessPath).isFile()) {
    const parsed = JSON.parse(readFileSync(accessPath, 'utf8'));
    const catalog = normalize(parsed, parsed);
    const permissionVersion = Number.isInteger(parsed.permissionVersion) && parsed.permissionVersion > 0 ? parsed.permissionVersion : 1;
    writeStore(pagesFile, catalog, { permissionVersion });
    try { renameSync(accessPath, accessPath + '.migrated'); }
    catch { /* 备份失败不影响已经落盘的页式目录。 */ }
    return { pagesFile, migrated: true, source: 'json', catalog, permissionVersion, catalogVersion: 1, updatedAtMs: Date.now() };
  }
  const catalog = normalize(defaults(), undefined);
  writeStore(pagesFile, catalog, { permissionVersion: 1 });
  return { pagesFile, migrated: false, source: 'defaults', catalog, permissionVersion: 1, catalogVersion: 1, updatedAtMs: Date.now() };
}

export function removeStore(accessPath) {
  const pagesFile = pagesPathFor(accessPath);
  for (const file of [pagesFile, pagesFile + '.tmp']) {
    try { unlinkSync(file); } catch { /* 文件可能本来就不存在。 */ }
  }
}

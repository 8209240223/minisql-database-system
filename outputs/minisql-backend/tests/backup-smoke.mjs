import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';
const root = mkdtempSync(join(tmpdir(), 'minisql-backup-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(root, 'db.pages'), MINISQL_BACKUP_DIR: join(root, 'backups') },
  windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(server, 'exit');
let errors = '', url;
server.stderr.on('data', chunk => { errors += chunk; });
const timer = setTimeout(() => server.kill(), 45000);
async function request(path, body, method = 'POST') {
  const response = await fetch(url + path, { method, headers: method === 'POST' ? { 'Content-Type': 'application/json' } : {}, body: method === 'POST' ? JSON.stringify(body ?? {}) : undefined, signal: AbortSignal.timeout(10000) });
  return { status: response.status, data: await response.json() };
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
try {
  url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => { output += chunk; const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/); if (match) resolve(match[0]); });
    server.once('error', reject);
    server.once('exit', () => reject(new Error(errors || 'Server exited before readiness')));
  });
  equal((await request('/execute', { sql: 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2);' })).status, 200);
  const backup = await request('/backup', { name: 'snap1' });
  equal(backup.status, 200);
  assert.equal(backup.data.backup, 'snap1.pages'); ++checks;
  assert.ok(backup.data.sha256.length === 64); ++checks;
  const manifestPath = join(root, 'backups', 'snap1.pages.json');
  const originalManifest = readFileSync(manifestPath, 'utf8');
  const manifest = JSON.parse(originalManifest);
  equal(manifest.version, 2);
  assert.ok(manifest.pageFormatVersion === 1 || manifest.pageFormatVersion === 2); ++checks;
  equal(manifest.walBytes, 0);
  writeFileSync(manifestPath, JSON.stringify({ ...manifest, version: 99 }), 'utf8');
  equal((await request('/restore', { name: 'snap1' })).status, 422);
  writeFileSync(manifestPath, JSON.stringify({ version: 1, name: manifest.name, createdAt: manifest.createdAt, bytes: manifest.bytes, sha256: manifest.sha256 }), 'utf8');
  equal((await request('/restore', { name: 'snap1' })).status, 200);
  equal(JSON.parse(readFileSync(manifestPath, 'utf8')).version, 2);
  equal((await request('/execute', { sql: 'INSERT INTO t VALUES(3);' })).status, 200);
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1],[2],[3]]);
  equal((await request('/restore', { name: 'snap1' })).status, 200);
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1],[2]]);
  const list = await request('/backups', undefined, 'GET');
  equal(list.status, 200);
  const listed = list.data.entries.find(entry => entry.name === 'snap1.pages');
  assert.ok(listed); ++checks;
  equal(listed.manifestVersion, 2);
  assert.ok(listed.pageFormatVersion === 1 || listed.pageFormatVersion === 2); ++checks;
  equal(listed.walBytes, 0);

  equal((await request('/backup', { name: 'base1' })).status, 200);
  equal((await request('/execute', { sql: 'INSERT INTO t VALUES(3);' })).status, 200);
  const incremental = await request('/backup', { name: 'inc1', base: 'base1' });
  equal(incremental.status, 200);
  equal(incremental.data.kind, 'incremental');
  equal(incremental.data.backup, 'inc1.delta');
  equal(incremental.data.base, 'base1.pages');
  const inc1Manifest = JSON.parse(readFileSync(join(root, 'backups', 'inc1.delta.json'), 'utf8'));
  equal(inc1Manifest.version, 3);
  equal(inc1Manifest.kind, 'incremental');
  equal(inc1Manifest.base, 'base1.pages');

  equal((await request('/execute', { sql: 'INSERT INTO t VALUES(4);' })).status, 200);
  equal((await request('/restore', { name: 'inc1.delta' })).status, 200);
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1],[2],[3]]);

  equal((await request('/backup', { name: 'inc2', base: 'inc1.delta' })).status, 200);
  equal((await request('/execute', { sql: 'INSERT INTO t VALUES(5);' })).status, 200);
  equal((await request('/restore', { name: 'inc2.delta' })).status, 200);
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1],[2],[3]]);

  const chainedList = await request('/backups', undefined, 'GET');
  const listedIncremental = chainedList.data.entries.find(entry => entry.name === 'inc1.delta');
  assert.ok(listedIncremental); ++checks;
  equal(listedIncremental.kind, 'incremental');
  equal(listedIncremental.manifestVersion, 3);
  equal(listedIncremental.base, 'base1.pages');
  writeFileSync(join(root, 'backups', 'inc1.delta.json'), JSON.stringify({ ...inc1Manifest, version: 99 }), 'utf8');
  equal((await request('/restore', { name: 'inc2.delta' })).status, 422);

  const opened = await request('/sessions');
  equal(opened.status, 201);
  const sessionId = opened.data.sessionId;
  equal((await request('/backup', { name: 'blocked' })).status, 409);
  await request('/sessions/' + sessionId + '/close');
  console.log(`${checks} backup/restore checks passed`);
} finally {
  server.kill();
  await exited;
  clearTimeout(timer);
}

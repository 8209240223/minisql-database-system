import { spawn } from 'node:child_process';
import { mkdtempSync, openSync, readFileSync, readSync, closeSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-backup-online-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(root, 'db.pages'), MINISQL_BACKUP_DIR: join(root, 'backups') },
  windowsHide: true,
  stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(server, 'exit');
let errors = '';
let url;
server.stderr.on('data', chunk => { errors += chunk; });

async function request(path, body, method = 'POST') {
  const response = await fetch(url + path, {
    method,
    headers: method === 'POST' ? { 'Content-Type': 'application/json' } : {},
    body: method === 'POST' ? JSON.stringify(body ?? {}) : undefined,
    signal: AbortSignal.timeout(60000),
  });
  return { status: response.status, data: await response.json() };
}

function pageFormatVersion(file) {
  const descriptor = openSync(file, 'r');
  try {
    const header = Buffer.alloc(12);
    if (readSync(descriptor, header, 0, header.length, 0) !== header.length) throw new Error('Short page header');
    return header.readUInt32LE(8);
  } finally { closeSync(descriptor); }
}

let checks = 0;
function check(value, message) {
  assert.ok(value, message);
  ++checks;
}
function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

try {
  url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.once('error', reject);
    server.once('exit', () => reject(new Error(errors || 'Server exited before readiness')));
  });

  equal((await request('/execute', { sql: 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2);' })).status, 200);
  const snapshot = await request('/backup', { name: 'online-snap', mode: 'online' });
  equal(snapshot.status, 200);
  equal(snapshot.data.kind, 'snapshot');
  equal(snapshot.data.backup, 'online-snap.pages');
  assert.equal(snapshot.data.sha256.length, 64);
  ++checks;

  const manifest = JSON.parse(readFileSync(join(root, 'backups', 'online-snap.pages.json'), 'utf8'));
  equal(manifest.version, 4);
  equal(manifest.kind, 'snapshot');
  equal(manifest.pageFormatVersion, pageFormatVersion(join(root, 'backups', 'online-snap.pages')));
  assert.ok(manifest.committedSequence >= 1);
  ++checks;
  if (manifest.walBytes > 0) assert.ok(existsSync(join(root, 'backups', 'online-snap.pages.wal')));

  const inserted = await request('/execute', { sql: 'INSERT INTO t VALUES(3),(4);' });
  equal(inserted.status, 200);
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1], [2], [3], [4]]);
  const restored = await request('/restore', { name: 'online-snap.pages' });
  equal(restored.status, 200);
  assert.ok(existsSync(restored.data.rollback)); ++checks;
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1], [2]]);

  const sessions = await request('/sessions');
  equal(sessions.status, 201);
  const sessionId = sessions.data.sessionId;
  equal((await request('/sessions/' + sessionId + '/execute', { sql: 'BEGIN; INSERT INTO t VALUES(5);' })).status, 200);
  const onlineDuringTransaction = await request('/backup', { name: 'active-snap', mode: 'online' });
  equal(onlineDuringTransaction.status, 200);
  equal(onlineDuringTransaction.data.kind, 'snapshot');
  equal((await request('/sessions/' + sessionId + '/execute', { sql: 'COMMIT;' })).status, 200);
  await request('/sessions/' + sessionId + '/close');
  const activeRestored = await request('/restore', { name: 'active-snap.pages' });
  equal(activeRestored.status, 200);
  assert.ok(existsSync(activeRestored.data.rollback)); ++checks;
  equal((await request('/execute', { sql: 'SELECT * FROM t ORDER BY id;' })).data.rows, [[1], [2]]);

  console.log(`${checks} online backup checks passed`);
} finally {
  server.kill();
  await exited;
}

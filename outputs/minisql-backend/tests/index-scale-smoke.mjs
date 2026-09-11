import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const scale = Number(process.env.INDEX_SCALE_ROWS ?? 1000);
if (!Number.isInteger(scale) || scale < 100 || scale > 200000) throw new Error('INDEX_SCALE_ROWS must be an integer in [100, 200000]');
const root = mkdtempSync(join(tmpdir(), 'minisql-index-scale-'));
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
    signal: AbortSignal.timeout(120000),
  });
  return { status: response.status, data: await response.json() };
}

function valuesSql(start, count) {
  const values = [];
  for (let offset = 0; offset < count; ++offset) {
    const id = start + offset;
    values.push(`(${id},${id * 10})`);
  }
  return `INSERT INTO t(id,value) VALUES ${values.join(',')};`;
}

let checks = 0;
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

  const created = await request('/execute', { sql: 'CREATE TABLE t(id INT, value INT);' });
  assert.equal(created.status, 200); ++checks;
  const session = await request('/sessions');
  assert.equal(session.status, 201); ++checks;
  const sessionId = session.data.sessionId;
  const batchSize = Math.max(200, Math.min(500, scale));
  for (let begin = 0; begin < scale; begin += batchSize) {
    const end = Math.min(scale, begin + batchSize);
    const inserted = await request('/sessions/' + sessionId + '/execute', { sql: valuesSql(begin, end - begin) });
    assert.equal(inserted.status, 200); ++checks;
  }
  const indexed = await request('/sessions/' + sessionId + '/execute', { sql: 'CREATE INDEX t_id_idx ON t(id);' });
  assert.equal(indexed.status, 200); ++checks;
  const point = await request('/sessions/' + sessionId + '/execute', { sql: 'SELECT id FROM t WHERE id=9999;' });
  assert.equal(point.status, 200); ++checks;
  if (scale > 9999) assert.deepEqual(point.data.rows, [[9999]]);
  else assert.deepEqual(point.data.rows, []);
  ++checks;
  const range = await request('/sessions/' + sessionId + '/execute', { sql: 'SELECT COUNT(*) FROM t WHERE id >= 100 AND id < 200;' });
  assert.equal(range.status, 200); ++checks;
  assert.deepEqual(range.data.rows, [[Math.min(100, Math.max(0, scale - 100))]]);
  ++checks;
  const catalog = await request('/sessions/' + sessionId + '/catalog', undefined, 'GET');
  assert.equal(catalog.status, 200); ++checks;
  const table = catalog.data.tables.find(entry => entry.name === 't');
  assert.ok(table); ++checks;
  assert.equal(table.rowCount, scale); ++checks;
  assert.ok(table.indexes.length >= 1); ++checks;
  assert.ok(table.indexes[0].height >= 2); ++checks;
  assert.ok(table.indexes[0].pageCount >= 1); ++checks;
  await request('/sessions/' + sessionId + '/close');
  console.log(`${checks} index scale checks passed at ${scale} rows (height=${table.indexes[0].height}, pages=${table.indexes[0].pageCount})`);
} finally {
  server.kill();
  await exited;
}

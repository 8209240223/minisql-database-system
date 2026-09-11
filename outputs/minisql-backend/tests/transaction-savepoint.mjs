import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-savepoint-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(root, 'db.pages'), MINISQL_BACKUP_DIR: join(root, 'backups') },
  windowsHide: true,
  stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(server, 'exit');
let errors = '';
let url;
server.stderr.on('data', chunk => { errors += chunk; });

async function execute(sql) {
  const response = await fetch(url + '/execute', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql }),
    signal: AbortSignal.timeout(30000),
  });
  return { status: response.status, data: await response.json() };
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

  const setup = await execute('CREATE TABLE t(id INT); INSERT INTO t VALUES(1);');
  assert.equal(setup.status, 200); ++checks;
  const dml = await execute('BEGIN; INSERT INTO t VALUES(2); SAVEPOINT s1; INSERT INTO t VALUES(3); ROLLBACK TO SAVEPOINT s1; COMMIT;');
  assert.equal(dml.status, 200); ++checks;
  const afterDml = await execute('SELECT * FROM t ORDER BY id;');
  assert.deepEqual(afterDml.data.rows, [[1], [2]]); ++checks;

  const ddl = await execute('BEGIN; CREATE TABLE u(id INT); SAVEPOINT s2; CREATE TABLE v(id INT); ROLLBACK TO s2; RELEASE SAVEPOINT s2; COMMIT;');
  assert.equal(ddl.status, 200); ++checks;
  const catalog = await execute('SELECT * FROM u;');
  assert.equal(catalog.status, 200); ++checks;
  const missing = await execute('SELECT * FROM v;');
  assert.equal(missing.status, 422); ++checks;

  const outside = await execute('SAVEPOINT nope;');
  assert.equal(outside.status, 422); ++checks;
  console.log(`${checks} savepoint checks passed`);
} finally {
  server.kill();
  await exited;
}

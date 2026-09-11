import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-transaction-overflow-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(root, 'db.pages'), MINISQL_BACKUP_DIR: join(root, 'backups'), MINISQL_MAX_BATCH_PAGES: '4' },
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
  const setup = await execute('CREATE TABLE t(id INT, value VARCHAR);');
  assert.equal(setup.status, 200); ++checks;
  const payload = 'x'.repeat(4000);
  const rows = Array.from({ length: 8 }, (_, index) => `(${index + 1},'${payload}')`).join(',');
  const overflow = await execute(`BEGIN; INSERT INTO t VALUES ${rows}; COMMIT;`);
  assert.equal(overflow.status, 422); ++checks;
  assert.ok(String(overflow.data.error?.message ?? '').includes('Write batch page limit exceeded')); ++checks;
  const health = await execute('SELECT COUNT(*) FROM t;');
  assert.equal(health.status, 200); ++checks;
  assert.deepEqual(health.data.rows, [[0]]); ++checks;
  console.log(`${checks} transaction overflow checks passed`);
} finally {
  server.kill();
  await exited;
}

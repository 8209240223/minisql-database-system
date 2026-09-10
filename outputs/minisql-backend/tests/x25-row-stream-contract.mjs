import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-row-stream-'));
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

  const setup = await execute('CREATE TABLE t(id INT, value INT); INSERT INTO t VALUES(1,10),(2,20),(3,30),(4,40);');
  assert.equal(setup.status, 200); ++checks;
  const sorted = await execute('SELECT id FROM t WHERE id >= 2 ORDER BY id DESC;');
  assert.equal(sorted.status, 200); ++checks;
  assert.deepEqual(sorted.data.rows, [[4], [3], [2]]); ++checks;
  assert.equal(sorted.data.resourceUsage.kind, 'Sort'); ++checks;
  assert.equal(sorted.data.resourceUsage.rows, 3); ++checks;
  const filtered = await execute('SELECT id FROM t WHERE id >= 3;');
  assert.equal(filtered.status, 200); ++checks;
  assert.equal(filtered.data.resourceUsage.kind, 'Project'); ++checks;
  assert.equal(filtered.data.resourceUsage.rows, 2); ++checks;
  const aggregated = await execute('SELECT value, COUNT(*) FROM t GROUP BY value;');
  assert.equal(aggregated.status, 200); ++checks;
  assert.ok(aggregated.data.resourceUsage.kind === 'Project' || aggregated.data.resourceUsage.kind === 'Aggregate'); ++checks;
  if (aggregated.data.resourceUsage.child) {
    assert.equal(aggregated.data.resourceUsage.child.kind, 'Aggregate'); ++checks;
  }
  assert.equal(aggregated.data.rows.length, 4); ++checks;
  const limited = await execute('SELECT id FROM t ORDER BY id LIMIT 2;');
  assert.equal(limited.status, 200); ++checks;
  assert.equal(limited.data.resourceUsage.kind, 'Limit'); ++checks;
  assert.deepEqual(limited.data.rows, [[1], [2]]); ++checks;
  console.log(`${checks} row stream resource checks passed`);
} finally {
  server.kill();
  await exited;
}

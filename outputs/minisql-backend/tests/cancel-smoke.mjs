import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readdirSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-cancel-http-'));
const sortDirectory = join(root, 'sort-runs');
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: {
    ...process.env,
    PORT: '0',
    MINISQL_DB: join(root, 'database.pages'),
    MINISQL_SESSION_IDLE_MS: '30000',
    MINISQL_SORT_MEMORY_ROWS: '500',
    MINISQL_AGGREGATE_MEMORY_ROWS: '500',
    MINISQL_TEMP_DIR: sortDirectory,
  },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});
const exited = once(server, 'exit');
const deadline = setTimeout(() => server.kill(), 120000);
let errors = '', checks = 0;
server.stderr.on('data', chunk => { errors += chunk; });
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }

try {
  const url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.on('error', reject);
    server.on('exit', () => reject(new Error(errors || 'server exited')));
  });
  async function request(path, sql, method = 'POST') {
    const response = await fetch(url + path, {
      method,
      headers: { 'Content-Type': 'application/json' },
      ...(sql === undefined ? {} : { body: JSON.stringify({ sql }) }),
      signal: AbortSignal.timeout(60000),
    });
    return { status: response.status, data: await response.json() };
  }

  const opened = await request('/sessions');
  equal(opened.status, 201);
  const prefix = '/sessions/' + opened.data.sessionId;
  equal((await request(prefix + '/execute', 'CREATE TABLE big(id INT);')).status, 200);
  for (let start = 0; start < 30000; start += 5000) {
    const values = Array.from({ length: 5000 }, (_, index) => '(' + (start + index) + ')').join(',');
    equal((await request(prefix + '/execute', 'INSERT INTO big VALUES ' + values + ';')).status, 200);
  }

  const idleCancel = await request(prefix + '/cancel');
  equal(idleCancel.status, 409);

  const pending = request(prefix + '/execute', 'SELECT id FROM big ORDER BY id;');
  let cancelResult;
  for (let attempt = 0; attempt < 200; ++attempt) {
    await delay(2);
    cancelResult = await request(prefix + '/cancel');
    if (cancelResult.status === 202) break;
    assert.equal(cancelResult.status, 409, JSON.stringify(cancelResult.data));
  }
  equal(cancelResult.status, 202);
  equal(cancelResult.data.cancelled, true);
  equal(cancelResult.data.error.code, 5002);

  const cancelled = await pending;
  equal(cancelled.status, 422);
  equal(cancelled.data.success, false);
  equal(cancelled.data.cancelled, true);
  equal(cancelled.data.error.code, 5002);
  equal(cancelled.data.transactionState, 'IDLE');

  equal((await request(prefix + '/execute', 'SELECT id FROM big WHERE id=0;')).data.rows, [[0]]);
  equal((await request(prefix + '/close')).status, 200);

  if (existsSync(sortDirectory)) {
    const leftovers = readdirSync(sortDirectory).filter(name => name.endsWith('.jsonl') || name.endsWith('.meta.json'));
    equal(leftovers, []);
  } else {
    equal(true, true);
  }
  console.log(`${checks} cancellation checks passed: cooperative cancellation, session survival and temporary cleanup`);
} catch (error) {
  console.error('server stderr:', errors);
  throw error;
} finally {
  clearTimeout(deadline);
  server.kill();
  await exited;
}

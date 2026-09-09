import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/multi-session-http-', import.meta.url)));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: {
    ...process.env,
    PORT: '0',
    MINISQL_DB: join(directory, 'database.pages'),
    MINISQL_MAX_SESSIONS: '3',
    MINISQL_TRANSACTION_LOCK_TIMEOUT_MS: '250',
    MINISQL_SESSION_IDLE_MS: '30000',
  },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});
const exited = once(server, 'exit');
const deadline = setTimeout(() => server.kill(), 60000);
let checks = 0;
let errors = '';
server.stderr.on('data', chunk => { errors += chunk; });

function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

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
      signal: AbortSignal.timeout(10000),
    });
    return { status: response.status, data: await response.json() };
  }

  const first = await request('/sessions');
  equal(first.status, 201);
  const second = await request('/sessions');
  equal(second.status, 201);
  const firstPrefix = '/sessions/' + first.data.sessionId;
  const secondPrefix = '/sessions/' + second.data.sessionId;

  const listed = await request('/sessions', undefined, 'GET');
  equal(listed.status, 200);
  equal(listed.data.count, 2);
  equal(listed.data.maxSessions, 3);

  equal((await request(firstPrefix + '/execute', 'CREATE TABLE t(id INT PRIMARY KEY, value INT);')).status, 200);
  equal((await request(firstPrefix + '/execute', 'BEGIN; INSERT INTO t VALUES(1,0),(2,0);')).data.transactionState, 'ACTIVE');

  const waitingRead = request(secondPrefix + '/execute', 'SELECT id, value FROM t ORDER BY id;');
  await new Promise(resolve => setTimeout(resolve, 40));
  const waitingList = await request('/sessions', undefined, 'GET');
  const waitingEntry = waitingList.data.entries.find(entry => entry.sessionId === second.data.sessionId);
  equal(waitingEntry.waitingForLock, true);
  equal(waitingEntry.ownsTransactionLock, false);
  equal((await request(firstPrefix + '/execute', 'COMMIT;')).data.transactionState, 'IDLE');
  const visible = await waitingRead;
  equal(visible.status, 200);
  equal(visible.data.rows, [[1, 0], [2, 0]]);

  equal((await request(firstPrefix + '/execute', 'BEGIN; INSERT INTO t VALUES(3,0);')).data.transactionState, 'ACTIVE');
  const timedOut = await request(secondPrefix + '/execute', 'SELECT id FROM t ORDER BY id;');
  equal(timedOut.status, 409);
  equal(timedOut.data.transactionState, 'IDLE');
  assert.match(timedOut.data.error.message, /Session lock wait exceeded/);
  ++checks;
  equal((await request(firstPrefix + '/execute', 'ROLLBACK;')).data.transactionState, 'IDLE');
  equal((await request(secondPrefix + '/execute', 'SELECT id, value FROM t ORDER BY id;')).data.rows, [[1, 0], [2, 0]]);

  equal((await request(firstPrefix + '/execute', 'BEGIN; INSERT INTO t VALUES(4,0);')).data.transactionState, 'ACTIVE');
  const closed = await request(firstPrefix + '/close');
  equal(closed.status, 200);
  equal(closed.data.transactionRolledBack, true);
  equal((await request(secondPrefix + '/execute', 'SELECT id, value FROM t ORDER BY id;')).data.rows, [[1, 0], [2, 0]]);
  equal((await request('/sessions', undefined, 'GET')).data.count, 1);
  equal((await request('/execute', 'SELECT id, value FROM t;')).status, 409);

  const competing = await request('/sessions');
  equal(competing.status, 201);
  const competingPrefix = '/sessions/' + competing.data.sessionId;
  equal((await request(secondPrefix + '/execute', 'BEGIN; UPDATE t SET value = value + 1 WHERE id = 1;')).data.transactionState, 'ACTIVE');
  const competingUpdate = request(competingPrefix + '/execute', 'BEGIN; UPDATE t SET value = value + 1 WHERE id = 1; COMMIT;');
  await new Promise(resolve => setTimeout(resolve, 40));
  const lockList = await request('/sessions', undefined, 'GET');
  equal(lockList.data.entries.find(entry => entry.sessionId === competing.data.sessionId).waitingForLock, true);
  equal((await request(secondPrefix + '/execute', 'COMMIT;')).data.transactionState, 'IDLE');
  const updated = await competingUpdate;
  equal(updated.status, 200);
  equal(updated.data.transactionState, 'IDLE');
  equal((await request(secondPrefix + '/execute', 'SELECT id, value FROM t ORDER BY id;')).data.rows, [[1, 2], [2, 0]]);
  equal((await request(competingPrefix + '/close')).status, 200);

  const third = await request('/sessions');
  equal(third.status, 201);
  const fourth = await request('/sessions');
  equal(fourth.status, 201);
  const fifth = await request('/sessions');
  equal(fifth.status, 409);
  assert.match(fifth.data.error.message, /Session limit reached/);
  ++checks;

  equal((await request(secondPrefix + '/close')).status, 200);
  equal((await request('/sessions/' + third.data.sessionId + '/close')).status, 200);
  equal((await request('/sessions/' + fourth.data.sessionId + '/close')).status, 200);
  equal((await request('/sessions', undefined, 'GET')).data.count, 0);
  equal((await request('/execute', 'SELECT id, value FROM t ORDER BY id;')).data.rows, [[1, 2], [2, 0]]);

  const capabilities = await request('/capabilities', undefined, 'GET');
  equal(capabilities.data.multiSession, true);
  equal(capabilities.data.maxSessions, 3);
  equal(capabilities.data.concurrencyModel, 'serialized-two-phase-database-lock');
  console.log(`${checks} multi-session HTTP checks passed: registry, lock waiting, timeout, rollback and session limits`);
} catch (error) {
  console.error('server stderr:', errors);
  throw error;
} finally {
  clearTimeout(deadline);
  server.kill();
  await exited;
}

import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';

const directory = mkdtempSync(join(tmpdir(), 'minisql-auto-checkpoint-'));
const database = join(directory, 'database.pages');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const session = await openSession(executable, database, { env: { MINISQL_AUTO_CHECKPOINT_WRITES: '2' } });
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function result(response) { assert.equal(response.success, true, JSON.stringify(response)); ++checks; return response; }
function stats(response) { assert.equal(response.success, true, JSON.stringify(response)); ++checks; return response; }

try {
  result(await session.request('execute', 'CREATE TABLE t(id INT);'));
  equal(stats(await session.request('statistics')).checkpointCount, 0);
  equal(stats(await session.request('statistics')).pendingAutoCheckpointWrites, 1);

  result(await session.request('execute', 'INSERT INTO t VALUES(1);'));
  equal(stats(await session.request('statistics')).checkpointCount, 1);
  equal(stats(await session.request('statistics')).pendingAutoCheckpointWrites, 0);

  result(await session.request('execute', 'INSERT INTO t VALUES(2);'));
  equal(stats(await session.request('statistics')).checkpointCount, 1);
  equal(stats(await session.request('statistics')).pendingAutoCheckpointWrites, 1);

  result(await session.request('execute', 'CHECKPOINT;'));
  equal(stats(await session.request('statistics')).checkpointCount, 2);
  equal(stats(await session.request('statistics')).pendingAutoCheckpointWrites, 0);

  result(await session.request('execute', 'INSERT INTO t VALUES(3);'));
  result(await session.request('execute', 'INSERT INTO t VALUES(4);'));
  equal(stats(await session.request('statistics')).checkpointCount, 3);
  equal((await session.request('execute', 'SELECT * FROM t ORDER BY id;')).results[0].rows, [[1],[2],[3],[4]]);
  ++checks;
  const policySession = async (name, env) => {
    const policyDatabase = join(directory, `${name}.pages`);
    const worker = await openSession(executable, policyDatabase, { env });
    return { database: policyDatabase, worker };
  };
  for (const [name, env, reason, statistic] of [
    ['wal-threshold', { MINISQL_AUTO_CHECKPOINT_WAL_BYTES: '1' }, 'wal-bytes', 'autoCheckpointWalBytes'],
    ['dirty-pages-threshold', { MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES: '1' }, 'dirty-pages', 'autoCheckpointDirtyPages'],
    ['dirty-ratio-threshold', { MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO: '0.01' }, 'dirty-ratio', 'autoCheckpointDirtyRatio'],
  ]) {
    const { worker } = await policySession(name, env);
    try {
      result(await worker.request('execute', 'CREATE TABLE p(id INT);'));
      const after = stats(await worker.request('statistics'));
      assert.ok(after[statistic] > 0, `${statistic} is not exposed`); ++checks;
      equal(after.checkpointCount, 1);
      assert.ok(after.lastAutoCheckpointReasons.includes(reason), `${reason} did not trigger`); ++checks;
      equal(after.pendingAutoCheckpointWalBytes, 0);
      equal(after.walBytes, 0);
    } finally { await worker.close(); }
  }
  const { worker: intervalWorker } = await policySession('interval-threshold', { MINISQL_AUTO_CHECKPOINT_INTERVAL_MS: '10' });
  try {
    result(await intervalWorker.request('execute', 'CREATE TABLE p(id INT);'));
    const beforeInterval = stats(await intervalWorker.request('statistics'));
    await new Promise(resolve => setTimeout(resolve, 30));
    result(await intervalWorker.request('execute', 'INSERT INTO p VALUES(1);'));
    const afterInterval = stats(await intervalWorker.request('statistics'));
    assert.ok(afterInterval.checkpointCount > beforeInterval.checkpointCount, 'interval did not trigger'); ++checks;
    assert.ok(afterInterval.lastAutoCheckpointReasons.includes('interval'), 'interval reason missing'); ++checks;
  } finally { await intervalWorker.close(); }
  const { worker: transactionWorker } = await policySession('transaction-threshold', { MINISQL_AUTO_CHECKPOINT_WRITES: '2' });
  try {
    result(await transactionWorker.request('execute', 'CREATE TABLE p(id INT);'));
    result(await transactionWorker.request('execute', 'BEGIN; INSERT INTO p VALUES(1); INSERT INTO p VALUES(2);'));
    equal(stats(await transactionWorker.request('statistics')).checkpointCount, 0);
    result(await transactionWorker.request('execute', 'COMMIT;'));
    const committed = stats(await transactionWorker.request('statistics'));
    equal(committed.checkpointCount, 1);
    equal(committed.pendingAutoCheckpointWrites, 0);
    assert.ok(committed.lastAutoCheckpointReasons.includes('writes'), 'transaction write threshold did not trigger'); ++checks;
  } finally { await transactionWorker.close(); }
  console.log(`${checks} automatic checkpoint checks passed`);
} finally {
  await session.close();
}

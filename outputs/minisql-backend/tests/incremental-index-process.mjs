import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openSession } from '../scripts/session-process.mjs';

const executable = './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-incremental-index-'));
let checks = 0;

function equal(actual, expected, message) {
  assert.deepEqual(actual, expected, message);
  ++checks;
}

async function maintenance(session) {
  const response = await session.request('statistics');
  equal(response.success, true, 'statistics succeeds');
  return response.indexMaintenance;
}

async function exercise(database, env, verifyReload) {
  let stage = 'open-initial';
  let session = await openSession(executable, database, { env });
  const values = Array.from({ length: 200 }, (_, id) => `(${id},${id % 10})`).join(',');
  try {
    stage = 'create';
    equal((await session.request('execute', 'CREATE TABLE t(id INT PRIMARY KEY,v INT); CREATE INDEX idx_v ON t(v);')).success, true);
    const afterCreate = await maintenance(session);
    equal(afterCreate.engine, env.MINISQL_INDEX_ENGINE === 'memory' ? 'memory' : 'page-file');
    const rebuildBaseline = afterCreate.fullRebuilds;

    stage = 'bulk-dml';
    equal((await session.request('execute', `INSERT INTO t VALUES ${values};`)).success, true);
    equal((await session.request('execute', 'UPDATE t SET v=9 WHERE id<50; DELETE FROM t WHERE id>=150;')).success, true);
    const afterDml = await maintenance(session);
    equal(afterDml.fullRebuilds, rebuildBaseline, 'DML must not rebuild an index');
    assert.ok(afterDml.entriesInserted >= 250, 'insert and update entries are counted'); ++checks;
    assert.ok(afterDml.entriesErased >= 100, 'update and delete entries are counted'); ++checks;
    equal((await session.request('execute', 'SELECT id FROM t WHERE v=9 ORDER BY id;')).results.at(-1).rows,
      [...Array.from({ length: 50 }, (_, id) => [id]), ...[59,69,79,89,99,109,119,129,139,149].map(id => [id])]);
    const inspected = await session.request('indexInspect', undefined, { table: 't', index: 'idx_v' });
    if (afterCreate.engine === 'page-file') equal(inspected.rootReachable, true);
    else equal(inspected.storage, 'memory');

    stage = 'unique';
    equal((await session.request('execute', 'CREATE UNIQUE INDEX uq_id ON t(id);')).success, true);
    stage = 'unique-statistics';
    const beforeFailure = await maintenance(session);
    stage = 'unique-failure';
    equal((await session.request('execute', 'INSERT INTO t VALUES(1,1234);')).success, false);
    stage = 'unique-query';
    equal((await session.request('execute', 'SELECT v FROM t WHERE id=1;')).results.at(-1).rows, [[9]]);
    stage = 'unique-after-statistics';
    equal((await maintenance(session)).fullRebuilds, beforeFailure.fullRebuilds, 'failed DML must not rebuild');

    stage = 'rollback';
    equal((await session.request('execute', 'BEGIN; INSERT INTO t VALUES(1000,4); UPDATE t SET v=8 WHERE id=50; DELETE FROM t WHERE id=51; ROLLBACK;')).success, true);
    equal((await session.request('execute', 'SELECT id,v FROM t WHERE id>=50 AND id<=51 ORDER BY id;')).results.at(-1).rows, [[50,0],[51,1]]);
    equal((await session.request('execute', 'SELECT id FROM t WHERE id=1000;')).results.at(-1).rows, []);
    const afterRollback = await maintenance(session);
    equal(afterRollback.fullRebuilds, beforeFailure.fullRebuilds, 'transaction rollback reloads without rebuilding');
    assert.ok(afterRollback.runtimeReloads >= 1, 'transaction rollback refreshes runtime handles'); ++checks;

    stage = 'savepoint';
    equal((await session.request('execute', 'BEGIN; SAVEPOINT s; INSERT INTO t VALUES(1001,5); UPDATE t SET v=6 WHERE id=52; ROLLBACK TO s; COMMIT;')).success, true);
    equal((await session.request('execute', 'SELECT id,v FROM t WHERE id>=52 AND id<=52;')).results.at(-1).rows, [[52,2]]);
    equal((await session.request('execute', 'SELECT id FROM t WHERE id=1001;')).results.at(-1).rows, []);
    const afterSavepoint = await maintenance(session);
    equal(afterSavepoint.fullRebuilds, beforeFailure.fullRebuilds, 'savepoint rollback reloads without rebuilding');
    assert.ok(afterSavepoint.runtimeReloads >= 2, 'savepoint rollback refreshes runtime handles'); ++checks;

    equal((await session.request('execute', 'INSERT INTO t VALUES(2000,7);')).success, true);
  } catch (error) {
    throw new Error(`${env.MINISQL_INDEX_ENGINE ?? 'page'} ${stage}: ${error.message}`, { cause: error });
  } finally {
    if (verifyReload) await session.terminate();
    else await session.close().catch(() => {});
  }

  stage = 'reopen';
  try { session = await openSession(executable, database, { env }); }
  catch (error) { throw new Error(`${env.MINISQL_INDEX_ENGINE ?? 'page'} ${stage}: ${error.message}`, { cause: error }); }
  try {
    equal((await session.request('execute', 'SELECT id FROM t WHERE id=2000;')).results.at(-1).rows, [[2000]]);
    equal((await session.request('execute', 'SELECT id FROM t WHERE v=7 ORDER BY id;')).results.at(-1).rows.at(-1), [2000]);
    equal((await maintenance(session)).fullRebuilds, 0, 'restart loads persisted indexes without rebuilding');
  } finally {
    await session.close();
  }
}

await exercise(join(root, 'page.pages'), {}, true);
await exercise(join(root, 'memory.pages'), { MINISQL_INDEX_ENGINE: 'memory' }, false);
console.log(`${checks} incremental index checks passed`);

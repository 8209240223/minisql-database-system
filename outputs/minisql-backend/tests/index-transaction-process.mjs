import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openSession } from '../scripts/session-process.mjs';

// X20 / 索引事务化：唯一索引 build → validate → publish 三阶段、堆与索引同批次，
// 以及索引一致性检查（indexVerify）与在线重建（indexRebuild）。
const executable = './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-index-transaction-'));
let checks = 0;

function equal(actual, expected, message) {
  assert.deepEqual(actual, expected, message);
  ++checks;
}
function ok(value, message) {
  assert.ok(value, message);
  ++checks;
}

async function catalogIndexes(session, table) {
  const catalog = await session.request('catalog');
  const entry = catalog.tables.find(candidate => candidate.name === table);
  return entry ? entry.indexes : [];
}

async function exercise(database, env) {
  const label = env.MINISQL_INDEX_ENGINE === 'memory' ? 'memory' : 'page';
  let stage = 'open';
  const session = await openSession(executable, database, { env });
  try {
    stage = 'create';
    equal((await session.request('execute',
      'CREATE TABLE t(id INT PRIMARY KEY, v INT); ' +
      'INSERT INTO t VALUES(1,10),(2,10),(3,20);')).success, true,
      `${label}: create table and seed`);

    // ---- 三阶段 build → validate → publish：重复键必须在 validate 阶段被拒绝，且不发布 ----
    stage = 'unique-build-failure';
    const duplicate = await session.request('execute', 'CREATE UNIQUE INDEX uq_v ON t(v);');
    equal(duplicate.success, false, `${label}: unique index over duplicates is rejected`);
    ok(/duplicate/i.test(duplicate.error?.message ?? ''), `${label}: duplicate diagnostic`);
    equal((await catalogIndexes(session, 't')).length, 0, `${label}: failed build publishes nothing`);
    equal((await session.request('execute', 'SELECT id FROM t WHERE v=10 ORDER BY id;')).results.at(-1).rows,
      [[1], [2]], `${label}: table still readable after failed build`);

    // ---- 成功路径：非唯一索引，三阶段全部通过后发布并进入 IndexScan ----
    stage = 'create-index';
    equal((await session.request('execute', 'CREATE INDEX idx_v ON t(v);')).success, true, `${label}: create index`);
    const indexes = await catalogIndexes(session, 't');
    equal(indexes.map(index => index.name), ['idx_v'], `${label}: catalog publishes the index`);
    const plan = await session.request('compile', 'SELECT id FROM t WHERE v=10;');
    ok(plan.plan.some(node => node.kind === 'IndexScan' && node.indexName === 'idx_v'), `${label}: planner uses the published index`);

    // ---- 一致性检查：堆与索引一致 ----
    stage = 'verify-initial';
    const initial = await session.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(initial.success, true, `${label}: indexVerify succeeds`);
    equal(initial.consistent, true, `${label}: index is consistent`);
    equal(initial.heapRows, 3, `${label}: heap row count`);
    equal(initial.indexRows, 3, `${label}: index entry count`);
    equal(initial.missingEntries, 0, `${label}: no missing entries`);
    equal(initial.danglingEntries, 0, `${label}: no dangling entries`);

    // ---- INSERT/UPDATE/DELETE 与索引在同一写批次：提交后仍一致，回滚后索引恢复 ----
    stage = 'dml';
    equal((await session.request('execute',
      'INSERT INTO t VALUES(4,20),(5,30); UPDATE t SET v=40 WHERE id=1; DELETE FROM t WHERE id=2;')).success, true,
      `${label}: dml`);
    const afterDml = await session.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(afterDml.consistent, true, `${label}: consistent after dml`);
    equal(afterDml.heapRows, 4, `${label}: heap rows after dml`);
    equal((await session.request('execute', 'SELECT id FROM t WHERE v=10;')).results.at(-1).rows, [], `${label}: stale entry is gone`);

    stage = 'rollback';
    equal((await session.request('execute',
      'BEGIN; INSERT INTO t VALUES(6,50); DELETE FROM t WHERE id=3; ROLLBACK;')).success, true, `${label}: transaction rollback`);
    const afterRollback = await session.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(afterRollback.consistent, true, `${label}: consistent after rollback`);
    equal((await session.request('execute', 'SELECT id FROM t WHERE v=20 ORDER BY id;')).results.at(-1).rows,
      [[3], [4]], `${label}: rolled-back index entries restored`);

    // ---- 在线重建：build → validate → publish，重建后仍一致且计划仍走索引 ----
    stage = 'rebuild';
    const rebuild = await session.request('indexRebuild', undefined, { table: 't', index: 'idx_v' });
    equal(rebuild.success, true, `${label}: online rebuild succeeds`);
    ok(rebuild.entries >= 4, `${label}: rebuild reports entries`);
    const afterRebuild = await session.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(afterRebuild.consistent, true, `${label}: consistent after rebuild`);
    equal(afterRebuild.heapRows, afterRebuild.indexRows, `${label}: rebuild keeps heap and index in step`);
    const planAfterRebuild = await session.request('compile', 'SELECT id FROM t WHERE v=20;');
    ok(planAfterRebuild.plan.some(node => node.kind === 'IndexScan' && node.indexName === 'idx_v'),
      `${label}: rebuild keeps the index usable`);

    // ---- 事务内重建：随事务回滚，索引页与运行时都恢复 ----
    stage = 'rebuild-in-transaction';
    equal((await session.request('execute',
      'BEGIN; INSERT INTO t VALUES(7,60); ROLLBACK;')).success, true, `${label}: transaction around rebuild`);
    const rebuildInTransaction = await session.request('execute', 'BEGIN;');
    equal(rebuildInTransaction.success, true, `${label}: begin`);
    const rebuilt = await session.request('indexRebuild', undefined, { table: 't', index: 'idx_v' });
    equal(rebuilt.success, true, `${label}: rebuild inside transaction is pending`);
    equal(rebuilt.commitState, 'pending', `${label}: rebuild joins the open transaction`);
    const rolledBackRebuild = await session.request('execute', 'ROLLBACK;');
    equal(rolledBackRebuild.success, true, `${label}: rollback the rebuild`);
    const afterTransactionalRebuild = await session.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(afterTransactionalRebuild.consistent, true, `${label}: consistent after transactional rebuild rollback`);
    equal((await session.request('execute', 'SELECT id FROM t WHERE id=7;')).results.at(-1).rows, [], `${label}: rolled-back insert gone`);

    // ---- 唯一索引在数据修正后可以建成 ----
    stage = 'unique-after-fix';
    equal((await session.request('execute', 'UPDATE t SET v=id WHERE id>=3;')).success, true, `${label}: make v unique`);
    equal((await session.request('execute', 'CREATE UNIQUE INDEX uq_v ON t(v);')).success, true, `${label}: unique index builds`);
    const uniqueVerify = await session.request('indexVerify', undefined, { table: 't', index: 'uq_v' });
    equal(uniqueVerify.consistent, true, `${label}: unique index consistent`);
    equal(uniqueVerify.unique, true, `${label}: unique flag reported`);
    equal((await session.request('execute', 'INSERT INTO t VALUES(8,40);')).success, false, `${label}: unique enforcement through index`);
    // 唯一冲突只是语句级失败：随写批次回滚索引页后实例必须保持可用（不能变成 commitState unknown）。
    equal((await session.request('execute', 'SELECT id FROM t WHERE id=8;')).results.at(-1).rows, [],
      `${label}: instance stays usable after a unique violation`);
    equal((await session.request('indexVerify', undefined, { table: 't', index: 'uq_v' })).consistent, true,
      `${label}: index survives a unique violation rollback`);
  } catch (error) {
    throw new Error(`${label} ${stage}: ${error.message}`, { cause: error });
  } finally {
    await session.close().catch(() => {});
  }

  // ---- 重启后索引页恢复，一致性检查仍然通过 ----
  const reopened = await openSession(executable, database, { env });
  try {
    const verify = await reopened.request('indexVerify', undefined, { table: 't', index: 'idx_v' });
    equal(verify.success, true, `${label}: reopen indexVerify`);
    equal(verify.consistent, true, `${label}: reopens consistent`);
  } finally {
    await reopened.close();
  }
}

await exercise(join(root, 'page.pages'), {});
await exercise(join(root, 'memory.pages'), { MINISQL_INDEX_ENGINE: 'memory' });
console.log(`${checks} index transaction checks passed`);

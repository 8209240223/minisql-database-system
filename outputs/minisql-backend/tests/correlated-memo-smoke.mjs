import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';

// X09 §6.17 后续 ②：绑定级 memo 跨语句复用。
// 必须在「同一进程」观测——spawnSync 每语句起新进程时缓存必然为空，验证不了跨语句复用。
// 这里用长驻 session 模式，并通过 statistics().correlatedMemo 读取命中计数。
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/correlated-memo-', import.meta.url)));
const executable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');

let checks = 0;
function equal(actual, expected, label) {
  assert.deepEqual(actual, expected, label ?? `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  ++checks;
}
async function run(session, sql) {
  const response = await session.request('execute', sql);
  assert.equal(response.success, true, JSON.stringify(response));
  return response.results.at(-1);
}
async function memo(session) {
  const stats = await session.request('statistics');
  assert.equal(stats.success, true, JSON.stringify(stats));
  assert.ok(stats.correlatedMemo, 'statistics should expose correlatedMemo');
  return stats.correlatedMemo;
}

// 投影中的标量相关子查询：不经 WHERE 去相关，走 runCorrelatedSubquery 按行绑定 + memo。
const projection = 'SELECT id, (SELECT s.val FROM s WHERE s.grp = x.grp) AS v FROM t x ORDER BY id;';
// 6 行外层、3 个不同 grp（10/10/20/20/30/30）→ 首次执行 3 miss + 3 hit。
const expectedFirst = [[1, 100], [2, 100], [3, null], [4, null], [5, 300], [6, 300]];

const session = await openSession(executable, file);
try {
  await run(session, 'CREATE TABLE t(id INT, grp INT); CREATE TABLE s(grp INT, val INT);'
    + 'INSERT INTO t VALUES(1,10),(2,10),(3,20),(4,20),(5,30),(6,30);'
    + 'INSERT INTO s VALUES(10,100),(30,300);');
  const afterSetup = await memo(session);
  equal(afterSetup.hits, 0, 'no correlated subquery yet');
  equal(afterSetup.entries, 0, 'memo empty before first query');

  // 首次执行：3 个绑定值各物化一次，重复绑定命中。
  equal((await run(session, projection)).rows, expectedFirst);
  const first = await memo(session);
  equal(first.misses, 3, 'three distinct bindings materialized');
  equal(first.hits, 3, 'repeated bindings hit within the statement');
  equal(first.entries, 3, 'three distinct (shape|binding) entries retained');
  const versionBefore = first.dataVersion;

  // 第二次执行同一 SQL（无写入）：缓存应跨语句复用 → 全命中、零新增未命中。
  equal((await run(session, projection)).rows, expectedFirst);
  const second = await memo(session);
  equal(second.misses, first.misses, 'second statement must not re-materialize');
  equal(second.hits, first.hits + 6, 'second statement reuses the memo across statements');
  equal(second.entries, 3, 'entries unchanged');

  // 写入使 memo 失效：dataVersion 自增，下一次执行重新物化且结果反映新数据。
  await run(session, 'INSERT INTO s VALUES(20,200);');
  const afterWrite = await memo(session);
  equal(afterWrite.dataVersion, versionBefore + 1, 'write bumps dataVersion');

  const updated = await run(session, projection);
  equal(updated.rows, [[1, 100], [2, 100], [3, 200], [4, 200], [5, 300], [6, 300]], 'memo must reflect the new row');
  const third = await memo(session);
  equal(third.misses, second.misses + 3, 'invalidation forces re-materialization');
  equal(third.hits, second.hits + 3, 'repeat bindings hit again after refill');
  equal(third.cacheVersion, third.dataVersion, 'cache version tracks data version');

  // 再次只读重复：仍然跨语句复用。
  await run(session, projection);
  const fourth = await memo(session);
  equal(fourth.misses, third.misses, 'read-only repeat does not re-materialize');
  equal(fourth.hits, third.hits + 6, 'memo retained after read-only statement');
} finally {
  await session.terminate();
}

console.log(`${checks} correlated-memo checks passed`);

// X18 4.3: 成本/估计并入 HTTP 契约——compile() 的顶层 plan / optimizedPlan 每个节点
// 都带 estimatedRows / estimatedCost / statsSource，顶层带 estimateModel /
// estimatedRowsAvailable。EXPLAIN 结果里的 plan 节点保持既有同源字段。
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-estimate-')), 'db.pages');
function run(mode, sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql || '', encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

const setup = 'CREATE TABLE t(a INT, b INT); CREATE TABLE u(x INT, y INT);'
  + 'INSERT INTO t VALUES(1,9),(2,8),(3,7),(4,6),(5,5),(6,4),(7,3),(8,2),(9,1),(10,0);'
  + 'INSERT INTO u VALUES(1,1),(2,2);';
equal(run('execute', setup).success, true);

const compile = run('compile', 'SELECT t.a FROM t JOIN u ON t.a=u.x WHERE t.b < 5;');
equal(compile.success, true);
equal(compile.estimateModel, 'stats-v1');
equal(compile.estimatedRowsAvailable, true);

// 顶层 plan 与 optimizedPlan 都存在，且每个节点都带三个估计字段。
function everyNodeHasEstimates(rows) {
  return rows.length > 0 && rows.every((n) => typeof n.estimatedRows === 'number' && typeof n.estimatedCost === 'number' && n.statsSource === 'stats-v1');
}
ok(everyNodeHasEstimates(compile.plan), 'compile() 顶层 plan 每个节点带 estimatedRows/estimatedCost/statsSource');
ok(everyNodeHasEstimates(compile.optimizedPlan), 'compile() 顶层 optimizedPlan 每个节点带估计字段');

// Filter 节点：直方图选择率已进入 estimate（b<5 → 约一半），estimatedRows 落在 (0, 总行数)。
const filterNode = compile.optimizedPlan.find((n) => n.kind === 'Filter');
const scanNode = compile.optimizedPlan.find((n) => n.kind === 'SeqScan');
ok(scanNode && scanNode.estimatedRows === 10, `SeqScan 估计行数等于 t 全表行数（${scanNode?.estimatedRows}）`);
ok(filterNode && filterNode.estimatedRows > 0 && filterNode.estimatedRows < 10, `Filter 直方图选择率生效（estimatedRows=${filterNode?.estimatedRows}）`);

// 计划 id 即 DFS 序：estimate 只按 id 打点，不破坏 serializer 的节点顺序/字段。
const ids = compile.optimizedPlan.map((n) => n.id);
equal(ids, [...ids].sort((a, b) => a - b), 'optimizedPlan 节点 id 仍是递增 DFS 序');
ok(compile.plan.every((n) => 'detail' in n && 'kind' in n), '既有 plan 字段（kind/detail）保持不被估计字段覆盖');

// 确定性：同 SQL 多次编译，估计行数一致。
const again = run('compile', 'SELECT t.a FROM t JOIN u ON t.a=u.x WHERE t.b < 5;').optimizedPlan
  .map((n) => n.estimatedRows);
equal(compile.optimizedPlan.map((n) => n.estimatedRows), again, 'compile 估计行数跨次确定');

// EXPLAIN 走 execute 路径：其 plan 节点仍携带同源字段（与 4.2-iv 保持一致）。
const explain = run('execute', 'EXPLAIN SELECT t.a FROM t JOIN u ON t.a=u.x WHERE t.b < 5;').results.at(-1);
equal(explain.kind, 'Explain');
equal(explain.estimatedRowsAvailable, true);
equal(explain.costModel, 'stats-v1');
ok(explain.optimizedPlan.every((n) => typeof n.estimatedRows === 'number' && n.statsSource === 'stats-v1'), 'EXPLAIN plan 节点继续携带估计字段');

console.log(`${checks} statistics-compile-estimate checks passed`);
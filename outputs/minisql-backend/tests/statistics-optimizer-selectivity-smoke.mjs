import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-opt-select-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }
function plan(sql) {
  const result = run('EXPLAIN ' + sql + ';');
  assert.equal(result.success, true, JSON.stringify(result));
  return result.results.at(-1);
}
// 真实统计：t(a) = 0..9，数值列 a 有等宽直方图（distinct=10, min=0, max=9）。
equal(run('CREATE TABLE t(a INT);').success, true);
equal(run('INSERT INTO t VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9);').success, true);

function joinKinds(explain) {
  return explain.optimizedPlan.filter((n) => n.kind === 'HashJoin' || n.kind === 'NestedLoopJoin').map((n) => n.kind);
}
// 高选择性旁路过滤器（x.a < 1 → 直方图选择率 0.1 → 左侧过滤后约 1 行）：
// 直方图驱动：L=1, R=10 → Hash(11) > NL(10) → 保留 NestedLoopJoin。
// 若无直方图（默认 0.25）则 L=2.5, R=10 → Hash(12.5) <= NL(25) → 选 HashJoin。
const selective = plan('SELECT x.a FROM t x JOIN t y ON x.a=y.a WHERE x.a < 1');
ok(joinKinds(selective).includes('NestedLoopJoin'), 'selective side filter keeps NestedLoopJoin (histogram selectivity ~0.1)');
ok(!joinKinds(selective).includes('HashJoin'), 'histogram selectivity shrinks left side below Hash break-even');

// 低/无选择性的旁路过滤器（x.a < 11 → 全部 10 行，选择率 1.0）：仍是 HashJoin。
const unselective = plan('SELECT x.a FROM t x JOIN t y ON x.a=y.a WHERE x.a < 11');
ok(joinKinds(unselective).includes('HashJoin'), 'non-selective filter keeps HashJoin');
ok(!joinKinds(unselective).includes('NestedLoopJoin'), 'non-selective filter not rewritten to nested loop');

// 连乘（交集）：x.a < 1 AND x.a >= 0 → 0.1 * 1.0 = 0.1 → 同样落 NestedLoopJoin。
const conjunction = plan('SELECT x.a FROM t x JOIN t y ON x.a=y.a WHERE x.a < 1 AND x.a >= 0');
ok(joinKinds(conjunction).includes('NestedLoopJoin'), 'conjunctive (AND) selectivity multiplies to same selective outcome');

// 确定性：同 SQL 同统计，join 选择序列稳定。
equal(joinKinds(plan('SELECT x.a FROM t x JOIN t y ON x.a=y.a WHERE x.a < 1')), joinKinds(selective), 'selective filter join choice deterministic');
equal(joinKinds(plan('SELECT x.a FROM t x JOIN t y ON x.a=y.a WHERE x.a < 11')), joinKinds(unselective), 'unselective filter join choice deterministic');

// 旁路下推后的 Filter 节点估计行数应反映直方图选择率（约 10*0.1=1 行）。
const pushedFilter = selective.optimizedPlan.find((n) => n.kind === 'Filter');
ok(pushedFilter && typeof pushedFilter.estimatedRows === 'number' && pushedFilter.estimatedRows >= 0 && pushedFilter.estimatedRows <= 1.5,
   'pushed-down Filter estimatedRows reflects histogram selectivity (~1 row)');
ok(pushedFilter && pushedFilter.statsSource === 'stats-v1', 'pushed-down Filter carries statsSource');

// 结果正确性不受 join 选择影响。
equal(run('SELECT COUNT(*) AS c FROM t x JOIN t y ON x.a=y.a WHERE x.a < 1;').results.at(-1).rows, [[1]]);
equal(run('SELECT COUNT(*) AS c FROM t x JOIN t y ON x.a=y.a WHERE x.a < 11;').results.at(-1).rows, [[10]]);
equal(run('SELECT COUNT(*) AS c FROM t x JOIN t y ON x.a=y.a WHERE x.a < 1 AND x.a >= 0;').results.at(-1).rows, [[1]]);

console.log(`${checks} statistics-optimizer-selectivity checks passed`);
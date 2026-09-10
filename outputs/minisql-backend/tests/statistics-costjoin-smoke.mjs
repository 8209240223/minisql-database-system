import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-costjoin-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }
function joinKinds(sql) {
  const result = run('EXPLAIN ' + sql);
  assert.equal(result.success, true, JSON.stringify(result));
  return result.results.at(-1).optimizedPlan.filter((n) => n.kind === 'HashJoin' || n.kind === 'NestedLoopJoin').map((n) => n.kind);
}
// 真实统计：t 多行（10 行），s 单行（1 行）。
equal(run('CREATE TABLE t(a INT, b INT); INSERT INTO t VALUES(1,9),(2,8),(3,7),(4,6),(5,5),(6,4),(7,3),(8,2),(9,1),(10,0);').success, true);
equal(run('CREATE TABLE s(a INT); INSERT INTO s VALUES(5);').success, true);

// 多行两端（真实 L=M=10）：Hash(20) <= NL(100) → 选 HashJoin。
const multi = joinKinds('SELECT x.a FROM t x JOIN t y ON x.a=y.a;');
ok(multi.includes('HashJoin'), 'multi-row equality join chosen as HashJoin by cost');
ok(!multi.includes('NestedLoopJoin'), 'hash join replaced nested loop on multi-row sides');

// 单行端（真实 s=1 行，L=10,R=1）：Hash(11) > NL(10) → 保留 NestedLoopJoin（成本驱动新行为）。
const single = joinKinds('SELECT x.a FROM t x JOIN s y ON x.a=y.a;');
ok(single.includes('NestedLoopJoin'), 'single-row-side join keeps NestedLoopJoin by cost');
ok(!single.includes('HashJoin'), 'single-row-side join not rewritten to hash');

// 确定性：同 SQL 同统计多次优化，join 选择序列一致（成本模型无随机/容器序依赖）。
equal(joinKinds('SELECT x.a FROM t x JOIN t y ON x.a=y.a;'), multi, 'join choice deterministic across compiles');
equal(joinKinds('SELECT x.a FROM t x JOIN s y ON x.a=y.a;'), single, 'single-row join choice is stable');

// 结果正确性不受优化 join 选择影响。
equal(run('SELECT COUNT(*) AS c FROM t x JOIN t y ON x.a=y.a;').results.at(-1).rows, [[10]]);
equal(run('SELECT COUNT(*) AS c FROM t x JOIN s y ON x.a=y.a;').results.at(-1).rows, [[1]]);

console.log(`${checks} statistics-costjoin checks passed`);
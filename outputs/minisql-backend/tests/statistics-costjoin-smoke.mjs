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
equal(run('CREATE TABLE t(a INT, b INT); INSERT INTO t VALUES(1,9),(2,8),(3,7),(4,6),(5,5),(6,4),(7,3),(8,2),(9,1),(10,0);').success, true);

// 多行等值连接：成本上 Hash(L+M) < NestedLoop(L*M) → 选 HashJoin，且无 NestedLoopJoin 节点（已替换）。
const kinds = joinKinds('SELECT x.a FROM t x JOIN t y ON x.a=y.a;');
ok(kinds.includes('HashJoin'), 'multi-row equality join chosen as HashJoin by cost');
ok(!kinds.includes('NestedLoopJoin'), 'hash join replaced nested loop on multi-row sides');

// 确定性：同 SQL 同统计多次优化，join 选择序列一致（成本模型无随机/容器序依赖）。
equal(joinKinds('SELECT x.a FROM t x JOIN t y ON x.a=y.a;'), kinds, 'join choice deterministic across compiles');
equal(joinKinds('SELECT x.a FROM t x JOIN t y ON x.a=y.a;'), kinds, 'join choice stable on repeated compile');

// 结果正确性不受优化 join 选择影响：HashJoin 路径返回正确行数。
equal(run('SELECT COUNT(*) AS c FROM t x JOIN t y ON x.a=y.a;').results.at(-1).rows, [[10]]);

// 注意：当前 parser 不支持「派生表 + JOIN」，且优化器对表统一按有界默认行数估算，
// SQL 层面暂无法构造单行端走到「保留 NestedLoopJoin」分支；该新分支留待派生表 JOIN / 真实统计接入后再验证。
console.log(`${checks} statistics-costjoin checks passed`);
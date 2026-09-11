import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-correlated-agg-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
function compile(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'compile'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1); }
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
const kinds = (sql, key = 'plan') => new Set((compile(sql)[key] || []).map(node => node.kind));

// X09 4.x 收尾：相关标量子查询在 HAVING / 聚合投影 / ORDER BY 位置的去相关。
// 分两类路径：
//   (a) 子计划可由 in-process joinRows 执行（Project/Filter/Scan/Join）→ 仍去相关为
//       SemiJoin/AntiSemiJoin/Apply；
//   (b) 子计划含聚合等 joinRows 不支持的节点 → 退回执行期按行绑定（Correlated* 表达式）。
// 两类都必须给出与 SQL 语义一致的结果。
const setup = 'CREATE TABLE t(id INT, grp INT); CREATE TABLE u(k INT, val INT);'
  + 'INSERT INTO t VALUES(1,10),(2,20),(3,30),(4,40);'
  + 'INSERT INTO u VALUES(10,5),(10,7),(20,3),(50,1);';
equal(run(setup).success, true);
// u 分布：k=10 → 2 行(5,7)；k=20 → 1 行(3)；k=30/40 → 无行；k=50 不被引用。

// --- HAVING 相关标量子查询（聚合之上的外层列必须重映射到分组槽位）---
equal(query('SELECT grp FROM t GROUP BY grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = t.grp) > 0 ORDER BY grp;').rows, [[10],[20]]);
equal(query('SELECT grp FROM t GROUP BY grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = t.grp) > 1 ORDER BY grp;').rows, [[10]]);
equal(query('SELECT grp FROM t GROUP BY grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = t.grp) = 0 ORDER BY grp;').rows, [[30],[40]]);
// 比较型（子查询在右侧）与表别名限定名。
equal(query('SELECT grp FROM t GROUP BY grp HAVING grp > (SELECT MIN(u.k) FROM u WHERE u.k = t.grp) ORDER BY grp;').rows, []);
equal(query('SELECT x.grp FROM t x GROUP BY x.grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = x.grp) > 0 ORDER BY x.grp;').rows, [[10],[20]]);
// HAVING 内的相关 EXISTS / NOT EXISTS / IN。
equal(query('SELECT grp FROM t GROUP BY grp HAVING EXISTS (SELECT 1 FROM u WHERE u.k = t.grp) ORDER BY grp;').rows, [[10],[20]]);
equal(query('SELECT grp FROM t GROUP BY grp HAVING NOT EXISTS (SELECT 1 FROM u WHERE u.k = t.grp) ORDER BY grp;').rows, [[30],[40]]);
equal(query('SELECT grp FROM t GROUP BY grp HAVING grp IN (SELECT u.k FROM u WHERE u.k = t.grp) ORDER BY grp;').rows, [[10],[20]]);
// HAVING 两侧同时含聚合函数与相关子查询（每组 COUNT(*)=1）。
equal(query('SELECT grp, COUNT(*) FROM t GROUP BY grp HAVING COUNT(*) <= (SELECT COUNT(*) FROM u WHERE u.k = t.grp) ORDER BY grp;').rows, [[10,1],[20,1]]);

// --- 聚合投影 / ORDER BY 位置的相关子查询 ---
equal(query('SELECT grp, (SELECT COUNT(*) FROM u WHERE u.k = t.grp) AS c FROM t GROUP BY grp ORDER BY grp;').rows, [[10,2],[20,1],[30,0],[40,0]]);
// ORDER BY 相关子查询（含聚合）：k=10→max(val)=7、k=20→3、k=30/40→NULL（空集补 NULL，排最后）。
equal(query('SELECT id, grp FROM t ORDER BY (SELECT MAX(u.val) FROM u WHERE u.k = t.grp) ASC, id ASC;').rows, [[2,20],[1,10],[3,30],[4,40]]);

// --- WHERE 侧：子查询含聚合时退回执行期绑定，结果仍须正确 ---
equal(query('SELECT id FROM t WHERE grp = (SELECT MAX(u.k) FROM u WHERE u.k = t.grp) ORDER BY id;').rows, [[1],[2]]);
equal(query('SELECT COUNT(*) FROM t WHERE grp = (SELECT MAX(u.k) FROM u WHERE u.k = t.grp);').rows, [[2]]);
equal(query('SELECT id FROM t WHERE grp IN (SELECT MAX(u.k) FROM u WHERE u.k = t.grp) ORDER BY id;').rows, [[1],[2]]);
// NOT IN 陷阱：grp=30/40 的子查询返回单行 NULL（MAX 空集），NOT IN {NULL} → NULL → 排除。
equal(query('SELECT id FROM t WHERE grp NOT IN (SELECT MAX(u.k) FROM u WHERE u.k = t.grp) ORDER BY id;').rows, []);

// --- 语义拒绝：HAVING 的相关子查询引用未分组的基表列 ---
const rejected = run('SELECT grp FROM t GROUP BY grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = t.id) > 0;');
equal(rejected.success, false);
equal(rejected.error.code, 2003);

// --- 结构化断言（适配 main 的计划契约）：WHERE 含子查询时顶层节点标记为 SemiJoin，
// 无论子计划是否含聚合；HAVING 之上仍保留 Aggregate 节点。结果正确性由上面的执行断言覆盖。 ---
equal(kinds('SELECT id FROM t WHERE grp IN (SELECT u.k FROM u WHERE u.k = t.grp);').has('SemiJoin'), true);
equal(kinds('SELECT id FROM t WHERE grp IN (SELECT MAX(u.k) FROM u WHERE u.k = t.grp);').has('SemiJoin'), true);
equal(kinds('SELECT grp FROM t GROUP BY grp HAVING (SELECT COUNT(*) FROM u WHERE u.k = t.grp) > 0;').has('Aggregate'), true);

console.log(`${checks} correlated-aggregate checks passed`);

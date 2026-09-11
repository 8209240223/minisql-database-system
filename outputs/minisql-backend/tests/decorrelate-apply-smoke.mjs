import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-decorr-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1); }
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }

// 相关 NOT IN 去相关 → AntiSemiJoin(残差)，含 NULL 内层语义。
const setup = 'CREATE TABLE t(id INT, grp INT); CREATE TABLE s(grp INT, val INT);'
  + 'INSERT INTO t VALUES(1,10),(2,20),(3,30);'
  + 'INSERT INTO s VALUES(10,100),(20,NULL),(30,30),(99,999);';
equal(run(setup).success, true);

// NOT IN：grp=20 内层含 NULL → NOT IN 为 NULL → 排除；其余命中按内层值判定。
equal(query('SELECT id FROM t x WHERE x.grp NOT IN (SELECT s.val FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[1]]);
// IN：grp=30 与内层 30 相等命中；grp=20 内层 NULL 不相等也算不命中→排除；grp=10 {100} 不命中。
equal(query('SELECT id FROM t x WHERE x.grp IN (SELECT s.val FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[3]]);
// NOT EXISTS / EXISTS：s 含 grp=10、20、30 → 全部存在对应 s 行。
equal(query('SELECT id FROM t x WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, []);
equal(query('SELECT id FROM t x WHERE EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[1],[2],[3]]);

// 标量相关 → Apply(scalar)：
// 等式（子查询在右侧）：id=1(10) vs 100 不等；id=2(20) 内层 NULL→NULL；id=3(30) 相等。
equal(query('SELECT id FROM t x WHERE x.grp = (SELECT s.val FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[3]]);
// 比较（子查询在左侧）：内层 NULL 的比较为 NULL → 排除 id=2；100>0、30>0 保留。
equal(query('SELECT id FROM t x WHERE (SELECT s.val FROM s WHERE s.grp = x.grp) > 0 ORDER BY id;').rows, [[1],[3]]);
// 子查询空集（内层补 NULL，等值 → NULL → 排除）。
equal(query('SELECT id FROM t x WHERE x.grp = (SELECT s.val FROM s WHERE s.grp = x.grp AND s.grp > 1000) ORDER BY id;').rows, []);

// 派生表基座 + 相关 IN：多行共享分组值，集合语义。
const agg = 'CREATE TABLE a(id INT, g INT); CREATE TABLE b(g INT, v INT);'
  + "INSERT INTO a VALUES(1,10),(2,10),(3,20),(4,20); INSERT INTO b VALUES(10,5),(20,7);";
equal(run(agg).success, true);
equal(query('SELECT id FROM a x WHERE x.g IN (SELECT b.g FROM b WHERE b.g = x.g) ORDER BY id;').rows, [[1],[2],[3],[4]]);
equal(query('SELECT id FROM a x WHERE x.g NOT IN (SELECT b.g FROM b WHERE b.g = x.g) ORDER BY id;').rows, []);
equal(query('SELECT id FROM a x WHERE x.g = (SELECT b.g FROM b WHERE b.g = x.g) ORDER BY id;').rows, [[1],[2],[3],[4]]);

console.log(`${checks} decorrelate/apply checks passed`);
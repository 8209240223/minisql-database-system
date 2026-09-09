import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-correlated-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1); }
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }

// 多行共享相同分组键，验证相关子查询按绑定参数分组物化（集合语义半连接）的结果正确。
const setup = 'CREATE TABLE t(id INT, grp INT); CREATE TABLE s(grp INT, val INT);'
  + 'INSERT INTO t VALUES(1,10),(2,10),(3,20),(4,20),(5,30),(6,30);'
  + 'INSERT INTO s VALUES(10,100),(30,300);';
equal(run(setup).success, true);

// EXISTS 相关：仅 grp=10、grp=30 命中。
equal(query('SELECT id FROM t x WHERE EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[1],[2],[5],[6]]);
// IN 相关（整型等值：外层列 = 内层列）。
equal(query('SELECT id FROM t x WHERE x.grp IN (SELECT s.grp FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[1],[2],[5],[6]]);
// NOT EXISTS：没命中的组（grp=20）。
equal(query('SELECT id FROM t x WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[3],[4]]);
// 标量相关：每组取值（命中时返回 val）。
equal(query('SELECT id FROM t x WHERE (SELECT s.val FROM s WHERE s.grp = x.grp) IS NOT NULL ORDER BY id;').rows, [[1],[2],[5],[6]]);

// 跨语句不过期：在同一库先后执行，后一次必须反映新写入（验证语句级清缓存）。
equal(query('SELECT COUNT(*) AS c FROM t x WHERE EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp);').rows, [[4]]);
equal(run("INSERT INTO s VALUES(20,200);").success, true);
equal(query('SELECT id FROM t x WHERE EXISTS (SELECT 1 FROM s WHERE s.grp = x.grp) ORDER BY id;').rows, [[1],[2],[3],[4],[5],[6]]);

console.log(`${checks} correlated-exec checks passed`);
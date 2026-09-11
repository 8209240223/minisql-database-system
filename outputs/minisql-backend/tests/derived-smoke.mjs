import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-derived-')), 'db.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1); }
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function rejected(sql) { assert.equal(run(sql).success, false, sql); ++checks; }
const setup = 'CREATE TABLE t(id INT, a INT, b INT); CREATE TABLE s(id INT, v INT); INSERT INTO t VALUES(1,10,100),(2,20,200),(3,30,300); INSERT INTO s VALUES(1,7),(2,8),(3,9);';
equal(run(setup).success, true);
// 基本投影：内层 filter + alias，外层按别名列读取。
equal(query('SELECT d.x FROM (SELECT a AS x, b AS y FROM t WHERE a >= 20) AS d ORDER BY x;').rows, [[20],[30]]);
// 外层 WHERE 引用派生列。
equal(query('SELECT x, y FROM (SELECT a*2 AS x, b+1 AS y FROM t) AS d WHERE x > 30 ORDER BY x;').rows, [[40,201],[60,301]]);
// 外层 select * 展开派生列。
equal(query('SELECT * FROM (SELECT a FROM t) AS d ORDER BY a;').rows, [[10],[20],[30]]);
// qualified 引用 d.x。
equal(query('SELECT d.x, d.y FROM (SELECT a AS x, b AS y FROM t) d ORDER BY d.x;').rows, [[10,100],[20,200],[30,300]]);
// 派生表可再被外层 WHERE/ORDER 消费。
equal(query('SELECT z FROM (SELECT a AS z FROM t) AS q WHERE z=30;').rows, [[30]]);
// 内层聚合的派生表。
equal(query('SELECT cnt FROM (SELECT COUNT(*) AS cnt FROM s) AS c;').rows, [[3]]);
// 内层 DISTINCT 派生表。
equal(query('SELECT a FROM (SELECT DISTINCT a FROM t) AS d ORDER BY a;').rows, [[10],[20],[30]]);
// 多层派生表。
equal(query('SELECT e FROM (SELECT x+1 AS e FROM (SELECT a AS x FROM t) AS i) AS o ORDER BY e;').rows, [[11],[21],[31]]);
// 重复列名（语义歧义）拒绝。
rejected('SELECT * FROM (SELECT a AS x, b AS x FROM t) AS d;');
// 缺显式别名拒绝。
rejected('SELECT * FROM (SELECT a FROM t);');
// 内外层列不冲突（别名遮蔽原表）。
equal(query('SELECT v FROM (SELECT a AS v FROM t) AS d ORDER BY v;').rows, [[10],[20],[30]]);
console.log(`${checks} derived-table checks passed`);
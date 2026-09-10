import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-subquery-')), 'db.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1); }
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function rejected(sql) { assert.equal(run(sql).success, false, sql); ++checks; }
equal(run('CREATE TABLE t(id INT); CREATE TABLE s(id INT); CREATE TABLE sn(id INT); CREATE TABLE empty(id INT); INSERT INTO t VALUES(1),(2),(3); INSERT INTO s VALUES(2),(3); INSERT INTO sn VALUES(2),(NULL);').success, true);
equal(query('SELECT id FROM t WHERE id IN (SELECT id FROM s) ORDER BY id;').rows, [[2],[3]]);
equal(query('SELECT id FROM t WHERE id NOT IN (SELECT id FROM s) ORDER BY id;').rows, [[1]]);
equal(query('SELECT id FROM t WHERE EXISTS (SELECT id FROM s WHERE s.id=2) ORDER BY id;').rows, [[1],[2],[3]]);
assert.ok(query('EXPLAIN SELECT id FROM t WHERE EXISTS (SELECT id FROM s WHERE s.id=2);').plan.some(row => row.subqueryJoinKind === 'SemiJoin'));
++checks;
equal(query('SELECT id,id IN (SELECT id FROM s) AS flag FROM t ORDER BY id;').rows, [[1,false],[2,true],[3,true]]);
equal(query('SELECT id FROM t WHERE id IN (SELECT id FROM sn) ORDER BY id;').rows, [[2]]);
equal(query('SELECT id FROM t WHERE id NOT IN (SELECT id FROM sn) ORDER BY id;').rows, []);
equal(query('SELECT id FROM t WHERE id IN (SELECT id FROM empty) ORDER BY id;').rows, []);
equal(query('EXPLAIN ANALYZE SELECT id FROM t WHERE id IN (SELECT id FROM s);').executionStats.actualRows, 2);
equal(query('SELECT (SELECT id FROM s ORDER BY id LIMIT 1) FROM t LIMIT 1;').rows, [[2]]);
equal(query('SELECT (SELECT id FROM empty) FROM t LIMIT 1;').rows, [[null]]);
equal(query('SELECT id FROM t WHERE id=(SELECT id FROM s WHERE id=2) ORDER BY id;').rows, [[2]]);
equal(query('SELECT (SELECT id FROM s WHERE id=2)+3 FROM t LIMIT 1;').rows, [[5]]);
rejected('SELECT (SELECT id FROM s) FROM t LIMIT 1;');
equal(query('SELECT id FROM t x WHERE EXISTS (SELECT id FROM s WHERE s.id=x.id) ORDER BY id;').rows, [[2],[3]]);
equal(query('SELECT id FROM t x WHERE id IN (SELECT id FROM s WHERE s.id=x.id) ORDER BY id;').rows, [[2],[3]]);
equal(query('SELECT id,(SELECT COUNT(*) FROM s WHERE s.id=x.id) AS c FROM t x ORDER BY id;').rows, [[1,0],[2,1],[3,1]]);
equal(query('SELECT id,(SELECT s.id FROM s WHERE s.id=x.id) AS c FROM t x ORDER BY id;').rows, [[1,null],[2,2],[3,3]]);
equal(run('UPDATE t SET id=id+10 WHERE id IN (SELECT id FROM s);').success, true);
equal(query('SELECT id FROM t ORDER BY id;').rows, [[1],[12],[13]]);
rejected('SELECT id FROM t WHERE id IN (SELECT id,id FROM s);');
rejected('SELECT id FROM t WHERE EXISTS (DELETE FROM s);');
equal(run('DELETE FROM t WHERE EXISTS (SELECT id FROM s WHERE s.id=2);').success, true);
equal(query('SELECT id FROM t;').rows, []);
console.log(`${checks} subquery checks passed`);

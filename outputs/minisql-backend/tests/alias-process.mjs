import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/alias-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const database = join(directory, 'database.pages');
let checks = 0;
function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [database, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 10000 });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function query(sql) { const response = run(sql); assert.equal(response.success, true, JSON.stringify(response)); return response.results.at(-1); }
equal(run("CREATE TABLE t(id INT,name VARCHAR); INSERT INTO t(id,name) VALUES(1,'a'); INSERT INTO t(id,name) VALUES(2,'b'); INSERT INTO t(id,name) VALUES(2,'b');").success, true);
equal(query('SELECT s.id FROM t AS s ORDER BY s.id;').rows, [[1], [2], [2]]);
equal(query('SELECT s.name FROM t s WHERE s.id=2 ORDER BY s.name;').rows, [['b'], ['b']]);
equal(query('SELECT t.id FROM t WHERE t.id=1;').rows, [[1]]);
equal(query('SELECT S.ID FROM T AS s WHERE s.id=1;').rows, [[1]]);
equal(query('SELECT s.* FROM t AS s WHERE s.id=1;').columns, ['id', 'name']);
equal(query('SELECT s.* FROM t AS s WHERE s.id=1;').rows, [[1, 'a']]);
equal(query('SELECT name AS n,s.*,id+1 AS next FROM t s WHERE id=1;').columns, ['n', 'id', 'name', 'next']);
equal(query('SELECT name AS n,s.*,id+1 AS next FROM t s WHERE id=1;').rows, [['a', 1, 'a', 2]]);
equal(query('SELECT *,* FROM t WHERE id=1;').rows, [[1, 'a', 1, 'a']]);
equal(query('SELECT DISTINCT s.id AS id FROM t s ORDER BY id DESC;').rows, [[2], [1]]);
equal(query('SELECT DISTINCT s.id FROM t s ORDER BY id;').rows, [[1], [2]]);
equal(query('SELECT s.id+1 AS x FROM t s ORDER BY x DESC LIMIT 1;').rows, [[3]]);
equal(query('SELECT s.name FROM t s ORDER BY s.id DESC LIMIT 1;').rows, [['b']]);
equal(query('SELECT t.* FROM t WHERE t.id=1;').rows, [[1, 'a']]);
for (const sql of ['SELECT t.id FROM t s;', 'SELECT q.id FROM t s;', 'SELECT q.* FROM t s;', 'SELECT s.missing FROM t s;',
  'SELECT s.id AS x FROM t s WHERE x=1;', 'SELECT s.id AS x,s.name AS x FROM t s ORDER BY x;',
  'SELECT s.id AS name FROM t s ORDER BY name;', 'SELECT s.id FROM t s; SELECT s.id FROM t;']) equal(run(sql).error.type, 'SemanticError');
for (const sql of ['SELECT s.* AS allcols FROM t s;', 'SELECT s. FROM t s;', 'SELECT s.id.extra FROM t s;', 'SELECT s.*+1 FROM t s;', 'SELECT id FROM t AS;', 'SELECT .5 FROM t;']) equal(run(sql).success, false);
const compiled = run('SELECT s.id+1 AS x,s.* FROM t AS s WHERE s.id=1;', 'compile');
equal(compiled.ast.tableAlias, 's');
equal(compiled.ast.selectItems[1].expression.kind, 'Wildcard');
equal(compiled.plan[0].table, 't');
equal(compiled.plan[0].output.map(column => column.name), ['x', 'id', 'name']);
equal(compiled.plan[0].projections[0].left.columnId, 0);
const bad = run('SELECT id FROM t s;\nSELECT t.id FROM t s;');
equal(bad.error.line, 2);
equal(bad.error.column, 8);
equal(query('SELECT * FROM t;').rows.length, 3);
console.log(`${checks} alias/qualified-column assertions passed across fresh database processes`);

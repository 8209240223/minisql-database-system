import { mkdtempSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import initSqlJs from '../../minisql-workbench/node_modules/sql.js/dist/sql-wasm.js';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/null-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const SQL = await initSqlJs({ locateFile: name => fileURLToPath(new URL(`../../minisql-workbench/node_modules/sql.js/dist/${name}`, import.meta.url)) });
const reference = new SQL.Database();
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  if (mode === 'catalog') {
    const child = spawnSync(executable, [join(directory, 'database.pages'), mode], { encoding: 'utf8', windowsHide: true, timeout: 5000 });
    assert.ifError(child.error);assert.equal(child.status, 0);return JSON.parse(child.stdout);
  }
  const result = invoke(executable, [join(directory, 'database.pages'), mode], sql);
  assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1).rows; }
function compare(sql) {
  const expected = reference.exec(sql)[0]?.values ?? [];
  const actual = query(sql).map(row => row.map(value => typeof value === 'boolean' ? Number(value) : value));
  equal(actual, expected);
}
try {
  const fixture = "CREATE TABLE t(id INT NOT NULL,n INT,name VARCHAR); CREATE TABLE u(id INT NOT NULL,label VARCHAR NOT NULL);" +
    "INSERT INTO t(id,n,name) VALUES(1,2,''); INSERT INTO t(id,n,name) VALUES(2,NULL,NULL); INSERT INTO t(id,n,name) VALUES(3,NULL,'NULL');" +
    "INSERT INTO u(id,label) VALUES(1,'match'); INSERT INTO u(id,label) VALUES(1,'again'); INSERT INTO u(id,label) VALUES(9,'other');";
  reference.run(fixture);equal(run(fixture).success, true);
  for (const a of ['TRUE', 'FALSE', 'NULL']) {
    compare(`SELECT NOT ${a} FROM t WHERE id=1;`);
    for (const b of ['TRUE', 'FALSE', 'NULL']) for (const op of ['AND', 'OR']) compare(`SELECT ${a} ${op} ${b} FROM t WHERE id=1;`);
  }
  for (const sql of [
    'SELECT n=NULL,n!=NULL,n<NULL,NOT (n=NULL) FROM t ORDER BY id;',
    'SELECT n+NULL,n*NULL,-NULL,NULL/2 FROM t ORDER BY id;',
    'SELECT id,n IS NULL,n IS NOT NULL FROM t ORDER BY id;',
    'SELECT id FROM t WHERE NULL;',
    'SELECT id FROM t WHERE n=NULL;',
    'SELECT id FROM t WHERE n IS NULL ORDER BY id;',
    'SELECT DISTINCT n FROM t ORDER BY n NULLS LAST;',
    'SELECT DISTINCT name FROM t ORDER BY name NULLS FIRST;',
    'SELECT id,n FROM t ORDER BY n ASC NULLS FIRST,id;',
    'SELECT id,n FROM t ORDER BY n DESC NULLS LAST,id;',
    'SELECT a.id,b.id,b.label FROM t a LEFT JOIN u b ON a.id=b.id ORDER BY a.id,b.label NULLS LAST;',
    'SELECT a.*,b.* FROM t a LEFT OUTER JOIN u b ON a.id=b.id ORDER BY a.id,b.label NULLS LAST;',
    'SELECT a.id FROM t a LEFT JOIN u b ON a.id=b.id WHERE b.id IS NULL ORDER BY a.id;',
    'SELECT a.id,b.id FROM t a LEFT JOIN u b ON NULL ORDER BY a.id;',
    'SELECT a.id,b.id FROM t a LEFT JOIN u b ON FALSE ORDER BY a.id;',
    'SELECT a.id,b.label FROM t a LEFT JOIN u b ON a.id=b.id AND b.label=\'match\' ORDER BY a.id;',
  ]) compare(sql);
  equal(query('SELECT id FROM t ORDER BY n,id;'), [[1], [2], [3]]);
  equal(query('SELECT id FROM t ORDER BY n DESC,id;'), [[2], [3], [1]]);
  equal(query('SELECT name FROM t ORDER BY id;'), [[''], [null], ['NULL']]);
  equal(run('INSERT INTO t(id,n,name) VALUES(NULL,1,NULL);').success, false);
  equal(run('UPDATE t SET id=NULL;').success, false);
  equal(run('UPDATE t SET id=n;').error.type, 'ExecutionError');
  equal(query('SELECT id FROM t ORDER BY id;'), [[1], [2], [3]]);
  equal(run("UPDATE t SET n=NULL,name=NULL WHERE id=1;").results[0].affectedRows, 1);
  equal(query('SELECT n,name FROM t WHERE id=1;'), [[null, null]]);
  equal(run("UPDATE t SET n=4,name='restored' WHERE id=1;").results[0].affectedRows, 1);
  equal(query('SELECT n,name FROM t WHERE id=1;'), [[4, 'restored']]);
  equal(run('DELETE FROM t WHERE n=NULL;').results[0].affectedRows, 0);
  equal(run('DELETE FROM t WHERE n IS NULL;').results[0].affectedRows, 2);
  const catalog = run('', 'catalog');
  equal(catalog.tables[0].columns.map(column => column.nullable), [false, true, true]);
  const compiled = run('SELECT b.id FROM t a LEFT JOIN u b ON a.id=b.id;', 'compile');
  equal(compiled.plan[0].output[0].nullable, true);
  equal(compiled.ast.joins[0].kind, 'LeftJoin');
  equal(run('SELECT NULL AND 1 FROM t;').error.type, 'SemanticError');
  equal(run('SELECT TRUE+NULL FROM t;').error.type, 'SemanticError');
  equal(query('SELECT FALSE AND 1/0=1,TRUE OR 1/0=1 FROM t;'), [[false, true]]);
  equal(run('SELECT NULL AND 1/0=1 FROM t;').error.type, 'ExecutionError');
  console.log(`${checks} NULL/LEFT JOIN checks passed, including all three-valued AND/OR/NOT combinations and cross-process writes`);
} finally { reference.close(); }

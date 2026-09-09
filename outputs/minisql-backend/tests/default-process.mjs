import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/default-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);
  assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function catalog() { const result = spawnSync(executable, [file, 'catalog'], { encoding: 'utf8' });assert.equal(result.status, 0);return JSON.parse(result.stdout).tables; }
function rows() { const result = run('SELECT * FROM t ORDER BY id;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run("CREATE TABLE t(id INT NOT NULL,n BIGINT DEFAULT 9223372036854775807 NOT NULL,s VARCHAR NULL DEFAULT 'Tom''s',optional INT DEFAULT NULL);").success, true);
equal(catalog()[0].columns.map(c => c.defaultValue), [null, '9223372036854775807', "'Tom''s'", 'NULL']);
equal(run('INSERT INTO t(id) VALUES(1);').success, true);
equal(run('INSERT INTO t VALUES(2,DEFAULT,DEFAULT,DEFAULT);').success, true);
equal(run('INSERT INTO t(id,s) VALUES(3,NULL);').success, true);
equal(rows(), [[1, '9223372036854775807', "Tom's", null], [2, '9223372036854775807', "Tom's", null], [3, '9223372036854775807', null, null]]);
for (const sql of ['INSERT INTO t VALUES(DEFAULT,DEFAULT,DEFAULT,DEFAULT);', 'INSERT INTO t(id,n) VALUES(4,NULL);', 'INSERT INTO t(id) VALUES(DEFAULT+1);']) {
  const before = rows();equal(run(sql).success, false);equal(rows(), before);
}
for (const definition of ["a INT DEFAULT '1'", 'a INT DEFAULT 2147483648', 'a BIGINT DEFAULT 9223372036854775808', 'a INT NOT NULL DEFAULT NULL', 'a INT DEFAULT NULL NOT NULL', 'a INT DEFAULT 1 DEFAULT 2', 'a INT NULL NOT NULL', 'a INT DEFAULT 1+2', 'a INT DEFAULT TRUE']) {
  equal(run(`CREATE TABLE invalid(${definition});`).success, false);
  equal(catalog().some(t => t.name === 'invalid'), false);
}
const compiled = run("CREATE TABLE transient(a INT DEFAULT -1); INSERT INTO transient VALUES(DEFAULT);", 'compile');
equal(compiled.success, true);
equal(compiled.ast[0].columns[0].defaultValue, '-1');
equal(compiled.plan[0].output[0].defaultValue, '-1');
equal(compiled.plan[1].insertExpressions[0].value, -1);
equal(catalog().some(t => t.name === 'transient'), false);
equal(run("CREATE TABLE edges(a INT DEFAULT -2147483648,b BIGINT DEFAULT -9223372036854775808,c VARCHAR DEFAULT ''); INSERT INTO edges VALUES(DEFAULT,DEFAULT,DEFAULT);").success, true);
equal(run('SELECT * FROM edges;').results[0].rows, [[-2147483648, '-9223372036854775808', '']]);
equal(run('CREATE TABLE no_default(a INT,b INT); INSERT INTO no_default VALUES(DEFAULT,DEFAULT);').success, true);
equal(run('SELECT * FROM no_default;').results[0].rows, [[null, null]]);
equal(run('INSERT INTO edges DEFAULT VALUES;').success, true);
equal(run('SELECT * FROM edges;').results[0].rows.length, 2);
equal(run('INSERT INTO no_default DEFAULT VALUES;').success, true);
equal(run('SELECT * FROM no_default;').results[0].rows, [[null, null], [null, null]]);
const beforeRequired = rows();
equal(run('INSERT INTO t DEFAULT VALUES;').success, false);
equal(rows(), beforeRequired);
equal(run("UPDATE t SET n=1,s='changed',optional=5;").success, true);
equal(run('UPDATE t SET n=DEFAULT,s=DEFAULT,optional=DEFAULT WHERE id=1;').results[0].affectedRows, 1);
equal(rows()[0], [1, '9223372036854775807', "Tom's", null]);
equal(rows()[1], [2, 1, 'changed', 5]);
const beforeUpdate = rows();
equal(run('UPDATE t SET n=DEFAULT,id=DEFAULT;').success, false);
equal(rows(), beforeUpdate);
equal(run('UPDATE no_default SET a=DEFAULT,b=DEFAULT;').results[0].affectedRows, 2);
equal(run('UPDATE t SET n=DEFAULT,optional=CAST(n AS INT) WHERE id=2;').success, true);
equal(rows()[1], [2, '9223372036854775807', 'changed', 1]);
for (const sql of ['INSERT INTO edges(a) DEFAULT VALUES;', 'INSERT INTO edges DEFAULT VALUES(1);', 'UPDATE t SET n=DEFAULT+1;', 'UPDATE t SET n=CAST(DEFAULT AS BIGINT);']) equal(run(sql).success, false);
const defaultCompiled = run('INSERT INTO edges DEFAULT VALUES;', 'compile');
equal(defaultCompiled.ast.defaultValues, true);
equal(defaultCompiled.plan[0].values, [-2147483648, '-9223372036854775808', '']);
const updateCompiled = run('UPDATE t SET n=DEFAULT;', 'compile');
equal(updateCompiled.ast.assignments[0].expression.kind, 'Default');
equal(updateCompiled.plan[0].projections[0].value, '9223372036854775807');
console.log(`${checks} DEFAULT checks passed: metadata persistence, omission, explicit DEFAULT/NULL and rejected definitions`);

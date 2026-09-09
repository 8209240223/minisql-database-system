import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/unique-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function rows() { const result = run('SELECT * FROM t ORDER BY id;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run('CREATE TABLE t(id INT PRIMARY KEY,n BIGINT UNIQUE,s VARCHAR UNIQUE);').success, true);
equal(run("INSERT INTO t VALUES(1,9007199254740992,'a'); INSERT INTO t VALUES(2,9007199254740993,'b');").success, true);
for (const sql of ["INSERT INTO t VALUES(1,3,'c');", "INSERT INTO t VALUES(3,9007199254740993,'c');", "INSERT INTO t VALUES(3,3,'b');", "INSERT INTO t VALUES(NULL,3,'c');", "INSERT INTO t(n,s) VALUES(3,'c');", 'UPDATE t SET id=1;', 'UPDATE t SET n=0;', "UPDATE t SET s='a' WHERE id=2;", 'UPDATE t SET id=NULL;']) {
  const before = rows();equal(run(sql).success, false);equal(rows(), before);
}
equal(run('UPDATE t SET id=3-id;').success, true);
equal(rows(), [[1, '9007199254740993', 'b'], [2, '9007199254740992', 'a']]);
equal(run('UPDATE t SET n=NULL,s=NULL;').success, true);
equal(run('INSERT INTO t(id) VALUES(3);').success, true);
equal(rows(), [[1, null, null], [2, null, null], [3, null, null]]);
equal(run("UPDATE t SET s='same';").success, false);
equal(rows(), [[1, null, null], [2, null, null], [3, null, null]]);
equal(run('DELETE FROM t WHERE id=3; INSERT INTO t(id) VALUES(3);').success, true);
const catalogResult = spawnSync(executable, [file, 'catalog'], { encoding: 'utf8' });
equal(catalogResult.status, 0);
const columns = JSON.parse(catalogResult.stdout).tables[0].columns;
equal(columns.map(c => [c.primaryKey, c.unique, c.nullable]), [[true, false, false], [false, true, true], [false, true, true]]);
for (const definition of ['a INT PRIMARY KEY,b INT PRIMARY KEY', 'a INT PRIMARY KEY NULL', 'a INT NULL PRIMARY KEY', 'a INT UNIQUE UNIQUE', 'a INT PRIMARY KEY DEFAULT NULL']) equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run('CREATE TABLE d(a INT UNIQUE DEFAULT 7); INSERT INTO d DEFAULT VALUES;').success, true);
equal(run('INSERT INTO d DEFAULT VALUES;').success, false);
equal(run('SELECT a FROM d;').results[0].rows, [[7]]);
const compiled = run('CREATE TABLE transient(a INT PRIMARY KEY,b VARCHAR UNIQUE);', 'compile');
equal(compiled.ast.columns[0].primaryKey, true);
equal(compiled.plan[0].output[1].unique, true);
equal(compiled.plan[0].output[0].nullable, false);
console.log(`${checks} single-column PRIMARY KEY/UNIQUE checks passed, including final-state UPDATE and reopen`);

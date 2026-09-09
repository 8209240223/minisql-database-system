import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/check-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') { const result = invoke(executable, [file, mode], sql);assert.ok(!result.category, JSON.stringify(result));return result.data; }
function rows() { const result = run('SELECT * FROM t ORDER BY id;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run('CREATE TABLE t(id INT PRIMARY KEY,score INT,CHECK(score>=0),CHECK(id>0));').success, true);
equal(run('INSERT INTO t VALUES(1,0); INSERT INTO t VALUES(2,10);').success, true);
equal(rows(), [[1,0],[2,10]]);
for (const sql of ['INSERT INTO t VALUES(3,-1);', 'INSERT INTO t VALUES(-1,1);', 'UPDATE t SET score=-1;', 'UPDATE t SET id=0 WHERE id=1;']) {
  const before = rows();equal(run(sql).success, false);equal(rows(), before);
}
equal(run('INSERT INTO t(id) VALUES(3);').success, true);
equal(rows(), [[1,0],[2,10],[3,null]]);
equal(run('UPDATE t SET score=NULL WHERE id=1;').success, true);
equal(rows()[0], [1,null]);
equal(run('UPDATE t SET score=DEFAULT WHERE id=1;').success, true);
equal(rows()[0], [1,null]);
equal(run('CREATE TABLE defaults(id INT,score INT DEFAULT 0,CHECK(score>=0)); INSERT INTO defaults(id) VALUES(1);').success, true);
equal(run('SELECT * FROM defaults;').results[0].rows, [[1,0]]);
equal(run('UPDATE defaults SET score=-1;').success, false);
equal(run('SELECT * FROM defaults;').results[0].rows, [[1,0]]);
equal(run("CREATE TABLE unknown(id INT,CHECK(id=1)); INSERT INTO unknown(id) VALUES(NULL); INSERT INTO unknown(id) VALUES(2);").success, false);
equal(run('SELECT * FROM unknown;').results[0].rows, [[null]]);
const compiled = run('CREATE TABLE transient(a INT,CHECK(a>0));', 'compile');
equal(compiled.ast.checks.length, 1);
equal(compiled.plan[0].checks[0].kind, 'Binary');
equal(compiled.plan[0].checks[0].left.columnId, 0);
equal(run('CREATE TABLE invalid(a INT,CHECK(a+1));').success, false);
equal(run('CREATE TABLE nullable_check(a INT,CHECK(NULL)); INSERT INTO nullable_check VALUES(1);').success, true);
equal(run('CREATE TABLE invalid3(a INT,CHECK(a>));').success, false);
equal(run('CREATE TABLE reset(id INT,CHECK(id>0)); INSERT INTO reset VALUES(1);').success, true);
equal(run('INSERT INTO reset VALUES(2);').success, true);
equal(run('SELECT * FROM reset;').results[0].rows, [[1],[2]]);
for (const [name, condition, valid, invalid] of [
  ['strings', "value='O''Brien'", "'O''Brien'", "'other'"],
  ['casts', 'CAST(value AS INT)>0', "'12'", "'-1'"],
  ['nested', "NOT (value='bad') AND (value='good' OR value='fine')", "'good'", "'bad'"],
]) {
  equal(run(`CREATE TABLE ${name}(value VARCHAR,CHECK(${condition}));`).success, true);
  equal(run(`INSERT INTO ${name} VALUES(${valid});`).success, true);
  equal(run(`INSERT INTO ${name} VALUES(${invalid});`).success, false);
  equal(run(`UPDATE ${name} SET value=${invalid};`).success, false);
  equal(run(`SELECT * FROM ${name};`).results[0].rows.length, 1);
}
equal(run('CREATE TABLE constant_true(a INT,CHECK(TRUE)); INSERT INTO constant_true VALUES(1);').success, true);
equal(run('CREATE TABLE constant_false(a INT,CHECK(FALSE));').success, true);
equal(run('INSERT INTO constant_false VALUES(1);').success, false);
equal(run('SELECT * FROM constant_false;').results[0].rows, []);
equal(run('CREATE TABLE inline_checks(id INT PRIMARY KEY CHECK(id>0), score INT CHECK(score>=0) DEFAULT 10 CHECK(score<=100) NOT NULL);').success, true);
equal(run('INSERT INTO inline_checks(id) VALUES(1);').success, true);
for (const sql of ['INSERT INTO inline_checks VALUES(0,10);', 'INSERT INTO inline_checks VALUES(2,101);', 'UPDATE inline_checks SET score=-1;', 'UPDATE inline_checks SET score=NULL;']) {
  equal(run(sql).success, false);
  equal(run('SELECT * FROM inline_checks;').results[0].rows, [[1,10]]);
}
equal(run('CREATE TABLE inline_cross(a INT CHECK(a<=b),b INT); INSERT INTO inline_cross VALUES(1,2);').success, true);
equal(run('INSERT INTO inline_cross VALUES(3,2);').success, false);
equal(run('CREATE TABLE bad_inline(a INT CHECK(missing>0));').success, false);
equal(run('CREATE TABLE bad_inline_type(a INT CHECK(a+1));').success, false);
const inlinePlan = run("CREATE TABLE inline_transient(a VARCHAR CHECK(CAST(a AS INT)>0));", 'compile');
equal(inlinePlan.success, true);
equal(inlinePlan.plan[0].checkDefinitions, inlinePlan.ast.checks);
equal(inlinePlan.plan[0].checkDefinitions[0].left.value, 'INT');
console.log(`${checks} CHECK checks passed: table/column constraints, constants, strings, CAST, writes and persistence`);

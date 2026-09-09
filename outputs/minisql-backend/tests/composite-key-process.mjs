import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/composite-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function rows() { const result = run('SELECT * FROM t ORDER BY a,b;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run('CREATE TABLE t(a INT,b INT,s VARCHAR,PRIMARY KEY(a,b),UNIQUE(b,s));').success, true);
for (const values of ["1,1,'a'", "1,2,'a'", "2,1,'b'"]) equal(run(`INSERT INTO t VALUES(${values});`).success, true);
equal(rows(), [[1,1,'a'],[1,2,'a'],[2,1,'b']]);
for (const sql of ["INSERT INTO t VALUES(1,1,'c');", "INSERT INTO t VALUES(3,1,'a');", "INSERT INTO t VALUES(NULL,4,'x');", "INSERT INTO t(a,s) VALUES(4,'x');", 'UPDATE t SET a=1;', "UPDATE t SET s='a';"]) {
  const before = rows();equal(run(sql).success, false);equal(rows(), before);
}
equal(run('UPDATE t SET a=3-a;').success, true);
equal(rows(), [[1,1,'b'],[2,1,'a'],[2,2,'a']]);
equal(run('INSERT INTO t VALUES(3,1,NULL); INSERT INTO t VALUES(4,1,NULL);').success, true);
equal(rows().length, 5);
const catalogResult = spawnSync(executable, [file, 'catalog'], { encoding: 'utf8' });
equal(catalogResult.status, 0);
const table = JSON.parse(catalogResult.stdout).tables[0];
equal(table.keys, [{primary:true,columns:['a','b']},{primary:false,columns:['b','s']}]);
equal(table.columns.map(c => c.nullable), [false,false,true]);
for (const definition of ['a INT,PRIMARY KEY(a,missing)', 'a INT,UNIQUE(a,A)', 'a INT PRIMARY KEY,b INT,PRIMARY KEY(b)', 'a INT,b INT,PRIMARY KEY(a),PRIMARY KEY(b)', 'a INT NULL,b INT,PRIMARY KEY(a,b)', 'a INT DEFAULT NULL,b INT,PRIMARY KEY(a,b)', 'a INT,UNIQUE()', 'PRIMARY KEY(a)']) equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run("CREATE TABLE strings(a VARCHAR,b VARCHAR,UNIQUE(a,b)); INSERT INTO strings VALUES('ab','c'); INSERT INTO strings VALUES('a','bc');").success, true);
equal(run("INSERT INTO strings VALUES('ab','c');").success, false);
equal(run("INSERT INTO strings VALUES(NULL,'c'); INSERT INTO strings VALUES(NULL,'c');").success, true);
const compiled = run('CREATE TABLE transient(UNIQUE(b,a),a INT,b INT);', 'compile');
equal(compiled.ast.keys, [{primary:false,columns:['b','a']}]);
equal(compiled.plan[0].keys, [{primary:false,columns:['b','a']}]);
console.log(`${checks} composite-key checks passed: tuple uniqueness, NULL, UPDATE, persistence and ordered metadata`);

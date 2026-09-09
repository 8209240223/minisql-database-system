import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const root = mkdtempSync(fileURLToPath(new URL('./artifacts/update-', import.meta.url)));
const file = join(root, 'database.pages');
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let assertions = 0;
function run(sql, mode = 'execute') {
  const process = spawnSync(executable, [file, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 10000, maxBuffer: 8 * 1024 * 1024 });
  if (process.error) throw process.error;
  assert.ok(process.status === 0 || process.status === 1, process.stderr);
  return JSON.parse(process.stdout);
}
function equal(actual, expected) { assert.deepEqual(actual, expected); ++assertions; }
function query(sql) { const result = run(sql); assert.equal(result.success, true, JSON.stringify(result)); return result.results.at(-1).rows; }
equal(run("CREATE TABLE t(id INT,a INT,b INT,name VARCHAR); INSERT INTO t(id,a,b,name) VALUES(1,1,2,'甲'); INSERT INTO t(id,a,b,name) VALUES(2,3,0,'乙');").success, true);
equal(run('UPDATE t SET a=b,b=a WHERE id=1;').results[0].affectedRows, 1);
equal(query('SELECT a,b FROM t WHERE id=1;'), [[2, 1]]);
equal(run('UPDATE t SET a=a+10;').results[0].affectedRows, 2);
equal(query('SELECT a FROM t ORDER BY id;'), [[12], [13]]);
equal(run('UPDATE t SET a=1/0 WHERE id=99;').results[0].affectedRows, 0);
equal(run('UPDATE t SET a=100/b;').success, false);
equal(query('SELECT a FROM t ORDER BY id;'), [[12], [13]]);
equal(run('UPDATE t SET a=2147483647+b;').success, false);
equal(query('SELECT a FROM t ORDER BY id;'), [[12], [13]]);
for (const sql of ['UPDATE t SET a=1,A=2;', "UPDATE t SET a='bad';", 'UPDATE t SET missing=1;', 'UPDATE absent SET a=1;', 'UPDATE t SET a=missing;', 'UPDATE t SET a=1 WHERE a;', 'UPDATE t SET;', 'UPDATE t SET a=;', 'UPDATE t SET a==1;']) equal(run(sql).success, false);
const long = '中文'.repeat(500);
equal(run(`UPDATE t SET name='${long}';`).results[0].affectedRows, 2);
equal(query('SELECT name FROM t ORDER BY id;'), [[long], [long]]);
equal(run("UPDATE t SET name='短' WHERE id=1;").results[0].affectedRows, 1);
equal(query('SELECT name FROM t ORDER BY id;'), [['短'], [long]]);
equal(run(`UPDATE t SET name='${'x'.repeat(4100)}';`).success, false);
equal(query('SELECT name FROM t ORDER BY id;'), [['短'], [long]]);
const compiled = run('UPDATE t SET a=b,b=a WHERE id=1;', 'compile');
equal(compiled.ast.assignments.map(item => item.column), ['a', 'b']);
equal(compiled.plan[0].kind, 'Update');
equal(compiled.plan[0].columnMapping, [1, 2]);
equal(query('SELECT a,b FROM t WHERE id=1;'), [[12, 1]]);
equal(run('UPDATE t SET a=a WHERE id=1;').results[0].affectedRows, 1);
console.log(`${assertions} UPDATE assertions passed; every request reopens database in a separate process`);

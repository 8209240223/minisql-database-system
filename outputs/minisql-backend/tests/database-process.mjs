import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/process-', import.meta.url)));
const path = join(directory, 'database.pages');
function run(mode, sql = '') {
  const process = spawnSync(executable, [path, mode], { input: sql, encoding: 'utf8', timeout: 5000 });
  assert.ifError(process.error);
  assert.equal(process.status, 0, process.stdout + process.stderr);
  return JSON.parse(process.stdout);
}
run('execute', "CREATE TABLE t(id INT,name VARCHAR); INSERT INTO t(name,id) VALUES('process restart',1);");
assert.deepEqual(run('execute', 'SELECT name FROM t;').results[0].rows, [['process restart']]);
run('execute', "INSERT INTO t(id,name) VALUES(1,'process restart');");
assert.deepEqual(run('execute', 'SELECT DISTINCT name FROM t;').results[0].rows, [['process restart']]);
run('execute', "INSERT INTO t(id,name) VALUES(2,'sorted');");
assert.deepEqual(run('execute', 'SELECT name FROM t ORDER BY id DESC LIMIT 1;').results[0].rows, [['sorted']]);
assert.equal(run('catalog').tables[0].name, 't');
run('compile', 'CREATE TABLE transient(id INT);');
assert.equal(run('catalog').tables.length, 1);
assert.equal(run('execute', 'DELETE FROM t;').results[0].affectedRows, 3);
assert.deepEqual(run('execute', 'SELECT * FROM t;').results[0].rows, []);
console.log('7 separate-process persistence assertions passed');

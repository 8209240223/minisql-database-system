import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/composite-fk-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
function rows(table) {
  const result = run(`SELECT * FROM ${table} ORDER BY id;`);
  assert.equal(result.success, true, JSON.stringify(result));
  return result.results[0].rows;
}
equal(run('CREATE TABLE parent(id INT,a INT,b VARCHAR,PRIMARY KEY(a,b)); INSERT INTO parent VALUES(1,10,\'x\'); INSERT INTO parent VALUES(2,20,\'y\');').success, true);
equal(run('CREATE TABLE child(id INT PRIMARY KEY,x INT,y VARCHAR,FOREIGN KEY(x,y) REFERENCES parent(a,b));').success, true);
equal(run("INSERT INTO child VALUES(1,10,'x'); INSERT INTO child VALUES(2,NULL,'missing'); INSERT INTO child VALUES(3,99,NULL);").success, true);
for (const sql of ["INSERT INTO child VALUES(4,10,'y');", "UPDATE child SET x=20 WHERE id=1;", 'DELETE FROM parent;', 'UPDATE parent SET a=a+1;']) {
  const beforeChild = rows('child');
  const beforeParent = rows('parent');
  const result = run(sql);
  equal(result.success, false);
  equal(result.error.code, 5001);
  equal(rows('child'), beforeChild);
  equal(rows('parent'), beforeParent);
}
equal(run("UPDATE parent SET id=11 WHERE a=10; UPDATE parent SET a=a,b=b WHERE a=10;").success, true);
equal(run("UPDATE child SET x=20,y='y' WHERE id=1; DELETE FROM parent WHERE a=10;").success, true);
equal(run('DELETE FROM parent WHERE a=20;').success, false);
equal(run('UPDATE child SET y=NULL WHERE id=1; DELETE FROM parent;').success, true);
equal(rows('parent'), []);
equal(run('CREATE TABLE big_parent(id INT,a BIGINT,b BIGINT,UNIQUE(a,b)); INSERT INTO big_parent VALUES(1,9223372036854775807,-9223372036854775808);').success, true);
equal(run('CREATE TABLE big_child(id INT,x BIGINT DEFAULT 9223372036854775807,y BIGINT DEFAULT -9223372036854775808,FOREIGN KEY(x,y) REFERENCES big_parent(a,b)); INSERT INTO big_child(id) VALUES(1);').success, true);
equal(run('DELETE FROM big_parent;').success, false);
equal(run('UPDATE big_child SET x=0;').success, false);
equal(rows('big_child'), [[1,'9223372036854775807','-9223372036854775808']]);
const compiled = run('CREATE TABLE transient(x BIGINT,y BIGINT,FOREIGN KEY(y,x) REFERENCES big_parent(a,b));', 'compile');
equal(compiled.success, true);
equal(compiled.ast.foreignKeys, [{columns:['y','x'],table:'big_parent',referencedColumns:['a','b']}]);
equal(compiled.plan[0].foreignKeys, compiled.ast.foreignKeys);
equal(run('SELECT * FROM transient;').success, false);
for (const definition of [
  'x INT,y VARCHAR,FOREIGN KEY(x) REFERENCES parent(a,b)',
  'x INT,y VARCHAR,FOREIGN KEY(x,y) REFERENCES absent(a,b)',
  'x INT,y VARCHAR,FOREIGN KEY(x,y) REFERENCES parent(a,missing)',
  'x INT,y VARCHAR,FOREIGN KEY(x,missing) REFERENCES parent(a,b)',
  'x INT,y VARCHAR,FOREIGN KEY(x,x) REFERENCES parent(a,b)',
  'x INT,y INT,FOREIGN KEY(x,y) REFERENCES parent(a,a)',
  'x INT,y INT,FOREIGN KEY(x,y) REFERENCES parent(a,b)',
  'x INT,y VARCHAR,FOREIGN KEY(x,y) REFERENCES parent(id,b)',
  'x INT,y VARCHAR,FOREIGN KEY(x,y) REFERENCES parent()',
]) equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run('CREATE TABLE single_parent(id INT PRIMARY KEY); INSERT INTO single_parent VALUES(1); CREATE TABLE single_child(id INT,FOREIGN KEY(id) REFERENCES single_parent(id)); INSERT INTO single_child VALUES(1);').success, true);
equal(run('DELETE FROM single_parent;').success, false);
equal(run('CREATE TABLE mixed(id INT REFERENCES single_parent(id),x BIGINT,y BIGINT,FOREIGN KEY(x,y) REFERENCES big_parent(a,b));').success, true);
equal(run('INSERT INTO mixed VALUES(99,9223372036854775807,-9223372036854775808);').success, false);
equal(run('INSERT INTO mixed VALUES(1,0,-9223372036854775808);').success, false);
equal(run('INSERT INTO mixed VALUES(1,9223372036854775807,-9223372036854775808);').success, true);
const catalog = spawnSync(executable, [file, 'catalog'], {encoding:'utf8'});
equal(catalog.status, 0);
equal(JSON.parse(catalog.stdout).tables.find(t => t.name === 'child').foreignKeys,
  [{columns:['x','y'],table:'parent',referencedColumns:['a','b']}]);
equal(JSON.parse(catalog.stdout).tables.find(t => t.name === 'mixed').foreignKeys.length, 2);
console.log(`${checks} composite foreign-key checks passed: ordered tuples, NULL, RESTRICT, BIGINT, defaults, compile and reopen`);

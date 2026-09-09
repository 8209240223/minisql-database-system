import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/foreign-key-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function rows(table) { const result = run(`SELECT * FROM ${table} ORDER BY id;`);assert.equal(result.success, true);return result.results[0].rows; }
equal(run("CREATE TABLE parent(id INT PRIMARY KEY,name VARCHAR); INSERT INTO parent VALUES(1,'one'); INSERT INTO parent VALUES(2,'two');").success, true);
equal(run('CREATE TABLE child(id INT PRIMARY KEY,pid INT REFERENCES parent(id));').success, true);
equal(run('INSERT INTO child VALUES(1,1); INSERT INTO child VALUES(2,NULL);').success, true);
for (const sql of ['INSERT INTO child VALUES(3,99);','UPDATE child SET pid=99;','DELETE FROM parent;','UPDATE parent SET id=id+10;']) {
  const beforeParent = rows('parent'), beforeChild = rows('child');
  equal(run(sql).success, false);equal(rows('parent'), beforeParent);equal(rows('child'), beforeChild);
}
equal(run("UPDATE parent SET name='changed' WHERE id=1;").success, true);
equal(run('UPDATE child SET pid=2 WHERE id=1;').success, true);
equal(run('DELETE FROM parent WHERE id=1;').success, true);
equal(run('DELETE FROM parent WHERE id=2;').success, false);
equal(run('UPDATE child SET pid=NULL; DELETE FROM parent WHERE id=2;').success, true);
equal(rows('parent'), []);
equal(rows('child'), [[1,null],[2,null]]);
for (const definition of ['pid INT REFERENCES missing(id)', 'pid VARCHAR REFERENCES parent(id)', 'pid INT REFERENCES parent(name)', 'pid INT REFERENCES parent(missing)', 'pid INT REFERENCES parent(id) REFERENCES parent(id)'])
  equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run('CREATE TABLE target(id BIGINT,UNIQUE(id)); INSERT INTO target VALUES(9223372036854775807); CREATE TABLE bigchild(id BIGINT REFERENCES target(id) DEFAULT 9223372036854775807); INSERT INTO bigchild DEFAULT VALUES;').success, true);
equal(run('SELECT * FROM bigchild;').results[0].rows, [['9223372036854775807']]);
equal(run('DELETE FROM target;').success, false);
equal(run('DELETE FROM bigchild; DELETE FROM target;').success, true);
equal(run('INSERT INTO bigchild DEFAULT VALUES;').success, false);
const catalog = spawnSync(executable, [file,'catalog'], {encoding:'utf8'});
equal(catalog.status, 0);
equal(JSON.parse(catalog.stdout).tables.find(t => t.name==='child').columns[1].references, {table:'parent',column:'id'});
const compiled = run('CREATE TABLE transient(pid INT REFERENCES parent(id));','compile');
equal(compiled.ast.columns[0].references,{table:'parent',column:'id'});
equal(compiled.plan[0].output[0].references,{table:'parent',column:'id'});
console.log(`${checks} foreign-key checks passed: child writes, parent RESTRICT, defaults, metadata and reopen`);

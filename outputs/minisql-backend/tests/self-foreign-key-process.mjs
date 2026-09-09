import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/self-fk-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
let lastResult;
function equal(actual, expected) { assert.deepEqual(actual, expected, JSON.stringify(lastResult)); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);
  assert.ok(!result.category, JSON.stringify(result));lastResult = result.data;return result.data;
}
function rows(table) {
  const result = run(`SELECT * FROM ${table} ORDER BY id;`);
  assert.equal(result.success, true, JSON.stringify(result));return result.results[0].rows;
}
function rejected(sql, table) {
  const before = rows(table);const result = run(sql);
  equal(result.success, false);equal(result.error.code, 5001);equal(rows(table), before);
}
equal(run('CREATE TABLE tree(id INT PRIMARY KEY,parent INT REFERENCES tree(id));').success, true);
equal(run('INSERT INTO tree VALUES(1,1); INSERT INTO tree VALUES(2,1); INSERT INTO tree VALUES(3,2);').success, true);
equal(rows('tree'), [[1,1],[2,1],[3,2]]);
rejected('INSERT INTO tree VALUES(4,99);', 'tree');
rejected('DELETE FROM tree WHERE id=1;', 'tree');
rejected('DELETE FROM tree;', 'tree');
rejected('UPDATE tree SET id=id+10,parent=parent+10;', 'tree');
equal(run('DELETE FROM tree WHERE id=3; DELETE FROM tree WHERE id=2;').success, true);
rejected('UPDATE tree SET id=10 WHERE id=1;', 'tree');
equal(run('UPDATE tree SET id=10,parent=10 WHERE id=1;').success, true);
equal(rows('tree'), [[10,10]]);
equal(run('DELETE FROM tree WHERE id=10;').success, true);
equal(rows('tree'), []);
equal(run('INSERT INTO tree VALUES(5,NULL); INSERT INTO tree VALUES(6,5); UPDATE tree SET parent=6 WHERE id=5;').success, true);
rejected('DELETE FROM tree WHERE id=6;', 'tree');
equal(run('UPDATE tree SET parent=NULL; DELETE FROM tree;').success, true);
equal(rows('tree'), []);
equal(run('CREATE TABLE tuples(id INT,a INT,b VARCHAR,pa INT,pb VARCHAR,PRIMARY KEY(a,b),FOREIGN KEY(pa,pb) REFERENCES TuPlEs(a,b));').success, true);
equal(run("INSERT INTO tuples VALUES(1,10,'x',10,'x'); INSERT INTO tuples VALUES(2,20,'y',10,'x');").success, true);
rejected("UPDATE tuples SET a=11 WHERE id=1;", 'tuples');
rejected("INSERT INTO tuples VALUES(3,30,'z',10,'y');", 'tuples');
equal(run('UPDATE tuples SET pa=NULL WHERE id=2; DELETE FROM tuples WHERE id=1;').success, true);
equal(run("UPDATE tuples SET a=21,b='z',pa=21,pb='z' WHERE id=2;").success, true);
equal(run('DELETE FROM tuples;').success, true);
equal(run('CREATE TABLE later(parent INT REFERENCES later(id),id INT PRIMARY KEY); INSERT INTO later VALUES(1,1);').success, true);
equal(run('CREATE TABLE big_self(id BIGINT PRIMARY KEY,parent BIGINT REFERENCES big_self(id) DEFAULT 9223372036854775807); INSERT INTO big_self(id) VALUES(9223372036854775807);').success, true);
equal(rows('big_self'), [['9223372036854775807','9223372036854775807']]);
const compiled = run('CREATE TABLE transient(id INT,parent INT,PRIMARY KEY(id),FOREIGN KEY(parent) REFERENCES transient(id));', 'compile');
equal(compiled.success, true);
equal(compiled.plan[0].foreignKeys, [{columns:['parent'],table:'transient',referencedColumns:['id']}]);
equal(run('SELECT * FROM transient;').success, false);
for (const definition of ['id INT,parent INT REFERENCES invalid(id)', 'id INT PRIMARY KEY,parent VARCHAR REFERENCES invalid(id)', 'id INT PRIMARY KEY,parent INT REFERENCES invalid(missing)'])
  equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run('CREATE TABLE external_child(id INT REFERENCES big_self(id));').success, false);
equal(run('CREATE TABLE external_child(id BIGINT REFERENCES big_self(id)); INSERT INTO external_child VALUES(9223372036854775807);').success, true);
rejected('DELETE FROM big_self;', 'big_self');
equal(run('CREATE TABLE wide(id INT PRIMARY KEY,parent INT REFERENCES wide(id),pad VARCHAR);').success, true);
equal(run(Array.from({length:70}, (_, i) => `INSERT INTO wide VALUES(${i+1},${i===69?1:i+1},'${'x'.repeat(100)}');`).join('')).success, true);
rejected('DELETE FROM wide WHERE id=1;', 'wide');
equal(run(`UPDATE wide SET id=80,parent=80,pad='${'y'.repeat(1000)}' WHERE id=2;`).success, true);
equal(run('DELETE FROM wide WHERE id!=1 AND id!=70;').success, true);
equal(rows('wide').map(row => row.slice(0,2)), [[1,1],[70,1]]);
equal(run('DELETE FROM wide WHERE id=70; DELETE FROM wide;').success, true);
console.log(`${checks} self-referencing foreign-key checks passed: candidate state, RESTRICT, cycles, tuples, NULL, BIGINT and reopen`);

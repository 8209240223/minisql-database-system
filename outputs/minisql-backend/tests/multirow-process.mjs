import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/multirow-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0, last;
function equal(a, b) { assert.deepEqual(a, b, JSON.stringify(last)); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);
  assert.ok(!result.category, JSON.stringify(result)); last = result.data; return last;
}
function rows(table = 't') {
  const result = run(`SELECT * FROM ${table} ORDER BY id;`);
  equal(result.success, true);return result.results[0].rows;
}
equal(run('CREATE TABLE t(id INT PRIMARY KEY,n INT DEFAULT 7 CHECK(n>0),s VARCHAR,b BIGINT);').success, true);
const inserted = run("INSERT INTO t(s,id,b) VALUES('a',1,9007199254740993),('b',2,9223372036854775807);");
equal(inserted.success, true);equal(inserted.results.length, 1);equal(inserted.results[0].affectedRows, 2);
equal(rows(), [[1,7,'a','9007199254740993'],[2,7,'b','9223372036854775807']]);
equal(run("INSERT INTO t VALUES(3,DEFAULT,NULL,NULL),(4,2+3,'x',CAST('4' AS BIGINT));").success, true);
const compiled = run("INSERT INTO t(id,n) VALUES(5,1+2),(6,3*4);", 'compile');
equal(compiled.success, true);equal(compiled.ast.valueRows.length, 2);
equal(compiled.plan[0].insertRows.length, 2);
equal(compiled.optimizedPlan[0].insertRows[1].expressions[1].value, 12);
equal(rows().length, 4);
for (const sql of [
  'INSERT INTO t(id,n) VALUES(5,1),(5,2);',
  'INSERT INTO t(id,n) VALUES(5,1),(1,2);',
  'INSERT INTO t(id,n) VALUES(5,1),(6,0);',
  'INSERT INTO t(id,n) VALUES(5,1),(6,1/0);',
  "INSERT INTO t(id,n) VALUES(5,1),(6,CAST('bad' AS INT));",
  'INSERT INTO t(id,n) VALUES(5,1),(CAST(NULL AS INT),2);',
  'INSERT INTO t(id,n) VALUES(5,1),(6);',
  "INSERT INTO t(id,n) VALUES(5,1),(6,'bad');",
  'INSERT INTO t(id,n) VALUES(5,1),(6,n);',
  'INSERT INTO t(id,n) VALUES(5,1),();',
  'INSERT INTO t(id,n) VALUES(5,1),;',
  `INSERT INTO t(id,s) VALUES(5,'ok'),(6,'${'x'.repeat(4100)}');`,
]) {
  const before = rows(), bytes = readFileSync(file);
  const result = run(sql);equal(result.success, false);equal(result.completedStatements, 0);
  equal(rows(), before);equal(readFileSync(file), bytes);
}
equal(run('CREATE TABLE tree(id INT PRIMARY KEY,parent INT REFERENCES tree(id));').success, true);
equal(run('INSERT INTO tree VALUES(1,2),(2,1),(3,4),(4,4);').success, true);
equal(rows('tree'), [[1,2],[2,1],[3,4],[4,4]]);
equal(run('INSERT INTO tree VALUES(5,6),(6,99);').success, false);equal(rows('tree').length, 4);
equal(run('CREATE TABLE child(id INT PRIMARY KEY,parent INT REFERENCES t(id));').success, true);
equal(run('INSERT INTO child VALUES(1,1),(2,999);').success, false);equal(rows('child'), []);
equal(run('INSERT INTO child VALUES(1,1),(2,2);').success, true);
equal(run('CREATE TABLE pairs(id INT,a INT,b INT,UNIQUE(a,b));').success, true);
equal(run('INSERT INTO pairs VALUES(1,1,2),(2,1,2);').success, false);equal(rows('pairs'), []);
equal(run('INSERT INTO pairs VALUES(1,NULL,2),(2,NULL,2);').success, true);
equal(run('CREATE TABLE wide(id INT PRIMARY KEY,pad VARCHAR);').success, true);
const wide = run(`INSERT INTO wide VALUES${Array.from({length:80}, (_,i)=>`(${i+1},'${'x'.repeat(1000)}')`).join(',')};`);
equal(wide.success, true);equal(wide.results[0].affectedRows, 80);equal(rows('wide').length, 80);
const partial = run('INSERT INTO t(id) VALUES(8),(9); INSERT INTO t(id) VALUES(10),(8);');
equal(partial.success, false);equal(partial.completedStatements, 1);
equal(rows().map(row=>row[0]), [1,2,3,4,8,9]);
console.log(`${checks} multirow checks passed: mapping, defaults, expressions, constraints, final-state references and atomic failure`);

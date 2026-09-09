import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';

const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/aggregate-gate-', import.meta.url)));
const file = join(directory, 'database.pages');
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(source, mode = 'execute') {
  const result = invoke(executable, [file, mode], source);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
equal(run('CREATE TABLE t(id INT PRIMARY KEY,v INT); INSERT INTO t VALUES(1,10),(2,NULL);').success, true);
const before = readFileSync(file);
for (const source of ['SELECT AVG(v) FROM t;', 'SELECT v,AVG(v) FROM t GROUP BY v ORDER BY v;', 'SELECT AVG(v) FROM t HAVING AVG(v)>0;']) {
  for (const mode of ['compile','execute']) {
    const result = run(source, mode);
    equal(result.success, true);
    if (mode === 'compile') equal(result.plan.some(node => node.kind === 'Aggregate'), true);
    else equal(result.results[0].rows, source.includes('GROUP BY') ? [[10,'10.000000'],[null,null]] : [['10.000000']]);
    equal(readFileSync(file), before);
  }
}
const aborted = run('BEGIN; INSERT INTO t VALUES(3,30); SELECT AVG(v) FROM t; ROLLBACK;');
equal(aborted.success, true);equal(aborted.results[2].rows, [['20.000000']]);
equal(readFileSync(file), before);
equal(run('SELECT * FROM t ORDER BY id;').results[0].rows, [[1,10],[2,null]]);
equal(run('CREATE TABLE names(count INT,sum INT,avg INT,min INT,max INT); INSERT INTO names VALUES(1,2,3,4,5);').success, true);
equal(run('SELECT count,sum,avg,min,max FROM names;').results[0].rows, [[1,2,3,4,5]]);
console.log(`${checks} AVG compile/execute isolation checks passed`);

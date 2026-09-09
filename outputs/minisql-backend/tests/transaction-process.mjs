import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/transaction-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0, last;
function equal(a,b) { assert.deepEqual(a,b,JSON.stringify(last));++checks; }
function run(sql,mode='execute') {
  const result = invoke(executable,[file,mode],sql);
  assert.ok(!result.category,JSON.stringify(result));last=result.data;return last;
}
function rows() { const r=run('SELECT * FROM t ORDER BY id;');equal(r.success,true);return r.results[0].rows; }
equal(run('CREATE TABLE t(id INT PRIMARY KEY,n INT CHECK(n>0));').success,true);
let result=run('BEGIN; INSERT INTO t VALUES(1,1),(2,2); UPDATE t SET n=n+1; COMMIT;');
equal(result.success,true);equal(result.transactionState,'IDLE');
equal(result.results.map(r=>r.commitState),['committed','committed','committed','committed']);
equal(rows(),[[1,2],[2,3]]);
const before=readFileSync(file);
result=run('BEGIN TRANSACTION; INSERT INTO t VALUES(3,3); DELETE FROM t WHERE id=1; ROLLBACK TRANSACTION;');
equal(result.success,true);equal(rows(),[[1,2],[2,3]]);equal(readFileSync(file),before);
equal(result.results.map(r=>r.commitState),['rolledBack','rolledBack','rolledBack','rolledBack']);
for (const sql of [
  'BEGIN; INSERT INTO t VALUES(3,3); INSERT INTO t VALUES(1,4); COMMIT;',
  'BEGIN; INSERT INTO t VALUES(3,3); UPDATE t SET n=0; COMMIT;',
  'BEGIN; INSERT INTO t VALUES(3,3); SELECT 1/0 FROM t; COMMIT;',
  'BEGIN; INSERT INTO t VALUES(3,3); SELECT missing FROM t; COMMIT;',
  'BEGIN; INSERT INTO t VALUES(3,3); @',
  'BEGIN; INSERT INTO t VALUES(3,3); SELECT * FROM t',
  'BEGIN; INSERT INTO t VALUES(3,3); BEGIN;',
  'BEGIN; INSERT INTO t VALUES(3,3);',
]) {
  const bytes=readFileSync(file);result=run(sql);
  equal(result.success,false);equal(result.transactionRolledBack,true);equal(result.transactionState,'IDLE');
  equal(rows(),[[1,2],[2,3]]);equal(readFileSync(file),bytes);
}
equal(run('COMMIT;').error.code,6001);equal(run('ROLLBACK;').error.code,6001);
result=run('BEGIN; CREATE TABLE candidate(id INT); INSERT INTO candidate VALUES(1); ROLLBACK;');
equal(result.success,true);equal(run('SELECT * FROM candidate;').success,false);
equal(run('BEGIN; CREATE TABLE candidate(id INT); INSERT INTO candidate VALUES(1); COMMIT;').success,true);
equal(run('SELECT * FROM candidate;').results[0].rows,[[1]]);
result=run('INSERT INTO t VALUES(8,8); BEGIN; INSERT INTO t VALUES(9,9); INSERT INTO t VALUES(1,1);');
equal(result.success,false);equal(result.results[0].commitState,'committed');equal(result.results[2].commitState,'rolledBack');
equal(rows(),[[1,2],[2,3],[8,8]]);
equal(run('BEGIN; COMMIT; BEGIN; ROLLBACK;').success,true);
const compilation=run('BEGIN; CREATE TABLE temporary(id INT); ROLLBACK; SELECT * FROM temporary;','compile');
equal(compilation.success,false);
equal(run('BEGIN; CREATE TABLE temporary(id INT); COMMIT; SELECT * FROM temporary;','compile').success,true);
equal(run('SELECT * FROM temporary;').success,false);
console.log(`${checks} transaction process checks passed: commit, rollback, failure, EOF, DDL and durability`);

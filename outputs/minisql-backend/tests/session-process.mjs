import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/session-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(a,b) { assert.deepEqual(a,b);++checks; }
function readRows() {
  const response = invoke(executable,[file,'execute'],'SELECT * FROM t ORDER BY id;');
  assert.ok(!response.category,JSON.stringify(response));equal(response.data.success,true);return response.data.results[0].rows;
}
let session = await openSession(executable,file);
try {
  equal((await session.request('execute','CREATE TABLE t(id INT PRIMARY KEY,n BIGINT);')).success,true);
  equal((await session.request('execute','BEGIN;')).transactionState,'ACTIVE');
  equal((await session.request('execute','INSERT INTO t VALUES(1,9007199254740993);')).results[0].commitState,'pending');
  equal((await session.request('execute','SELECT * FROM t;')).results[0].rows,[[1,'9007199254740993']]);
  equal((await session.request('compile','SELECT * FROM t;')).success,true);
  const compiledError=await session.request('compile','SELECT missing FROM t;');
  equal(compiledError.success,false);equal(compiledError.transactionState,'ACTIVE');
  equal((await session.request('execute','COMMIT;')).transactionState,'IDLE');
  await assert.rejects(openSession(executable,file),/lock unavailable/i);++checks;
  const responses=await Promise.all([2,3,4].map(id=>session.request('execute',`INSERT INTO t VALUES(${id},${id});`)));
  equal(responses.map(r=>r.success),[true,true,true]);equal(new Set(responses.map(r=>r.id)).size,3);
  equal((await session.request('execute','BEGIN; INSERT INTO t VALUES(5,5);')).success,true);
  const rejected=await session.request('execute','INSERT INTO t VALUES(1,1);');
  equal(rejected.success,false);equal(rejected.transactionState,'ABORTED');
  equal((await session.request('execute','COMMIT;')).error.code,6001);
  equal((await session.request('execute','ROLLBACK;')).transactionState,'IDLE');
  equal((await session.request('execute','BEGIN; CREATE TABLE pending(id INT); INSERT INTO t VALUES(6,6);')).success,true);
  equal((await session.request('catalog')).tables.length,2);
  equal((await session.close()).success,true);
} finally { await session.terminate(); }
equal(readRows(),[[1,'9007199254740993'],[2,2],[3,3],[4,4]]);
session=await openSession(executable,file);
try {
  equal((await session.request('catalog')).tables.length,1);
  equal((await session.request('execute','BEGIN; INSERT INTO t VALUES(9,9);')).success,true);
  await session.terminate();
  await assert.rejects(session.request('execute','COMMIT;'),/terminated/);++checks;
} finally { await session.terminate(); }
equal(readRows().length,4);

function raw(frames) {
  const child=spawnSync(executable,[file,'session'],{input:frames,encoding:'utf8',windowsHide:true,timeout:10000,maxBuffer:16*1024*1024});
  assert.ifError(child.error);
  return {code:child.status,messages:child.stdout.trim().split('\n').map(line=>JSON.parse(line))};
}
const request=(id,operation,sql)=>JSON.stringify({id,operation,...(sql===undefined?{}:{sql})})+'\n';
let response=raw('{bad}\n'+request('a','execute','BEGIN;')+request('b','execute','INSERT INTO t VALUES(10,10);'));
equal(response.code,0);equal(response.messages[0].type,'ready');equal(response.messages[1].success,false);
equal(response.messages[3].transactionState,'ACTIVE');equal(readRows().length,4);
const before=readFileSync(file);
response=raw(request('a','execute','BEGIN;')+request('b','execute','INSERT INTO t VALUES(11,11);')+request('c','execute','COMMIT;').trimEnd());
equal(response.code,1);equal(response.messages.at(-1).error.code,1001);equal(readFileSync(file),before);
response=raw('x'.repeat(8*1024*1024+1)+'\n'+request('ok','execute','SELECT * FROM t;'));
equal(response.messages[1].success,false);equal(response.messages[2].id,'ok');equal(response.messages[2].success,true);
for (const frame of [
  JSON.stringify({id:1,operation:'catalog'}),
  JSON.stringify({id:'x',operation:'execute',sql:42}),
  JSON.stringify({id:'x',operation:'unknown'}),
  JSON.stringify({id:'x',operation:'catalog',unexpected:true}),
  '['.repeat(30)+'0'+']'.repeat(30),
]) {
  response=raw(frame+'\n'+request('after','catalog'));
  equal(response.messages[1].success,false);equal(response.messages[2].success,true);
}
await assert.rejects(openSession(executable,file,{maxOutputBytes:1}),/output limit/);++checks;
equal(readRows().length,4);
console.log(`${checks} persistent session checks passed: state, IDs, queue, lock, close, EOF, kill and malformed frames`);

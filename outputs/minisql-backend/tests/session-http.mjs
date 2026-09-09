import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/session-http-',import.meta.url)));
const server=spawn(process.execPath,[fileURLToPath(new URL('../scripts/database-bridge.mjs',import.meta.url))],{
  env:{...process.env,PORT:'0',MINISQL_DB:join(directory,'database.pages'),MINISQL_SESSION_IDLE_MS:'1000'},
  stdio:['ignore','pipe','pipe'],windowsHide:true,
});
const exited=once(server,'exit');
const deadline=setTimeout(()=>server.kill(),60000);
let checks=0,errors='';server.stderr.on('data',chunk=>{errors+=chunk;});
function equal(a,b){assert.deepEqual(a,b);++checks;}
try {
  const url=await new Promise((resolve,reject)=>{
    let output='';server.stdout.on('data',chunk=>{output+=chunk;const match=output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);if(match)resolve(match[0]);});
    server.on('error',reject);server.on('exit',()=>reject(new Error(errors||'server exited')));
  });
  async function request(path,sql,method='POST') {
    const response=await fetch(url+path,{method,headers:{'Content-Type':'application/json'},
      ...(sql===undefined?{}:{body:JSON.stringify({sql})}),signal:AbortSignal.timeout(10000)});
    return {status:response.status,data:await response.json()};
  }
  const opened=await request('/sessions');equal(opened.status,201);
  const prefix='/sessions/'+opened.data.sessionId;
  const second=await request('/sessions');equal(second.status,201);
  equal((await request('/sessions/'+second.data.sessionId+'/close')).status,200);
  equal((await request('/execute','SELECT 1;')).status,409);
  equal((await request('/sessions/missing/catalog',undefined,'GET')).status,404);
  equal((await request(prefix+'/execute','CREATE TABLE t(id INT PRIMARY KEY);')).status,200);
  equal((await request(prefix+'/execute','BEGIN;')).data.transactionState,'ACTIVE');
  equal((await request(prefix+'/execute','INSERT INTO t VALUES(1),(2);')).data.results[0].commitState,'pending');
  equal((await request(prefix+'/execute','SELECT * FROM t ORDER BY id;')).data.rows,[[1],[2]]);
  equal((await request(prefix+'/compile','SELECT * FROM t;')).status,200);
  equal((await request(prefix+'/execute','COMMIT;')).data.transactionState,'IDLE');
  equal((await request(prefix+'/execute','BEGIN; INSERT INTO t VALUES(3);')).status,200);
  equal((await request(prefix+'/execute','INSERT INTO t VALUES(1);')).data.transactionState,'ABORTED');
  equal((await request(prefix+'/execute','COMMIT;')).data.error.code,6001);
  equal((await request(prefix+'/execute','ROLLBACK;')).data.transactionState,'IDLE');
  equal((await request(prefix+'/execute','SELECT * FROM t ORDER BY id;')).data.rows,[[1],[2]]);
  equal((await request(prefix+'/execute','BEGIN; CREATE TABLE pending(id INT); INSERT INTO t VALUES(4);')).status,200);
  equal((await request(prefix+'/catalog',undefined,'GET')).data.tables.length,2);
  equal((await request(prefix+'/close')).status,200);
  equal((await request(prefix+'/execute','COMMIT;')).status,404);
  equal((await request('/catalog',undefined,'GET')).data.tables.length,1);
  equal((await request('/execute','SELECT * FROM t ORDER BY id;')).data.rows,[[1],[2]]);
  const idle=await request('/sessions');equal(idle.status,201);
  const idlePrefix='/sessions/'+idle.data.sessionId;
  equal((await request(idlePrefix+'/execute','BEGIN; INSERT INTO t VALUES(5);')).status,200);
  let available=false;
  const until=Date.now()+10000;
  while(Date.now()<until){
    const probe=await request('/catalog',undefined,'GET');
    if(probe.status===200){available=true;break;}
    assert.equal(probe.status,409);await new Promise(resolve=>setTimeout(resolve,100));
  }
  equal(available,true);
  equal((await request(idlePrefix+'/execute','COMMIT;')).status,404);
  equal((await request('/execute','SELECT * FROM t ORDER BY id;')).data.rows,[[1],[2]]);
  equal((await request('/capabilities',undefined,'GET')).data.quarantined,false);
  console.log(`${checks} HTTP session checks passed: cross-request transactions, ownership, close and idle rollback`);
} finally {clearTimeout(deadline);server.kill();await exited;}

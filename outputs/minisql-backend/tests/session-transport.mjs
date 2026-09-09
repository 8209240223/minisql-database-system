import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';
const fixture=fileURLToPath(new URL('./fixtures/session-worker.mjs',import.meta.url));
let checks=0;
const session=await openSession(process.execPath,fixture,{timeoutMs:2000});
try {
  assert.equal((await session.request('split')).success,true);++checks;
  const values=await Promise.all(Array.from({length:10},()=>session.request('ok')));
  assert.equal(new Set(values.map(value=>value.id)).size,10);++checks;
  assert.equal((await session.close()).success,true);++checks;
} finally {await session.terminate();}
for(const [operation,pattern] of [['mismatch',/Unexpected/],['malformed',/JSON|Unexpected|property/i],['oversize',/output limit/],['hang',/timed out/],['exit',/closed/]]){
  const child=await openSession(process.execPath,fixture,{timeoutMs:2000,maxOutputBytes:1024});
  try {
    await assert.rejects(child.request(operation),pattern);++checks;
    await child.closed;
    await assert.rejects(child.request('ok'));++checks;
  }finally{await child.terminate();}
}
console.log(`${checks} session transport checks passed: fragmented replies, queue, ID mismatch, invalid output, timeout and exit`);

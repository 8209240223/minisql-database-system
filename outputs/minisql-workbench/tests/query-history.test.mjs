import assert from 'node:assert/strict';
import test from 'node:test';
import { parseHistory, limitHistory, filterHistory, MAX_HISTORY_SQL } from '../src/query-history.ts';

const sample = { id: '1', sql: "SELECT '中文;DELETE';", at: 1234, durationMs: 1.5, rows: 2, mode: 'api', connection: 'MiniSQL C++', action: 'execute' };
test('roundtrip preserves SQL and real connection identity', () => assert.deepEqual(parseHistory(JSON.stringify([sample])), [sample]));
test('only approved fields survive restore', () => assert.deepEqual(parseHistory(JSON.stringify([{...sample,sessionId:'secret',password:'secret'}])), [sample]));
test('compile/error restore', () => {
  const value = {...sample,action:'compile',error:'语法错误'};
  assert.deepEqual(parseHistory(JSON.stringify([value])), [value]);
});
for (const raw of [null, '', '{bad', '{}', '[null]', JSON.stringify([sample,sample]), ' '.repeat(300000)]) {
  test(`invalid container ${String(raw).slice(0,30)}`, () => assert.deepEqual(parseHistory(raw), []));
}
for (const patch of [{sql:0},{sql:'x'.repeat(MAX_HISTORY_SQL+1)},{at:-1},{at:8640000000000001},{durationMs:-1},{rows:1.2},{mode:'unknown'},{action:'other'},{connection:[]},{error:12},{error:'x'.repeat(1001)}]) {
  test(`invalid item ${Object.keys(patch)[0]} ${JSON.stringify(patch).slice(0,40)}`, () => assert.deepEqual(parseHistory(JSON.stringify([{...sample,...patch}])), []));
}
test('bounded newest-first retention', () => {
  const result = limitHistory(Array.from({length:80},(_,i)=>({...sample,id:String(i)})));
  assert.equal(result.length,50);assert.equal(result[0].id,'0');assert.equal(result.at(-1).id,'49');
});
test('text budget retains complete SQL, never truncates it', () => {
  const items = Array.from({length:8},(_,i)=>({...sample,id:String(i),sql:'x'.repeat(MAX_HISTORY_SQL)}));
  const result = limitHistory(items);
  assert.ok(result.length>0 && result.length<8);
  assert.ok(result.every(item=>item.sql.length===MAX_HISTORY_SQL));
  assert.deepEqual(parseHistory(JSON.stringify(result)),result);
});
test('search source, SQL and errors without modifying history', () => {
  const items=[sample,{...sample,id:'2',action:'compile',sql:'SELECT missing;',error:'missing column'}];
  for(const query of ['编译','MISSING']) assert.deepEqual(filterHistory(items,query),[items[1]]);
  assert.deepEqual(filterHistory(items,'真实'),items);
  assert.deepEqual(filterHistory(items,'  '),items);
  assert.deepEqual(filterHistory(items,'not-present'),[]);
});

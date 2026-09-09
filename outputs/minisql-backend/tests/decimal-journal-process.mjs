import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const executable=fileURLToPath(new URL('../bin/journal_probe.exe',import.meta.url));
const type=process.argv[2] ?? 'decimal';
assert.ok(['decimal','bool','date','varchar'].includes(type));
const root=relative(process.cwd(),mkdtempSync(fileURLToPath(new URL(`./artifacts/${type}-journal-`,import.meta.url))));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(mode,path,point=''){const result=spawnSync(executable,[type+'-'+mode,path,point],{encoding:'utf8',timeout:10000,windowsHide:true});assert.ok(!result.error,String(result.error));assert.notEqual(result.status,1,result.stderr);return result;}
for(const point of ['prepared','published','applied-page','data-synced','checkpointed']){
 const file=join(root,point+'.pages');equal(run('init',file).status,0);
 const before=readFileSync(file);equal(run('commit',file,point).status,77);
 if(point==='prepared'||point==='published')equal(readFileSync(file),before);
 for(let reopen=0;reopen<2;++reopen){
  const result=run('read',file);equal(result.status,0);
  const oldValue={bool:'false',decimal:'1.000000',date:'1970-01-01',varchar:'old'}[type];
  const newValue={bool:'true',decimal:'2.000000',date:'2024-02-29',varchar:'new'}[type];
  equal(result.stdout.trim(),point==='prepared'?`ROWS=1;FIRST=${oldValue}`:`ROWS=240;FIRST=${newValue}`);
 }
}
console.log(`${checks} ${type.toUpperCase()} journal crash recovery checks passed`);

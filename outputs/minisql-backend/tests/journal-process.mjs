import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, statSync, linkSync, unlinkSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const executable = fileURLToPath(new URL('../bin/journal_probe.exe', import.meta.url));
const directory = relative(process.cwd(), mkdtempSync(fileURLToPath(new URL('./artifacts/journal-', import.meta.url))));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(mode, path, point = '') { return spawnSync(executable, [mode,path,point], {encoding:'utf8', windowsHide:true, timeout:10000}); }
function init(name) {
  const path = join(directory, name + '.db');const result = run('init',path);
  equal(result.status,0);return path;
}
function expectRows(path,count,value) {
  const result = run('read',path);equal(result.status,0);
  assert.ok(result.stdout.includes(`ROWS=${count};FIRST=${value}`), result.stdout + result.stderr);++checks;
}
for (const point of ['prepared','published','applied-page','data-synced','checkpointed']) {
  const path = init(point), before = readFileSync(path);
  equal(run('commit',path,point).status,77);
  if (point === 'prepared' || point === 'published') equal(readFileSync(path),before);
  expectRows(path,point==='prepared'?1:8,point==='prepared'?'old':'new');
  expectRows(path,point==='prepared'?1:8,point==='prepared'?'old':'new');
  if (point !== 'prepared') equal(statSync(path+'.wal').size,0);
}
const interrupted = init('repeat-recovery');
equal(run('commit',interrupted,'published').status,77);
const beforeReplay = readFileSync(interrupted);
equal(run('read',interrupted,'recovery-page').status,77);
assert.notDeepEqual(readFileSync(interrupted),beforeReplay);++checks;
equal(run('read',interrupted,'recovery-page').status,77);
expectRows(interrupted,8,'new');
for (const point of ['prepared','published']) {
  const path = init('throw-'+point), result = run('throw',path,point);
  equal(result.status,0);
  assert.ok(result.stdout.includes(point==='prepared'?'ROLLBACK_OK':'REOPEN_REQUIRED'),result.stderr);++checks;
  expectRows(path,point==='prepared'?1:8,point==='prepared'?'old':'new');
}
for (const kind of ['header','record','truncated']) {
  const path = init('corrupt-'+kind);equal(run('commit',path,'published').status,77);
  const before = readFileSync(path);let journal = readFileSync(path+'.wal');
  if (kind === 'truncated') journal = journal.subarray(0,journal.length-1);
  else journal[kind==='header'?8:8196] ^= 1;
  writeFileSync(path+'.wal',journal);
  equal(run('read',path).status,1);equal(readFileSync(path),before);
}
const foreign = init('foreign'), donor = init('donor');
equal(run('commit',donor,'published').status,77);
const foreignBefore = readFileSync(foreign);
writeFileSync(foreign+'.wal',readFileSync(donor+'.wal'));
equal(run('read',foreign).status,1);equal(readFileSync(foreign),foreignBefore);
const empty = init('empty');equal(run('empty',empty).status,0);expectRows(empty,1,'old');
const legacy = init('legacy');
const legacyBytes = readFileSync(legacy);legacyBytes.writeUInt32LE(1,8);legacyBytes.fill(0,24,40);
let crc = 2166136261;
for (let i=0;i<4096;++i) if(i<4||i>=8) crc = Math.imul(crc ^ legacyBytes[i],16777619)>>>0;
legacyBytes.writeUInt32LE(crc,4);writeFileSync(legacy,legacyBytes);
expectRows(legacy,1,'old');equal(readFileSync(legacy).readUInt32LE(8),1);
equal(run('commit',legacy).status,0);expectRows(legacy,8,'new');equal(readFileSync(legacy).readUInt32LE(8),2);
const locked = init('locked');
const holder = spawn(executable,['hold',locked],{stdio:['pipe','pipe','pipe'],windowsHide:true});
const exited = new Promise(resolve=>holder.once('exit',resolve));
try {
  await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>reject(new Error('lock holder startup timeout')),10000);
    holder.stdout.once('data',data=>{clearTimeout(timer);if(data.toString().includes('LOCKED'))resolve();else reject(new Error(data.toString()));});
    holder.once('error',error=>{clearTimeout(timer);reject(error);});
  });
  const denied=run('read',locked);equal(denied.status,1);assert.match(denied.stderr,/lock unavailable/i);++checks;
} finally { holder.kill();await exited; }
expectRows(locked,1,'old');
const alias=join(directory,'hardlink.db');linkSync(locked,alias);
try { equal(run('read',alias).status,1); } finally { unlinkSync(alias); }
console.log(`${checks} journal checks passed: commit boundaries, redo, repeated recovery, corruption, identity, locks and legacy migration`);

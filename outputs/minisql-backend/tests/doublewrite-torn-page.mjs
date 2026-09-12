import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// X22 torn-page 防护：数据页在写主文件前先落入双写缓冲（.dwb），
// 主文件同步后才释放槽位；打开时用仍有效的槽位修复未完成的页写。
// 对照实验：在重做日志不可用（删除 .wal）时，双写缓冲是唯一能恢复已提交页的手段。
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-doublewrite-'));
const page = 4096;
let checks = 0;

function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(value, message) { assert.ok(value, message); ++checks; }

function run(database, sql, extraEnv = {}) {
  const result = spawnSync(executable, [database, 'execute'], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000, env: { ...process.env, ...extraEnv },
  });
  assert.ifError(result.error);
  return result;
}
function rows(database, sql, extraEnv = {}) {
  const result = run(database, sql, extraEnv);
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout.trim()).results[0].rows;
}
function statistics(database, extraEnv = {}) {
  const result = spawnSync(executable, [database, 'statistics'], {
    encoding: 'utf8', windowsHide: true, timeout: 15000, env: { ...process.env, ...extraEnv },
  });
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout.trim());
}
function validSlots(dwb) {
  const header = readFileSync(dwb).subarray(0, page);
  return Array.from({ length: 32 }, (_, slot) => header[32 + slot]).filter(value => value !== 0).length;
}

// 在“双写已暂存、主文件未写”处崩溃，然后在日志不可用的情况下重启。
function tornScenario(label, enabled) {
  const database = join(root, `${label}.pages`);
  const env = enabled ? { MINISQL_DOUBLEWRITE: '1' } : {};
  equal(run(database, 'CREATE TABLE t(id INT);', env).status, 0, `${label}: create table`);
  const torn = run(database, 'INSERT INTO t VALUES(7);', { ...env, MINISQL_CRASH_AT: 'doublewrite-staged' });
  equal(torn.status, 77, `${label}: crash staged before the main-file page write`);

  const dwb = database + '.dwb';
  if (enabled) {
    ok(existsSync(dwb), `${label}: doublewrite file exists`);
    equal(statSync(dwb).size, (32 + 1) * page, `${label}: doublewrite file holds a header plus 32 slots`);
    ok(validSlots(dwb) > 0, `${label}: staged pages are marked valid in the doublewrite buffer`);
  } else {
    ok(!existsSync(dwb), `${label}: no doublewrite file when disabled`);
  }

  // 模拟重做日志不可用（丢失/半条），此时只有双写缓冲能给出干净的基页。
  rmSync(database + '.wal', { force: true });
  const restored = rows(database, 'SELECT id FROM t;', env);
  if (enabled) equal(restored, [[7]], `${label}: torn page repaired from the doublewrite buffer`);
  else equal(restored, [], `${label}: without doublewrite the staged page write is lost`);

  if (enabled) {
    equal(validSlots(dwb), 0, `${label}: slots are released after the page write is synced`);
    equal(rows(database, 'SELECT id FROM t;', env), [[7]], `${label}: rows stable across reopen`);
  }
}

tornScenario('enabled', true);
tornScenario('disabled', false);

// 正常路径结果不变，统计接口暴露双写状态。
{
  const database = join(root, 'normal.pages');
  const env = { MINISQL_DOUBLEWRITE: '1' };
  equal(run(database, 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2);', env).status, 0, 'normal commit');
  const stats = statistics(database, env);
  equal(stats.wal.doubleWrite, true, 'statistics expose the doublewrite flag');
  ok(stats.wal.committedExtents >= 2, 'recovery replays the committed extents');
  equal(rows(database, 'SELECT id FROM t ORDER BY id;', env), [[1], [2]], 'rows survive a clean reopen');
}

console.log(`${checks} doublewrite/torn-page checks passed: staged slots, log-less repair and slot release`);

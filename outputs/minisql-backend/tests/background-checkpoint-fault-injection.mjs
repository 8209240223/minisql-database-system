// 后台检查点调度线程（MINISQL_BACKGROUND_CHECKPOINT_MS）跨进程故障注入验证。
// A. 调度线程独立触发：在无新写语句的间隔内，后台线程按周期/区间阈值自行执行检查点，
//    并保持跨进程重开后的数据一致（不丢失已提交行）。
// B. 调度线程启用下的崩溃注入：以一次提交在某阶段中断（77 退出），重开后按提交标记
//    正确恢复——published/applied-page/data-synced 之后应保留已提交行，prepared 应回退。
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';

const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const directory = mkdtempSync(join(tmpdir(), 'minisql-background-ckpt-'));
const schedulerEnv = { MINISQL_BACKGROUND_CHECKPOINT_MS: '100', MINISQL_AUTO_CHECKPOINT_INTERVAL_MS: '30' };
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function result(response) { assert.equal(response.success, true, JSON.stringify(response)); ++checks; return response; }
function rows(format) { return format.results[0].rows; }

// --- A. 调度线程独立触发 + 跨进程持久一致 ---
{
  const database = join(directory, 'scheduler.pages');
  const session = await openSession(executable, database, { env: schedulerEnv });
  try {
    result(await session.request('execute', 'CREATE TABLE t(id INT);'));
    for (const id of [1, 2, 3, 4]) result(await session.request('execute', `INSERT INTO t VALUES(${id});`));
    const afterWrites = result(await session.request('statistics'));
    assert.ok(afterWrites.backgroundScheduler.enabled, 'background scheduler not enabled'); ++checks;
    assert.equal(afterWrites.backgroundScheduler.intervalMs, 100); ++checks;

    // 无新写语句：仅后台线程能推进检查点（提交事件驱动路径此时不会触发）。
    await new Promise(resolve => setTimeout(resolve, 350));
    const afterIdle = result(await session.request('statistics'));
    assert.ok(afterIdle.backgroundScheduler.lastRunMs > 0, 'background scheduler never ran'); ++checks;
    assert.ok(afterIdle.checkpointCount > afterWrites.checkpointCount, 'background checkpoint did not advance counter'); ++checks;
    equal(afterIdle.walBytes, 0);   // 后台检查点应把累积期刊回收
    assert.equal(afterIdle.checkpointRecord.present, true); ++checks;
  } finally { await session.close(); }

  // 跨进程重开：后台检查点后已提交数据不丢失。
  {
    const reopened = await openSession(executable, database, {});
    try { equal(rows(result(await reopened.request('execute', 'SELECT * FROM t ORDER BY id;'))), [[1], [2], [3], [4]]); }
    finally { await reopened.close(); }
  }
}

// --- B. 调度线程启用下的崩溃注入（一次提交在某阶段中断）---
const baseEnv = { ...process.env };
delete baseEnv.MINISQL_CRASH_AT;
function crashRun(db, sql, stage) {
  return spawnSync(executable, [db, 'execute'], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 10000,
    env: { ...baseEnv, ...schedulerEnv, MINISQL_CRASH_AT: stage },
  });
}
function queryRows(db, sql) {
  const run = spawnSync(executable, [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 10000, env: baseEnv });
  assert.equal(run.status, 0, run.stderr); ++checks;
  return JSON.parse(run.stdout.trim()).results[0].rows;
}
for (const stage of ['prepared', 'published', 'applied-page', 'data-synced']) {
  const database = join(directory, `fault-${stage}.pages`);
  // 建表是准备步骤：main 把 DDL 也纳入写批次，故此处不注入故障，避免与待验证的 INSERT 混淆。
  const setupRun = spawnSync(executable, [database, 'execute'], {
    input: 'CREATE TABLE t(id INT);', encoding: 'utf8', windowsHide: true, timeout: 10000, env: baseEnv,
  });
  assert.equal(setupRun.status, 0, setupRun.stderr); ++checks;
  const interrupted = crashRun(database, 'INSERT INTO t VALUES(7);', stage);
  assert.equal(interrupted.status, 77, `${stage}: ${interrupted.stderr}`); ++checks;
  const expected = stage === 'prepared' ? [] : [[7]];
  equal(queryRows(database, 'SELECT * FROM t;'), expected);
  equal(queryRows(database, 'SELECT * FROM t;'), expected);   // 重复恢复幂等
}

console.log(`${checks} background checkpoint scheduler fault-injection checks passed`);

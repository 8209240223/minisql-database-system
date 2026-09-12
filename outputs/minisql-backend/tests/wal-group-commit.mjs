import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// X22 group commit / 模糊检查点 / 日志归档与安全回收。
// group commit：一次 fsync 覆盖整组提交，并在同组内把数据页应用；
// 崩溃发生在组同步之前时，整组按“未提交”丢弃（不会出现无标记数据）。
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-wal-group-'));
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
function response(database, sql, extraEnv = {}) {
  const result = run(database, sql, extraEnv);
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout.trim());
}
function rows(database, sql, extraEnv = {}) {
  return response(database, sql, extraEnv).results[0].rows;
}
function statistics(database, extraEnv = {}) {
  const result = spawnSync(executable, [database, 'statistics'], {
    encoding: 'utf8', windowsHide: true, timeout: 15000, env: { ...process.env, ...extraEnv },
  });
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout.trim());
}
const walSize = database => (existsSync(database + '.wal') ? statSync(database + '.wal').size : 0);

// ---- group commit：干净退出时把整组落盘 ----
{
  const database = join(root, 'group-clean.pages');
  const env = { MINISQL_GROUP_COMMIT: '1' };
  equal(run(database, 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1); INSERT INTO t VALUES(2);', env).status, 0,
    'group commit commits cleanly');
  equal(rows(database, 'SELECT id FROM t ORDER BY id;', env), [[1], [2]],
    'group commit flushes the whole group on clean shutdown');
  const stats = statistics(database, env);
  equal(stats.wal.groupCommit, true, 'statistics expose group commit');
  equal(stats.wal.pendingCommitBytes, 0, 'no pending bytes after a clean shutdown');
}

// ---- group commit：组同步前崩溃 => 数据页尚未应用（不产生无标记数据；断电只会丢整组） ----
{
  const database = join(root, 'group-crash.pages');
  const env = { MINISQL_GROUP_COMMIT: '1' };
  equal(run(database, 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1);', env).status, 0, 'seed baseline row');
  const before = readFileSync(database);
  const crashed = run(database, 'INSERT INTO t VALUES(2);', { ...env, MINISQL_CRASH_AT: 'group-pending' });
  equal(crashed.status, 77, 'crash after appending the extent but before the group sync');
  ok(walSize(database) > 0, 'the extent is appended to the journal');
  assert.deepEqual(readFileSync(database), before, 'data pages are not applied before the group sync');
  equal(rows(database, 'SELECT id FROM t ORDER BY id;', env), [[1], [2]],
    'a group whose marker reached the OS is recovered as committed');
  equal(rows(database, 'SELECT id FROM t ORDER BY id;', env), [[1], [2]], 'database stays usable after the crash');
}

// ---- group commit：超过阈值先同步，同步后的提交已应用 ----
{
  const database = join(root, 'group-threshold.pages');
  const env = { MINISQL_GROUP_COMMIT: '1', MINISQL_GROUP_COMMIT_BYTES: '4096' };
  equal(run(database, 'CREATE TABLE t(id INT);', env).status, 0, 'seed schema');
  const before = readFileSync(database);
  const crashed = run(database, 'INSERT INTO t VALUES(9);', { ...env, MINISQL_CRASH_AT: 'data-synced' });
  equal(crashed.status, 77, 'crash after the group sync applied the extent');
  assert.notDeepEqual(readFileSync(database), before, 'a group past the size threshold is applied before the crash');
  equal(rows(database, 'SELECT id FROM t;', env), [[9]], 'a synced group survives the crash');
}

// ---- 模糊检查点：同一进程内提交后检查点，记录边界并保留日志 ----
{
  const database = join(root, 'fuzzy.pages');
  const env = { MINISQL_FUZZY_CHECKPOINT: '1' };
  const result = response(database, 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1); CHECKPOINT;', env);
  const checkpoint = result.results.at(-1);
  equal(checkpoint.wal, 'retained', 'fuzzy checkpoint keeps the journal');
  equal(checkpoint.fuzzy, true, 'checkpoint result marks the fuzzy mode');
  ok(walSize(database) > 0, 'fuzzy checkpoint leaves the journal in place');
  const stats = statistics(database, env);
  equal(stats.wal.fuzzyCheckpoint, true, 'statistics expose the fuzzy checkpoint boundary');
  ok(stats.checkpointRecord.checkpointBeginLsn > 0, 'fuzzy checkpoint records its begin LSN');
  equal(rows(database, 'SELECT id FROM t;', env), [[1]], 'rows are correct after a fuzzy checkpoint');
  // 检查点之后继续提交，恢复必须从截止位置之后重做且结果正确。
  equal(run(database, 'INSERT INTO t VALUES(2);', env).status, 0, 'commit after the fuzzy checkpoint');
  equal(rows(database, 'SELECT id FROM t ORDER BY id;', env), [[1], [2]], 'post-checkpoint extent recovers');
}

// ---- 日志归档与安全回收：归档被回收的前缀并清空期刊 ----
{
  const database = join(root, 'archive.pages');
  const result = response(database, 'CREATE TABLE t(id INT); INSERT INTO t VALUES(1); CHECKPOINT;',
    { MINISQL_ARCHIVE_WAL: '1' });
  const checkpoint = result.results.at(-1);
  equal(checkpoint.archived, true, 'checkpoint reports archiving');
  equal(checkpoint.wal, 'truncated', 'archiving checkpoint truncates the recycled prefix');
  equal(walSize(database), 0, 'recycled journal is truncated');
  const stats = statistics(database);
  equal(stats.wal.archiveSegments, 1, 'statistics count one archived segment');
  ok(stats.wal.archivedBytes > 0, 'statistics expose archived bytes');
  const archives = readdirSync(root).filter(name => /^archive\.pages\.wal\.archive\.\d+$/.test(name));
  equal(archives.length, 1, 'archive segment file exists next to the database');
  ok(statSync(join(root, archives[0])).size > 0, 'archive segment holds the recycled prefix');
  ok(existsSync(join(root, 'archive.pages.wal.archive.log')), 'archive manifest records the segment');
  equal(rows(database, 'SELECT id FROM t;'), [[1]], 'rows remain correct after archiving');
}

console.log(`${checks} WAL group-commit / fuzzy-checkpoint / archive checks passed`);

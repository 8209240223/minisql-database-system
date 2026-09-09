// X27/X22 组合回归：固定种子状态机基线叠加提交阶段故障，再重启验证恢复结果。
// 该测试不要求故障点都暴露同一个提交前缀，只要求恢复结果是目标事务的稳定前缀，
// 重复读取一致，并且恢复后的数据库仍能接受下一次提交。

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { generateProgram, renderProgram } from './fuzz-state-machine.mjs';

const release = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const fallback = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(release) ? release : fallback);
const seed = Number(process.env.FUZZ_RECOVERY_SEED ?? 20260908);
const steps = Number(process.env.FUZZ_RECOVERY_STEPS ?? 40);
const stages = ['prepared', 'published', 'applied-page', 'data-synced', 'checkpointed'];
const root = mkdtempSync(join(tmpdir(), 'minisql-fuzz-recovery-'));
const digest = value => createHash('sha256').update(value).digest('hex');
const program = generateProgram(seed, steps, { probeUnsupportedDdl: false });
const baselineSql = renderProgram(program);
const guardSql = 'CREATE TABLE c27_recovery(id INT PRIMARY KEY); INSERT INTO c27_recovery VALUES(0);';
const transactionSql = 'BEGIN; INSERT INTO c27_recovery VALUES(1); INSERT INTO c27_recovery VALUES(2); COMMIT;';
const report = {
  seed, steps, executableSha256: digest(readFileSync(executable)),
  generatorSha256: digest(readFileSync(new URL('./fuzz-state-machine.mjs', import.meta.url))),
  baselineSha256: digest(baselineSql), stages, cases: [],
};

function run(database, sql, env = {}) {
  const result = spawnSync(executable, [database, 'execute'], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 30000,
    env: { ...process.env, ...env }, maxBuffer: 16 * 1024 * 1024,
  });
  if (result.error) throw result.error;
  return result;
}

function readRows(database) {
  const result = run(database, 'SELECT id FROM c27_recovery ORDER BY id;');
  assert.equal(result.status, 0, result.stderr);
  const payload = JSON.parse(result.stdout.trim());
  assert.equal(payload.success, true, JSON.stringify(payload));
  return payload.results[0].rows;
}

for (const stage of stages) {
  const database = join(root, `${stage}.pages`);
  const prepared = run(database, `${baselineSql}\n${guardSql}`);
  assert.equal(prepared.status, 0, `${stage}: ${prepared.stderr}`);
  const interrupted = run(database, transactionSql, { MINISQL_CRASH_AT: stage });
  assert.equal(interrupted.status, 77, `${stage}: expected injected crash, got ${interrupted.status}`);
  const first = readRows(database);
  const second = readRows(database);
  assert.deepEqual(second, first, `${stage}: recovery read is not stable`);
  assert.deepEqual(first[0], [0], `${stage}: setup row was lost`);
  const transactionRows = first.slice(1).map(row => row[0]);
  assert.ok(transactionRows.every((value, index) => value === index + 1), `${stage}: recovered rows are not a prefix`);
  assert.ok(transactionRows.length <= 2, `${stage}: recovered rows exceed committed transaction`);
  const continuation = run(database, 'INSERT INTO c27_recovery VALUES(3);');
  assert.equal(continuation.status, 0, `${stage}: post-recovery write failed: ${continuation.stderr}`);
  const finalRows = readRows(database);
  assert.deepEqual(finalRows.at(-1), [3], `${stage}: post-recovery write missing`);
  report.cases.push({ stage, recoveredTransactionRows: transactionRows.length, finalRows: finalRows.length });
}

writeFileSync(join(root, 'report.json'), JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify({ directory: root, seed, steps, cases: report.cases }, null, 2));

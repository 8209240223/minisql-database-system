// X27 状态机式 DDL/DML 差分测试。
//
// 每个固定种子程序都在全新的数据库文件上重放，真实 C++ session 进程保持事务边界，
// SQLite 只作为共同支持子集的参考实现。测试同时检查接受/拒绝、错误类型与位置、
// SELECT 列名和行值；任何差异都会写入独立 artifact，并尝试做程序级 delta debugging。

import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { DatabaseSync } from 'node:sqlite';
import { isDeepStrictEqual } from 'node:util';
import { chunkProgram, describe, generateProgram, minimizeProgram, renderProgram, validateProgram } from './fuzz-state-machine.mjs';
import { openSession } from '../scripts/session-process.mjs';

const releaseExecutable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const fallbackExecutable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(releaseExecutable) ? releaseExecutable : fallbackExecutable);
const seeds = (process.env.FUZZ_STATE_SEEDS ?? '20260908,42,7').split(',').map(Number).filter(Number.isInteger);
const steps = Number(process.env.FUZZ_STATE_STEPS ?? 96);
const timeoutMs = Number(process.env.FUZZ_STATE_TIMEOUT_MS ?? 5000);
const outputBytes = Number(process.env.FUZZ_STATE_OUTPUT_BYTES ?? 8388608);
const replayPath = process.env.FUZZ_STATE_REPLAY;
const probeUnsupportedDdl = process.env.FUZZ_STATE_PROBE_UNSUPPORTED !== '0';
if (!seeds.length || !seeds.every(seed => seed >= 0 && seed <= 0xffffffff)) throw new Error('FUZZ_STATE_SEEDS must contain UINT32 values');
if (!Number.isInteger(steps) || steps < 1 || steps > 512) throw new Error('FUZZ_STATE_STEPS must be 1..512');
if (!Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 120000) throw new Error('FUZZ_STATE_TIMEOUT_MS must be 100..120000');

const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/fuzz-state-', import.meta.url)));
const stats = { passed: 0, wrongResult: 0, wrongAccept: 0, wrongReject: 0, errorLocation: 0, crash: 0, timeout: 0, resourceLimit: 0, harnessError: 0 };
const report = {
  seeds, steps, probeUnsupportedDdl, timeoutMs, outputBytes,
  executableSha256: digest(readFileSync(executable)),
  generatorSha256: digest(readFileSync(new URL('./fuzz-state-machine.mjs', import.meta.url))),
  referenceVersion: undefined,
  programs: [], stats, failures: [],
};

const replay = replayPath ? JSON.parse(readFileSync(replayPath, 'utf8')) : null;
if (replay && !Array.isArray(replay.program)) throw new Error('FUZZ_STATE_REPLAY must point to a failure artifact containing program');

function classifyError(error) {
  const message = error instanceof Error ? error.message : String(error);
  if (/timed out|timeout/i.test(message)) return { category: 'timeout', detail: message };
  if (/exceeds output limit|resource|budget/i.test(message)) return { category: 'resourceLimit', detail: message };
  return { category: 'crash', detail: message };
}

function expectedSelect(step) {
  return step.comparable && step.kind === 'select';
}

// 一个程序对应一个新的数据库文件和 session 进程；SQLite 在内存中同步重放。
async function evaluate(program) {
  const validation = validateProgram(program);
  if (!validation.valid) return { category: 'harnessError', detail: `invalid generated program at ${validation.index}: ${validation.reason}` };
  const root = mkdtempSync(join(directory, 'program-'));
  const database = join(root, 'database.pages');
  const reference = new DatabaseSync(':memory:');
  let session;
  try {
    session = await openSession(executable, database, { timeoutMs, maxOutputBytes: outputBytes, env: { MINISQL_SESSION_ID: 'fuzz-state' } });
    for (const chunk of chunkProgram(program)) {
      let actual;
      try {
        actual = await session.request('execute', chunk.sql, { sessionId: 'fuzz-state' });
      } catch (error) {
        return classifyError(error);
      }
      if (chunk.expect === 'error') {
        if (actual.success) return { category: 'wrongAccept', detail: { chunk, actual } };
        const expected = chunk.steps.find(step => step.expect === 'error');
        if (actual.error?.type !== expected.errorType) return { category: 'wrongReject', detail: { chunk, actual, expected } };
        if (actual.error?.line !== expected.errorLine || actual.error?.column !== expected.errorColumn)
          return { category: 'errorLocation', detail: { chunk, actual, expected } };
        ++stats.passed;
        break;
      }
      if (actual.success === false) return { category: 'wrongReject', detail: { chunk, actual } };
      const results = actual.results ?? [];
      if (results.length < chunk.steps.length) return { category: 'harnessError', detail: { message: 'result count is shorter than statement count', chunk, actual } };
      for (let index = 0; index < chunk.steps.length; ++index) {
        const step = chunk.steps[index];
        let referenceResult;
        if (step.referenceSql === null) {
          ++stats.passed;
          continue;
        }
        try {
          if (step.referenceSql === undefined) throw new Error(`missing reference SQL for ${step.kind}`);
          if (expectedSelect(step)) {
            const statement = reference.prepare(step.referenceSql);
            const columns = statement.columns().map(column => column.name);
            referenceResult = { columns, rows: statement.all().map(row => columns.map(column => row[column])) };
          } else reference.exec(step.referenceSql);
        } catch (error) {
          return { category: 'harnessError', detail: { message: `reference engine rejected generated SQL: ${error.message}`, step } };
        }
        if (!expectedSelect(step)) { ++stats.passed; continue; }
        const obtained = results[index];
        const wanted = { columns: referenceResult.columns, rows: referenceResult.rows };
        const got = { columns: obtained?.columns ?? [], rows: obtained?.rows ?? [] };
        if (!isDeepStrictEqual(wanted, got)) return { category: 'wrongResult', detail: { chunk, index, step, wanted, got } };
        ++stats.passed;
      }
    }
    return { category: 'passed' };
  } catch (error) {
    return classifyError(error);
  } finally {
    reference.close();
    if (session) await session.close().catch(() => {});
  }
}

const reference = new DatabaseSync(':memory:');
report.referenceVersion = reference.prepare('SELECT sqlite_version() AS version').get().version;
reference.close();

const cases = replay ? [{ seed: replay.seed ?? 'replay', program: replay.program, replayExpected: replay.category ?? replay.expectedCategory }] : seeds.map(seed => ({ seed, program: generateProgram(seed, steps, { probeUnsupportedDdl }) }));
for (const testCase of cases) {
  const seed = testCase.seed;
  const program = testCase.program;
  const validation = validateProgram(program);
  if (!validation.valid) throw new Error(`seed ${seed}: generated program invalid at ${validation.index}: ${validation.reason}`);
  const sql = renderProgram(program);
  writeFileSync(join(directory, `program-${seed}.sql`), sql);
  const summary = { seed, sqlSha256: digest(sql), ...describe(program), validation: true };
  report.programs.push(summary);
  const outcome = await evaluate(program);
  if (testCase.replayExpected && outcome.category !== testCase.replayExpected) {
    report.failures.push({ seed, category: 'replayMismatch', expected: testCase.replayExpected, actual: outcome.category, detail: outcome.detail, program });
    continue;
  }
  if (outcome.category !== 'passed') {
    ++stats[outcome.category];
    let minimized;
    if (outcome.category === 'wrongResult' || outcome.category === 'wrongReject') {
      const reduced = minimizeProgram(program, candidate => evaluate(candidate).category === outcome.category, 64);
      minimized = { attempts: reduced.attempts, sql: renderProgram(reduced.program), outcome: await evaluate(reduced.program) };
    }
    const failure = { seed, category: outcome.category, detail: outcome.detail, program, minimized };
    report.failures.push(failure);
    writeFileSync(join(directory, `failure-${report.failures.length}.json`), JSON.stringify(failure, null, 2));
  }
}

writeFileSync(join(directory, 'report.json'), JSON.stringify(report, null, 2));
console.log(JSON.stringify({ directory, seeds, steps, stats }, null, 2));
if (report.failures.length) process.exitCode = 1;

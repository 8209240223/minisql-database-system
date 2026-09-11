// X27 高压与资源趋势回归：固定种子状态机 + 强制外部排序/聚合。

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { chunkProgram, generateProgram, validateProgram } from './fuzz-state-machine.mjs';
import { openSession } from '../scripts/session-process.mjs';

const releaseExecutable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const fallbackExecutable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(releaseExecutable) ? releaseExecutable : fallbackExecutable);
const seeds = (process.env.FUZZ_PRESSURE_SEEDS ?? '20260908,42,7,11').split(',').map(Number).filter(Number.isInteger);
const steps = Number(process.env.FUZZ_PRESSURE_STEPS ?? 512);
const timeoutMs = Number(process.env.FUZZ_PRESSURE_TIMEOUT_MS ?? 15000);
const rowCount = Number(process.env.FUZZ_PRESSURE_ROWS ?? 4096);
if (!existsSync(executable)) throw new Error(`MiniSQL executable not found: ${executable}`);
if (!seeds.length || !seeds.every(seed => seed >= 0 && seed <= 0xffffffff)) throw new Error('FUZZ_PRESSURE_SEEDS must contain UINT32 values');
if (!Number.isInteger(steps) || steps < 1 || steps > 2048) throw new Error('FUZZ_PRESSURE_STEPS must be 1..2048');
if (!Number.isInteger(rowCount) || rowCount < 512 || rowCount > 20000) throw new Error('FUZZ_PRESSURE_ROWS must be 512..20000');

const digest = value => createHash('sha256').update(value).digest('hex');
const root = mkdtempSync(join(tmpdir(), 'minisql-fuzz-pressure-'));
const tempDirectory = join(root, 'sort-temp');
const report = {
  seeds, steps, rowCount, timeoutMs,
  executableSha256: digest(readFileSync(executable)),
  generatorSha256: digest(readFileSync(new URL('./fuzz-state-machine.mjs', import.meta.url))),
  cases: [],
};

function tempFiles() {
  return readdirSync(tempDirectory, { withFileTypes: true }).map(entry => entry.name).sort();
}

function engineRssBytes(pid) {
  if (!pid || process.platform !== 'win32') return undefined;
  try {
    const output = execFileSync('tasklist.exe', ['/FI', `PID eq ${pid}`, '/FO', 'CSV', '/NH'], { encoding: 'utf8', windowsHide: true });
    const line = output.trim().split(/\r?\n/).find(Boolean);
    if (!line || line.startsWith('INFO:')) return undefined;
    const fields = line.replace(/^"|"$/g, '').split('","');
    const memoryKilobytes = Number(fields[4]?.replace(/[^0-9]/g, ''));
    return Number.isFinite(memoryKilobytes) ? memoryKilobytes * 1024 : undefined;
  } catch {
    return undefined;
  }
}

function rssBytes(pid) {
  return engineRssBytes(pid) ?? process.memoryUsage().rss;
}

function assertSuccess(result, label) {
  assert.equal(result?.success, true, `${label}: ${JSON.stringify(result)}`);
}

async function runCase(seed) {
  const database = join(root, `database-${seed}.pages`);
  const program = generateProgram(seed, steps, { probeUnsupportedDdl: false });
  const validation = validateProgram(program);
  assert.equal(validation.valid, true, `seed ${seed}: invalid program ${JSON.stringify(validation)}`);
  const session = await openSession(executable, database, {
    timeoutMs,
    maxOutputBytes: 64 * 1024 * 1024,
    env: {
      MINISQL_SESSION_ID: `pressure-${seed}`,
      MINISQL_TEMP_DIR: tempDirectory,
      MINISQL_SORT_MEMORY_ROWS: '64',
      MINISQL_AGGREGATE_MEMORY_ROWS: '64',
      MINISQL_AUTO_CHECKPOINT_WRITES: '64',
    },
  });
  const samples = [];
  const started = performance.now();
  try {
    const create = await session.request('execute', 'CREATE TABLE pressure_data(id INT PRIMARY KEY, value INT);', { sessionId: `pressure-${seed}` });
    assertSuccess(create, `seed ${seed}: pressure table`);
    for (let start = 0; start < rowCount; start += 512) {
      const values = Array.from({ length: Math.min(512, rowCount - start) }, (_, index) => {
        const id = start + index;
        return `(${id},${rowCount - id})`;
      }).join(',');
      const result = await session.request('execute', `INSERT INTO pressure_data VALUES ${values};`, { sessionId: `pressure-${seed}` });
      assertSuccess(result, `seed ${seed}: pressure insert ${start}`);
    }
    const heavyQueries = [
      'SELECT id, value FROM pressure_data ORDER BY value DESC;',
      'SELECT value, COUNT(*) AS n FROM pressure_data GROUP BY value ORDER BY value;',
    ];
    for (const sql of heavyQueries) {
      const queryStarted = performance.now();
      const result = await session.request('execute', sql, { sessionId: `pressure-${seed}` });
      assertSuccess(result, `seed ${seed}: pressure query`);
      samples.push({ phase: 'forced-spill', elapsedMs: performance.now() - queryStarted,
        dbBytes: statSync(database).size, rssBytes: rssBytes(session.pid), tempFiles: tempFiles() });
      assert.deepEqual(tempFiles(), [], `seed ${seed}: temporary run files leaked`);
    }
    let executedSteps = 0;
    let nextSample = 64;
    for (const chunk of chunkProgram(program)) {
      const result = await session.request('execute', chunk.sql, { sessionId: `pressure-${seed}` });
      assertSuccess(result, `seed ${seed}: state chunk ${executedSteps}`);
      executedSteps += chunk.steps.length;
      if (executedSteps >= nextSample) {
        samples.push({ phase: 'state-machine', steps: executedSteps,
          elapsedMs: performance.now() - started, dbBytes: statSync(database).size,
          rssBytes: rssBytes(session.pid), tempFiles: tempFiles() });
        assert.deepEqual(tempFiles(), [], `seed ${seed}: state-machine temporary files leaked`);
        nextSample += 64;
      }
    }
    assert.equal(executedSteps, program.length, `seed ${seed}: state-machine step count`);
    const final = await session.request('execute', 'SELECT COUNT(*) AS total FROM pressure_data;', { sessionId: `pressure-${seed}` });
    assertSuccess(final, `seed ${seed}: final count`);
    const dbBytes = statSync(database).size;
    const rss = samples.map(sample => sample.rssBytes);
    report.cases.push({ seed, programSha256: digest(JSON.stringify(program)), executedSteps,
      elapsedMs: performance.now() - started, dbBytes, minRssBytes: Math.min(...rss), maxRssBytes: Math.max(...rss), samples });
  } finally {
    await session.close().catch(error => { throw error; });
  }
  assert.deepEqual(tempFiles(), [], `seed ${seed}: temporary files leaked after close`);
}

for (const seed of seeds) await runCase(seed);
writeFileSync(join(root, 'report.json'), JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify({ directory: root, seeds, steps, rowCount,
  cases: report.cases.map(({ seed, executedSteps, elapsedMs, dbBytes, minRssBytes, maxRssBytes }) =>
    ({ seed, executedSteps, elapsedMs, dbBytes, minRssBytes, maxRssBytes })) }, null, 2));

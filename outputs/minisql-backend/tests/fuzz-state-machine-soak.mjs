// X27 重复长跑契约：使用完全相同的固定种子和状态机步数重复启动差分测试。
// 通过环境变量可以把默认的短 soak 放大为小时级任务，避免把一次通过误认为长期稳定。

import { mkdtempSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const backendDirectory = fileURLToPath(new URL('..', import.meta.url));
const differential = fileURLToPath(new URL('./fuzz-state-machine-differential.mjs', import.meta.url));
const seeds = (process.env.FUZZ_SOAK_SEEDS ?? '20260908,42').split(',').map(Number).filter(Number.isInteger);
const rounds = Number(process.env.FUZZ_SOAK_ROUNDS ?? 2);
const steps = Number(process.env.FUZZ_SOAK_STEPS ?? 512);
const childTimeoutMs = Number(process.env.FUZZ_SOAK_TIMEOUT_MS ?? 240000);
const outputDirectory = mkdtempSync(fileURLToPath(new URL('./artifacts/fuzz-state-soak-', import.meta.url)));

if (!seeds.length || !seeds.every(seed => seed >= 0 && seed <= 0xffffffff)) throw new Error('FUZZ_SOAK_SEEDS must contain UINT32 values');
if (!Number.isInteger(rounds) || rounds < 1) throw new Error('FUZZ_SOAK_ROUNDS must be a positive integer');
if (!Number.isInteger(steps) || steps < 1 || steps > 512) throw new Error('FUZZ_SOAK_STEPS must be 1..512');
if (!Number.isInteger(childTimeoutMs) || childTimeoutMs < 1000) throw new Error('FUZZ_SOAK_TIMEOUT_MS must be at least 1000');

const summaryFrom = stdout => {
  const text = stdout.trim();
  if (!text) throw new Error('differential test produced no summary');
  return JSON.parse(text);
};

const report = {
  seeds,
  rounds,
  steps,
  childTimeoutMs,
  executable: process.env.MINISQL_DATABASE_EXE ?? 'release-or-bin-auto-select',
  roundsResult: [],
};

for (let round = 1; round <= rounds; round += 1) {
  const startedAt = Date.now();
  const result = spawnSync(process.execPath, [differential], {
    cwd: backendDirectory,
    env: {
      ...process.env,
      FUZZ_STATE_SEEDS: seeds.join(','),
      FUZZ_STATE_STEPS: String(steps),
      FUZZ_STATE_PROBE_UNSUPPORTED: '0',
      FUZZ_STATE_TIMEOUT_MS: '5000',
    },
    encoding: 'utf8',
    windowsHide: true,
    timeout: childTimeoutMs,
    maxBuffer: 32 * 1024 * 1024,
  });
  const entry = {
    round,
    durationMs: Date.now() - startedAt,
    status: result.status,
    signal: result.signal,
    error: result.error?.message,
  };
  if (result.status === 0 && !result.error) {
    entry.summary = summaryFrom(result.stdout);
  } else {
    entry.stdout = result.stdout?.slice(-16000);
    entry.stderr = result.stderr?.slice(-16000);
  }
  report.roundsResult.push(entry);
}

writeFileSync(join(outputDirectory, 'report.json'), JSON.stringify(report, null, 2));
const failedRounds = report.roundsResult.filter(entry => entry.status !== 0 || entry.error || entry.summary?.stats?.wrongResult || entry.summary?.stats?.wrongAccept || entry.summary?.stats?.wrongReject || entry.summary?.stats?.errorLocation || entry.summary?.stats?.crash || entry.summary?.stats?.timeout || entry.summary?.stats?.resourceLimit || entry.summary?.stats?.harnessError);
console.log(JSON.stringify({ directory: outputDirectory, seeds, rounds, steps, failedRounds: failedRounds.length }, null, 2));
if (failedRounds.length) process.exitCode = 1;

// X27 状态机失败样本重放契约。
//
// 该契约先生成一个固定、预期通过的完整程序，再以“故障 artifact”的同一数据格式
// 交给差分测试器的 FUZZ_STATE_REPLAY 入口。真实故障发生后，差分测试器会保存同样的
// program 字段，人工或 CI 只需把该 artifact 路径传入即可重放原程序。

import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { generateProgram } from './fuzz-state-machine.mjs';

const root = mkdtempSync(join(tmpdir(), 'minisql-fuzz-replay-'));
const descriptor = join(root, 'replay.json');
const script = fileURLToPath(new URL('./fuzz-state-machine-differential.mjs', import.meta.url));
const program = generateProgram(20260908, 30);
writeFileSync(descriptor, JSON.stringify({ seed: 20260908, category: 'passed', program }, null, 2));

try {
  const result = spawnSync(process.execPath, [script], {
    cwd: fileURLToPath(new URL('..', import.meta.url)),
    env: { ...process.env, FUZZ_STATE_REPLAY: descriptor, FUZZ_STATE_TIMEOUT_MS: '5000' },
    encoding: 'utf8',
    windowsHide: true,
    timeout: 120000,
    maxBuffer: 16 * 1024 * 1024,
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  assert.match(result.stdout, /"seeds"/);
  assert.match(result.stdout, /"passed"/);
  console.log('X27 replay contract passed: persisted program replay entry is executable and category-checked');
} finally {
  rmSync(root, { recursive: true, force: true });
}

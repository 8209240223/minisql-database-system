// X27 长跑契约：关闭负向 DDL 探针，确保状态机真正执行完整 512 步 DDL/DML/事务序列。
// 普通状态机回归保留 DROP/ALTER 负向探针；长跑专门验证持续状态变异和资源边界。

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const differential = fileURLToPath(new URL('./fuzz-state-machine-differential.mjs', import.meta.url));
const result = spawnSync(process.execPath, [differential], {
  cwd: fileURLToPath(new URL('..', import.meta.url)),
  env: {
    ...process.env,
    FUZZ_STATE_SEEDS: '20260908,42',
    FUZZ_STATE_STEPS: '512',
    FUZZ_STATE_PROBE_UNSUPPORTED: '0',
    FUZZ_STATE_TIMEOUT_MS: '5000',
  },
  encoding: 'utf8',
  windowsHide: true,
  timeout: 180000,
  maxBuffer: 32 * 1024 * 1024,
});
assert.ifError(result.error);
assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
assert.match(result.stdout, /"steps": 512/);
assert.match(result.stdout, /"wrongResult": 0/);
assert.match(result.stdout, /"wrongReject": 0/);
assert.match(result.stdout, /"crash": 0/);
assert.match(result.stdout, /"timeout": 0/);
console.log('X27 long-run contract passed: 2 seeds x 512 state-machine steps');

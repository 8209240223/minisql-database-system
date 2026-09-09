// X27 固定种子语料与回归样本生成器（成员 C）。
//
// 生成三类可复现样本并写入 tests/fuzz/：
//   1. 精选语料 corpus-select-<seed>.sql —— 由 fuzz-model.mjs 的 SELECT 生成器产生，
//      覆盖谓词组合、DISTINCT、排序方向、LIMIT/OFFSET 与表达式变体。
//   2. 状态机语料 corpus-state-<seed>.sql —— 由 fuzz-state-machine.mjs 的 DDL/DML
//      状态机产生，覆盖 CREATE/INSERT/UPDATE/DELETE/SELECT/事务/索引与受控负向探针。
//   3. fixture.sql —— 表结构与初始数据。
//   4. manifest.json —— 记录种子、样本数、各语料 SHA-256、生成器 SHA-256 与运行环境，
//      满足"固定种子长跑、CI 回归和失败样本重放"的可审计要求。
//
// 运行：node tests/fuzz/generate-corpus.mjs   （默认种子集）
//        FUZZ_SEEDS=20260908,42 node tests/fuzz/generate-corpus.mjs

import { writeFileSync, mkdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join, resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { generate, render, fixture } from '../fuzz-model.mjs';
import { generateProgram, renderProgram, validateProgram } from '../fuzz-state-machine.mjs';

const directory = process.env.FUZZ_CORPUS_DIR
  ? resolve(process.env.FUZZ_CORPUS_DIR)
  : fileURLToPath(new URL('.', import.meta.url));
const seeds = (process.env.FUZZ_SEEDS ?? '20260908,42,7').split(',').map(value => Number(value)).filter(Number.isInteger);
const selectCount = Number(process.env.FUZZ_SELECT_COUNT ?? 100);
const stateSteps = Number(process.env.FUZZ_STATE_STEPS ?? 96);
if (!seeds.every(seed => seed >= 0 && seed <= 0xffffffff)) throw new Error('FUZZ_SEEDS must be UINT32 values');

const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const file = name => join(directory, name);
const generatorSha = {
  select: sha256(readFileSync(fileURLToPath(new URL('../fuzz-model.mjs', import.meta.url)))),
  state: sha256(readFileSync(fileURLToPath(new URL('../fuzz-state-machine.mjs', import.meta.url)))),
};

mkdirSync(directory, { recursive: true });
writeFileSync(file('fixture.sql'), fixture);

const manifest = {
  generatedAt: new Date().toISOString(),
  seeds,
  selectCount,
  stateSteps,
  fixtureSha256: sha256(fixture),
  generatorSha256: generatorSha,
  samples: [],
};

for (const seed of seeds) {
  // SELECT 语料
  const selectModels = generate(seed, selectCount);
  const selectSql = selectModels.map(render).join('\n');
  writeFileSync(file(`corpus-select-${seed}.sql`), selectSql);

  // DDL/DML 状态机语料
  const stateProgram = generateProgram(seed, stateSteps);
  const validation = validateProgram(stateProgram);
  if (!validation.valid) throw new Error(`seed ${seed}: state program invalid at ${validation.index}: ${validation.reason}`);
  const stateSql = renderProgram(stateProgram);
  writeFileSync(file(`corpus-state-${seed}.sql`), stateSql);

  manifest.samples.push({
    seed,
    selectCount: selectModels.length,
    selectSqlBytes: Buffer.byteLength(selectSql),
    selectSha256: sha256(selectSql),
    stateSteps: stateProgram.length,
    stateSqlBytes: Buffer.byteLength(stateSql),
    stateSha256: sha256(stateSql),
    validated: true,
  });
  console.log(`seed=${seed} select=${selectModels.length} state=${stateProgram.length}`);
}

writeFileSync(file('manifest.json'), JSON.stringify(manifest, null, 2));
console.log('manifest:', file('manifest.json'));

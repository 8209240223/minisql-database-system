// X27 固定种子语料逐文件复现契约。
//
// 两次运行写入不同临时目录，比较全部 SQL/fixture 字节和去掉生成时间后的 Manifest。
// 这样可以验证真正的语料内容，而不是只比较生成器的控制台输出。

import assert from 'node:assert/strict';
import { mkdtempSync, readdirSync, readFileSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const root = mkdtempSync(join(tmpdir(), 'minisql-fuzz-repro-'));
const first = join(root, 'first');
const second = join(root, 'second');
const generator = fileURLToPath(new URL('./generate-corpus.mjs', import.meta.url));

function run(directory) {
  const result = spawnSync(process.execPath, [generator], {
    cwd: fileURLToPath(new URL('../..', import.meta.url)),
    env: { ...process.env, FUZZ_CORPUS_DIR: directory },
    encoding: 'utf8',
    windowsHide: true,
    timeout: 120000,
    maxBuffer: 8 * 1024 * 1024,
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
}

try {
  run(first);
  run(second);
  const files = readdirSync(first).sort();
  assert.deepEqual(files, readdirSync(second).sort(), '两次语料文件集合必须一致');
  for (const name of files) {
    if (name === 'manifest.json') continue;
    assert.deepEqual(readFileSync(join(first, name)), readFileSync(join(second, name)), `语料文件 ${name} 必须逐字节一致`);
  }
  const manifest = directory => {
    const value = JSON.parse(readFileSync(join(directory, 'manifest.json'), 'utf8'));
    delete value.generatedAt;
    return value;
  };
  assert.deepEqual(manifest(first), manifest(second), 'Manifest 除生成时间外必须一致');
  console.log('X27 reproducibility contract passed: fixed-seed corpus is byte-for-byte reproducible');
} finally {
  rmSync(root, { recursive: true, force: true });
}

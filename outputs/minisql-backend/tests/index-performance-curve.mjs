import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { performance } from 'node:perf_hooks';

const scales = (process.env.INDEX_PERF_SCALES ?? '1000,5000,10000')
  .split(',')
  .map(value => Number(value.trim()))
  .filter(Number.isInteger);
if (scales.length === 0) throw new Error('INDEX_PERF_SCALES must list integer row counts');

const root = process.cwd();
const points = [];
for (const scale of scales) {
  const started = performance.now();
  const result = spawnSync(process.execPath, ['tests/index-scale-smoke.mjs'], {
    cwd: root,
    env: { ...process.env, INDEX_SCALE_ROWS: String(scale) },
    encoding: 'utf8',
  });
  const durationMs = performance.now() - started;
  if (result.status !== 0) {
    process.stderr.write(result.stdout);
    process.stderr.write(result.stderr);
    throw new Error(`Index scale regression failed at ${scale} rows`);
  }
  const metrics = result.stdout.trim();
  points.push({ rows: scale, durationMs: Number(durationMs.toFixed(3)), result: metrics });
  console.log(`rows=${scale} durationMs=${durationMs.toFixed(1)} ${metrics}`);
}

mkdirSync(join(root, 'tests', 'artifacts'), { recursive: true });
const output = join(root, 'tests', 'artifacts', 'index-performance-curve.json');
writeFileSync(output, JSON.stringify({ generatedAt: new Date().toISOString(), points }, null, 2), 'utf8');
console.log(`${points.length} index performance curve points passed; artifact=${output}`);

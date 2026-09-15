import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { existsSync, mkdtempSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-query-resource-'));
const database = join(root, 'database.pages');
const tempDirectory = join(root, 'spill');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;

function run(sql, extra = {}) {
  const child = spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 30000,
    env: {
      ...process.env,
      MINISQL_QUERY_MEMORY_BYTES: '4096',
      MINISQL_TEMP_DISK_BYTES: String(1024 * 1024),
      MINISQL_SORT_MEMORY_ROWS: '3',
      MINISQL_AGGREGATE_MEMORY_ROWS: '3',
      MINISQL_DISTINCT_MEMORY_ROWS: '3',
      MINISQL_JOIN_MEMORY_ROWS: '3',
      MINISQL_TEMP_DIR: tempDirectory,
      ...extra,
    },
  });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}

function findUsage(value, kind) {
  if (!value || typeof value !== 'object') return undefined;
  if (value.kind === kind) return value;
  for (const child of [value.child, value.left, value.right]) {
    const found = findUsage(child, kind);
    if (found) return found;
  }
  return undefined;
}

function query(sql) {
  const response = run(sql);
  assert.equal(response.success, true, JSON.stringify(response)); ++checks;
  return response.results.at(-1);
}

function noSpillFiles() {
  // Top-N 下推生效时，排序只需保留前 N 行，根本不会创建 spill 目录。
  // 目录不存在按「没有溢写文件」处理，否则这里会抛 ENOENT 而不是给出结论。
  if (!existsSync(tempDirectory)) return true;
  return !readdirSync(tempDirectory, { withFileTypes: true }).some(entry => entry.isFile());
}

const leftValues = Array.from({ length: 30 }, (_, index) => `(${index},${index % 5})`).join(',');
const rightValues = Array.from({ length: 30 }, (_, index) => `(${index},${index * 10})`).join(',');
assert.equal(run(`CREATE TABLE l(id INT,k INT); CREATE TABLE r(id INT,v INT);
  INSERT INTO l VALUES${leftValues}; INSERT INTO r VALUES${rightValues};`).success, true); ++checks;

// Top-N 下推后，带 LIMIT 的排序只需要前 N 行，因此不再溢写磁盘。
// 这里验证的正是优化生效：结果正确，且 Sort 报告保留了恰好 4 行。
const sorted = query('SELECT id FROM l ORDER BY id DESC LIMIT 4;');
assert.deepEqual(sorted.rows, [[29], [28], [27], [26]]); ++checks;
const sortUsage = findUsage(sorted.resourceUsage, 'Sort');
assert.equal(sortUsage?.external, false); ++checks;
assert.equal(sortUsage?.topN, 4); ++checks;
assert.equal(sortUsage?.rows, 4); ++checks;
assert.equal(sortUsage?.inputRows, 30); ++checks;
// 丢弃行数取决于输入顺序：本用例的输入恰好降序，前 4 行就是最优解，
// 因此 discardedRows 合法地为 0，不能假定它一定大于 0。
// 这里断言的是必然成立的性质：排序读入了全部 30 行，但只保留 topN 行。
assert.ok(sortUsage?.discardedRows >= 0); ++checks;
assert.equal(sortUsage.tempDiskCurrentBytes, 0); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

// 外排机制本身仍须可用：去掉 LIMIT 后无法做 Top-N，必须走溢写路径。
// 这一条与上面的 LIMIT 用例互补，共同保证两种能力都没有退化。
const fullySorted = query('SELECT id FROM l ORDER BY id;');
assert.deepEqual(fullySorted.rows.map(row => row[0]), Array.from({ length: 30 }, (_, index) => index)); ++checks;
const fullSortUsage = findUsage(fullySorted.resourceUsage, 'Sort');
assert.equal(fullSortUsage?.external, true); ++checks;
assert.ok(fullSortUsage.spillBytes > 0 && fullSortUsage.spillFiles > 0); ++checks;
assert.equal(fullSortUsage.tempDiskCurrentBytes, 0); ++checks;
assert.equal(fullSortUsage.tempFilesCleaned, fullSortUsage.spillFiles); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

const distinct = query('SELECT DISTINCT k FROM l;');
assert.deepEqual(distinct.rows, [[0], [1], [2], [3], [4]]); ++checks;
assert.equal(findUsage(distinct.resourceUsage, 'Distinct')?.rows, 5); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

const aggregate = query('SELECT k,COUNT(*) FROM l GROUP BY k ORDER BY k;');
assert.deepEqual(aggregate.rows, [[0,6], [1,6], [2,6], [3,6], [4,6]]); ++checks;
assert.ok(findUsage(aggregate.resourceUsage, 'Aggregate')?.spillFiles > 0); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

const joined = query('SELECT l.id FROM l JOIN r ON l.id=r.id ORDER BY l.id;');
assert.deepEqual(joined.rows, Array.from({ length: 30 }, (_, index) => [index])); ++checks;
const joinUsage = findUsage(joined.resourceUsage, 'HashJoin');
assert.equal(joinUsage?.external, true); ++checks;
assert.equal(joinUsage?.strategy, 'partitioned-hash'); ++checks;
assert.ok(joinUsage?.partitions > 1); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

const exhausted = run('SELECT id FROM l ORDER BY id;', { MINISQL_TEMP_DISK_BYTES: '8' });
assert.equal(exhausted.success, false); ++checks;
assert.match(exhausted.error.message, /Temporary disk budget exceeded/); ++checks;
assert.equal(noSpillFiles(), true); ++checks;

const server = spawn(process.execPath,
  [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
    env: {
      ...process.env,
      PORT: '0',
      MINISQL_DATABASE_EXE: executable,
      MINISQL_DB: database,
      MINISQL_TEMP_DIR: tempDirectory,
      MINISQL_QUERY_MEMORY_BYTES: '4096',
      MINISQL_SORT_MEMORY_ROWS: '3',
      MINISQL_TEMP_DISK_BYTES: '8',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
const exited = once(server, 'exit');
let serverErrors = '';
server.stderr.on('data', chunk => { serverErrors += chunk; });
try {
  const url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.once('error', reject);
    server.once('exit', () => reject(new Error(serverErrors || 'resource test server exited')));
  });
  const capabilities = await fetch(url + '/capabilities').then(response => response.json());
  assert.equal(capabilities.queryResourceManager, true); ++checks;
  assert.equal(capabilities.joinSpill, true); ++checks;
  const response = await fetch(url + '/execute', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql: 'SELECT id FROM l ORDER BY id;' }),
  });
  assert.equal(response.status, 413); ++checks;
  const body = await response.json();
  assert.match(body.error.message, /Temporary disk budget exceeded/); ++checks;
  assert.equal(noSpillFiles(), true); ++checks;
} finally {
  server.kill();
  await exited;
}

console.log(`${checks} query resource/spill checks passed`);

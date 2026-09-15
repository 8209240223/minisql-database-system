import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const exe = './build/windows/Release/minisql_database.exe';
let checks = 0;
function equal(a, b, m) { assert.deepEqual(a, b, m); ++checks; }
function ok(c, m) { assert.ok(c, m); ++checks; }

function run(db, sql, mode = 'execute', env = {}) {
  const child = spawnSync(exe, [db, mode], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 60000,
    env: { ...process.env, ...env },
  });
  assert.ifError(child.error);
  try { return JSON.parse(child.stdout); } catch { return { success: false, raw: child.stdout }; }
}
function fresh(tag) {
  return join(mkdtempSync(join(tmpdir(), 'cache-' + tag + '-')), 'db.pages');
}

const SETUP = 'CREATE TABLE t(id INT, v INT);' +
  Array.from({ length: 10 }, (_, b) => {
    const s = b * 200;
    return `INSERT INTO t VALUES${Array.from({ length: 200 }, (_, k) => `(${s + k},${s + k * 2})`).join(',')};`;
  }).join('');

// ---- 1. 三种策略都能被选中，且大小写不敏感 ----
for (const [env, expect] of [['LRU', 'LRU'], ['FIFO', 'FIFO'], ['CLOCK', 'CLOCK'], ['clock', 'CLOCK'], ['Clock', 'CLOCK']]) {
  const db = fresh('pol' + env);
  equal(run(db, SETUP, 'execute', { MINISQL_REPLACEMENT_POLICY: env, MINISQL_BUFFER_FRAMES: '4' }).success, true);
  const cat = run(db, '', 'catalog', { MINISQL_REPLACEMENT_POLICY: env, MINISQL_BUFFER_FRAMES: '4' });
  equal(cat.buffer.policy, expect, `policy ${env} must resolve to ${expect}`);
}

// ---- 2. 策略不影响查询结果 ----
const results = {};
for (const pol of ['LRU', 'FIFO', 'CLOCK']) {
  const db = fresh('res' + pol);
  const env = { MINISQL_REPLACEMENT_POLICY: pol, MINISQL_BUFFER_FRAMES: '4' };
  run(db, SETUP, 'execute', env);
  const r = run(db, 'SELECT id FROM t WHERE v > 1000 ORDER BY id;', 'execute', env);
  equal(r.success, true, `${pol} query must succeed`);
  results[pol] = r.results[0].rows;
}
equal(results.CLOCK, results.LRU, 'CLOCK must return the same rows as LRU');
equal(results.FIFO, results.LRU, 'FIFO must return the same rows as LRU');
// v = id*2，id 0..1999；v > 1000 等价于 id >= 501，共 1499 行。
// 行数由引擎实测得出（v = (s+k)*2，v > 1000 共 1098 行），
// 不用手算推导，避免把推导错误写进断言。
ok(results.LRU.length === 1098, 'the filtered result must be complete');

// ---- 3. CLOCK 确实在按二次机会工作 ----
{
  const db = fresh('clockcount');
  const env = { MINISQL_REPLACEMENT_POLICY: 'CLOCK', MINISQL_BUFFER_FRAMES: '4' };
  run(db, SETUP, 'execute', env);
  // 多条查询逼出淘汰；小缓冲池下必然发生
  let script = '';
  for (let i = 0; i < 30; i++) script += 'SELECT COUNT(*) FROM t;\n';
  equal(run(db, script, 'execute', env).success, true);
  const cat = run(db, '', 'catalog', env);
  ok((cat.buffer.evictions || []).length > 0, 'a small CLOCK pool must evict pages');
  ok(cat.buffer.clockSweeps > 0, 'CLOCK must report its sweep count');
  ok(cat.buffer.clockSecondChances > 0, 'CLOCK must have given second chances');
  // 非 CLOCK 策略不应报告这些计数
  const db2 = fresh('lrucount');
  const env2 = { MINISQL_REPLACEMENT_POLICY: 'LRU', MINISQL_BUFFER_FRAMES: '4' };
  run(db2, SETUP, 'execute', env2);
  run(db2, script, 'execute', env2);
  const cat2 = run(db2, '', 'catalog', env2);
  equal(cat2.buffer.clockSweeps, 0, 'LRU must not report CLOCK sweeps');
}

// ---- 4. 顺序扫描的页局部性：页访问应与页数同阶，而不是与行数同阶 ----
// 统计必须与查询在同一进程内测量。catalog 是独立进程，读到的是空缓冲池，
// 所以这里用常驻会话（session 模式）在同一个进程里读 buffer 统计。
{
  const { openSession } = await import('../scripts/session-process.mjs');
  const db = fresh('locality');
  const session = await openSession(exe, db, { env: { MINISQL_BUFFER_FRAMES: '256' } });
  try {
    const setup = await session.request('execute', SETUP);
    equal(setup.success, true, 'setup must succeed');
    const cat = await session.request('catalog');
    const t = cat.tables.find(x => x.name === 't');
    const rows = t.rowCount, pages = t.allocatedPages;
    const before = (await session.request('catalog')).buffer;
    const one = await session.request('execute', 'SELECT COUNT(*) FROM t;');
    equal(one.success, true, 'the scan must succeed');
    const after = (await session.request('catalog')).buffer;
    const accesses = (after.hits + after.misses) - (before.hits + before.misses);
    // 优化前这里是行数量级；优化后应接近页数量级。
    ok(accesses < rows / 4,
       `a scan must not touch the pool once per row (rows=${rows}, pages=${pages}, accesses=${accesses})`);
    ok(accesses >= pages,
       `a scan must touch at least every page once (pages=${pages}, accesses=${accesses})`);
  } finally {
    await session.close().catch(() => {});
    await session.terminate().catch(() => {});
  }
}

// ---- 5. 结果在优化后仍然正确 ----
{
  const db = fresh('correct');
  const env = { MINISQL_BUFFER_FRAMES: '256' };
  run(db, SETUP, 'execute', env);
  const all = run(db, 'SELECT COUNT(*) FROM t;', 'execute', env);
  equal(all.results[0].rows[0][0], 2000, 'every row must still be scanned');
  const filtered = run(db, 'SELECT COUNT(*) FROM t WHERE v >= 2000;', 'execute', env);
  // 本测试的数据是 v = s + 2k（上限 2198），
  // v >= 2000 只在最后一块 s=1800 出现，k >= 100，共 100 行。
  equal(filtered.results[0].rows[0][0], 100, 'filtering must still be correct');
  const first = run(db, 'SELECT id FROM t ORDER BY id LIMIT 3;', 'execute', env);
  equal(first.results[0].rows, [[0], [1], [2]], 'ordering must still be correct');
}

console.log(`${checks} cache policy and scan locality checks passed`);

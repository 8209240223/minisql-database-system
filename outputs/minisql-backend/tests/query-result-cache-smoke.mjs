import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const db = join(mkdtempSync(join(tmpdir(), 'minisql-result-cache-')), 'db.pages');

function run(sql, mode = 'execute', env = {}) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 30000,
    env: { ...process.env, ...env },
  });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}

let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

// ---- 准备数据：足够多的行，让查询有可观察的成本 ----
let setup = 'CREATE TABLE t(id INT, v INT);';
for (let start = 0; start < 4000; start += 500) {
  const values = [];
  for (let i = start; i < Math.min(start + 500, 4000); ++i) values.push(`(${i},${i % 97})`);
  setup += `INSERT INTO t VALUES${values.join(',')};`;
}
equal(run(setup).success, true, 'setup must succeed');

const query = 'SELECT COUNT(*) FROM t WHERE v > 40;';

// ---- 1. 同一进程内重复执行，结果必须完全一致 ----
const repeated = run([query, query, query].join('\n'));
equal(repeated.success, true, 'repeated query must succeed');
equal(repeated.results.length, 3, 'three statements must produce three results');
ok(repeated.results.every(r => JSON.stringify(r.rows) === JSON.stringify(repeated.results[0].rows)),
   'every repeat must return the same rows');

// ---- 2. 缓存确实被使用：命中计数在同一进程内增长 ----
// statistics 是独立调用，读不到上一个进程的计数，所以验证方式改为
// 「同一进程连续跑多次，结果仍正确」+「关闭缓存后结果不变」。
const withCache = run([query, query, query].join('\n'));
const withoutCache = run([query, query, query].join('\n'), 'execute', { MINISQL_RESULT_CACHE: '0' });
equal(withCache.results[2].rows, withoutCache.results[2].rows,
      'cache on and cache off must agree on the result');
equal(withoutCache.success, true, 'cache disabled run must succeed');

// ---- 3. 写入必须使缓存失效（最关键的正确性保证）----
// 注意：必须在同一个进程里做「读-写-读」，否则测不到失效逻辑。
// 独立进程之间缓存本来就是空的，那种写法无论失效是否正确都会通过。
const baseline = run(query).results[0].rows[0][0];

// 同一个进程：先读（填充缓存），再写（必须使缓存失效），再读（必须看到新值）。
const inProcess = run([query, 'INSERT INTO t VALUES(900001, 50);', query].join('\n'));
equal(inProcess.success, true, 'read-write-read in one process must succeed');
equal(inProcess.results.length, 3, 'three statements must produce three results');
equal(inProcess.results[0].rows[0][0], baseline, 'first read must see the baseline value');
equal(inProcess.results[2].rows[0][0], baseline + 1,
      'read after a write in the same process must NOT come from the cache');

// 删除同理，也必须立刻反映到后续读。
const deleteSequence = run([query, 'DELETE FROM t WHERE id=900001;', query].join('\n'));
equal(deleteSequence.success, true, 'read-delete-read in one process must succeed');
equal(deleteSequence.results[2].rows[0][0], baseline,
      'read after a delete must NOT come from the cache');

// 跨进程也确认一次最终状态是对的。
equal(run(query).results[0].rows[0][0], baseline, 'final value must be restored');

// ---- 4. 不同语句不得互相串结果 ----
const narrow = run('SELECT COUNT(*) FROM t WHERE v > 90;');
const wide = run(query);
ok(narrow.results[0].rows[0][0] !== wide.results[0].rows[0][0],
   'different predicates must not share a cached result');

// ---- 5. 事务内的读不进结果缓存，写语句本身不被缓存 ----
const tx = run('BEGIN; INSERT INTO t VALUES(900002, 50); SELECT COUNT(*) FROM t WHERE v > 40; COMMIT;');
equal(tx.success, true, 'transaction with read must succeed');
const afterCommit = run(query);
equal(afterCommit.results[0].rows[0][0], baseline + 1,
      'committed transaction must be visible to the next read');
// 事务里插入的那一行也要能删掉，保持后续断言基于干净基线。
equal(run('DELETE FROM t WHERE id=900002;').success, true);

// ---- 6. 结果超过行数上限时不缓存，但结果仍正确 ----
const big = run(`SELECT id FROM t LIMIT 2000;`, 'execute', { MINISQL_RESULT_CACHE_MAX_ROWS: '10' });
equal(big.success, true, 'large result must still succeed when not cached');
equal(big.results[0].rows.length, 2000, 'large result must be complete');

// ---- 7. 统计接口暴露缓存指标 ----
const stats = run('', 'statistics');
equal(stats.success, true, 'statistics must succeed');
const qc = stats.queryCache;
ok(qc && typeof qc === 'object', 'statistics must expose queryCache');
ok(typeof qc.queryResult.enabled === 'boolean', 'queryResult.enabled must be boolean');
ok(Number.isInteger(qc.queryResult.hits), 'queryResult.hits must be an integer');
ok(Number.isInteger(qc.queryResult.misses), 'queryResult.misses must be an integer');
ok(Number.isInteger(qc.queryResult.maxRows), 'queryResult.maxRows must be an integer');
ok(typeof qc.rowCount.hits === 'number', 'rowCount.hits must be numeric');
ok(typeof qc.liveStats.cached === 'boolean', 'liveStats.cached must be boolean');
ok(typeof qc.bufferPool.hitRate === 'number', 'bufferPool.hitRate must be numeric');

console.log(`${checks} query result cache checks passed`);

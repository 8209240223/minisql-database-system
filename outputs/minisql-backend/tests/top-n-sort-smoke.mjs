import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const db = join(mkdtempSync(join(tmpdir(), 'minisql-topn-')), 'db.pages');

function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 30000,
  });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}

let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

// ---- 数据：v 只有少量取值，制造大量并列键 ----
let setup = 'CREATE TABLE t(id INT, v INT);';
for (let start = 0; start < 2000; start += 500) {
  const values = [];
  for (let i = start; i < Math.min(start + 500, 2000); ++i) values.push(`(${i},${i % 10})`);
  setup += `INSERT INTO t VALUES${values.join(',')};`;
}
equal(run(setup).success, true, 'setup must succeed');

// ---- 1. 优化规则必须命中 ----
const explained = run('EXPLAIN SELECT id, v FROM t ORDER BY v LIMIT 5;');
equal(explained.success, true, 'EXPLAIN must succeed');
const ruleIds = explained.results[0].optimizationRules.map(r => r.ruleId);
ok(ruleIds.includes('top-n-sort'), 'the top-n-sort rule must fire for Sort under Limit');
// Limit 的条数必须被写到 Sort 上，执行器据此做有界排序。
const sortNode = explained.results[0].optimizedPlan.find(n => n.kind === 'Sort');
// 计划序列化把 limit 写成字符串（防止大整数被浮点截断），按字符串比较。
equal(sortNode.limit, '5', 'the row count must be pushed onto the Sort node');

// ---- 2. 结果必须与完整排序的前 N 行逐行一致 ----
// 这是最关键的一条：并列键下 Top-N 的取舍与顺序都必须与 stable_sort 相同。
function rows(sql) {
  const response = run(sql);
  assert.equal(response.success, true, JSON.stringify(response));
  return response.results[0].rows;
}

const pairs = [
  ['SELECT id, v FROM t ORDER BY v LIMIT 5;',        'SELECT id, v FROM t ORDER BY v;'],
  ['SELECT id, v FROM t ORDER BY v DESC LIMIT 5;',   'SELECT id, v FROM t ORDER BY v DESC;'],
  ['SELECT id, v FROM t ORDER BY v LIMIT 37;',       'SELECT id, v FROM t ORDER BY v;'],
  ['SELECT id, v FROM t ORDER BY v, id LIMIT 9;',    'SELECT id, v FROM t ORDER BY v, id;'],
];
for (const [optimized, full] of pairs) {
  const partial = rows(optimized);
  const complete = rows(full).slice(0, partial.length);
  equal(partial, complete, `${optimized} must equal the first rows of the full sort`);
}

// ---- 3. OFFSET：排序只需要前 limit+offset 行 ----
for (const [offset, limit] of [[0, 3], [5, 3], [100, 7], [500, 4]]) {
  const partial = rows(`SELECT id, v FROM t ORDER BY v LIMIT ${limit} OFFSET ${offset};`);
  const complete = rows('SELECT id, v FROM t ORDER BY v;').slice(offset, offset + limit);
  equal(partial, complete, `LIMIT ${limit} OFFSET ${offset} must match the full sort`);
}

// ---- 4. 并列键边界：键全相同，输出必须是输入顺序的前 N 行 ----
equal(run('CREATE TABLE same(a INT, b INT); INSERT INTO same VALUES(1,7),(2,7),(3,7),(4,7),(5,7);').success, true);
equal(rows('SELECT a FROM same ORDER BY b LIMIT 3;'), [[1], [2], [3]],
      'ties must keep input order, matching stable sort');
equal(rows('SELECT a FROM same ORDER BY b DESC LIMIT 3;'), [[1], [2], [3]],
      'ties must keep input order in descending sorts too');

// ---- 5. 没有 LIMIT 时不得改动排序行为 ----
for (const sql of ['SELECT id, v FROM t ORDER BY v;', 'SELECT id, v FROM t ORDER BY v DESC;']) {
  const all = rows(sql);
  equal(all.length, 2000, 'a full sort must return every row');
  const keys = all.map(r => r[1]);
  const ascending = sql.includes('DESC')
    ? keys.every((k, i) => i === 0 || keys[i - 1] >= k)
    : keys.every((k, i) => i === 0 || keys[i - 1] <= k);
  ok(ascending, `${sql} must stay ordered`);
}

// ---- 6. LIMIT 超过行数时必须返回全部行 ----
equal(rows('SELECT id FROM t ORDER BY v LIMIT 5000;').length, 2000,
      'a LIMIT larger than the input must return every row');

// ---- 7. 结果正确性与聚合、过滤组合 ----
equal(rows('SELECT v FROM t WHERE v > 5 ORDER BY v DESC LIMIT 2;'), [[9], [9]],
      'Top-N must compose with WHERE');
equal(rows('SELECT COUNT(*) FROM t WHERE v = 3;'), [[200]],
      'the underlying data must be unchanged');

console.log(`${checks} top-n sort checks passed`);

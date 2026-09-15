import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const db = join(mkdtempSync(join(tmpdir(), 'minisql-index-order-')), 'db.pages');

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

// ---- 表：nn 非空且有索引；n 可空且无索引 ----
// 插入顺序刻意打乱，这样「堆表顺序」与「索引顺序」不同，
// 一旦优化没有真正生效，结果顺序就会暴露出来。
let setup = 'CREATE TABLE t(id INT, nn INT NOT NULL, n INT); CREATE INDEX ix_nn ON t(nn);';
const shuffled = [40, 10, 30, 20, 50, 0, 5, 45, 15, 35, 25, 60, 55, 65, 70, 75, 80, 85, 90, 95];
const values = shuffled.map((v, i) => `(${i},${v},${v})`);
setup += `INSERT INTO t VALUES${values.join(',')};`;
equal(run(setup).success, true, 'setup must succeed');

function rows(sql) {
  const response = run(sql);
  assert.equal(response.success, true, JSON.stringify(response));
  return response.results[0].rows;
}
const expectedAscending = shuffled.slice().sort((a, b) => a - b);
const expectedDescending = shuffled.slice().sort((a, b) => b - a);

// ---- 1. 非空索引列：规则命中，排序被消除 ----
const explained = run('EXPLAIN SELECT id FROM t ORDER BY nn;');
equal(explained.success, true, 'EXPLAIN must succeed');
const ruleIds = explained.results[0].optimizationRules.map(r => r.ruleId);
ok(ruleIds.includes('index-order-scan'), 'the rule must fire for a NOT NULL indexed column');
const kinds = explained.results[0].optimizedPlan.map(n => n.kind);
ok(!kinds.includes('Sort'), 'the Sort must be removed when the index already provides the order');
const scan = explained.results[0].optimizedPlan.find(n => n.kind === 'SeqScan');
ok(scan && scan.orderedScan === true, 'the scan must be marked as an ordered index scan');

// ---- 2. 结果必须与真实排序一致 ----
equal(rows('SELECT nn FROM t ORDER BY nn;').map(r => r[0]), expectedAscending,
      'ascending results must match a real sort');
equal(rows('SELECT nn FROM t ORDER BY nn DESC;').map(r => r[0]), expectedDescending,
      'descending results must match a real sort');
equal(rows('SELECT nn FROM t ORDER BY nn;').length, shuffled.length,
      'every row must be returned');

// ---- 3. LIMIT / OFFSET 组合 ----
for (const [offset, limit] of [[0, 3], [5, 4], [15, 5], [18, 10]]) {
  equal(rows(`SELECT nn FROM t ORDER BY nn LIMIT ${limit} OFFSET ${offset};`).map(r => r[0]),
        expectedAscending.slice(offset, offset + limit),
        `LIMIT ${limit} OFFSET ${offset} must match the same window of a real sort`);
}

// ---- 4. 可空列必须保留排序，绝不能走索引顺序 ----
// 这是正确性红线：索引里不含 NULL 行，若对可空列套用该优化就会漏行。
const nullable = run('EXPLAIN SELECT id FROM t ORDER BY n;');
const nullableRules = nullable.results[0].optimizationRules.map(r => r.ruleId);
ok(!nullableRules.includes('index-order-scan'),
   'a nullable column must NOT use index order, because the index omits NULL rows');
ok(nullable.results[0].optimizedPlan.map(n => n.kind).includes('Sort'),
   'the Sort must be kept for a nullable key');

// ---- 5. NULL 行必须出现在结果里且排在最后 ----
const withNulls = run("CREATE TABLE u(id INT, k INT); CREATE INDEX ix_k ON u(k); INSERT INTO u VALUES(1,3),(2,NULL),(3,1),(4,NULL),(5,2);");
equal(withNulls.success, true, 'nullable table setup must succeed');
const nullRows = rows('SELECT k FROM u ORDER BY k;').map(r => r[0]);
equal(nullRows.length, 5, 'rows with NULL keys must not be dropped');
equal(nullRows.slice(0, 3), [1, 2, 3], 'non-null keys must come first in order');
equal(nullRows.slice(3), [null, null], 'NULL keys must come last by default');

// ---- 6. 没有索引时不得改变行为 ----
equal(run('CREATE TABLE v(id INT, z INT NOT NULL); INSERT INTO v VALUES(1,3),(2,1),(3,2);').success, true);
equal(rows('SELECT z FROM v ORDER BY z;').map(r => r[0]), [1, 2, 3],
      'a column without an index must still sort correctly');

// ---- 7. 多键排序不得套用单列索引顺序 ----
equal(rows('SELECT nn FROM t ORDER BY nn, id DESC;').map(r => r[0]), expectedAscending,
      'a multi-key sort must still produce the right order');

console.log(`${checks} index order scan checks passed`);

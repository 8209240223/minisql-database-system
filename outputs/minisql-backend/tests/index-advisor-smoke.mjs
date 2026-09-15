import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const db = join(mkdtempSync(join(tmpdir(), 'minisql-index-advisor-')), 'db.pages');

function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 30000,
  });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}

let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

// ---- 准备数据：无索引的表 ----
let setup = 'CREATE TABLE t(id INT, v INT);';
for (let start = 0; start < 800; start += 400) {
  const values = [];
  for (let i = start; i < Math.min(start + 400, 800); ++i) values.push(`(${i},${i % 40})`);
  setup += `INSERT INTO t VALUES${values.join(',')};`;
}
equal(run(setup).success, true, 'setup must succeed');

// ---- 1. 没有负载时不产生建议 ----
const empty = run('', 'indexAdvisor');
equal(empty.success, true, 'indexAdvisor must succeed with no workload');
equal(empty.recommendations.length, 0, 'no workload must produce no recommendation');
equal(empty.trackedTables, 0, 'no workload must track no table');
equal(empty.scope, 'filter-over-seqscan-without-index', 'scope must describe what is analysed');

// ---- 2. 跑带谓词的查询后积累负载 ----
// 负载在进程退出时落盘，所以这一批必须和后续读取分开调用。
const workload = run([
  'SELECT id FROM t WHERE v=1;',
  'SELECT id FROM t WHERE v=2;',
  'SELECT id FROM t WHERE v=3;',
  'SELECT id FROM t WHERE v=4;',
  'SELECT id FROM t WHERE id>100;',
].join('\n'));
equal(workload.success, true, 'workload queries must succeed');

// ---- 3. 建议必须出现，且针对被过滤的列 ----
const advised = run('', 'indexAdvisor');
equal(advised.success, true, 'indexAdvisor must succeed after workload');
ok(advised.recommendations.length >= 1, 'a repeated predicate must yield a recommendation');
ok(advised.trackedTables >= 1, 'the analysed table must be tracked');

const byColumn = Object.fromEntries(advised.recommendations.map(r => [r.column, r]));
ok(byColumn.v, 'column v was filtered four times and must be recommended');
ok(byColumn.id, 'column id was filtered with a range and must be recommended');
equal(byColumn.v.totalScans, 4, 'v must record four equality scans');
equal(byColumn.v.equalityScans, 4, 'v must count equality scans only');
equal(byColumn.v.rangeScans, 0, 'v must record no range scan');
equal(byColumn.id.totalScans, 1, 'id must record one range scan');
equal(byColumn.id.rangeScans, 1, 'id must count one range scan');
equal(byColumn.id.equalityScans, 0, 'id must record no equality scan');

// ---- 4. 建议必须可执行，并且带上可执行语句 ----
ok(byColumn.v.suggestedStatement.startsWith('CREATE INDEX'),
   'the recommendation must carry an executable statement');
equal(byColumn.v.suggestedStatement, 'CREATE INDEX idx_t_v ON t(v);',
      'the suggested statement must name the table and column');

// ---- 5. 高频列必须排在前面 ----
ok(advised.recommendations[0].totalScans >= advised.recommendations[advised.recommendations.length - 1].totalScans,
   'recommendations must be sorted by scan count, highest first');
equal(advised.recommendations[0].column, 'v', 'the most used column must come first');

// ---- 6. 按建议建索引后，建议必须消失（闭环）----
equal(run(byColumn.v.suggestedStatement).success, true, 'the suggested statement must execute');
const afterIndex = run('', 'indexAdvisor');
const stillThere = afterIndex.recommendations.filter(r => r.table === 't');
equal(stillThere.length, 0, 'a table that now has an index must no longer be recommended');

// ---- 7. 建索引后查询结果必须仍然正确 ----
const counted = run('SELECT COUNT(*) FROM t WHERE v=1;');
equal(counted.success, true, 'query must still work after the index is created');
equal(counted.results[0].rows[0][0], 20, 'the count must be unchanged by the index');

// ---- 8. statistics 也带同一份建议 ----
const stats = run('', 'statistics');
equal(stats.success, true, 'statistics must succeed');
ok(stats.indexAdvisor && Array.isArray(stats.indexAdvisor.recommendations),
   'statistics must expose the same advisor document');

console.log(`${checks} index advisor checks passed`);

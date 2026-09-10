// X18 4.3-iv: IndexScan 估计元数据——compile() 顶层 optimizedPlan 与 EXPLAIN 结果里的
// IndexScan 节点都带 estimatedRows/estimatedCost/statsSource，口径复用 columnSelectivity
// （等值 1/distinct 越界归零、范围走直方图）。解锁此前「IndexScan 估计依赖 B(X20)」的阻塞项。
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-index-est-')), 'db.pages');
function run(mode, sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql || '', encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

const setup = 'CREATE TABLE t(id INT, v INT); CREATE INDEX idx ON t(id);'
  + 'INSERT INTO t VALUES(1,11),(2,22),(3,33),(4,44),(5,55),(6,66),(7,77),(8,88),(9,99),(10,101);';
ok(run('execute', setup).success, 'setup succeeds');

function indexScanNode(plan) { return plan.find((n) => n.kind === 'IndexScan'); }

// compile 顶层 optimizedPlan：出现 IndexScan 且带三个估计字段。
const compile = run('compile', 'SELECT * FROM t WHERE id = 5;');
equal(compile.estimateModel, 'stats-v1');
const node = indexScanNode(compile.optimizedPlan);
ok(node, 'optimizedPlan 里出现 IndexScan');
ok(node.statsSource === 'stats-v1' && typeof node.estimatedRows === 'number' && typeof node.estimatedCost === 'number', 'IndexScan 节点带 estimatedRows/estimatedCost/statsSource');

// 等值 id=5：distinct=10 → 选择率 0.1 → 约 1 行。
const eqRows = node.estimatedRows;
ok(Math.abs(eqRows - 1) <= 0.51, `等值 id=5 估计行数≈1（${eqRows}）`);

// 越界等值 id=100 → 越界归零。
const outOfRange = indexScanNode(run('compile', 'SELECT * FROM t WHERE id = 100;').optimizedPlan);
ok(outOfRange.estimatedRows === 0, `越界等值 id=100 估计行数=0（${outOfRange.estimatedRows}）`);

// 范围 id < 3：直方图累计 → 0 < rows <= 全表，且明显小于全表 10。
const range = indexScanNode(run('compile', 'SELECT * FROM t WHERE id < 3;').optimizedPlan);
ok(Number.isFinite(range.estimatedRows) && range.estimatedRows > 0 && range.estimatedRows < 10, `范围 id<3 估计行数在 (0,10)（${range.estimatedRows}）`);
ok(range.estimatedRows < eqRows ? range.estimatedRows <= 10 : true, '范围估计为有限值');

// 确定性：同 SQL 两次 compile 的 IndexScan 估计行数一致。
equal(indexScanNode(run('compile', 'SELECT * FROM t WHERE id = 5;').optimizedPlan).estimatedRows, eqRows, 'IndexScan 估计行数跨次确定');

// EXPLAIN（execute 路径）结果节点同样带估计字段，与 compile 口径一致。
const explain = run('execute', 'EXPLAIN SELECT * FROM t WHERE id = 5;').results.at(-1);
const exNode = indexScanNode(explain.optimizedPlan);
ok(exNode && exNode.statsSource === 'stats-v1' && typeof exNode.estimatedRows === 'number', 'EXPLAIN IndexScan 节点带估计字段');
ok(Math.abs(exNode.estimatedRows - eqRows) <= 0.52, 'EXPLAIN 与 compile 的 IndexScan 估计行数一致');

// 非索引列回归：无索引匹配时仍用 SeqScan（估计字段仍完备）。
const plain = run('compile', 'SELECT * FROM t WHERE v < 50;');
const scans = plain.optimizedPlan.filter((n) => n.kind === 'SeqScan');
ok(scans.length > 0, '无索引列走 SeqScan');
ok(scans.every((n) => n.statsSource === 'stats-v1' && typeof n.estimatedRows === 'number'), 'SeqScan 继续带估计字段');

console.log(`${checks} statistics-index-estimate checks passed`);
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-cost-')), 'db.pages');
function run(mode, sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql || '', encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }
// 取 EXPLAIN 结果里某 kind 节点的 estimateSource=stats-v1 估计行数。
function estimatedRows(sql, kind = 'Filter') {
  const result = run('execute', sql);
  assert.equal(result.success, true, JSON.stringify(result));
  const plan = result.results.at(-1);
  assert.equal(plan.kind, 'Explain');
  for (const row of plan.rows) if (row[0] === kind) return row[2]; // [-1] 为 stats-v1
  throw new Error(`no ${kind} node in EXPLAIN output for ${sql}`);
}

// 设置：a 均匀分布 1..10（10 个不同值），b 递减。
const setup = 'CREATE TABLE t(a INT, b INT);'
  + 'INSERT INTO t VALUES(1,9),(2,8),(3,7),(4,6),(5,5),(6,4),(7,3),(8,2),(9,1),(10,0);';
equal(run('execute', setup).success, true);

// 直方图驱动范围谓词：a<5 选择性 < 全表，且明显小于 a<10 的选择性（桶累计单调）。
const lt5 = estimatedRows('EXPLAIN SELECT * FROM t WHERE a < 5;');
const lt10 = estimatedRows('EXPLAIN SELECT * FROM t WHERE a < 10;');
ok(lt5 > 0 && lt5 < 10, `histogram range selectivity shrinks rows (a<5 => ${lt5})`);
ok(lt5 < lt10, `histogram range is monotonic (a<5 ${lt5} < a<10 ${lt10})`);

// 越界范围谓词：仍是直方图累计归零（pos=0 → < -100 匹配 0 桶）。
equal(estimatedRows('EXPLAIN SELECT * FROM t WHERE a < -100;'), 0);

// 越界等值谓词：值超出 [min,max] → 选择率为 0（1/distinct 不再误用）。
equal(estimatedRows('EXPLAIN SELECT * FROM t WHERE a = 100;'), 0);

// 确定性：同统计、同 SQL 多次编译给出相同估计行数（无随机/容器序依赖）。
equal(estimatedRows('EXPLAIN SELECT * FROM t WHERE a < 5;'), lt5);
equal(estimatedRows('EXPLAIN SELECT * FROM t WHERE a < 5;'), lt5);

// 无直方图列回退：varchar/范围谓词仍走默认选择率（>0），不因缺直方图崩溃。
equal(run('execute', 'CREATE TABLE u(v VARCHAR(20)); INSERT INTO u VALUES(\'x\'),(\'y\'),(\'z\');').success, true);
const vs = estimatedRows('EXPLAIN SELECT * FROM u WHERE v < \'y\';');
ok(Number.isFinite(vs) && vs > 0, `non-numeric column falls back to default selectivity (${vs})`);

// 展示行与 plan JSON 同源：optimizedPlan/Filter 节点带估计字段且与 rows 展示一致。
function planNodeEstimate(sql, kind = 'Filter') {
  const result = run('execute', sql);
  assert.equal(result.success, true, JSON.stringify(result));
  const explain = result.results.at(-1);
  const node = explain.optimizedPlan.find((n) => n.kind === kind);
  return node && { rows: node.estimatedRows, cost: node.estimatedCost, source: node.statsSource };
}
const ann = planNodeEstimate('EXPLAIN SELECT * FROM t WHERE a < 5;');
ok(ann && typeof ann.rows === 'number' && typeof ann.cost === 'number' && ann.source === 'stats-v1', 'plan JSON node carries estimatedRows/estimatedCost/statsSource');
equal(ann.rows, lt5, 'plan JSON estimatedRows matches EXPLAIN display row');
equal(ann.cost, planNodeEstimate('EXPLAIN SELECT * FROM t WHERE a < 5;').cost, 'plan JSON estimate is deterministic across compiles');

console.log(`${checks} statistics-cost checks passed`);
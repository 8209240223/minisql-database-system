import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-stats-')), 'db.pages');
function run(mode, sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql || '', encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

// 设置：单表含数值列（int/float）与可空值。
const setup = 'CREATE TABLE s(id INT, score FLOAT, name VARCHAR(40));'
  + 'INSERT INTO s VALUES(1,1.0,\'a\'),(2,3.5,\'b\'),(3,7.25,\'c\'),(4,NULL,\'d\'),(5,11.0,\'e\'),(5,2.5,\'f\'),(1,9.0,\'g\');';
equal(run('execute', setup).success, true);

const stats = run('statistics', '');
equal(stats.success, true);
equal(stats.version, 'stats-v1-histogram');
ok(typeof stats.generatedAtMs === 'number' && stats.generatedAtMs > 0 && Number.isInteger(stats.generatedAtMs), 'statistics exposes generatedAtMs');

const table = stats.tables.find((t) => t.name === 's');
ok(table, 'table s present in statistics');
equal(table.rowCount, 7);

const id = table.columns.find((c) => c.name === 'id');
ok(id, 'column id present');
equal(id.type, 'int');
equal(id.distinctCount, 5); // 1,2,3,4,5
equal(id.nullCount, 0);
equal(id.min, 1);
equal(id.max, 5);
ok(id.histogram && id.histogram.bucketCount >= 1 && id.histogram.bucketCount <= 16, 'id histogram present with 1..16 buckets');
equal(id.histogram.buckets.length, id.histogram.bucketCount);
const idBucketSum = id.histogram.buckets.reduce((sum, c) => sum + c, 0);
equal(idBucketSum, id.distinctCount); // distinct-value histogram buckets partition all distinct values

const score = table.columns.find((c) => c.name === 'score');
ok(score, 'column score present');
ok(score.histogram, 'float column also has a histogram');
equal(score.nullCount, 1);

const name = table.columns.find((c) => c.name === 'name');
ok(name, 'column name present');
ok(!name.histogram, 'varchar column has no numeric histogram'); // 非数值列不产直方图

console.log(`${checks} statistics-histogram checks passed`);
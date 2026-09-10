import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-stats-')), 'db.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
equal(run('CREATE TABLE t(id INT,name VARCHAR,v FLOAT); INSERT INTO t VALUES(1,\'a\',1.5),(2,\'a\',NULL),(3,NULL,2.5); CREATE INDEX t_name_idx ON t(name);').success, true);
const stats = run('', 'statistics');
equal(stats.success, true);
equal(stats.statisticsVersion, 1);
assert.ok(Number.isInteger(stats.refreshedAt)); ++checks;
const table = stats.tables[0];
equal(table.name, 't');
equal(table.rowCount, 3);
assert.ok(table.allocatedPages > 0); ++checks;
const byName = Object.fromEntries(table.columns.map(column => [column.name, column]));
equal(byName.id.distinctCount, 3);
equal(byName.name.distinctCount, 1);
equal(byName.name.nullCount, 1);
equal(Math.abs(byName.name.nullRatio - 1 / 3) < 1e-12, true);
equal(byName.v.nullCount, 1);
equal(byName.v.distinctCount, 2);
equal(byName.id.minValue, 1);
equal(byName.id.maxValue, 3);
equal(byName.name.histogram[0].value, 'a');
equal(byName.name.histogram[0].count, 2);
assert.ok(Array.isArray(byName.id.valueHistogram) && byName.id.valueHistogram.length >= 1); ++checks;
assert.ok(Array.isArray(table.indexes) && table.indexes.length === 1); ++checks;
equal(table.indexes[0].name, 't_name_idx');
assert.ok(table.indexes[0].entries >= 1); ++checks;
console.log(`${checks} statistics checks passed`);

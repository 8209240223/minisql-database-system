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
equal(run('CREATE TABLE t(id INT,name VARCHAR,v FLOAT); INSERT INTO t VALUES(1,\'a\',1.5),(2,\'a\',NULL),(3,NULL,2.5);').success, true);
const stats = run('', 'statistics');
equal(stats.success, true);
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
console.log(`${checks} statistics checks passed`);

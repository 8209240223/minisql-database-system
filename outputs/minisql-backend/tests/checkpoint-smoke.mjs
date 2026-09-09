import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, extname } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-checkpoint-')), 'db.pages');
function run(sql) {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
equal(run('CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2); CHECKPOINT;').success, true);
const wal = db.slice(0, -extname(db).length) + '.wal';
assert.ok(!existsSync(wal) || statSync(wal).size === 0, 'checkpoint leaves no pending WAL'); ++checks;
equal(run('SELECT * FROM t ORDER BY id;').results[0].rows, [[1],[2]]);
const transaction = run('BEGIN; CHECKPOINT; ROLLBACK;');
equal(transaction.success, false);
equal(transaction.error.code, 6001);
equal(transaction.transactionState, 'IDLE');
console.log(`${checks} checkpoint checks passed`);

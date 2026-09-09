import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const database = join(mkdtempSync(join(tmpdir(), 'minisql-result-budget-')), 'database.pages');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;
function run(sql) {
  const child = spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 30000,
    env: { ...process.env, MINISQL_MAX_RESULT_ROWS: '2' },
  });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }

equal(run('CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2),(3);').success, true);
const rejected = run('SELECT id FROM t ORDER BY id;');
equal(rejected.success, false);
equal(rejected.error.code, 5001);
equal(rejected.results.at(-1), undefined);
equal(run('SELECT id FROM t ORDER BY id LIMIT 2;').results.at(-1).rows, [[1],[2]]);
equal(run('SELECT COUNT(*) FROM t;').results.at(-1).rows, [[3]]);
equal(run('SELECT id FROM t ORDER BY id DESC LIMIT 1;').results.at(-1).rows, [[3]]);
console.log(`${checks} result budget checks passed`);

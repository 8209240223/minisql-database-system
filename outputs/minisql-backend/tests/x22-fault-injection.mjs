import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-x22-fault-'));
let checks = 0;
const baseEnv = { ...process.env };
delete baseEnv.MINISQL_CRASH_AT;

function run(database, sql, extraEnv = {}) {
  return spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 10000,
    env: { ...baseEnv, ...extraEnv },
  });
}

function response(database, sql) {
  const result = run(database, sql);
  assert.equal(result.status, 0, result.stderr);
  ++checks;
  return JSON.parse(result.stdout.trim());
}

for (const stage of ['prepared', 'published', 'applied-page', 'data-synced', 'checkpointed']) {
  const database = join(root, `${stage}.pages`);
  assert.equal(run(database, 'CREATE TABLE t(id INT);').status, 0);
  ++checks;
  const interrupted = run(database, 'INSERT INTO t VALUES(1);', { MINISQL_CRASH_AT: stage });
  assert.equal(interrupted.status, 77, `${stage}: ${interrupted.stderr}`);
  ++checks;
  const expectedRows = stage === 'prepared' ? [] : [[1]];
  equalRows(response(database, 'SELECT * FROM t;'), expectedRows, stage);
  equalRows(response(database, 'SELECT * FROM t;'), expectedRows, `${stage}-repeat`);
}

function equalRows(result, expected, stage) {
  assert.equal(result.success, true, `${stage}: ${JSON.stringify(result)}`);
  assert.deepEqual(result.results[0].rows, expected, stage);
  ++checks;
}

console.log(`${checks} X22 fault-injection checks passed: prepared, published, applied-page, data-synced and checkpointed`);

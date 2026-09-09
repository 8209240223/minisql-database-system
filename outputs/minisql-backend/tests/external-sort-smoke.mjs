import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-external-sort-'));
const database = join(root, 'database.pages');
const sortDirectory = join(root, 'sort-runs');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;

function run(sql, sortRows = '10000') {
  const child = spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 30000,
    env: { ...process.env, MINISQL_SORT_MEMORY_ROWS: sortRows, MINISQL_TEMP_DIR: sortDirectory },
  });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}

function query(sql, sortRows = '10000') {
  const response = run(sql, sortRows);
  assert.equal(response.success, true, JSON.stringify(response));
  ++checks;
  return response.results.at(-1);
}

function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

equal(run(`
CREATE TABLE s(id INT,k INT,n INT);
INSERT INTO s VALUES
  (1,5,NULL),(2,1,7),(3,3,NULL),(4,2,9),(5,1,8),
  (6,4,NULL),(7,3,6),(8,2,5),(9,5,4),(10,1,3),
  (11,NULL,2),(12,4,1),(13,NULL,NULL);
`).success, true);

const ascending = [
  [10,1,3],[2,1,7],[5,1,8],
  [8,2,5],[4,2,9],
  [7,3,6],[3,3,null],
  [12,4,1],[6,4,null],
  [9,5,4],[1,5,null],
  [11,null,2],[13,null,null],
];
const descending = [
  [11,null,2],[13,null,null],
  [9,5,4],[1,5,null],
  [12,4,1],[6,4,null],
  [7,3,6],[3,3,null],
  [4,2,9],[8,2,5],
  [5,1,8],[2,1,7],[10,1,3],
];

const externalAscending = query('SELECT id,k,n FROM s ORDER BY k ASC NULLS LAST,n ASC NULLS LAST,id ASC;', '4');
const memoryAscending = query('SELECT id,k,n FROM s ORDER BY k ASC NULLS LAST,n ASC NULLS LAST,id ASC;', '100');
equal(externalAscending.rows, ascending);
equal(memoryAscending.rows, ascending);
assert.deepEqual(externalAscending.rows, memoryAscending.rows);
++checks;

const externalDescending = query('SELECT id,k,n FROM s ORDER BY k DESC NULLS FIRST,n DESC NULLS LAST,id ASC;', '4');
const memoryDescending = query('SELECT id,k,n FROM s ORDER BY k DESC NULLS FIRST,n DESC NULLS LAST,id ASC;', '100');
equal(externalDescending.rows, descending);
equal(memoryDescending.rows, descending);
assert.deepEqual(externalDescending.rows, memoryDescending.rows);
++checks;

assert.ok(existsSync(sortDirectory), 'external sort should create its configured temporary directory');
++checks;
const leftoverRuns = readdirSync(sortDirectory).filter(name => name.endsWith('.jsonl') || name.endsWith('.meta.json'));
equal(leftoverRuns, []);

console.log(`${checks} external sort checks passed`);

import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-external-aggregate-'));
const database = join(root, 'database.pages');
const sortDirectory = join(root, 'aggregate-runs');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;

function run(sql, aggregateRows = '10000') {
  const child = spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 30000,
    env: {
      ...process.env,
      MINISQL_AGGREGATE_MEMORY_ROWS: aggregateRows,
      MINISQL_SORT_MEMORY_ROWS: aggregateRows,
      MINISQL_TEMP_DIR: sortDirectory,
    },
  });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}

function query(sql, aggregateRows = '10000') {
  const response = run(sql, aggregateRows);
  assert.equal(response.success, true, JSON.stringify(response));
  ++checks;
  return response.results.at(-1);
}

function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

equal(run(`
CREATE TABLE a(id INT,k INT,n INT);
INSERT INTO a VALUES
  (1,1,10),(2,1,NULL),(3,2,5),(4,2,15),
  (5,NULL,7),(6,3,20),(7,3,20),(8,NULL,NULL),
  (9,4,-5),(10,4,5),(11,5,100),(12,5,NULL);
`).success, true);

const sql = 'SELECT k,COUNT(*),SUM(n),MIN(n),MAX(n),AVG(n) FROM a GROUP BY k ORDER BY k NULLS LAST;';
const expected = [
  [1,2,10,10,10,'10.000000'],
  [2,2,20,5,15,'10.000000'],
  [3,2,40,20,20,'20.000000'],
  [4,2,0,-5,5,'0.000000'],
  [5,2,100,100,100,'100.000000'],
  [null,2,7,7,7,'7.000000'],
];

const external = query(sql, '4');
const memory = query(sql, '100');
equal(external.rows, expected);
equal(memory.rows, expected);
assert.deepEqual(external.rows, memory.rows);
++checks;

assert.ok(existsSync(sortDirectory), 'external aggregate should create its configured temporary directory');
++checks;
const leftovers = readdirSync(sortDirectory).filter(name => name.endsWith('.jsonl') || name.endsWith('.meta.json'));
equal(leftovers, []);

console.log(`${checks} external aggregate checks passed`);

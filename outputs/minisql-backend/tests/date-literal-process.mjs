import assert from 'node:assert/strict';
import { existsSync, mkdtempSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const root = mkdtempSync(fileURLToPath(new URL('./artifacts/date-literal-', import.meta.url)));
const file = join(root, 'database.pages');
const release = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const fallback = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(release) ? release : fallback);
let checks = 0;

function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [file, mode], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 10000,
    maxBuffer: 16 * 1024 * 1024,
  });
  if (child.error) throw child.error;
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}

function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

const setup = run(`
  CREATE TABLE dated_rows(
    id INT PRIMARY KEY,
    event_day DATE,
    code VARCHAR(32) NOT NULL,
    amount DECIMAL(10,2) NOT NULL
  );
  INSERT INTO dated_rows VALUES
    (1, DATE '2024-06-30', 'first', 10.00),
    (2, DATE '1999-12-31', 'second', 20.00),
    (3, NULL, 'third', 30.00);
  CREATE INDEX dated_rows_day ON dated_rows(event_day);
  CREATE INDEX dated_rows_code ON dated_rows(code);
  CREATE INDEX dated_rows_amount ON dated_rows(amount);
`);
equal(setup.success, true);

const compiled = run("UPDATE dated_rows SET code='' WHERE (id!=2 OR event_day=DATE '2024-06-30');", 'compile');
equal(compiled.success, true);
const filter = compiled.optimizedPlan.find(node => node.kind === 'Filter');
assert.equal(filter.predicate.right.right.type, 'date');
++checks;

const updatedCodes = run("UPDATE dated_rows SET code='' WHERE (id!=2 OR event_day=DATE '2024-06-30');");
equal(updatedCodes.success, true);
equal(updatedCodes.results[0].affectedRows, 2);

const updatedAmount = run("UPDATE dated_rows SET amount=0.00 WHERE event_day=DATE '1999-12-31';");
equal(updatedAmount.success, true);
equal(updatedAmount.results[0].affectedRows, 1);

const selected = run('SELECT id,event_day,code,amount FROM dated_rows ORDER BY id;');
equal(selected.success, true);
equal(selected.results[0].rows, [
  [1, '2024-06-30', '', '10.00'],
  [2, '1999-12-31', 'second', '0.00'],
  [3, null, '', '30.00'],
]);

const boolSetup = run("CREATE TABLE bool_rows(id INT PRIMARY KEY, enabled BOOL NOT NULL); INSERT INTO bool_rows VALUES(1,FALSE),(2,TRUE);");
equal(boolSetup.success, true);
const boolCompiled = run('SELECT id FROM bool_rows WHERE enabled!=TRUE;', 'compile');
equal(boolCompiled.success, true);
const boolFilter = boolCompiled.optimizedPlan.find(node => node.kind === 'Filter');
assert.equal(boolFilter.predicate.right.type, 'bool');
++checks;
const boolSelected = run('SELECT id FROM bool_rows WHERE enabled!=TRUE ORDER BY id;');
equal(boolSelected.results[0].rows, [[1]]);

console.log(`${checks} DATE literal planner/index UPDATE checks passed`);

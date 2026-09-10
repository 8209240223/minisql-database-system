import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/insert-columns-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [join(directory, 'database.pages'), mode], sql);
  assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function rows() { const result = run('SELECT * FROM t ORDER BY id;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run('CREATE TABLE t(id INT NOT NULL,n BIGINT,label VARCHAR);').success, true);
equal(run("INSERT INTO t VALUES(1,CAST('9223372036854775807' AS BIGINT),'first');").success, true);
equal(run('INSERT INTO t(id) VALUES(2+1);').success, true);
equal(run("INSERT INTO t(label,ID) VALUES('second',2);").success, true);
equal(rows(), [[1, '9223372036854775807', 'first'], [2, null, 'second'], [3, null, null]]);
for (const sql of [
  'INSERT INTO t VALUES(4);', "INSERT INTO t VALUES(4,4,'four',4);",
  "INSERT INTO t(label) VALUES('missing');", 'INSERT INTO t(id,ID) VALUES(4,4);',
  'INSERT INTO t(missing) VALUES(4);', 'INSERT INTO t() VALUES();',
  'INSERT INTO t VALUES();', 'INSERT INTO t(id) VALUES(NULL);',
  'INSERT INTO t(id) VALUES(CAST(NULL AS INT));',
]) {
  const before = rows();equal(run(sql).success, false);equal(rows(), before);
}
const compiled = run('INSERT INTO t(id) VALUES(5);', 'compile');
equal(compiled.success, true);
equal(compiled.plan[0].columnMapping, [0]);
equal(compiled.plan[0].values, [5, null, null]);
equal(compiled.optimizedPlan[0].insertExpressions.map(value => value.value), [5, null, null]);
const positional = run("INSERT INTO t VALUES(5,6,'five');", 'compile');
equal(positional.plan[0].columnMapping, [0, 1, 2]);
equal(positional.ast.names, []);
equal(rows().length, 3);
equal(run('CREATE TABLE nullable_only(a INT,b VARCHAR); INSERT INTO nullable_only(b) VALUES(NULL);').success, true);
equal(run('SELECT * FROM nullable_only;').results[0].rows, [[null, null]]);
console.log(`${checks} INSERT column/default-null checks passed across fresh processes`);

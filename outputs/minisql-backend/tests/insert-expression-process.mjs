import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/insert-expression-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [join(directory, 'database.pages'), mode], sql);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
function rows() { const result = run('SELECT i,b,s FROM t ORDER BY i;');assert.equal(result.success, true);return result.results[0].rows; }
equal(run('CREATE TABLE t(i INT NOT NULL,b BIGINT,s VARCHAR);').success, true);
equal(run("INSERT INTO t(s,i,b) VALUES(CAST(9223372036854775807 AS VARCHAR),CAST('42' AS INT),CAST('9007199254740993' AS BIGINT));").success, true);
equal(rows(), [[42, '9007199254740993', '9223372036854775807']]);
equal(run("INSERT INTO t(i,b,s) VALUES(1+2*3,CAST(2147483647 AS BIGINT)+1,CAST(NULL AS VARCHAR));").success, true);
equal(rows(), [[7, 2147483648, null], [42, '9007199254740993', '9223372036854775807']]);
for (const [sql, code] of [
  ["INSERT INTO t(i,b,s) VALUES(CAST('bad' AS INT),1,'x');", 5001],
  ["INSERT INTO t(i,b,s) VALUES(8,9223372036854775807+1,'x');", 5001],
  ["INSERT INTO t(i,b,s) VALUES(8,1/0,'x');", 5001],
  ["INSERT INTO t(i,b,s) VALUES(CAST(NULL AS INT),1,'x');", 5001],
  ["INSERT INTO t(i,b,s) VALUES(i+1,1,'x');", 2003],
  ["INSERT INTO t(i,b,s) VALUES(t.i+1,1,'x');", 2003],
  ["INSERT INTO t(i,b,s) VALUES(CAST('2147483648' AS BIGINT),1,'x');", 2003],
  ["INSERT INTO t(i,b,s) VALUES(8,CAST('1' AS VARCHAR),'x');", 2003],
  ["INSERT INTO t(i,b,s) VALUES(8,1);", 2003],
  ["INSERT INTO t(i,i,s) VALUES(8,1,'x');", 2003],
]) {
  const before = rows();
  const result = run(sql);
  equal(result.success, false);equal(result.error.code, code);equal(rows(), before);
}
const compiled = run("INSERT INTO t(i,b,s) VALUES(2+3,CAST('1' AS BIGINT),'five');", 'compile');
equal(compiled.success, true);
equal(compiled.ast.valueExpressions[1].kind, 'Cast');
equal(compiled.optimizedPlan[0].insertExpressions[0].value, 5);
equal(compiled.optimizedPlan[0].insertExpressions[1].type, 'bigint');
equal(compiled.optimizedPlan[0].columnMapping, [0, 1, 2]);
equal(rows().length, 2);
const batch = run("INSERT INTO t(i,b,s) VALUES(CAST('9' AS INT),9,'nine'); INSERT INTO t(i,b,s) VALUES(CAST('bad' AS INT),10,'bad');");
equal(batch.success, false);equal(batch.completedStatements, 1);equal(rows().length, 3);
equal(rows()[1], [9, 9, 'nine']);
console.log(`${checks} INSERT expression checks passed: casts, arithmetic, mapping, no write on evaluation failure and cross-process persistence`);

import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/cast-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [join(directory, 'database.pages'), mode], sql);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
function query(expression, suffix = '') {
  const result = run(`SELECT ${expression} FROM t ${suffix};`);
  assert.equal(result.success, true, JSON.stringify(result));
  return result.results.at(-1);
}
equal(run("CREATE TABLE t(i INT,b BIGINT,s VARCHAR); INSERT INTO t(i,b,s) VALUES(1,9223372036854775807,'42');").success, true);
equal(query('CAST(s AS INT),CAST(i AS BIGINT),CAST(b AS VARCHAR)').rows, [[42, 1, '9223372036854775807']]);
equal(query('CAST(s AS INT),CAST(i AS BIGINT),CAST(b AS VARCHAR)').columnTypes, ['int', 'bigint', 'varchar']);
equal(query("CAST('-9223372036854775808' AS BIGINT)").rows, [['-9223372036854775808']]);
equal(query("CAST('+2147483647' AS INT),CAST('-2147483648' AS INT)").rows, [[2147483647, -2147483648]]);
equal(query("CAST('00042' AS INT),CAST('-0' AS BIGINT)").rows, [[42, 0]]);
equal(query('CAST(NULL AS INT),CAST(NULL AS BIGINT),CAST(NULL AS VARCHAR)').rows, [[null, null, null]]);
equal(query('CAST(CAST(b AS VARCHAR) AS BIGINT)').rows, [['9223372036854775807']]);
equal(query("CAST(i AS BIGINT)+2147483647").rows, [[2147483648]]);
equal(query("CAST('abc' AS VARCHAR)").rows, [['abc']]);
for (const text of ['', ' ', ' 1', '1 ', '12abc', '1.0', '1e2', '+', '-', '+-1', '++1', '--1', '9223372036854775808', '-9223372036854775809']) {
  const result = run(`SELECT CAST('${text}' AS BIGINT) FROM t;`);
  equal(result.success, false);
  equal(result.error.code, 5001);
  equal(result.error.line, 1);
  equal(result.error.column, 8);
}
equal(run('SELECT CAST(b AS INT) FROM t;').success, false);
equal(run('SELECT CAST(TRUE AS INT) FROM t;').error.code, 2003);
equal(run('SELECT CAST(i AS DECIMAL) FROM t;').error.code, 2002);
equal(run('SELECT CAST(i INT) FROM t;').error.code, 2002);
equal(query("i", "WHERE FALSE AND CAST('bad' AS INT)=1").rows, []);
equal(query("i", "WHERE TRUE OR CAST('bad' AS INT)=1").rows, [[1]]);
equal(query("CAST('bad' AS INT)", 'LIMIT 0').rows, []);
equal(run("UPDATE t SET i=CAST(s AS INT),s=CAST(b AS VARCHAR);").success, true);
equal(query('i,s').rows, [[42, '9223372036854775807']]);
equal(run('UPDATE t SET i=CAST(b AS INT),s=\'changed\';').success, false);
equal(query('i,s').rows, [[42, '9223372036854775807']]);
const compiled = run('SELECT CAST(i+1 AS BIGINT) FROM t;', 'compile');
equal(compiled.success, true);
equal(compiled.ast.selectItems[0].expression.kind, 'Cast');
equal(compiled.optimizedPlan[0].projections[0].type, 'bigint');
equal(run(`SELECT ${'CAST('.repeat(260)}i${' AS INT)'.repeat(260)} FROM t;`).error.code, 2002);
console.log(`${checks} CAST checks passed: exact conversions, null, errors, short circuit, persistence and AST`);

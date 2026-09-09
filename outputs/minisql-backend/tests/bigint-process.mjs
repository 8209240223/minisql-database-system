import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const dir = mkdtempSync(fileURLToPath(new URL('./artifacts/bigint-', import.meta.url)));
const exe = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(a, b) { assert.deepEqual(a, b); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(exe, [join(dir, 'database.pages'), mode], sql);
  assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function query(sql) { const result = run(sql);assert.equal(result.success, true, JSON.stringify(result));return result.results.at(-1); }
equal(run('CREATE TABLE t(id INT,b BIGINT,n BIGINT NOT NULL);').success, true);
equal(run('INSERT INTO t(id,b,n) VALUES(1,9223372036854775807,-9223372036854775808);').success, true);
equal(run('INSERT INTO t(id,b,n) VALUES(2,9007199254740993,2); INSERT INTO t(id,b,n) VALUES(3,NULL,3);').success, true);
equal(query('SELECT b,n FROM t WHERE id=1;').rows, [['9223372036854775807', '-9223372036854775808']]);
equal(query('SELECT b FROM t ORDER BY b;').rows, [['9007199254740993'], ['9223372036854775807'], [null]]);
equal(query('SELECT b FROM t WHERE b>9007199254740992;').rows.length, 2);
equal(query('SELECT b-1,n+1 FROM t WHERE id=1;').rows, [['9223372036854775806', '-9223372036854775807']]);
equal(query('SELECT 9223372036854775807-1 FROM t WHERE id=1;').rows, [['9223372036854775806']]);
equal(query('SELECT b+id FROM t WHERE id=2;').rows, [['9007199254740995']]);
equal(query('SELECT n/2 FROM t WHERE id=1;').rows, [['-4611686018427387904']]);
equal(query('SELECT b+1 FROM t WHERE id=3;').rows, [[null]]);
for (const sql of ['SELECT b+1 FROM t WHERE id=1;', 'SELECT n/-1 FROM t WHERE id=1;', 'SELECT -n FROM t WHERE id=1;', 'SELECT b*b FROM t WHERE id=2;', 'SELECT 9223372036854775808 FROM t;', 'SELECT -9223372036854775809 FROM t;', 'UPDATE t SET id=b;', 'INSERT INTO t(id,b,n) VALUES(2147483648,1,1);', "INSERT INTO t(id,b,n) VALUES(4,'4',4);", 'UPDATE t SET n=NULL;']) equal(run(sql).success, false);
equal(run('UPDATE t SET b=id+1 WHERE id=3;').results[0].affectedRows, 1);
equal(query('SELECT b FROM t WHERE id=3;').rows, [[4]]);
equal(query('SELECT b FROM t WHERE id=3;').columnTypes, ['bigint']);
const compiled = run('SELECT 9223372036854775807-1 FROM t;', 'compile');
equal(compiled.optimizedPlan[0].projections[0].value, '9223372036854775806');
equal(compiled.optimizedPlan[0].projections[0].type, 'bigint');
equal(compiled.integerEncoding, 'safe-number-or-decimal-string');
equal(query('SELECT x.b,y.n FROM t x JOIN t y ON x.b=y.n WHERE x.id=3;').rows, []);
equal(query('SELECT DISTINCT b FROM t ORDER BY b DESC;').rows, [['9223372036854775807'], ['9007199254740993'], [4]]);
equal(run('UPDATE t SET b=b+1;').success, false);
equal(query('SELECT b FROM t WHERE id=1;').rows, [['9223372036854775807']]);
equal(query('SELECT 9007199254740991,9007199254740992,-9007199254740991,-9007199254740992 FROM t WHERE id=1;').rows,
  [[9007199254740991, '9007199254740992', -9007199254740991, '-9007199254740992']]);
equal(query('SELECT b FROM t WHERE FALSE;').columnTypes, ['bigint']);
equal(query('SELECT x.b,y.b FROM t x JOIN t y ON x.b=y.b WHERE x.id=2;').rows,
  [['9007199254740993', '9007199254740993']]);
equal(query('SELECT x.b,y.b FROM t x LEFT JOIN t y ON FALSE WHERE x.id=2;').rows,
  [['9007199254740993', null]]);
console.log(`${checks} BIGINT SQL, precision, type and cross-process persistence checks passed`);

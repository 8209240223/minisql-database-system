import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
import { DatabaseSync } from 'node:sqlite';

const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/aggregate-',import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
const file = join(directory,'database.pages');
const reference = new DatabaseSync(':memory:');
let checks=0;
function equal(actual,expected){assert.deepEqual(actual,expected);++checks;}
function referenceQuery(sql){const statement=reference.prepare(sql);const columns=statement.columns().map(column=>column.name);statement.setReturnArrays(true);return {columns,values:statement.all()};}
function run(source){const result=invoke(executable,[file,'execute'],source);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(source){const result=run(source);assert.equal(result.success,true,JSON.stringify(result));++checks;return result.results.at(-1).rows;}
const fixture="CREATE TABLE t(id INT,v INT,g VARCHAR); CREATE TABLE r(id INT,v INT); CREATE TABLE empty_t(id INT,v INT,g VARCHAR);"+
 "INSERT INTO t VALUES(1,10,'a'),(2,NULL,'a'),(3,20,'b'),(4,-5,'b'),(5,NULL,NULL),(6,NULL,NULL);"+
 'INSERT INTO r VALUES(1,2),(1,3),(3,NULL);';
equal(run(fixture).success,true);reference.exec(fixture);
const queries=[
 'SELECT COUNT(*),COUNT(v),SUM(v),MIN(v),MAX(v) FROM t;',
 'SELECT g,COUNT(*),COUNT(v),SUM(v),MIN(v),MAX(v) FROM t GROUP BY g ORDER BY g NULLS LAST;',
 'SELECT g,v,COUNT(*) FROM t GROUP BY g,v ORDER BY g NULLS LAST,v NULLS LAST;',
 'SELECT g,COUNT(*) FROM t WHERE id<5 GROUP BY g HAVING SUM(v)>12 ORDER BY COUNT(*) DESC,g NULLS LAST;',
 'SELECT COUNT(*),COUNT(v),SUM(v),MIN(v),MAX(v) FROM empty_t;',
 'SELECT g,COUNT(*) FROM empty_t GROUP BY g;',
 'SELECT COUNT(*),SUM(v) FROM t WHERE FALSE;',
 'SELECT g,COUNT(*) FROM t WHERE FALSE GROUP BY g;',
 'SELECT COUNT(*) FROM empty_t HAVING COUNT(*)=0;',
 'SELECT COUNT(*) FROM empty_t HAVING COUNT(*)>0;',
 'SELECT 7 FROM empty_t GROUP BY id;',
 'SELECT 7,COUNT(*) FROM empty_t HAVING TRUE;',
 'SELECT g FROM t GROUP BY g ORDER BY SUM(v) DESC NULLS FIRST,g NULLS LAST;',
 'SELECT g,COUNT(*) AS n FROM t GROUP BY g ORDER BY n,g NULLS LAST LIMIT 1 OFFSET 1;',
 'SELECT g,COUNT(*) FROM t GROUP BY g HAVING SUM(v) IS NULL ORDER BY g NULLS LAST;',
 'SELECT COUNT(NULL),SUM(NULL),MIN(NULL),MAX(NULL) FROM t;',
 'SELECT COUNT(1/0),SUM(1/0) FROM empty_t;',
 'SELECT x.g,COUNT(y.v),SUM(y.v) FROM t x LEFT JOIN r y ON x.id=y.id GROUP BY x.g ORDER BY x.g NULLS LAST;',
 'SELECT x.id,COUNT(*),MIN(y.v) FROM t x JOIN r y ON x.id=y.id GROUP BY x.id ORDER BY x.id;',
 'SELECT id+1,COUNT(*) FROM t GROUP BY id ORDER BY id;',
 'SELECT (id+1)*2,COUNT(*) FROM t GROUP BY id+1 ORDER BY id+1;',
 'SELECT DISTINCT COUNT(*) FROM t GROUP BY g ORDER BY COUNT(*);',
 'SELECT COUNT(*),SUM(v) FROM t WHERE id>0 HAVING SUM(v)>0;',
];
for(const source of queries){const expected=referenceQuery(source);equal(query(source),expected.values);}
equal(query('SELECT v>0,COUNT(*) FROM t GROUP BY v>0 ORDER BY v>0;'),[[false,1],[true,2],[null,3]]);
equal(query('SELECT MIN(v>0),MAX(v>0),COUNT(v>0) FROM t;'),[[false,true,3]]);
equal(query("SELECT MIN(g),MAX(g) FROM t;"),[['a','b']]);
equal(query('SELECT COUNT(*) FROM t HAVING NULL;'),[]);
equal(query('SELECT 1 FROM empty_t HAVING TRUE;'),[[1]]);
equal(query('SELECT 1 FROM empty_t HAVING FALSE;'),[]);
equal(query('SELECT COUNT(*) FROM t GROUP BY NULL;'),[[6]]);
equal(query('SELECT COUNT(*) FROM empty_t GROUP BY NULL;'),[]);
equal(query('SELECT COUNT(*) FROM t WHERE FALSE GROUP BY 1/0;'),[]);
equal(query('SELECT * FROM t GROUP BY g,id,v ORDER BY id;'),query('SELECT * FROM t ORDER BY id;'));
equal(run('CREATE TABLE big(v BIGINT); INSERT INTO big VALUES(9007199254740993),(1);').success,true);
equal(query('SELECT COUNT(*),SUM(v),MIN(v),MAX(v) FROM big;'),[[2,'9007199254740994',1,'9007199254740993']]);
equal(run('DELETE FROM big; INSERT INTO big VALUES(9223372036854775807),(1);').success,true);
for(const source of ['SELECT SUM(v) FROM big;', 'SELECT SUM(1/0) FROM t;', 'SELECT COUNT(*) FROM t GROUP BY 1/0;', 'SELECT COUNT(*) FROM t HAVING COUNT(*)/0>0;']){
 const before=readFileSync(file);const result=run(source);equal(result.success,false);equal(result.error.code,5001);equal(readFileSync(file),before);
}
const before=readFileSync(file);
const failed=run("BEGIN; INSERT INTO t VALUES(9,9,'x'); SELECT SUM(v) FROM big; COMMIT;");
equal(failed.success,false);equal(failed.error.code,5001);equal(readFileSync(file),before);
equal(query('SELECT COUNT(*) FROM t;'),[[6]]);
const transaction=run("BEGIN; INSERT INTO t VALUES(9,9,'x'); SELECT COUNT(*),SUM(v) FROM t; ROLLBACK; SELECT COUNT(*),SUM(v) FROM t;");
equal(transaction.success,true);equal(transaction.results[2].rows,[[7,34]]);equal(transaction.results[4].rows,[[6,25]]);
equal(transaction.results[2].commitState,'rolledBack');
equal(query('SELECT COUNT(*),SUM(v) FROM t;'),[[6,25]]);
reference.close();
console.log(`${checks} aggregate execution checks passed, including ${queries.length} SQLite differential queries; exact AVG has a separate suite`);

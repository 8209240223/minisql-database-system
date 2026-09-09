import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/date-column-',import.meta.url)));
const file=join(directory,'database.pages');
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(sql,mode='execute'){const result=invoke(executable,[file,mode],sql);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(sql){const result=run(sql);equal(result.success,true);return result.results.at(-1).rows;}
equal(run("CREATE TABLE dates(id INT PRIMARY KEY,d DATE,required DATE NOT NULL DEFAULT DATE '2000-02-29');").success,true);
equal(run("INSERT INTO dates(id,d) VALUES(1,DATE '1999-12-31'),(2,CAST('2000-01-01' AS DATE)),(3,NULL);").success,true);
equal(query('SELECT * FROM dates ORDER BY id;'),[[1,'1999-12-31','2000-02-29'],[2,'2000-01-01','2000-02-29'],[3,null,'2000-02-29']]);
equal(query("SELECT id FROM dates WHERE d<DATE '2000-01-01';"),[[1]]);
equal(query('SELECT d FROM dates ORDER BY d DESC NULLS FIRST;'),[[null],['2000-01-01'],['1999-12-31']]);
equal(query('SELECT COUNT(d),MIN(d),MAX(d) FROM dates;'),[[2,'1999-12-31','2000-01-01']]);
equal(query('SELECT required,COUNT(*) FROM dates GROUP BY required HAVING COUNT(*)>1;'),[['2000-02-29',3]]);
equal(query("SELECT date '2000-02-29',COUNT(*) FROM dates GROUP BY DATE '2000-02-29';"),[['2000-02-29',3]]);
equal(query('SELECT CAST(CAST(d AS VARCHAR) AS DATE),CAST(NULL AS DATE) FROM dates ORDER BY id;'),[['1999-12-31',null],['2000-01-01',null],[null,null]]);
equal(run("UPDATE dates SET d=DATE '2024-02-29',required=DEFAULT WHERE id=1;").success,true);
equal(query('SELECT d FROM dates WHERE id=1;'),[['2024-02-29']]);
for(const text of ['0001-01-01','9999-12-31','1900-02-28','2000-02-29','2024-02-29','1970-01-01']) {
  equal(query(`SELECT DATE '${text}',CAST('${text}' AS DATE) FROM dates WHERE id=1;`),[[text,text]]);
}
for(const text of ['0000-01-01','10000-01-01','1900-02-29','2100-02-29','2023-02-29','2024-04-31','2024-00-01','2024-13-01','2024-01-00','2024-01-32','2024-1-01','2024/01/01',' 2024-01-01','2024-01-01 ','2024-01-01T00:00:00','']) {
  equal(run(`SELECT DATE '${text}' FROM dates;`).error.code,2003);
  equal(run(`SELECT CAST('${text}' AS DATE) FROM dates;`).error.code,5001);
}
for(const sql of ["INSERT INTO dates(id,d) VALUES(4,'2024-01-01');",'UPDATE dates SET d=1;',
  'SELECT d+1 FROM dates;','SELECT SUM(d) FROM dates;','SELECT AVG(d) FROM dates;',
  "SELECT d='2024-02-29' FROM dates;",'SELECT CAST(d AS INT) FROM dates;','SELECT CAST(TRUE AS DATE) FROM dates;',
  'SELECT CAST(1 AS DATE) FROM dates;','SELECT CAST(d AS BOOL) FROM dates;',
  "CREATE TABLE bad(d DATE DEFAULT '2024-01-01');","CREATE TABLE bad(d DATE DEFAULT DATE '2023-02-29');"]){
  const before=readFileSync(file);equal(run(sql).error.code,2003);equal(readFileSync(file),before);
}
equal(run("CREATE TABLE p(d DATE PRIMARY KEY); INSERT INTO p VALUES(DATE '0001-01-01'),(DATE '9999-12-31'); CREATE TABLE c(d DATE REFERENCES p(d)); INSERT INTO c VALUES(DATE '0001-01-01'),(NULL);").success,true);
equal(query('SELECT d FROM p ORDER BY d;'),[['0001-01-01'],['9999-12-31']]);
equal(run("INSERT INTO p VALUES(CAST('0001-01-01' AS DATE));").error.code,5001);
equal(run("INSERT INTO c VALUES(DATE '2000-01-01');").error.code,5001);
equal(run("DELETE FROM p WHERE d=DATE '0001-01-01';").error.code,5001);
equal(query('SELECT c.d,p.d FROM c LEFT JOIN p ON c.d=p.d ORDER BY c.d;'),[['0001-01-01','0001-01-01'],[null,null]]);
equal(run("CREATE TABLE ck(d DATE CHECK(d>=DATE '2000-01-01')); INSERT INTO ck VALUES(DATE '2024-02-29'),(NULL);").success,true);
equal(run("INSERT INTO ck VALUES(DATE '1999-12-31');").error.code,5001);
equal(run("CREATE TABLE cast_ck(s VARCHAR CHECK(CAST(s AS DATE)>=DATE '2000-01-01')); INSERT INTO cast_ck VALUES('2024-02-29');").success,true);
equal(run("INSERT INTO cast_ck VALUES('1999-12-31');").error.code,5001);
equal(query('SELECT * FROM ck ORDER BY d;'),[['2024-02-29'],[null]]);
const before=readFileSync(file);
equal(run("INSERT INTO dates(id,d) VALUES(4,DATE '2024-01-01'),(5,CAST('bad' AS DATE));").error.code,5001);equal(readFileSync(file),before);
equal(run("BEGIN; UPDATE dates SET d=DATE '2020-01-01'; CREATE TABLE undone(d DATE); ROLLBACK;").success,true);equal(readFileSync(file),before);
equal(run("BEGIN; UPDATE dates SET d=DATE '2020-01-01'; COMMIT;").success,true);
equal(query('SELECT DISTINCT d FROM dates;'),[['2020-01-01']]);
const compileBefore=readFileSync(file);
const compiled=run("CREATE TABLE compiled(d DATE DEFAULT DATE '2000-01-01'); SELECT MIN(d) FROM compiled;",'compile');
equal(compiled.success,true);equal(compiled.plan.find(node=>node.kind==='Aggregate').output[0].type,'date');equal(readFileSync(file),compileBefore);
equal(run('SELECT * FROM compiled;').success,false);
equal(run("CREATE TABLE contextual(date DATE); INSERT INTO contextual VALUES(DATE '2000-01-01');").success,true);
equal(query('SELECT date FROM contextual;'),[['2000-01-01']]);
const literalError=run("SELECT\nDATE '2023-02-29' FROM dates;").error;
equal([literalError.code,literalError.line,literalError.column],[2003,2,1]);
const castError=run("SELECT\nCAST('2023-02-29' AS DATE) FROM dates;").error;
equal([castError.code,castError.line,castError.column],[5001,2,1]);
equal(run("CREATE TABLE mixed(d DATE,b BOOL,n BIGINT,x DECIMAL(5,2),s VARCHAR); INSERT INTO mixed VALUES(DATE '1969-12-31',TRUE,9223372036854775807,1.20,'date');").success,true);
equal(query('SELECT d,b,x,s FROM mixed;'),[['1969-12-31',true,'1.20','date']]);
equal(query('SELECT COUNT(d),MIN(d),MAX(d) FROM dates WHERE FALSE;'),[[0,null,null]]);
console.log(`${checks} DATE column persistence checks passed`);

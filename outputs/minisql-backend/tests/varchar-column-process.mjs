import assert from 'node:assert/strict';
import { mkdtempSync,readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/varchar-column-',import.meta.url)));
const file=join(directory,'database.pages');
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(sql,mode='execute'){const r=invoke(executable,[file,mode],sql);assert.ok(!r.category,JSON.stringify(r));return r.data;}
function query(sql){const r=run(sql);equal(r.success,true);return r.results.at(-1).rows;}
const chinese='\u4e2d\u6587',astral='\u{1f600}';
equal(run("CREATE TABLE strings(id INT PRIMARY KEY,s VARCHAR(0002),d VARCHAR(7) NOT NULL DEFAULT 'O''Brien');").success,true);
equal(run(`INSERT INTO strings(id,s) VALUES(1,'${chinese}'),(2,'${astral}a'),(3,''),(4,NULL);`).success,true);
equal(query('SELECT * FROM strings ORDER BY id;'),[[1,chinese,"O'Brien"],[2,astral+'a',"O'Brien"],[3,'',"O'Brien"],[4,null,"O'Brien"]]);
equal(query(`SELECT id FROM strings WHERE s='${chinese}';`),[[1]]);
equal(query('SELECT s FROM strings ORDER BY s;'),[[''],[chinese],[astral+'a'],[null]]);
equal(query('SELECT COUNT(s),MIN(s),MAX(s) FROM strings;'),[[3,'',astral+'a']]);
equal(query('SELECT d,COUNT(*) FROM strings GROUP BY d;'),[["O'Brien",4]]);
equal(query(`SELECT CAST('${astral}' AS VARCHAR(1)),CAST('e\u0301' AS VARCHAR(2)),CAST('\u00e9' AS VARCHAR(1)) FROM strings WHERE id=1;`),[[astral,'e\u0301','\u00e9']]);
equal(query("SELECT CAST(12.30 AS VARCHAR(5)),CAST(TRUE AS VARCHAR(4)),CAST(DATE '0001-01-01' AS VARCHAR(10)) FROM strings WHERE id=1;"),[['12.30','TRUE','0001-01-01']]);
equal(query("SELECT CAST(CAST('12' AS VARCHAR(2)) AS INT),CAST(CAST('TRUE' AS VARCHAR(4)) AS BOOL),CAST(CAST('2024-02-29' AS VARCHAR(10)) AS DATE) FROM strings WHERE id=1;"),[[12,true,'2024-02-29']]);
equal(query('SELECT CAST(s AS VARCHAR),CAST(s AS VARCHAR(10)),CAST(NULL AS VARCHAR(1)) FROM strings ORDER BY id;'),[[chinese,chinese,null],[astral+'a',astral+'a',null],['','',null],[null,null,null]]);
equal(run("UPDATE strings SET s=CAST('xy' AS VARCHAR(10)),d=DEFAULT WHERE id=3;").success,true);
equal(query('SELECT s FROM strings WHERE id=3;'),[['xy']]);
for(const sql of ["INSERT INTO strings(id,s) VALUES(5,'abc');","UPDATE strings SET s='abc';",`SELECT CAST('${astral}a' AS VARCHAR(1)) FROM strings;`,"SELECT CAST('e\u0301' AS VARCHAR(1)) FROM strings;","SELECT CAST(FALSE AS VARCHAR(4)) FROM strings;","SELECT CAST(12.30 AS VARCHAR(4)) FROM strings;"]){
 const before=readFileSync(file);equal(run(sql).error.code,5001);equal(readFileSync(file),before);
}
for(const sql of ["CREATE TABLE bad(s VARCHAR(0));","CREATE TABLE bad(s VARCHAR(1) DEFAULT 'aa');",'INSERT INTO strings(id,s) VALUES(5,1);','SELECT s=1 FROM strings;','SELECT SUM(s) FROM strings;','SELECT CAST(NULL AS VARCHAR(0)) FROM strings;'])equal(run(sql).error.code,2003);
for(const sql of ['CREATE TABLE bad(s VARCHAR());','CREATE TABLE bad(s VARCHAR(-1));','CREATE TABLE bad(s VARCHAR(1,2));','CREATE TABLE bad(s VARCHAR(4294967296));'])equal(run(sql).error.code,2002);
equal(run('CREATE TABLE max_length(s VARCHAR(4294967295));').success,true);
equal(run("INSERT INTO max_length VALUES('x');").success,true);
equal(query('SELECT * FROM max_length;'),[['x']]);
equal(run("CREATE TABLE p(s VARCHAR(4) PRIMARY KEY); CREATE TABLE c(s VARCHAR(8) REFERENCES p(s)); INSERT INTO p VALUES('ab'); INSERT INTO c VALUES('ab'),(NULL);").success,true);
equal(run("INSERT INTO p VALUES('ab');").error.code,5001);
equal(run("INSERT INTO c VALUES('none');").error.code,5001);
equal(run('DELETE FROM p;').error.code,5001);
equal(query('SELECT p.s,c.s FROM c LEFT JOIN p ON c.s=p.s ORDER BY c.s;'),[['ab','ab'],[null,null]]);
equal(run("CREATE TABLE ck(s VARCHAR(2) CHECK(s=CAST('ab' AS VARCHAR(3)))); INSERT INTO ck VALUES('ab'),(NULL);").success,true);
equal(run("INSERT INTO ck VALUES('cd');").error.code,5001);
equal(query('SELECT * FROM ck ORDER BY s;'),[['ab'],[null]]);
const before=readFileSync(file);
equal(run("INSERT INTO strings(id,s) VALUES(5,'ok'),(6,'too long');").error.code,5001);equal(readFileSync(file),before);
equal(run("BEGIN; UPDATE strings SET s='ok'; CREATE TABLE undone(s VARCHAR(1)); ROLLBACK;").success,true);equal(readFileSync(file),before);
equal(run("BEGIN; UPDATE strings SET s='ok'; COMMIT;").success,true);
equal(query('SELECT DISTINCT s FROM strings;'),[['ok']]);
const compiled=run('CREATE TABLE compiled(s VARCHAR(3)); SELECT s,MIN(s) FROM compiled GROUP BY s;','compile');
equal(compiled.success,true);equal(compiled.plan.find(n=>n.kind==='Project').output.map(c=>c.type),['varchar(3)','varchar(3)']);
equal(run('SELECT * FROM compiled;').success,false);
const error=run("INSERT INTO strings(id,s) VALUES(5,\n'abc');").error;
equal([error.code,error.line,error.column],[5001,2,1]);
equal(run('CREATE TABLE large(s VARCHAR(5000));').success,true);
equal(run(`INSERT INTO large VALUES('${'x'.repeat(3995)}');`).success,true);
equal(run(`INSERT INTO large VALUES('${'x'.repeat(3996)}');`).error.code,4001);
equal(query('SELECT s FROM large;'),[['x'.repeat(3995)]]);
console.log(`${checks} VARCHAR(n) persistence checks passed`);

import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
import { spawnSync } from 'node:child_process';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/decimal-column-',import.meta.url)));
const file=join(directory,'database.pages');
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(sql,mode='execute'){const result=invoke(executable,[file,mode],sql);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(sql){const result=run(sql);assert.equal(result.success,true,JSON.stringify(result));++checks;return result.results.at(-1).rows;}
equal(run('CREATE TABLE d(id INT PRIMARY KEY,amount DECIMAL(08,02) NOT NULL DEFAULT 1.20,rate DECIMAL(6,4),note VARCHAR);').success,true);
equal(run("INSERT INTO d VALUES(1,12.3,0.125,'x'),(2,-2,NULL,'y'); INSERT INTO d(id) VALUES(3);").success,true);
equal(query('SELECT * FROM d ORDER BY id;'),[[1,'12.30','0.1250','x'],[2,'-2.00',null,'y'],[3,'1.20',null,null]]);
equal(query('SELECT amount FROM d ORDER BY amount;'),[['-2.00'],['1.20'],['12.30']]);
equal(query('SELECT COUNT(amount),SUM(amount),AVG(amount),MIN(amount),MAX(amount) FROM d;'),[[3,'11.50','3.833333','-2.00','12.30']]);
equal(query('SELECT rate,COUNT(*) FROM d GROUP BY rate ORDER BY rate;'),[['0.1250',1],[null,2]]);
equal(query('SELECT amount+0.1,amount*rate FROM d WHERE id=1;'),[['12.40','1.537500']]);
equal(run('UPDATE d SET amount=CAST(amount+1 AS DECIMAL(8,2)),rate=CAST(0.25005 AS DECIMAL(6,4)) WHERE id=1;').success,true);
equal(query('SELECT amount,rate FROM d WHERE id=1;'),[['13.30','0.2501']]);
equal(run('UPDATE d SET amount=DEFAULT WHERE id=1;').success,true);
equal(query('SELECT amount FROM d WHERE id=1;'),[['1.20']]);
equal(query('SELECT DISTINCT amount FROM d ORDER BY amount;'),[['-2.00'],['1.20']]);
for(const source of [
 'INSERT INTO d(id,amount) VALUES(4,1.234);',
 'UPDATE d SET amount=amount+1;',
 "INSERT INTO d(id,amount) VALUES(4,'1.20');",
 'INSERT INTO d(id,amount) VALUES(4,NULL);',
 'CREATE TABLE bad(v DECIMAL(0,0));','CREATE TABLE bad(v DECIMAL(39,0));',
 'CREATE TABLE bad(v DECIMAL(2,3));','CREATE TABLE bad(v DECIMAL(3,2) DEFAULT 0.001);',
 'CREATE TABLE bad(v DECIMAL(3,2) DEFAULT 100);'
]) {const before=readFileSync(file);equal(run(source).error.code,2003);equal(readFileSync(file),before);}
const before=readFileSync(file);
equal(run('INSERT INTO d(id,amount) VALUES(4,1),(5,1000000);').error.code,5001);equal(readFileSync(file),before);
const overflow=run('INSERT INTO d(id,amount) VALUES(9,\n1000000);').error;
equal(overflow.code,5001);equal(overflow.line,2);equal(overflow.column,1);
equal(run('CREATE TABLE u(v DECIMAL(4,2) UNIQUE); INSERT INTO u VALUES(1.2);').success,true);
equal(run('INSERT INTO u VALUES(1.20);').error.code,5001);
equal(run('INSERT INTO u VALUES(NULL),(NULL);').success,true);
equal(query('SELECT * FROM u ORDER BY v;'),[['1.20'],[null],[null]]);
equal(run('CREATE TABLE p(v DECIMAL(4,2) PRIMARY KEY); CREATE TABLE c(v DECIMAL(4,2) REFERENCES p(v)); INSERT INTO p VALUES(1.2); INSERT INTO c VALUES(1.20);').success,true);
equal(run('INSERT INTO c VALUES(2.0);').error.code,5001);
equal(run('DELETE FROM p;').error.code,5001);
equal(query('SELECT p.v,c.v FROM p JOIN c ON p.v=c.v;'),[['1.20','1.20']]);
equal(run('CREATE TABLE ck(v DECIMAL(4,2) CHECK(v>=0.0)); INSERT INTO ck VALUES(1.2);').success,true);
equal(run('INSERT INTO ck VALUES(-0.01);').error.code,5001);
equal(query('SELECT * FROM ck;'),[['1.20']]);
const maximum='99999999999999999999999999999999999999';
equal(run(`CREATE TABLE edge(v DECIMAL(38,0),s DECIMAL(38,38)); INSERT INTO edge VALUES(CAST('${maximum}' AS DECIMAL(38,0)),0.00000000000000000000000000000000000001),(CAST('-${maximum}' AS DECIMAL(38,0)),0.99999999999999999999999999999999999999);`).success,true);
equal(query('SELECT * FROM edge ORDER BY v;'),[['-'+maximum,'0.99999999999999999999999999999999999999'],[maximum,'0.00000000000000000000000000000000000001']]);
const snapshot=readFileSync(file);
const transaction=run('BEGIN; UPDATE d SET amount=CAST(9.99 AS DECIMAL(8,2)); SELECT SUM(amount) FROM d; ROLLBACK;');
equal(transaction.success,true);equal(transaction.results[2].rows,[['29.97']]);equal(readFileSync(file),snapshot);
equal(run('BEGIN; CREATE TABLE rolled(v DECIMAL(8,2)); INSERT INTO rolled VALUES(1.2); ROLLBACK;').success,true);
equal(run('SELECT * FROM rolled;').error.code,2003);
equal(readFileSync(file),snapshot);
equal(run('BEGIN; INSERT INTO d(id,amount) VALUES(4,4.25); COMMIT;').success,true);
equal(query('SELECT amount FROM d WHERE id=4;'),[['4.25']]);
equal(run('DELETE FROM d WHERE amount<0.0;').success,true);
equal(query('SELECT id FROM d ORDER BY id;'),[[1],[3],[4]]);
const catalogProcess=spawnSync(executable,[file,'catalog'],{encoding:'utf8',timeout:5000,windowsHide:true});
equal(catalogProcess.status,0);equal(catalogProcess.error,undefined);
const catalog=JSON.parse(catalogProcess.stdout);
equal(catalog.tables.find(t=>t.name==='d').columns[1].type,'decimal(8,2)');
const beforeCompile=readFileSync(file);
const compiled=run('CREATE TABLE compile_decimal(v DECIMAL(8,2)); SELECT AVG(v) FROM compile_decimal;','compile');
equal(compiled.success,true);equal(readFileSync(file),beforeCompile);
equal(run('SELECT * FROM compile_decimal;').error.code,2003);
console.log(`${checks} DECIMAL column persistence checks passed`);

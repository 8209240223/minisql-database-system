import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/decimal-literal-',import.meta.url)));
const file=join(directory,'database.pages');
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(sql,mode='execute'){const result=invoke(executable,[file,mode],sql);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(sql){const result=run(sql);assert.equal(result.success,true,JSON.stringify(result));++checks;return result.results.at(-1).rows;}
equal(run('CREATE TABLE t(v INT); INSERT INTO t VALUES(2),(10),(-2);').success,true);
equal(query('SELECT 00012.3400,+0.10,-0.00,0.00000001 FROM t LIMIT 1;'),[['12.3400','0.10','0.00','0.00000001']]);
equal(query('SELECT 0.1+0.2,1.25*0.125,1.0/3.0,1.00000001/-2.0 FROM t LIMIT 1;'),[['0.3','0.15625','0.333333','-0.50000001']]);
equal(query('SELECT 2.0<10.0,-10.0<-2.0,1.0=1.00,0.1+0.2=0.3,1.0=1 FROM t LIMIT 1;'),[[true,true,true,true,true]]);
equal(query('SELECT v+0.1 FROM t ORDER BY v+0.1;'),[['-1.9'],['2.1'],['10.1']]);
equal(query('SELECT COUNT(0.1),MIN(v+0.0),MAX(v+0.0),SUM(v+0.1),AVG(v+0.1) FROM t;'),[[3,'-2.0','10.0','10.3','3.433333']]);
equal(query('SELECT AVG(0.00000001),SUM(0.00000001) FROM t;'),[['0.00000001','0.00000003']]);
equal(query('SELECT AVG(0.1),SUM(0.1),MIN(0.1),MAX(0.1) FROM t WHERE FALSE;'),[[null,null,null,null]]);
equal(query('SELECT v+0.5,COUNT(*) FROM t GROUP BY v+00.5 ORDER BY v+0.5;'),[['-1.5',1],['2.5',1],['10.5',1]]);
equal(query('SELECT CAST(2.5 AS INT),CAST(-2.5 AS BIGINT),CAST(0001.2300 AS VARCHAR) FROM t LIMIT 1;'),[[3,-3,'1.2300']]);
equal(query('SELECT FALSE AND 1.0/0.0>0,TRUE OR 1.0/0.0>0 FROM t LIMIT 1;'),[[false,true]]);
const compiled=run('SELECT 00012.3400,0.00000001,1.0+2.00,SUM(v+0.1),AVG(0.00000001) FROM t;','compile');
equal(compiled.success,true);
equal(compiled.plan[0].output.map(c=>c.type),['decimal(6,4)','decimal(8,8)','decimal(38,2)','decimal(38,1)','decimal(38,8)']);
equal(compiled.tokens.filter(t=>t.type==='DECIMAL').map(t=>t.text),['00012.3400','0.00000001','1.0','2.00','0.1','0.00000001']);
equal(compiled.optimizedPlan[0].projections[2].kind,'Literal');
equal(compiled.optimizedPlan[0].projections[2].value,'3.00');
const maximum='9999999999999999999999999999999999999.9';
equal(query(`SELECT ${maximum} FROM t LIMIT 1;`),[[maximum]]);
equal(run(`SELECT ${maximum}+0.1 FROM t;`).error.code,5001);
equal(run(`SELECT SUM(${maximum}) FROM t;`).error.code,5001);
equal(run(`SELECT AVG(${maximum}) FROM t;`).error.code,5001);
const avgOverflow=run(`SELECT\nAVG(${maximum}) FROM t;`);
equal(avgOverflow.error.line,2);equal(avgOverflow.error.column,5);
const averageMaximum='99999999999999999999999999999999.999999';
equal(query(`SELECT AVG(${averageMaximum}) FROM t;`),[[averageMaximum]]);
for(const raw of ['1.','1.2.3','1.2abc','.5']) equal(run(`SELECT ${raw} FROM t;`).error.code,2001);
// grammar.md 的 float_literal 允许尾随指数段：带指数的字面量是合法的 FLOAT，
// 不是非法 DECIMAL（此前这条断言写于 FLOAT 指数形式实现之前，已过时）。
equal(query('SELECT 1.2e3,1.2E-3 FROM t LIMIT 1;'),[[1200,0.0012]]);
for(const raw of ['1.'+'0'.repeat(38),'0.'+'0'.repeat(38)+'1']) equal(run(`SELECT ${raw} FROM t;`).error.code,2003);
equal(run('SELECT 1.0+TRUE FROM t;').error.code,2003);
equal(run("SELECT 1.0='1.0' FROM t;").error.code,2003);
const before=readFileSync(file);
equal(run('INSERT INTO t VALUES(1.2);').error.code,2003);equal(readFileSync(file),before);
equal(run('CREATE TABLE guarded(v INT CHECK(v>-0.5 AND v<10.5)); INSERT INTO guarded VALUES(0),(10);').success,true);
equal(query('SELECT * FROM guarded ORDER BY v;'),[[0],[10]]);
equal(run('INSERT INTO guarded VALUES(-1);').error.code,5001);
equal(run('INSERT INTO t VALUES(CAST(2.5 AS INT));').success,true);
equal(query('SELECT v FROM t WHERE v=3;'),[[3]]);
console.log(`${checks} DECIMAL literal checks passed`);

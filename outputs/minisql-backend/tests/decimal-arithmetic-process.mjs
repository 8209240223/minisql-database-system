import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/decimal-arithmetic-',import.meta.url)));
const file=join(directory,'database.pages');
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(sql,mode='execute'){const result=invoke(executable,[file,mode],sql);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(sql){const result=run(sql);assert.equal(result.success,true,JSON.stringify(result));++checks;return result.results.at(-1).rows;}
equal(run('CREATE TABLE t(g INT,v BIGINT); INSERT INTO t VALUES(1,1),(1,2),(2,2),(2,10),(3,NULL);').success,true);
equal(query('SELECT AVG(v)+1,1+AVG(v),AVG(v)-1,1-AVG(v),AVG(v)*2,2*AVG(v),AVG(v)/2,2/AVG(v) FROM t WHERE g=1;'),[['2.500000','2.500000','0.500000','-0.500000','3.000000','3.000000','0.750000','1.333333']]);
equal(query('SELECT AVG(v)*AVG(v),-(AVG(v)*AVG(v)),AVG(v)*AVG(v)+1,AVG(v)*AVG(v)/AVG(v) FROM t WHERE g=1;'),[['2.250000000000','-2.250000000000','3.250000000000','1.500000000000']]);
equal(query('SELECT AVG(v)/(-AVG(v)),AVG(v)-AVG(v),AVG(v)/3 FROM t WHERE g=1;'),[['-1.000000','0.000000','0.500000']]);
equal(query('SELECT g,AVG(v)*AVG(v) AS square FROM t GROUP BY g ORDER BY square;'),[[1,'2.250000000000'],[2,'36.000000000000'],[3,null]]);
equal(query('SELECT g FROM t GROUP BY g HAVING AVG(v)*AVG(v)>10 ORDER BY AVG(v)*AVG(v);'),[[2]]);
equal(query('SELECT AVG(v)*AVG(v)=AVG(v)+AVG(v)/2,AVG(v)*AVG(v)>AVG(v),AVG(v)*AVG(v)<3 FROM t WHERE g=1;'),[[true,true,true]]);
equal(query('SELECT CAST(AVG(v)*AVG(v) AS INT),CAST(AVG(v)*AVG(v) AS VARCHAR) FROM t WHERE g=1;'),[[2,'2.250000000000']]);
equal(query('SELECT AVG(v)+NULL,AVG(v)/0,AVG(v)*AVG(v) FROM t WHERE FALSE;'),[[null,null,null]]);
equal(query('SELECT FALSE AND AVG(v)/0>1,TRUE OR AVG(v)/0>1 FROM t;'),[[false,true]]);
equal(query('SELECT DISTINCT AVG(v)*AVG(v) FROM t GROUP BY g ORDER BY AVG(v)*AVG(v) DESC NULLS LAST;'),[['36.000000000000'],['2.250000000000'],[null]]);
const compiled=run('SELECT AVG(v)+1,AVG(v)*AVG(v),AVG(v)*AVG(v)/2 FROM t;','compile');
equal(compiled.success,true);
equal(compiled.plan[0].output.map(column=>column.type),['decimal(38,6)','decimal(38,12)','decimal(38,12)']);
const before=readFileSync(file);
const division=run('SELECT\nAVG(v)/0 FROM t;');
equal(division.success,false);equal(division.error.code,5001);
equal(division.error.line,2);equal(division.error.column,7);
equal(readFileSync(file),before);
equal(run('SELECT AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v) FROM t;').error.code,2003);
equal(run('CREATE TABLE big(v BIGINT); INSERT INTO big VALUES(9223372036854775807);').success,true);
equal(run('SELECT AVG(v)*AVG(v) FROM big;').error.code,5001);
equal(query('SELECT TRUE OR AVG(v)*AVG(v)>0 FROM big;'),[[true]]);
const transaction=run('BEGIN; INSERT INTO t VALUES(9,9); SELECT AVG(v)/0 FROM t; COMMIT;');
equal(transaction.success,false);equal(transaction.error.code,5001);
equal(query('SELECT COUNT(*) FROM t;'),[[5]]);
console.log(`${checks} decimal arithmetic process checks passed`);

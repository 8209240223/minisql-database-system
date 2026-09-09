import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';

const directory=mkdtempSync(fileURLToPath(new URL('./artifacts/avg-',import.meta.url)));
const executable=fileURLToPath(new URL('../bin/minisql_database.exe',import.meta.url));
const file=join(directory,'database.pages');
let checks=0;
function equal(a,b){assert.deepEqual(a,b);++checks;}
function run(source){const result=invoke(executable,[file,'execute'],source);assert.ok(!result.category,JSON.stringify(result));return result.data;}
function query(source){const result=run(source);assert.equal(result.success,true,JSON.stringify(result));++checks;return result.results.at(-1).rows;}
equal(run('CREATE TABLE t(g INT,v BIGINT); CREATE TABLE empty_t(v INT); INSERT INTO t VALUES(1,1),(1,0),(1,0),(2,-1),(2,-1),(2,0),(3,NULL);').success,true);
equal(query('SELECT g,AVG(v),COUNT(v) FROM t GROUP BY g ORDER BY g;'),[[1,'0.333333',3],[2,'-0.666667',3],[3,null,0]]);
equal(query('SELECT AVG(v),-AVG(v),+AVG(v) FROM t;'),[['-0.166667','0.166667','-0.166667']]);
equal(query('SELECT g,CAST(AVG(v) AS INT),CAST(AVG(v) AS VARCHAR) FROM t GROUP BY g ORDER BY g;'),[[1,0,'0.333333'],[2,-1,'-0.666667'],[3,null,null]]);
equal(query('SELECT AVG(v),COUNT(*) FROM empty_t;'),[[null,0]]);
equal(query('SELECT AVG(v) FROM empty_t GROUP BY v;'),[]);
equal(query('SELECT AVG(v) FROM t WHERE FALSE;'),[[null]]);
equal(query('SELECT AVG(NULL) FROM t;'),[[null]]);
equal(query('SELECT g FROM t GROUP BY g HAVING AVG(v)>0 ORDER BY AVG(v);'),[[1]]);
equal(query('SELECT g FROM t GROUP BY g HAVING 0>AVG(v);'),[[2]]);
equal(query('SELECT g FROM t GROUP BY g HAVING AVG(v) IS NULL;'),[[3]]);
equal(run('CREATE TABLE big(v BIGINT); INSERT INTO big VALUES(9223372036854775807),(9223372036854775807);').success,true);
equal(query('SELECT AVG(v) FROM big;'),[['9223372036854775807.000000']]);
equal(query('SELECT AVG(v)=9223372036854775807,AVG(v)>9223372036854775806 FROM big;'),[[true,true]]);
equal(query('SELECT CAST(AVG(v) AS BIGINT) FROM big;'),[['9223372036854775807']]);
equal(run('SELECT CAST(AVG(v) AS INT) FROM big;').error.code,5001);
equal(run('SELECT SUM(v) FROM big;').error.code,5001);
equal(run('DELETE FROM big; INSERT INTO big VALUES(-9223372036854775808),(-9223372036854775808);').success,true);
equal(query('SELECT AVG(v) FROM big;'),[['-9223372036854775808.000000']]);
equal(run('DELETE FROM big; INSERT INTO big VALUES(-9223372036854775808),(9223372036854775807);').success,true);
equal(query('SELECT AVG(v) FROM big;'),[['-0.500000']]);
equal(query('SELECT CAST(AVG(v) AS INT) FROM big;'),[[-1]]);
equal(run('CREATE TABLE ordered(g INT,v INT); INSERT INTO ordered VALUES(1,2),(2,10),(3,-10),(4,-2),(5,NULL);').success,true);
equal(query('SELECT g,AVG(v) FROM ordered GROUP BY g ORDER BY AVG(v);'),[[3,'-10.000000'],[4,'-2.000000'],[1,'2.000000'],[2,'10.000000'],[5,null]]);
equal(query('SELECT g FROM ordered GROUP BY g ORDER BY AVG(v) DESC LIMIT 2;'),[[5],[2]]);
equal(query('SELECT g FROM ordered GROUP BY g HAVING AVG(v)<10 AND AVG(v)>=2 ORDER BY g;'),[[1]]);
equal(query('SELECT AVG(v)=AVG(v),AVG(v)!=0 FROM ordered;'),[[true,false]]);
equal(query('SELECT DISTINCT AVG(v) FROM t GROUP BY g ORDER BY AVG(v);'),[['-0.666667'],['0.333333'],[null]]);
const transaction=run('BEGIN; INSERT INTO ordered VALUES(6,12); SELECT AVG(v) FROM ordered; ROLLBACK; SELECT AVG(v) FROM ordered;');
equal(transaction.success,true);equal(transaction.results[2].rows,[['2.400000']]);equal(transaction.results[4].rows,[['0.000000']]);
equal(transaction.results[2].commitState,'rolledBack');
equal(query('SELECT AVG(v)=0,AVG(v)=AVG(v),-AVG(v) FROM empty_t;'),[[null,null,null]]);
let seed=42n;
function next(){seed=BigInt.asUintN(64,seed*6364136223846793005n+1442695040888963407n);return BigInt.asIntN(64,seed);}
function average(values){
 const sum=values.reduce((a,b)=>a+b,0n), negative=sum<0n;
 const scaled=(negative?-sum:sum)*1000000n, count=BigInt(values.length);
 let units=scaled/count;
 if((scaled%count)*2n>=count)++units;
 const digits=units.toString().padStart(7,'0');
 return (negative&&units!==0n?'-':'')+digits.slice(0,-6)+'.'+digits.slice(-6);
}
const expected=[],values=[];
for(let group=0;group<128;++group){
 const samples=Array.from({length:7},()=>next());
 expected.push([group,average(samples)]);
 for(const sample of samples)values.push(`(${group},${sample})`);
}
equal(run('CREATE TABLE generated(g INT,v BIGINT); INSERT INTO generated VALUES'+values.join(',')+';').success,true);
equal(query('SELECT g,AVG(v) FROM generated GROUP BY g ORDER BY g;'),expected);
console.log(`${checks} exact AVG process checks passed: large sums, NULL, ordering, comparisons and transactions`);

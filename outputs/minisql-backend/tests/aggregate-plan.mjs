import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';

const executable = fileURLToPath(new URL('../bin/minisql_compile.exe', import.meta.url));
const fixture = 'CREATE TABLE t(id INT,v INT,b BIGINT,s VARCHAR); CREATE TABLE r(id INT,v INT);';
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function compile(source) {
  const result = invoke(executable, [], fixture + source);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
function plan(source) {
  const result = compile(source); equal(result.success, true);
  return {raw:result.plan.filter(node=>node.statementIndex===2), optimized:result.optimizedPlan.filter(node=>node.statementIndex===2)};
}
const complex = plan('SELECT v,COUNT(*) AS n FROM t WHERE id>0 GROUP BY v HAVING COUNT(*)>1 ORDER BY SUM(id) DESC LIMIT 3;');
equal(complex.raw.map(node=>node.kind), ['Limit','Sort','Project','Filter','Aggregate','Filter','SeqScan']);
const aggregate = complex.raw.find(node=>node.kind==='Aggregate');
equal(aggregate.groupKeys[0].columnId,1);
equal(aggregate.aggregates.map(item=>[item.function,item.columnId,item.argument?.columnId]), [['COUNT',1,undefined],['SUM',2,0]]);
equal(aggregate.output.map(column=>[column.columnId,column.type,column.nullable]), [[0,'int',true],[1,'bigint',false],[2,'bigint',true]]);
const project = complex.raw.find(node=>node.kind==='Project');
equal(project.projections.map(item=>item.columnId), [0,1,2]);
equal(project.output.map(item=>item.columnId), [0,1,2]);
equal(complex.raw[1].output.map(item=>[item.name,item.columnId]), [['v',0],['n',1]]);
equal(complex.raw[1].sortKeys[0].index,2);
equal(complex.raw[3].predicate.left.columnId,1);
equal(complex.raw[5].predicate.left.columnId,0);
equal(complex.raw[3].output,aggregate.output);
equal(complex.raw.slice(0,5).every(node=>!node.preservesRowId),true);
const five = plan('SELECT COUNT(*),COUNT(v),SUM(v),AVG(b),MIN(s),MAX(v) FROM t;').raw.find(node=>node.kind==='Aggregate');
equal(five.groupKeys,[]);
equal(five.aggregates.map(item=>item.type),['bigint','bigint','bigint','decimal(38,6)','varchar','int']);
equal(five.aggregates.map(item=>item.nullable),[false,false,true,true,true,true]);
equal(five.aggregates[0].argument,null);
equal(five.aggregates[1].argument.columnId,1);
const group = plan('SELECT (v+01)*2 FROM t GROUP BY t.v+1,v+01;').raw;
equal(group[1].groupKeys.length,1);
equal(group[0].projections[0].left.columnId,0);
equal(group[1].aggregates,[]);
const repeat = plan('SELECT SUM(v),sum(T.v)+1 FROM t HAVING SUM(v)>0 ORDER BY SUM(v);').raw;
equal(repeat.find(node=>node.kind==='Aggregate').aggregates.length,1);
const hidden = plan('SELECT 1 FROM t HAVING COUNT(*)>0;').raw;
equal(hidden.map(node=>node.kind),['Project','Filter','Aggregate','SeqScan']);
equal(hidden[2].aggregates.length,1);
equal(hidden[0].projections[0].kind,'Literal');
const emptyState = plan('SELECT 1 FROM t HAVING TRUE;');
equal(emptyState.raw[2].output,[]);
equal(emptyState.optimized.map(node=>node.kind),['Project','Aggregate','SeqScan']);
const folded = plan('SELECT SUM(1+2),COUNT(*) FROM t GROUP BY 2+3 HAVING TRUE;');
equal(folded.raw.find(node=>node.kind==='Aggregate').aggregates[0].argument.kind,'Binary');
equal(folded.optimized.find(node=>node.kind==='Aggregate').aggregates[0].argument.value,3);
equal(folded.optimized.find(node=>node.kind==='Aggregate').groupKeys[0].value,5);
const joined = plan('SELECT x.id,COUNT(y.v) FROM t x LEFT JOIN r y ON x.id=y.id GROUP BY x.id;').raw;
equal(joined.map(node=>node.kind),['Project','Aggregate','LeftJoin','SeqScan','SeqScan']);
equal(joined[1].aggregates[0].argument.columnId,5);
const wildcard = plan('SELECT t.*,COUNT(*) FROM t GROUP BY s,b,v,id;').raw;
equal(wildcard[0].projections.map(item=>item.columnId),[3,2,1,0,4]);
equal(plan('SELECT DISTINCT v,COUNT(*) AS n FROM t GROUP BY v ORDER BY n;').raw.map(node=>node.kind),['Sort','Distinct','Project','Aggregate','SeqScan']);
equal(plan('SELECT -AVG(v) FROM t HAVING AVG(v)>1;').raw[0].output[0].type,'decimal(38,6)');
equal(plan('SELECT v FROM t GROUP BY v ORDER BY COUNT(*);').raw[1].output[1].nullable,false);
const casts=plan('SELECT CAST(AVG(v) AS INT),CAST(AVG(v) AS BIGINT),CAST(AVG(v) AS VARCHAR) FROM t;').raw;
equal(casts[0].output.map(column=>column.type),['int','bigint','varchar']);
equal(casts[0].projections.map(expression=>expression.left.type),Array(3).fill('decimal(38,6)'));
equal(casts[1].aggregates.length,1);
const arithmetic=plan('SELECT AVG(v)+1,AVG(v)*AVG(v),AVG(v)*AVG(v)/2,-(AVG(v)*AVG(v)) FROM t;').raw;
equal(arithmetic[0].output.map(column=>column.type),['decimal(38,6)','decimal(38,12)','decimal(38,12)','decimal(38,12)']);
equal(arithmetic[0].projections.map(expression=>expression.type),arithmetic[0].output.map(column=>column.type));
equal(arithmetic[1].aggregates.length,1);
const decimalCast=plan('SELECT CAST(AVG(v) AS DECIMAL(06,02)) FROM t;').raw;
equal(decimalCast[0].output[0].type,'decimal(6,2)');
equal(decimalCast[0].projections[0].type,'decimal(6,2)');
equal(decimalCast[0].projections[0].left.type,'decimal(38,6)');
for (const source of [
  'SELECT v,COUNT(*) FROM t;', 'SELECT SUM(COUNT(*)) FROM t;',
  'SELECT v FROM t WHERE COUNT(*)>0;', 'SELECT COUNT(*) FROM t GROUP BY SUM(v);',
  'SELECT COUNT(*) AS n FROM t HAVING n>0;', 'SELECT COUNT(*) FROM t ORDER BY v;',
  'SELECT DISTINCT COUNT(*) FROM t ORDER BY SUM(v);',
]) {const result=compile(source);equal(result.success,false);equal(result.error.code,2003);}
const serialized = JSON.stringify(complex.raw);
equal(serialized.includes('AggregateExpr'),false);
equal(compile('SELECT v,COUNT(*) AS n FROM t WHERE id>0 GROUP BY v HAVING COUNT(*)>1 ORDER BY SUM(id) DESC LIMIT 3;').plan.filter(node=>node.statementIndex===2),complex.raw);
console.log(`${checks} aggregate plan checks passed: slots, clauses, reuse, types, optimizer and invalid bindings`);

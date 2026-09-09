import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';

const executable = fileURLToPath(new URL('../bin/minisql_compile.exe', import.meta.url));
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(source, parseOnly = true) {
  const result = invoke(executable, parseOnly ? ['--parse-only'] : [], source);
  assert.ok(!result.category, JSON.stringify(result));
  return result.data;
}
function parse(source) {
  const result = run(source);
  equal(result.success, true);
  return result.ast;
}
const ast = parse('SELECT dept, COUNT(*) AS n, SUM(v+1), AVG(v), MIN(v), MAX(v) FROM t WHERE v>0 GROUP BY dept, id+1 HAVING COUNT(*)>1 ORDER BY n DESC LIMIT 2 OFFSET 1;');
equal(ast.selectItems.slice(1).map(item => item.expression.kind), Array(5).fill('AggregateExpr'));
equal(ast.selectItems.slice(1).map(item => item.expression.value), ['COUNT','SUM','AVG','MIN','MAX']);
equal(ast.selectItems[1].expression.left.kind, 'Wildcard');
equal(ast.selectItems[2].expression.left.value, '+');
equal(ast.groupBy.map(item => item.value), ['dept','+']);
equal(ast.having.left.value, 'COUNT');
equal(ast.where.value, '>');
equal(ast.orderBy[0].expression.value, 'n');
equal(ast.limit, '2');equal(ast.offset, '1');
equal(parse('SELECT count,sum,avg,min,max FROM t;').selectItems.map(item => item.expression.kind), Array(5).fill('Identifier'));
equal(parse('SELECT t.count FROM t;').selectItems[0].expression.value, 't.count');
equal(parse('SELECT coUnt (/* argument */ v) FROM t;').selectItems[0].expression.value, 'COUNT');
equal(parse("SELECT SUM(CAST('3' AS BIGINT)*v) FROM t;").selectItems[0].expression.left.left.kind, 'Cast');
equal(parse('SELECT MAX(v)+MIN(v) FROM t HAVING MAX(v) IS NOT NULL;').having.left.kind, 'AggregateExpr');
equal(parse('SELECT COUNT(*) FROM t;').groupBy, []);
equal(parse('SELECT v FROM t GROUP BY v;').having, null);
const position = parse('-- 中文\r\nSELECT SUM(\r\n v) FROM t;').selectItems[0].expression;
equal([position.line,position.column,position.left.line,position.left.column], [2,8,3,2]);
// 聚合嵌套属于语义限制；AST 必须保留结构供语义阶段准确诊断。
equal(parse('SELECT SUM(COUNT(*)) FROM t;').selectItems[0].expression.left.kind, 'AggregateExpr');
for (const source of [
  'SELECT COUNT() FROM t;', 'SELECT SUM(*) FROM t;', 'SELECT AVG(*) FROM t;',
  'SELECT MIN(*) FROM t;', 'SELECT MAX(*) FROM t;', 'SELECT COUNT(v,id) FROM t;',
  'SELECT SUM(v,) FROM t;', 'SELECT COUNT(t.*) FROM t;', 'SELECT COUNT(*+1) FROM t;',
  'SELECT SUM(v FROM t;', 'SELECT COUNT(v) FROM t GROUP v;',
  'SELECT v FROM t GROUP BY;', 'SELECT v FROM t GROUP BY v,;',
  'SELECT v FROM t HAVING;', 'SELECT v FROM t HAVING v>0 GROUP BY v;',
  'SELECT v FROM t ORDER BY v GROUP BY v;', 'SELECT v FROM t GROUP BY v WHERE v>0;',
  'SELECT v FROM t GROUP BY v GROUP BY v;', 'SELECT unknown(v) FROM t;',
  'SELECT ' + 'SUM('.repeat(300) + 'v' + ')'.repeat(300) + ' FROM t;',
]) {
  const result = run(source); equal(result.success, false); equal(result.error.code, 2002);
}
for (const source of [
  'SELECT COUNT(*) FROM t;', 'SELECT v FROM t GROUP BY v;',
  'SELECT COUNT(*) FROM t HAVING COUNT(*)>0;', 'SELECT COUNT(*) FROM t ORDER BY SUM(v);',
]) {
  const result = run('CREATE TABLE t(v INT);' + source, false);
  equal(result.success, true); equal(result.plan.some(node => node.kind === 'Aggregate'), true);
}
equal(run('CREATE TABLE t(v INT); SELECT v FROM t WHERE COUNT(*)>0;', false).error.code, 2003);
equal(run('CREATE TABLE t(count INT); SELECT count FROM t;', false).success, true);
console.log(`${checks} aggregate syntax/AST and compilation checks passed`);

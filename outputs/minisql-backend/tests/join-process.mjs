import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/join-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const reference = new DatabaseSync(':memory:');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function referenceQuery(sql) {
  const statement = reference.prepare(sql);
  const columns = statement.columns().map(column => column.name);
  statement.setReturnArrays(true);
  return { columns, values: statement.all() };
}
function run(sql, mode = 'execute') {
  const outcome = invoke(executable, [join(directory, 'database.pages'), mode], sql);
  assert.ok(!outcome.category, JSON.stringify(outcome));
  return outcome.data;
}
const fixture = "CREATE TABLE t(id INT,name VARCHAR); CREATE TABLE u(id INT,score INT); CREATE TABLE v(id INT,label VARCHAR); CREATE TABLE empty_table(id INT);" +
  "INSERT INTO t(id,name) VALUES(1,'a'); INSERT INTO t(id,name) VALUES(2,'b'); INSERT INTO t(id,name) VALUES(2,'b');" +
  "INSERT INTO u(id,score) VALUES(1,80); INSERT INTO u(id,score) VALUES(2,90); INSERT INTO u(id,score) VALUES(2,95); INSERT INTO u(id,score) VALUES(4,77);" +
  "INSERT INTO v(id,label) VALUES(2,'中文');";
try {
  reference.exec(fixture);
  equal(run(fixture).success, true);
  const queries = [
    'SELECT t.name,u.score FROM t JOIN u ON t.id=u.id ORDER BY t.name,u.score;',
    'SELECT s.id AS sid,e.score FROM t AS s INNER JOIN u AS e ON s.id=e.id WHERE e.score>80 ORDER BY sid,e.score;',
    'SELECT x.*,y.* FROM t x JOIN u y ON x.id=y.id ORDER BY x.id,y.score;',
    'SELECT * FROM t x JOIN u y ON x.id=y.id ORDER BY x.id,y.score;',
    'SELECT DISTINCT x.id,y.score FROM t x JOIN u y ON x.id=y.id ORDER BY x.id,y.score;',
    'SELECT x.name,y.score+1 AS adjusted FROM t x JOIN u y ON x.id=y.id ORDER BY adjusted DESC LIMIT 2 OFFSET 1;',
    'SELECT x.name FROM t x JOIN u y ON x.id=y.id ORDER BY y.score DESC LIMIT 2;',
    'SELECT a.id AS aid,b.id AS bid FROM t a JOIN t b ON a.id<b.id ORDER BY aid,bid;',
    'SELECT x.id AS xid,y.score,z.label FROM t x JOIN u y ON x.id=y.id JOIN v z ON y.id=z.id ORDER BY xid,y.score,z.label;',
    'SELECT x.id AS xid,z.id AS zid FROM t x JOIN empty_table z ON x.id=z.id ORDER BY xid,zid;',
    'SELECT z.id AS zid,x.id AS xid FROM empty_table z JOIN t x ON z.id=x.id ORDER BY zid,xid;',
    'SELECT x.id AS xid,y.id AS yid FROM t x JOIN u y ON 1=0 ORDER BY xid,yid;',
    'SELECT x.id AS xid,y.id AS yid FROM t x JOIN u y ON 1=1 ORDER BY xid,yid;',
    'SELECT name,score FROM t JOIN u ON t.id=u.id ORDER BY name,score;',
    'SELECT x.name,y.score FROM t x JOIN u y ON 1=1 AND x.id=y.id ORDER BY x.name,y.score;',
    'SELECT x.name,y.score FROM t x JOIN u y ON x.id+1=y.id OR x.id=y.id WHERE y.score>=80 ORDER BY x.name,y.score;',
  ];
  for (const sql of queries) {
    const expected = referenceQuery(sql);
    const actual = run(sql);
    equal(actual.success, true);
    equal(actual.results[0].rows, expected.values);
    equal(actual.results[0].columns, expected.columns);
  }
  for (const sql of [
    'SELECT id FROM t JOIN u ON t.id=u.id;',
    'SELECT t.id FROM t JOIN u ON id=1;',
    'SELECT x.id FROM t x JOIN u x ON x.id=x.id;',
    'SELECT t.id FROM t JOIN t ON t.id=t.id;',
    'SELECT x.id FROM t x JOIN u y ON x.id=z.id JOIN v z ON y.id=z.id;',
    'SELECT x.id FROM t x JOIN u y ON x.name=y.id;',
    'SELECT x.id FROM t x JOIN u y ON x.id;',
    'SELECT x.id FROM t x JOIN missing y ON x.id=y.id;',
    'SELECT t.id FROM t x JOIN u y ON x.id=y.id;',
  ]) equal(run(sql).error.type, 'SemanticError');
  for (const sql of ['SELECT * FROM t CROSS JOIN u;', 'SELECT * FROM t NATURAL JOIN u;',
    'SELECT * FROM t JOIN u;', 'SELECT * FROM t JOIN u USING(id);']) equal(run(sql).success, false);
  const compiled = run('SELECT x.*,y.score FROM t x JOIN u y ON 1=1 AND x.id=y.id;', 'compile');
  equal(compiled.ast.joins[0].alias, 'y');
  const plan = compiled.plan.find(node => node.kind === 'NestedLoopJoin');
  equal(plan.children.length, 2);
  equal(plan.output.length, 4);
  equal(plan.preservesRowId, false);
  equal(compiled.optimizedPlan.find(node => node.kind === 'HashJoin').predicate.operator, '=');
  equal(run('SELECT x.id FROM t x JOIN u y ON 1/0=1;').error.type, 'ExecutionError');
  equal(run('SELECT x.id FROM t x JOIN u y ON 1=0 AND 1/0=1;').success, true);
  console.log(`${checks} INNER JOIN assertions passed, including 16 SQLite differential queries`);
} finally { reference.close(); }

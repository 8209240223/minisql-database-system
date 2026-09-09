import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const executable = fileURLToPath(new URL('../bin/minisql_compile.exe', import.meta.url));
let count = 0;
function run(sql) {
  const process = spawnSync(executable, [], { input: sql, encoding: 'utf8', timeout: 5000 });
  assert.ifError(process.error);
  assert.equal(process.signal, null);
  const result = JSON.parse(process.stdout);
  assert.equal(process.status, result.success ? 0 : 1);
  return result;
}
function test(name, body) {
  body();
  count++;
  console.log(`PASS ${name}`);
}
const create = 'CREATE TABLE t(id INT, name VARCHAR, age INT);';
test('empty input', () => assert.deepEqual(run('').plan, []));
test('four statement plans', () => {
  const result = run(create + "INSERT INTO t(age,name,id) VALUES(20,'Tom''s book',1); SELECT name,id,name FROM t WHERE age>18; DELETE FROM t WHERE id=1;");
  assert.equal(result.success, true);
  assert.deepEqual(result.plan.filter(p => p.parent === -1).map(p => p.kind), ['CreateTable', 'Insert', 'Project', 'Delete']);
  const insert = result.plan.find(p => p.kind === 'Insert');
  assert.deepEqual(insert.columnMapping, [2, 1, 0]);
  assert.deepEqual(insert.values, [1, "Tom's book", 20]);
  const project = result.plan.find(p => p.kind === 'Project');
  assert.deepEqual(project.output.map(c => c.columnId), [1, 0, 1]);
  const nodes = new Map(result.plan.map(p => [p.id, p]));
  for (const node of result.plan) {
    for (const id of node.children) {
      assert.equal(nodes.get(id).parent, node.id);
      assert.equal(nodes.get(id).depth, node.depth + 1);
    }
  }
  const deletion = result.plan.find(p => p.kind === 'Delete');
  assert.equal(nodes.get(deletion.children[0]).preservesRowId, true);
  assert.equal(result.stages.executor, 'notImplemented');
});
test('star expands schema and no WHERE omits Filter', () => {
  const result = run(create + 'SELECT * FROM t; DELETE FROM t;');
  assert.equal(result.success, true);
  assert.deepEqual(result.plan.find(p => p.kind === 'Project').output.map(c => c.name), ['id', 'name', 'age']);
  assert.equal(result.plan.some(p => p.kind === 'Filter'), false);
});
test('case insensitive names and types', () => {
  assert.equal(run("create table T(ID int,Name varchar); insert into t(name,id) values('A',1); select ID from t;").success, true);
});
test('signed INT32 boundaries', () => {
  assert.equal(run(create + "INSERT INTO t(id,name,age) VALUES(-2147483648,'',+2147483647); SELECT id FROM t WHERE id=-2147483648;").success, true);
});
for (const [name, sql] of [
  ['missing table', 'SELECT id FROM t;'],
  ['future table not visible', 'SELECT id FROM t;' + create],
  ['duplicate table', create + create],
  ['duplicate CREATE column', 'CREATE TABLE t(id INT,ID INT);'],
  ['missing projection column', create + 'SELECT score FROM t;'],
  ['missing WHERE column', create + 'DELETE FROM t WHERE score=1;'],
  ['mixed comparison', create + "SELECT * FROM t WHERE id='x';"],
  ['WHERE not boolean', create + 'SELECT * FROM t WHERE age;'],
  ['NOT not boolean', create + 'SELECT * FROM t WHERE NOT age;'],
  ['AND not boolean', create + 'SELECT * FROM t WHERE age AND id=1;'],
  ['OR not boolean', create + 'SELECT * FROM t WHERE id=1 OR name;'],
  ['INSERT count mismatch', create + 'INSERT INTO t(id,name,age) VALUES(1);'],
  ['INSERT duplicate target', create + "INSERT INTO t(id,name,ID) VALUES(1,'A',2);"],
  ['INSERT omitted required column', 'CREATE TABLE required(id INT NOT NULL,name VARCHAR); INSERT INTO required(name) VALUES(\'A\');'],
  ['INSERT wrong type', create + "INSERT INTO t(id,name,age) VALUES('A',1,2);"],
  ['positive INT assignment overflow', create + "INSERT INTO t(id,name,age) VALUES(2147483648,'A',2);"],
  ['negative overflow', create + "INSERT INTO t(id,name,age) VALUES(-2147483649,'A',2);"],
]) {
  test(name, () => {
    const result = run(sql);
    assert.equal(result.success, false);
    assert.equal(result.plan, undefined);
    assert.ok(result.error.message.length > 0);
  });
}
test('INT comparison promotes against BIGINT literal', () => {
  assert.equal(run(create + 'SELECT * FROM t WHERE id=2147483648;').success, true);
});
test('deterministic serialization', () => {
  const sql = create + "SELECT name FROM t WHERE age>=18 AND NOT id=2;";
  assert.deepEqual(run(sql), run(sql));
});
test('unknown WHERE column location', () => {
  const result = run(create + '\nSELECT id FROM t\nWHERE score=1;');
  assert.equal(result.error.line, 3);
  assert.equal(result.error.column, 7);
});
test('separate compile requests do not create tables', () => {
  assert.equal(run(create).success, true);
  assert.equal(run('SELECT * FROM t;').success, false);
});
console.log(`${count} semantic/planner regressions passed`);

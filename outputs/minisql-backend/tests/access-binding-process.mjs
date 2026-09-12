import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openSession } from '../scripts/session-process.mjs';

// X25：受权对象、动作与只读判定都来自 C++ 绑定结果，取代旧的 SQL 文本扫描。
// 本契约直接对会话 bindAccess 操作断言，覆盖字符串字面量、别名、派生表、CTE、
// 自连接与按对象动作。
const executable = './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-binding-access-'));
let checks = 0;

function equal(actual, expected, message) {
  assert.deepEqual(actual, expected, message);
  ++checks;
}
function objects(binding) {
  return (binding.objects ?? []).map(object => [object.object, object.action]);
}

const session = await openSession(executable, join(root, 'db.pages'));
try {
  equal((await session.request('execute',
    'CREATE TABLE a(id INT, x INT); CREATE TABLE b(id INT, y INT);')).success, true, 'fixtures created');

  // 字符串字面量里的 FROM/JOIN 不会生成对象（旧的文本扫描会误判）。
  const literal = await session.request('bindAccess', "SELECT id FROM a WHERE 'FROM b' = 'x';");
  equal(literal.bound, true, 'literal query binds');
  equal(objects(literal), [['a', 'select']], 'string literals are not objects');

  // 别名是作用域名，不产生独立对象。
  const alias = await session.request('bindAccess', 'SELECT p.id FROM a AS p WHERE p.x > 0;');
  equal(objects(alias), [['a', 'select']], 'aliases are scope names');

  // 派生表别名不产生对象，内层基表保留。
  const derived = await session.request('bindAccess', 'SELECT d.id FROM (SELECT id FROM a) AS d;');
  equal(objects(derived), [['a', 'select']], 'derived-table alias is excluded');

  // CTE 名不产生对象，只保留基表。
  const cte = await session.request('bindAccess', 'WITH w AS (SELECT id FROM a) SELECT id FROM w;');
  equal(objects(cte), [['a', 'select']], 'CTE names are not persisted objects');

  // 自连接：同一物理表出现两次仍只授权一次对象。
  const selfJoin = await session.request('bindAccess', 'SELECT l.id FROM a AS l JOIN a AS r ON l.id = r.id;');
  equal(objects(selfJoin), [['a', 'select']], 'self join binds one object');

  // 每个对象带自己的动作：目标表 DELETE，子查询表只 SELECT。
  const perObject = await session.request('bindAccess', 'DELETE FROM a WHERE id IN (SELECT id FROM b);');
  equal(perObject.statementAction, 'delete', 'statement action is delete');
  equal(objects(perObject).sort(), [['a', 'delete'], ['b', 'select']].sort(), 'each object carries its own action');

  // 写语句的整体动作可被入口用来做只读兜底。
  const insert = await session.request('bindAccess', 'INSERT INTO a VALUES(1,2);');
  equal(insert.statementAction, 'insert', 'insert statement action');
  const select = await session.request('bindAccess', 'SELECT * FROM a;');
  equal(select.statementAction, 'select', 'select statement action');
  const explain = await session.request('bindAccess', 'EXPLAIN SELECT * FROM a;');
  equal(explain.statementAction, 'select', 'EXPLAIN keeps the read action');

  // 无法闭合的绑定必须 fail-closed（bound=false），调用方据此拒绝。
  const unbound = await session.request('bindAccess', 'SELECT id FROM missing_table;');
  equal(unbound.bound, false, 'unresolved table fails closed');
  equal(unbound.objects.length, 0, 'unbound statement exposes no objects');
} finally {
  await session.close().catch(() => {});
}
console.log(`${checks} binding-based access checks passed`);

import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
// CROSS JOIN 与逗号连接冒烟测试。
// 两者语义相同（笛卡尔积），区别只在写法：CROSS JOIN 显式、逗号连接是 SQL-92 之前的简写。
const db = join(mkdtempSync(join(tmpdir(), 'minisql-cross-')), 'db.pages');
function run(sql, mode = 'execute') {
// 以子进程执行 SQL；mode 传 'compile' 时只编译。
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
// 标准输入喂 SQL。
  assert.ifError(child.error);
// 子进程启动失败要暴露。
  return JSON.parse(child.stdout);
// 返回结构化结果。
}
function rows(sql) {
// 执行查询并断言成功。
  const value = run(sql);
// 执行。
  assert.equal(value.success, true, JSON.stringify(value));
// 必须成功。
  return value.results.at(-1).rows;
// 返回结果行。
}
assert.equal(run("CREATE TABLE a(id INT, x VARCHAR); CREATE TABLE b(id INT, y VARCHAR); INSERT INTO a VALUES(1,'a1'),(2,'a2'); INSERT INTO b VALUES(10,'b1'),(20,'b2'),(30,'b3');").success, true);
// 两张表：a 有两行、b 有三行，笛卡尔积应为 6 行。
assert.deepEqual(rows('SELECT COUNT(*) FROM a CROSS JOIN b;'), [[6]]);
// CROSS JOIN 行数等于两侧行数之积。
assert.deepEqual(rows('SELECT COUNT(*) FROM a, b;'), [[6]]);
// 逗号连接的语义与 CROSS JOIN 完全一致。
assert.deepEqual(rows('SELECT a.id, b.id FROM a CROSS JOIN b ORDER BY a.id, b.id;'),
  [[1,10],[1,20],[1,30],[2,10],[2,20],[2,30]]);
// 逐对组合的顺序与内容都要正确。
assert.deepEqual(rows('SELECT a.id, b.id FROM a, b ORDER BY a.id, b.id;'),
  [[1,10],[1,20],[1,30],[2,10],[2,20],[2,30]]);
// 逗号连接产出同一份结果。
assert.deepEqual(rows('SELECT a.id, b.id FROM a CROSS JOIN b WHERE b.id=20 ORDER BY a.id;'), [[1,20],[2,20]]);
// CROSS JOIN 之后接 WHERE，等价于对笛卡尔积做过滤。
assert.deepEqual(rows('SELECT a.id, b.id FROM a, b WHERE a.id=2 AND b.id=30;'), [[2,30]]);
// 逗号连接配 WHERE 是实际工程中最常见的写法。
assert.deepEqual(rows('SELECT COUNT(*) FROM a CROSS JOIN b CROSS JOIN a c;'), [[12]]);
// 三表连接：2×3×2 = 12 行。
assert.equal(run('SELECT * FROM a CROSS JOIN b ON a.id=b.id;').success, false);
// CROSS JOIN 不允许带 ON，必须明确拒绝而不是静默忽略。
assert.deepEqual(rows('SELECT a.id, b.id FROM a JOIN b ON a.id=b.id;'), []);
// 带 ON 的普通 JOIN 不受影响（这里没有匹配行，所以为空）。
assert.deepEqual(rows('SELECT a.id, b.id FROM a LEFT JOIN b ON a.id=b.id ORDER BY a.id;'), [[1,null],[2,null]]);
// LEFT JOIN 仍按外连接语义补 NULL，说明 CROSS 的改动没有波及外连接。
assert.deepEqual(rows('SELECT a.id, b.id FROM a CROSS JOIN b ORDER BY a.id, b.id LIMIT 3;'), [[1,10],[1,20],[1,30]]);
// CROSS JOIN 可与分页组合。
const compiled = run('SELECT a.id FROM a CROSS JOIN b;', 'compile');
// 编译模式验证 CROSS JOIN 能生成计划。
assert.equal(compiled.success, true);
// 编译必须成功。
assert.equal(compiled.plan[0].kind, 'Project');
// 根节点应是投影。
console.log('12 CROSS JOIN smoke checks passed');

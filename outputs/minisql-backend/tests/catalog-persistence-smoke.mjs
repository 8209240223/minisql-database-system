import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
// 目录持久化回归：覆盖外键与索引共存时的目录重写路径。
//
// 背景：CREATE INDEX 走的是"先插入新行、再删除旧行"的 replace，
// 会把被建索引的那张表的目录行搬到页尾，于是下次打开数据库时该表的槽序发生变化。
// 如果加载时按物理槽序边扫边建表，而子表排在父表之前，子表的外键就找不到父表，
// 整库会被判成 STORAGE_CORRUPTION 而再也打不开。本用例锁死这个场景。
const root = mkdtempSync(join(tmpdir(), 'minisql-catalog-persist-'));
const db = join(root, 'db.pages');
function run(sql) {
// 以子进程执行 SQL；每次调用都会重新打开数据库，因此能真实走到"重新加载目录"的路径。
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
// 启动子进程，标准输入喂 SQL。
  assert.ifError(child.error);
// 子进程启动失败要立刻暴露。
  return JSON.parse(child.stdout);
// 返回引擎的结构化结果。
}
function rows(sql) {
// 执行查询并断言成功。
  const value = run(sql);
// 执行。
  assert.equal(value.success, true, JSON.stringify(value));
// 必须成功，否则说明目录已损坏。
  return value.results.at(-1).rows;
// 返回结果行。
}
assert.equal(run('CREATE TABLE p(id INT PRIMARY KEY, age INT);').success, true);
// 父表：带主键与普通列。
assert.equal(run('CREATE TABLE o(id INT, pid INT, FOREIGN KEY(pid) REFERENCES p(id));').success, true);
// 子表：通过外键引用父表，这是触发缺陷的必要条件。
assert.equal(run('INSERT INTO p VALUES(1,20),(2,30);').success, true);
// 插入父表数据。
assert.equal(run('INSERT INTO o VALUES(1,1),(2,2);').success, true);
// 插入子表数据。
assert.equal(run('CREATE INDEX idx_p_age ON p(age);').success, true);
// 关键动作：对"被外键引用的父表"建索引，迫使父表目录行被搬位。
assert.deepEqual(rows('SELECT id FROM p ORDER BY id;'), [[1],[2]]);
// 重新打开后父表仍可读——缺陷版本这里会报 STORAGE_CORRUPTION。
assert.deepEqual(rows('SELECT id FROM o ORDER BY id;'), [[1],[2]]);
// 重新打开后子表仍可读。
assert.equal(run('INSERT INTO o VALUES(3,1);').success, true);
// 外键语义仍然有效。
assert.equal(run('INSERT INTO o VALUES(4,999);').success, false);
// 违反外键的插入必须被拒绝。
assert.equal(run('DELETE FROM p WHERE id=1;').success, false);
// 删除仍被引用的父行必须被拒绝。
assert.equal(run('CREATE INDEX idx_o_pid ON o(pid);').success, true);
// 对子表建索引一直是可以的，这里作为对照，确保修复没有收窄能力。
assert.deepEqual(rows('SELECT id FROM p ORDER BY id;'), [[1],[2]]);
// 再次重新打开，父表仍可读。
assert.equal(run('CREATE TABLE g(id INT PRIMARY KEY, mgr INT, FOREIGN KEY(mgr) REFERENCES g(id));').success, true);
// 自引用外键：拓扑排序必须能终止而不是陷入死循环。
assert.equal(run('INSERT INTO g VALUES(1,1);').success, true);
// 自引用行可以插入。
assert.deepEqual(rows('SELECT id FROM g;'), [[1]]);
// 自引用表重新打开后仍可读。
console.log('9 catalog persistence checks passed');

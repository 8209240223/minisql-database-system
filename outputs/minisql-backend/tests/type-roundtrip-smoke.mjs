import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
// 类型编解码往返回归：锁死"建表成功但重新打开读不出"这一类静默数据损坏。
//
// 背景：目录把列类型编码成 {typeId, typeParameters} 落盘。编码侧接受的范围
// （如 varcharLength 只要求非零，上限 uint32）必须与解码侧 decodeType 的校验
// 完全一致。曾经出现过解码侧额外收紧到 65535 的情况，导致 VARCHAR(4294967295)
// 建表成功、落盘正常，但下次打开目录时解码失败，整库报 STORAGE_CORRUPTION。
// 本用例对每个类型的边界值做一次"建表 -> 新进程读回"的往返，覆盖两者一致性。
const root = mkdtempSync(join(tmpdir(), 'minisql-type-roundtrip-'));
function run(db, sql) {
// 每次调用都起新进程，因此一定会重新加载目录、走完整的解码路径。
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, 'execute'], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
// 用标准输入喂 SQL。
  assert.ifError(child.error);
// 子进程启动失败要立刻暴露。
  return JSON.parse(child.stdout);
// 返回引擎的结构化结果。
}
const types = [
// 覆盖全部基础类型与两类带参类型的边界。
  'INT', 'BIGINT', 'FLOAT', 'BOOL', 'DATE',
  'VARCHAR(1)', 'VARCHAR(255)', 'VARCHAR(65535)', 'VARCHAR(65536)', 'VARCHAR(4294967295)',
  'DECIMAL(1,0)', 'DECIMAL(10,2)', 'DECIMAL(38,0)', 'DECIMAL(38,38)',
];
let checked = 0;
for (const [index, type] of types.entries()) {
// 每个类型用独立的库文件，避免相互干扰。
  const db = join(root, `t${index}.pages`);
// 库文件路径。
  const created = run(db, `CREATE TABLE t(v ${type});`);
// 建表。
  if (!created.success) continue;
// 建表被拒属于合法行为（例如超出类型上限），不计入往返检查。
  const reopened = run(db, 'SELECT * FROM t;');
// 重新打开并查询：这一步会重新解码列类型，是旧缺陷暴露的位置。
  assert.equal(reopened.success, true,
// 建表成功就必须能读回，否则说明编码接受的范围比解码宽。
    `type ${type} round trip failed: ${JSON.stringify(reopened.error ?? reopened)}`);
// 断言失败时带上类型名，便于定位。
  checked += 1;
// 统计实际完成往返的类型数。
}
assert.ok(checked >= 10, `expected at least 10 round-tripped types, got ${checked}`);
// 保证测试确实覆盖了足够多的类型，而不是因早期失败被跳过。
const oversize = run(join(root, 'oversize.pages'), 'CREATE TABLE t(v DECIMAL(39,0));');
// DECIMAL 精度超上限应被拒绝，而不是建表成功后再读不出。
assert.equal(oversize.success, false);
// 编码侧与解码侧都限制 38，因此这里必须在建表阶段就被拒绝。
const badVarchar = run(join(root, 'bad.pages'), 'CREATE TABLE t(v VARCHAR(0));');
// VARCHAR 长度为零非法。
assert.equal(badVarchar.success, false);
// 必须在建表阶段被拒绝。
console.log(`${checked} type round-trip checks passed`);

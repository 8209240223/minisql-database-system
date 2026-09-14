import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
// LIKE 模式匹配冒烟测试：覆盖通配符、大小写、NULL、组合条件与写语句。
const db = join(mkdtempSync(join(tmpdir(), 'minisql-like-')), 'db.pages');
function run(sql, mode = 'execute') {
// 以子进程方式执行一条 SQL，返回解析后的 JSON 结果。
// mode 传 'compile' 时只编译不执行，用于验证语句能生成计划。
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
// 用标准输入方式喂 SQL，mode 决定 execute 还是 compile。
  assert.ifError(child.error);
// 子进程本身启动失败要立刻暴露。
  return JSON.parse(child.stdout);
// 返回引擎的结构化结果。
}
function rows(sql) {
// 执行查询并断言成功，返回最后一条语句的行集。
  const value = run(sql);
// 执行。
  assert.equal(value.success, true, JSON.stringify(value));
// 查询必须成功。
  return value.results.at(-1).rows;
// 取最后一条语句的结果行。
}
assert.equal(run("CREATE TABLE t(id INT, note VARCHAR); INSERT INTO t VALUES(1,'apple'),(2,'banana'),(3,'avocado'),(4,'cherry'),(5,NULL);").success, true);
// 建表并准备数据：包含 NULL 行用于验证三值逻辑。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE 'a%' ORDER BY id;"), [[1],[3]]);
// 前缀匹配：apple 与 avocado 都命中。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE '%a' ORDER BY id;"), [[2]]);
// 后缀匹配：只有 banana 以 a 结尾（apple/avocado 以 e/o 结尾，cherry 以 y 结尾）。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE '%an%' ORDER BY id;"), [[2]]);
// 包含匹配：banana 含 "an"。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE 'a____' ORDER BY id;"), [[1]]);
// 下划线匹配单个字符：apple 是 a 加四个字符，avocado 长度不符。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE '%' ORDER BY id;"), [[1],[2],[3],[4]]);
// 单个 % 匹配任意非 NULL 字符串，NULL 行不参与。
assert.deepEqual(rows("SELECT id FROM t WHERE note NOT LIKE 'a%' ORDER BY id;"), [[2],[4]]);
// NOT LIKE 取反，NULL 行结果为空不入选。
assert.deepEqual(rows("SELECT id FROM t WHERE note NOT LIKE 'a%' OR note IS NULL ORDER BY id;"), [[2],[4],[5]]);
// 显式补上 IS NULL 才能把空值行纳入。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE 'a%' AND id > 1 ORDER BY id;"), [[3]]);
// LIKE 可与其它条件组合。
assert.deepEqual(rows("SELECT id FROM t WHERE NOT note LIKE 'a%' AND note IS NOT NULL ORDER BY id;"), [[2],[4]]);
// 前缀 NOT 与中缀 NOT LIKE 语义一致。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE 'APPLE';"), []);
// LIKE 大小写敏感，'APPLE' 不匹配 'apple'。
assert.deepEqual(rows("SELECT id FROM t WHERE note NOT LIKE 'zzz%' ORDER BY id;"), [[1],[2],[3],[4]]);
// 模式不匹配时 NOT LIKE 返回全部非空行。
assert.equal(run("SELECT id FROM t WHERE note LIKE 1;").success, false);
// 非字符串操作数应被拒绝而不是静默转换。
assert.equal(run("UPDATE t SET note='apricot' WHERE note LIKE 'app%';").success, true);
// LIKE 在 UPDATE 的 WHERE 里同样可用。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE 'ap%' ORDER BY id;"), [[1]]);
// 更新后 apple 变成 apricot。
assert.equal(run("DELETE FROM t WHERE note LIKE '%y';").success, true);
// LIKE 在 DELETE 的 WHERE 里同样可用：删除 cherry。
assert.deepEqual(rows("SELECT id FROM t WHERE note LIKE '%a%' ORDER BY id;"), [[1],[2],[3]]);
// 删除 cherry 之后，含字母 a 的还剩 apricot(1)、banana(2) 与 avocado(3) 三行。
const compiled = run("SELECT id FROM t WHERE note LIKE 'a%';", 'compile');
// 编译模式验证 LIKE 能生成计划而不是只靠执行期兜底。
assert.equal(compiled.success, true);
// 编译必须成功。
assert.equal(compiled.ast.where.kind, 'Like');
// WHERE 里的 LIKE 应被解析成 Like 节点。
const checkCompiled = run("CREATE TABLE guarded(v VARCHAR, CHECK(v LIKE 'a%'));", 'compile');
// CHECK 约束里的 LIKE：这条路径会走表达式的序列化/反序列化，
// 早期实现只覆盖了执行求值、漏了序列化，导致带 LIKE 的 CHECK 直接报
// "Invalid serialized CHECK expression"。
assert.equal(checkCompiled.success, true);
// 编译必须成功。
assert.equal(checkCompiled.ast.checks[0].kind, 'Like');
// CHECK 表达式应被解析成 Like 节点。
assert.equal(run("CREATE TABLE guarded2(v VARCHAR, CHECK(v LIKE 'a%'));").success, true);
// 真正落盘建表。
assert.equal(run("INSERT INTO guarded2 VALUES('apple');").success, true);
// 满足 CHECK 的值可以插入。
assert.equal(run("INSERT INTO guarded2 VALUES('zzz');").success, false);
// 违反 CHECK 的值必须被拒绝。
assert.equal(run("SELECT * FROM guarded2;").success, true);
// 重新打开数据库：这一步会从磁盘反序列化 CHECK 里的 Like 表达式。
assert.deepEqual(rows("SELECT v FROM guarded2;"), [['apple']]);
// 表内容正确，且带 LIKE 的 CHECK 在重启后依然生效。
console.log('22 LIKE smoke checks passed');

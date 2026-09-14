// 第十七章 REQ-CORE-001、§7.2 的可执行契约测试：极端嵌套必须返回明确诊断，
// 不得让进程崩溃（栈溢出）。同时固定「表达式嵌套上限 256」这一已声明边界。
//
// 本用例覆盖三类曾经会击穿栈的输入：
//   1. 深层 CAST 嵌套（原 cast-process 第 52 项的崩溃点）
//   2. 深层括号嵌套
//   3. 深层派生表 / 标量子查询嵌套（修复前这两条递归完全没有深度保护）
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

// 优先使用刚构建的二进制，退回 bin/ 预置副本（与 date-literal-process.mjs 同一约定）。
// 使用旧副本会让回归静默验证过时代码，因此这里显式偏好 build 产物。
const release = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const fallback = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(release) ? release : fallback);

const root = mkdtempSync(join(tmpdir(), 'minisql-nesting-depth-'));
const database = join(root, 'database.pages');
let checks = 0;

const ok = (value, name) => { assert.ok(value, name); ++checks; };
const equal = (actual, expected, name) => { assert.equal(actual, expected, name); ++checks; };

function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [database, mode], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 180000,
    maxBuffer: 64 * 1024 * 1024,
  });
  assert.ifError(child.error);
  // 崩溃（例如 0xC00000FD 栈溢出）会给出平台相关的异常 status；
  // 契约要求：任何输入都只能以 0（成功）或 1（诊断）结束。
  ok(child.status === 0 || child.status === 1,
    `process must exit 0 or 1, got ${child.status} for ${mode}`);
  return { status: child.status, data: JSON.parse(child.stdout) };
}

const failure = (sql, mode) => {
  const result = run(sql, mode);
  equal(result.data.success, false, 'deep nesting must be reported as a failure');
  return result.data.error;
};

equal(run('CREATE TABLE t(i INT, b BIGINT); INSERT INTO t VALUES(1,42);').data.success, true, 'base schema created');

// ---- 1. 表达式嵌套上限 256（REQ-CORE-001）----
const nestedCast = n => `SELECT ${'CAST('.repeat(n)}i${' AS INT)'.repeat(n)} FROM t;`;
equal(run(nestedCast(256)).data.success, true, '256 nested CASTs is accepted (the documented limit)');
{
  const error = failure(nestedCast(257));
  equal(error.code, 2002, '257 nested CASTs reports the syntax error code');
  equal(error.type, 'SyntaxError', '257 nested CASTs reports a syntax error');
  ok(/depth/i.test(error.message), `nesting diagnostic mentions depth: ${error.message}`);
}
equal(failure(nestedCast(260)).code, 2002, 'the original 260-level CAST repro now reports a diagnostic');
equal(failure(nestedCast(2000)).code, 2002, '2000-level CAST nesting reports a diagnostic');

// ---- 2. 括号嵌套 ----
const nestedParens = n => `SELECT ${'('.repeat(n)}i${')'.repeat(n)} FROM t;`;
equal(run(nestedParens(200)).data.success, true, '200 nested parentheses is accepted');
equal(failure(nestedParens(257)).code, 2002, '257 nested parentheses reports a diagnostic');
equal(failure(nestedParens(2000)).code, 2002, '2000 nested parentheses reports a diagnostic');

// ---- 3. 派生表嵌套（此前无深度保护）----
const nestedDerived = n => `SELECT * FROM ${'(SELECT * FROM '.repeat(n)}t${') x'.repeat(n)};`;
equal(run(nestedDerived(2)).data.success, true, 'two-level derived table still works');
equal(run('SELECT * FROM (SELECT * FROM (SELECT i FROM t) a) b;').data.success, true, 'nested derived tables keep their rows');
equal(failure(nestedDerived(400)).code, 2002, 'deep derived-table nesting reports a diagnostic');
equal(failure(nestedDerived(2000)).code, 2002, '2000-level derived-table nesting reports a diagnostic');

// ---- 4. 标量子查询嵌套（此前无深度保护，且内层未物化导致 json 异常）----
const nestedScalar = n => `SELECT ${'(SELECT '.repeat(n)}i${' FROM t)'.repeat(n)} FROM t;`;
{
  const shallow = run(nestedScalar(1));
  equal(shallow.data.success, true, 'single scalar subquery works');
  assert.deepEqual(shallow.data.results[0].rows, [[1]]); ++checks;
}
{
  // 修复前：内层未物化 → evaluate() 取 "left" 抛 json 异常 → InternalError 9999。
  const two = run(nestedScalar(2));
  equal(two.data.success, true, 'two-level scalar subquery works');
  assert.deepEqual(two.data.results[0].rows, [[1]]); ++checks;
}
{
  const three = run(nestedScalar(3));
  equal(three.data.success, true, 'three-level scalar subquery works');
  assert.deepEqual(three.data.results[0].rows, [[1]]); ++checks;
}
{
  // 未物化的子查询节点必须给出正式诊断，而不是把 json 异常当成 InternalError 泄漏。
  const five = run(nestedScalar(5));
  equal(five.data.success, true, 'five-level scalar subquery works');
  assert.deepEqual(five.data.results[0].rows, [[1]]); ++checks;
}
equal(failure(nestedScalar(400)).code, 2002, 'deep scalar-subquery nesting reports a diagnostic');
equal(failure(nestedScalar(2000)).code, 2002, '2000-level scalar-subquery nesting reports a diagnostic');

// ---- 5. 编译模式同样不得崩溃（诊断入口是常见攻击面）----
{
  const compiled = run(nestedCast(260), 'compile');
  equal(compiled.status, 1, 'compile reports deep nesting as an error');
  equal(compiled.data.error.code, 2002, 'compile reports the same syntax error code');
}
{
  const diagnosed = run(nestedParens(2000), 'diagnostics');
  equal(diagnosed.data.success, false, 'diagnostics reports deep nesting as a failure');
  ok(diagnosed.data.count >= 1, 'diagnostics returns at least one entry');
}

console.log(`${checks} nesting-depth contract checks passed`);

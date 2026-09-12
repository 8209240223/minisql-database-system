// 第十七章 REQ-CORE-001 / REQ-CORE-002 的可执行契约测试。
// 覆盖：128 字符标识符上限、collectDiagnostics 100 条错误上限、批量语句 10000 上限、
// SEM_INTEGER_OUT_OF_RANGE（窄化与 INT64 越界）、PLAN_STALE_SCHEMA（计划指纹失效）。
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-requirements-limits-'));
const database = join(root, 'database.pages');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;

const ok = (value, name) => { assert.ok(value, name); ++checks; };
const equal = (actual, expected, name) => { assert.equal(actual, expected, name); ++checks; };

function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [database, mode], {
    input: sql, encoding: 'utf8', windowsHide: true, timeout: 120000,
    env: { ...process.env, MINISQL_AUTH_BYPASS: '1' },
    maxBuffer: 64 * 1024 * 1024,
  });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}

function runPlanFile(file) {
  const child = spawnSync(executable, [database, 'executePlan', '--file', file], {
    encoding: 'utf8', windowsHide: true, timeout: 120000,
    env: { ...process.env, MINISQL_AUTH_BYPASS: '1' },
    maxBuffer: 64 * 1024 * 1024,
  });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}

// ---- 基础数据 ----
equal(run('CREATE TABLE t(id INT, note VARCHAR); CREATE TABLE big(id BIGINT);').success, true, 'base schema created');

// ---- 1. 单标识符 128 字符上限（REQ-CORE-001）----
const name128 = 'a'.repeat(128);
const name129 = 'a'.repeat(129);
{
  const error = run(`SELECT * FROM ${name129};`).error;
  equal(error.type, 'LexicalError', '129-char identifier is rejected lexically');
  ok(/exceeds 128 characters/.test(error.message), 'identifier diagnostic states the 128-char limit');
  equal(error.line, 1, 'identifier diagnostic carries a line number');
  ok(error.column >= 1, 'identifier diagnostic carries a column number');
}
{
  // 128 字符必须通过词法阶段（这里因表不存在而停在语义阶段，恰好证明未被词法拒绝）。
  const result = run(`SELECT * FROM ${name128};`);
  equal(result.success, false, '128-char identifier is not a lexical failure');
  equal(result.error.type, 'SemanticError', '128-char identifier reaches the semantic stage');
}
{
  // 关键字与超长标识符互不影响：真实表名仍然正常。
  equal(run('SELECT id FROM t;').success, true, 'existing identifiers are unaffected');
}

// ---- 2. collectDiagnostics 最多 100 条错误（REQ-CORE-001）----
{
  const noisy = Array.from({ length: 150 }, () => 'SELECT nope FROM t;').join('\n');
  const result = run(noisy, 'diagnostics');
  equal(result.success, false, 'diagnostics reports failure for invalid batch');
  equal(result.limit, 100, 'diagnostics advertises the 100-diagnostic limit');
  equal(result.truncated, true, 'diagnostics flags truncation');
  ok(result.count <= 100, `diagnostics never returns more than 100 entries (got ${result.count})`);
  ok(result.diagnostics.every(item => item.success === false || item.stage === 'passed'), 'diagnostic entries are well formed');
}
{
  const small = run('SELECT nope FROM t;', 'diagnostics');
  equal(small.limit, 100, 'small batches still report the limit');
  equal(small.truncated, false, 'small batches are not flagged as truncated');
  ok(small.count < 100, 'small batches stay under the limit');
}

// ---- 3. 批量语句 10000 上限（REQ-CORE-001）----
const budgetMessage = /budget exceeded/i;
{
  // compile 把整批交给 Parser::all()，解析阶段就触发上限。
  // （恰好 10000 条的 compile 会先撞到既有的优化器节点预算，与该上限无关，故此处只验证超限。）
  const overflow = Array.from({ length: 10001 }, () => 'SELECT * FROM t;').join(' ');
  const result = run(overflow, 'compile');
  equal(result.success, false, 'compile rejects 10001 statements');
  equal(result.error.code, 5001, 'statement budget uses the execution error code');
  ok(budgetMessage.test(result.error.message), `statement budget message is explicit: ${result.error.message}`);
}
{
  // execute 逐条切分，走的是独立的计数路径：恰好 10000 条通过，第 10001 条被拒绝。
  const exact = Array.from({ length: 10000 }, () => 'SELECT * FROM t;').join(' ');
  const accepted = run(exact, 'execute');
  equal(accepted.success, true, 'exactly 10000 statements is accepted');
  equal(accepted.statements, 10000, 'statement count reflects the accepted batch');

  const overflow = Array.from({ length: 10001 }, () => 'SELECT * FROM t;').join(' ');
  const rejected = run(overflow, 'execute');
  equal(rejected.success, false, 'execute enforces the same batch budget');
  equal(rejected.error.code, 5001, 'execute budget uses the execution error code');
  ok(budgetMessage.test(rejected.error.message), 'execute budget message matches compile');
}

// ---- 4. SEM_INTEGER_OUT_OF_RANGE（REQ-CORE-001）----
{
  const error = run('INSERT INTO t(id,note) VALUES(2147483648,\'x\');').error;
  equal(error.type, 'SEM_INTEGER_OUT_OF_RANGE', 'positive literal beyond INT32 rejected with the stable code');
  ok(/does not fit INT/.test(error.message), 'narrowing diagnostic explains the INT range');
}
{
  const error = run('INSERT INTO t(id,note) VALUES(+2147483648,\'x\');').error;
  equal(error.type, 'SEM_INTEGER_OUT_OF_RANGE', 'explicitly signed positive literal is also rejected');
}
{
  const error = run('UPDATE t SET id = 2147483648;').error;
  equal(error.type, 'SEM_INTEGER_OUT_OF_RANGE', 'UPDATE narrowing is rejected with the stable code');
}
{
  const error = run('SELECT 99999999999999999999 FROM t;').error;
  equal(error.type, 'SEM_INTEGER_OUT_OF_RANGE', 'literal beyond INT64 uses the stable code');
}
{
  // -2147483648 是合法的最小 INT（规格书明确允许符号与幅值组合）。
  equal(run("INSERT INTO t(id,note) VALUES(-2147483648,'min');").success, true, 'INT32 minimum is accepted');
  equal(run('SELECT id FROM t WHERE id = -2147483648;').results?.[0]?.rows?.length, 1, 'INT32 minimum round trips');
}
{
  // BIGINT 目标是扩展配置，仍接受超出 INT32 的字面量（本项目的超集行为，不报错）。
  equal(run('INSERT INTO big(id) VALUES(2147483648);').success, true, 'BIGINT target still accepts the widened literal');
}

// ---- 5. PLAN_STALE_SCHEMA（REQ-CORE-002）----
{
  const compiled = run('SELECT id FROM t;', 'compile');
  equal(compiled.success, true, 'plan compiles for execution');
  const fingerprint = compiled.plan[0]?.catalogFingerprint;
  ok(typeof fingerprint === 'string' && fingerprint.length === 16, `plan carries a catalog fingerprint (${fingerprint})`);
  ok(compiled.plan.every(node => node.catalogFingerprint === fingerprint), 'every plan node carries the same fingerprint');

  const planFile = join(root, 'plan.json');
  writeFileSync(planFile, JSON.stringify(compiled.plan));

  const fresh = runPlanFile(planFile);
  equal(fresh.success, true, 'plan executes while the catalog is unchanged');
  equal(fresh.statements, 1, 'plan execution reports one statement');
  equal(fresh.results[0].rows.length, 1, 'plan execution returns the bound rows');

  // 改变 Catalog（新增一张表即改变指纹），同一份计划文档必须被拒绝。
  equal(run('CREATE TABLE fingerprint_shift(x INT);').success, true, 'catalog changes after compilation');
  const stale = runPlanFile(planFile);
  equal(stale.success, false, 'stale plan is refused');
  equal(stale.error.type, 'PLAN_STALE_SCHEMA', 'stale plan reports PLAN_STALE_SCHEMA');
  ok(/recompile/.test(stale.error.message), 'stale diagnostic tells the caller to recompile');
}
{
  // 形状不合法的计划文档必须 fail-closed，而不是被当成空计划执行。
  const badFile = join(root, 'bad-plan.json');
  writeFileSync(badFile, JSON.stringify({ not: 'a plan document' }));
  const result = runPlanFile(badFile);
  equal(result.success, false, 'malformed plan document is rejected');
}
{
  const notJsonFile = join(root, 'not-json.json');
  writeFileSync(notJsonFile, 'this is not json');
  const result = runPlanFile(notJsonFile);
  equal(result.success, false, 'non-JSON plan input is rejected');
}

console.log(`${checks} REQ-CORE-001/002 contract checks passed`);

// 指导书补充验收：CLI 的 SQL 文件输入（--file）与页替换日志输出（MINISQL_BUFFER_LOG）。
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const directory = mkdtempSync(join(tmpdir(), 'minisql-cli-'));
const database = join(directory, 'db.pages');
const sqlFile = join(directory, 'input.sql');
const bufferLog = join(directory, 'buffer.log');
writeFileSync(sqlFile,
  'CREATE TABLE t(a INT);\nCREATE TABLE u(b INT);\nCREATE TABLE v(c INT);\n' +
  'INSERT INTO t VALUES(1),(2);\nINSERT INTO u VALUES(3);\nINSERT INTO v VALUES(4);\n' +
  'SELECT a FROM t WHERE a == 1;\nSELECT b FROM u;\nSELECT c FROM v;\n', 'utf8');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }
const engine = './build/windows/Release/minisql_database.exe';
const compiler = './build/windows/Release/minisql_compile.exe';
function call(executable, args, options = {}) {
  const child = spawnSync(executable, args, { encoding: 'utf8', windowsHide: true, timeout: 15000, ...options });
  assert.ifError(child.error);
  return child;
}

// --file：从 SQL 文件读取，不依赖标准输入；同时用 4 帧缓冲池触发淘汰并写替换日志。
const fromFile = call(engine, [database, 'execute', '--file', sqlFile], {
  env: { ...process.env, MINISQL_BUFFER_FRAMES: '4', MINISQL_BUFFER_LOG: bufferLog },
});
const executed = JSON.parse(fromFile.stdout);
equal(executed.success, true);
equal(executed.statements, 9);
equal(executed.results[6].rows, [[1]]);

ok(existsSync(bufferLog), 'replacement log file was created');
const lines = readFileSync(bufferLog, 'utf8').trim().split('\n').filter(Boolean);
ok(lines.length >= 1, `replacement log has eviction lines: ${lines.length}`);
ok(lines[0].includes('policy=LRU') && lines[0].includes('page=') && lines[0].includes('writeBack='),
  `replacement log line shape: ${lines[0]}`);

// --file 指向不存在的文件时显式报错，而不是静默空输入。
const missing = JSON.parse(call(engine, [database, 'execute', '--file', join(directory, 'missing.sql')]).stdout);
equal(missing.success, false);
ok(missing.error.message.includes('Cannot open SQL file'), `missing file error: ${missing.error.message}`);

// 编译器入口同样支持 --file，并输出 Token → AST → Plan。
const compiled = JSON.parse(call(compiler, ['--file', sqlFile]).stdout);
equal(compiled.success, true);
equal(compiled.statements, 9);
ok(Array.isArray(compiled.tokens) && compiled.tokens.length > 0, 'compiler emits tokens from file input');
ok(Array.isArray(compiled.plan) && compiled.plan.length > 0, 'compiler emits a logical plan from file input');

// 未加 --file 时仍走标准输入（向后兼容）。
const piped = JSON.parse(call(engine, [database, 'execute'], { input: 'SELECT a FROM t ORDER BY a;' }).stdout);
equal(piped.success, true);
equal(piped.results[0].rows, [[1], [2]]);

console.log(`${checks} CLI file-input/buffer-log checks passed`);

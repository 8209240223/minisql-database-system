// 指导书/课件补充验收：`==` 等价 `=`、`<>` 等价 `!=`，以及语义错误消息形态。
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-dialect-')), 'db.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

equal(run("CREATE TABLE t(id INT, name VARCHAR, age INT); INSERT INTO t VALUES(1,'Alice',20),(2,'Tom',19);").success, true);

// `==` 等价 `=`（课件第 11 页列出的多字符运算符）。
equal(run('SELECT id FROM t WHERE age == 20;').results[0].rows, [[1]]);
equal(run("SELECT id FROM t WHERE age == 20 AND name == 'Tom';").results[0].rows, []);
// `<>` 等价 `!=`（标准 SQL 不等号）。
equal(run('SELECT id FROM t WHERE age <> 20;').results[0].rows, [[2]]);
equal(run('SELECT id FROM t WHERE age <> 20 OR age <> 19;').results[0].rows, [[1], [2]]);
// 词素保留源码原文，语义按规范算子处理。
const compiled = run('SELECT id FROM t WHERE age == 20;', 'compile');
equal(compiled.success, true);
ok(compiled.tokens.some(token => token.type === 'OPERATOR' && token.text === '=='), 'lexeme keeps the source spelling');

// 语义错误消息：列不存在时给出表名。
const missing = run('SELECT score FROM t;', 'compile');
equal(missing.error.code, 2003);
ok(missing.error.message.includes("does not exist in table 't'"), `column message names the table: ${missing.error.message}`);

// 算术类型错误：定位到运算符并给出两侧类型。
const arithmetic = run("SELECT id FROM t WHERE age + 'x' > 20;", 'compile');
equal(arithmetic.error.code, 2003);
equal(arithmetic.error.message, "operator '+' cannot be applied to INT and VARCHAR");

// INSERT 多列类型不匹配：一次报出全部不匹配列。
const insert = run("INSERT INTO t(id,name) VALUES ('Alice', 1);", 'compile');
equal(insert.error.code, 2003);
ok(insert.error.message.includes('t.id') && insert.error.message.includes('t.name'),
  `insert reports every mismatched column: ${insert.error.message}`);

console.log(`${checks} SQL dialect/diagnostic checks passed`);

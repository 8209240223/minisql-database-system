import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-diagnostics-')), 'db.pages');
function run(sql, mode = 'diagnostics') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
const mixed = run('CREATE TABLE t(id INT); SELECT missing FROM t; SELECT * FROM missing; INSERT INTO t VALUES(1);');
equal(mixed.success, false);
equal(mixed.count, 4);
equal(mixed.diagnostics[0].success, true);
equal(mixed.diagnostics[1].stage, 'semantic');
equal(mixed.diagnostics[2].stage, 'semantic');
equal(mixed.diagnostics[3].success, true);
equal(run('SELECT * FROM t;', 'execute').success, false);
const syntax = run('SELECT FROM t; SELECT WHERE;');
equal(syntax.success, false);
equal(syntax.count, 2);
equal(syntax.diagnostics.every(item => item.stage === 'parser'), true);
const valid = run('CREATE TABLE a(id INT); INSERT INTO a VALUES(1); SELECT * FROM a;');
equal(valid.success, true);
equal(valid.count, 3);
const lexical = run("SELECT 'unterminated");
equal(lexical.success, false);
equal(lexical.diagnostics[0].stage, 'lexer');
console.log(`${checks} batch diagnostics checks passed`);

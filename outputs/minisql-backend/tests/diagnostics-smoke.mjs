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

// X12: recovery-mode tokenizer reports EVERY lexical error in the batch (not
// just the first) and still parses the remaining valid statements.
const multiLex = run('SELECT @@ @; CREATE TABLE a(id INT); INSERT INTO a VALUES(1); SELECT * FROM a;');
equal(multiLex.count, 6);
const lexerStages = multiLex.diagnostics.filter(d => d.stage === 'lexer');
equal(lexerStages.length, 2); // two '@' errors are both reported
equal(lexerStages.every(d => d.recoverable === true && d.statementIndex === 0), true);
equal(multiLex.diagnostics.every(d => Number.isInteger(d.endLine) && Number.isInteger(d.endColumn)), true);
// the valid statements after the lexical errors still succeed
equal(multiLex.diagnostics.filter(d => d.success === true).length, 3);

// X12 (clause-level): one SELECT with two broken clauses reports BOTH syntax
// errors within the same statement, not just the first.
const clause = run('SELECT * FROM t WHERE = GROUP BY x ORDER BY ;');
equal(clause.count, 2);
equal(clause.diagnostics.every(d => d.stage === 'parser' && d.statementIndex === 0), true);
equal(clause.diagnostics.length, 2);
equal(run('CREATE TABLE people(id INT);', 'execute').success, true);
const typo = run('SELECT * FROM peopl;');
assert.match(typo.diagnostics[0].suggestion ?? '', /people/); ++checks;

console.log(`${checks} batch diagnostics checks passed`);

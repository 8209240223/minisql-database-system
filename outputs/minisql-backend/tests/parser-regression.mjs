import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
const exe=new URL('../bin/minisql_compile.exe',import.meta.url);
import {fileURLToPath} from 'node:url';
function run(sql){const p=spawnSync(fileURLToPath(exe),['--parse-only'],{input:sql,encoding:'utf8',timeout:5000});assert(!p.error,String(p.error));return JSON.parse(p.stdout);}
assert.equal(run('').success,true);
assert.equal(run('SELECT a FROM t;').ast.kind,'Select');
assert.equal(run('SELECT a FROM t WHERE a=1 OR b=2 AND c=3;').ast.where.right.value,'AND');
assert.equal(run('DELETE FROM t WHERE NOT a=1 AND b=2;').ast.where.left.value,'NOT');
assert.equal(run('CREATE TABLE t(a INT); INSERT INTO t(a) VALUES(1); SELECT * FROM t; DELETE FROM t;').statements,4);
for(const sql of ['SELECT a FROM t','CREATE TABLE t(a nonsense);','SELECT a FROM t WHERE a+;','SELECT a FROM t WHERE a=1=2;','SELECT @;'])assert.equal(run(sql).success,false,sql);
assert.equal(run("SELECT 'unclosed").error.column,8);
assert.equal(run('SELECT a FROM t WHERE '+ 'NOT '.repeat(300)+'a=1;').success,false);
assert.equal(run('INSERT INTO t(a) VALUES(1),(2+3),(DEFAULT);').ast.valueRows.length,3);
assert.equal(run('INSERT INTO t(a) VALUES(1),();').success,false);
assert.equal(run('INSERT INTO t(a) VALUES(1),;').success,false);
assert.deepEqual(run('BEGIN TRANSACTION; COMMIT; ROLLBACK;').ast.map(node=>node.kind),['Begin','Commit','Rollback']);
assert.equal(run('BEGIN bad;').success,false);
assert.equal(run('COMMIT').success,false);
// Verify new-style error messages carry actual-token context (ee77006 + 7dc0736)
const err1 = run('CREATE UNIQUE TABLE t(a INT);');
assert.equal(err1.success, false);
assert.match(err1.error.message, /Expected INDEX after UNIQUE but found/);
const err2 = run('SELECT * FROM t WHERE EXISTS (NOT);');
assert.equal(err2.success, false);
assert.match(err2.error.message, /EXISTS requires a SELECT subquery but found/);
const err3 = run('SELECT SUM(*) FROM t;');
assert.equal(err3.success, false);
assert.match(err3.error.message, /Only COUNT accepts '\*' but found/);
// Verify business-constraint errors carry column/constraint names (dc090a0)
const err4 = run('CREATE TABLE t(a INT PRIMARY KEY PRIMARY KEY);');
assert.equal(err4.success, false);
assert.match(err4.error.message, /Duplicate PRIMARY KEY on column 'a'/);
const err5 = run('CREATE TABLE t(a INT NULL PRIMARY KEY);');
assert.equal(err5.success, false);
assert.match(err5.error.message, /PRIMARY KEY on column 'a' cannot declare NULL/);
const err6 = run('SELECT * AS x FROM t;');
assert.equal(err6.success, false);
assert.match(err6.error.message, /Wildcard '\*' cannot have an alias/);
// Verify lexer error messages carry actual character context (lexer.cpp)
const lex1 = run('SELECT @;');
assert.equal(lex1.success, false);
assert.match(lex1.error.message, /Illegal character.*'@'/);
assert.equal(lex1.error.code, 2001); // Lexical
const lex2 = run('SELECT a==b;');
assert.equal(lex2.success, false);
assert.match(lex2.error.message, /Unsupported comparison operator '=='/);
const lex3 = run('SELECT 1.2.3+;');
assert.equal(lex3.success, false);
assert.match(lex3.error.message, /Unsupported numeric literal.*followed by '\.'/);
// Verify the remaining lexer error paths carry actual context (lexer.cpp)
const lex4 = run('SELECT /* unterminated');
assert.equal(lex4.success, false);
assert.match(lex4.error.message, /Unterminated block comment.*expected '\*\/'/);
assert.equal(lex4.error.code, 2001); // Lexical
const lex5 = run('SELECT 1.');
assert.equal(lex5.success, false);
assert.match(lex5.error.message, /Malformed DECIMAL literal.*expected digits after '\.'.*end of input/);
const lex6 = run('SELECT 1e');
assert.equal(lex6.success, false);
assert.match(lex6.error.message, /Malformed FLOAT literal.*expected digits after exponent.*end of input/);
const lex7 = run("SELECT 'a\nb';");
assert.equal(lex7.success, false);
assert.match(lex7.error.message, /Newline in string literal.*use '\\n' escape instead \(found/);
assert.equal(lex7.error.code, 2001); // Lexical
const lex8 = run("SELECT 'unterminated");
assert.equal(lex8.success, false);
assert.match(lex8.error.message, /Unterminated string literal.*expected closing '''/);
const lex9 = run('SELECT .5;');
assert.equal(lex9.success, false);
assert.match(lex9.error.message, /Unsupported numeric literal starting with '\.': use '0\.' prefix/);
const lex10 = run('SELECT a<>b;');
assert.equal(lex10.success, false);
assert.match(lex10.error.message, /Unsupported comparison operator '<>'/);
// Verify EXISTS / scalar-subquery recursion is depth-limited (parser.cpp)
const deepExists = 'SELECT * FROM t WHERE ' + 'EXISTS(SELECT 1 FROM u WHERE '.repeat(300) + 'EXISTS(SELECT 1)' + ')'.repeat(300) + ';';
assert.equal(run(deepExists).success, false);
const deepScalar = 'SELECT ' + '(SELECT '.repeat(300) + '1' + ')'.repeat(300) + ' FROM t;';
assert.equal(run(deepScalar).success, false);
console.log('55 parser/lexer regression checks passed (including 16 error-message format checks)');

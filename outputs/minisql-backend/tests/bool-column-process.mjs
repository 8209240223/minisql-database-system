import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/bool-column-', import.meta.url)));
const file = join(directory, 'database.pages');
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
let checks = 0;
function equal(a, b) { assert.deepEqual(a, b); ++checks; }
function run(sql, mode = 'execute') { const result = invoke(executable, [file, mode], sql); assert.ok(!result.category, JSON.stringify(result)); return result.data; }
function query(sql) { const result = run(sql); equal(result.success, true); return result.results.at(-1).rows; }
equal(run('CREATE TABLE flags(id INT PRIMARY KEY,b BOOL,required BOOL NOT NULL DEFAULT TRUE);').success, true);
equal(run('INSERT INTO flags(id,b) VALUES(1,TRUE),(2,false),(3,NULL);').success, true);
equal(query('SELECT * FROM flags ORDER BY id;'), [[1,true,true],[2,false,true],[3,null,true]]);
equal(query('SELECT id FROM flags WHERE b ORDER BY id;'), [[1]]);
equal(query('SELECT id FROM flags WHERE NOT b ORDER BY id;'), [[2]]);
equal(query('SELECT b,NOT b,b AND TRUE,b AND FALSE,b OR TRUE,b OR FALSE,b IS NULL FROM flags ORDER BY id;'),
  [[true,false,true,false,true,true,false],[false,true,false,false,true,false,false],[null,null,null,false,true,null,true]]);
equal(query('SELECT b,COUNT(*) FROM flags GROUP BY b HAVING COUNT(*)>0 ORDER BY b;'), [[false,1],[true,1],[null,1]]);
equal(query('SELECT COUNT(b),MIN(b),MAX(b) FROM flags;'), [[2,false,true]]);
equal(query('SELECT DISTINCT b FROM flags ORDER BY b NULLS FIRST;'), [[null],[false],[true]]);
const truth = [];
for (const a of [true,false,null]) for (const b of [true,false,null])
  truth.push([a,b,a === false || b === false ? false : a === null || b === null ? null : true,
    a === true || b === true ? true : a === null || b === null ? null : false]);
equal(query('SELECT a.b,z.b,a.b AND z.b,a.b OR z.b FROM flags a JOIN flags z ON TRUE ORDER BY a.id,z.id;'), truth);
equal(run('UPDATE flags SET b=NOT b,required=id>1;').success, true);
equal(query('SELECT * FROM flags ORDER BY id;'), [[1,false,false],[2,true,true],[3,null,true]]);
equal(run('UPDATE flags SET required=DEFAULT;').success, true);
equal(query("SELECT CAST(b AS VARCHAR),CAST(CAST(b AS VARCHAR) AS BOOL),CAST(NULL AS BOOL) FROM flags ORDER BY id;"), [['FALSE',false,null],['TRUE',true,null],[null,null,null]]);
equal(query("SELECT CAST('tRuE' AS BOOL),CAST('FALSE' AS BOOL),CAST(TRUE AS BOOL) FROM flags WHERE id=1;"), [[true,false,true]]);
for (const sql of [
  'INSERT INTO flags(id,b) VALUES(4,1);', "INSERT INTO flags(id,b) VALUES(4,'true');",
  'UPDATE flags SET b=0;', 'UPDATE flags SET required=NULL;', 'SELECT b+1 FROM flags;',
  'SELECT b=1 FROM flags;', 'SELECT SUM(b) FROM flags;', 'SELECT AVG(b) FROM flags;',
  'SELECT CAST(1 AS BOOL) FROM flags;', 'SELECT CAST(TRUE AS INT) FROM flags;',
  'SELECT CAST(0.0 AS BOOL) FROM flags;', 'SELECT CAST(TRUE AS DECIMAL(3,0)) FROM flags;',
  'CREATE TABLE bad(v BOOL DEFAULT 1);', "CREATE TABLE bad(v BOOL DEFAULT 'FALSE');",
  'CREATE TABLE bad(v BOOL NOT NULL DEFAULT NULL);'
]) { const before = readFileSync(file); equal(run(sql).error.code, 2003); equal(readFileSync(file), before); }
for (const value of ['1','0','',' true','false ','trueX','yes']) {
  equal(run(`SELECT CAST('${value}' AS BOOL) FROM flags;`).error.code, 5001);
}
equal(run('CREATE TABLE parent(b BOOL PRIMARY KEY); INSERT INTO parent VALUES(TRUE),(FALSE); CREATE TABLE child(b BOOL REFERENCES parent(b)); INSERT INTO child VALUES(TRUE),(NULL);').success, true);
equal(run('INSERT INTO parent VALUES(TRUE);').error.code, 5001);
equal(run('DELETE FROM parent WHERE b;').error.code, 5001);
equal(query('SELECT f.id,p.b FROM flags f LEFT JOIN parent p ON f.b=p.b ORDER BY f.id;'), [[1,false],[2,true],[3,null]]);
equal(run('CREATE TABLE checked(b BOOL CHECK(b)); INSERT INTO checked VALUES(TRUE),(NULL);').success, true);
equal(run('INSERT INTO checked VALUES(FALSE);').error.code, 5001);
equal(run("CREATE TABLE cast_check(s VARCHAR CHECK(CAST(s AS BOOL))); INSERT INTO cast_check VALUES('true');").success, true);
equal(run("INSERT INTO cast_check VALUES('false');").error.code, 5001);
equal(query('SELECT * FROM cast_check;'), [['true']]);
equal(run('CREATE TABLE unique_bool(b BOOL UNIQUE); INSERT INTO unique_bool VALUES(FALSE),(TRUE),(NULL),(NULL);').success, true);
equal(run('INSERT INTO unique_bool VALUES(FALSE);').error.code, 5001);
const atomic = readFileSync(file);
equal(run("INSERT INTO flags(id,b) VALUES(4,TRUE),(5,CAST('bad' AS BOOL));").error.code,5001);
equal(readFileSync(file),atomic);
equal(run("BEGIN; UPDATE flags SET b=FALSE; INSERT INTO flags(id,b) VALUES(4,CAST('bad' AS BOOL)); COMMIT;").success,false);
equal(readFileSync(file),atomic);
const before = readFileSync(file);
equal(run('BEGIN; UPDATE flags SET b=TRUE; CREATE TABLE undone(v BOOL); ROLLBACK;').success, true);
equal(readFileSync(file), before);
equal(run('SELECT * FROM undone;').success, false);
equal(run('BEGIN; UPDATE flags SET b=FALSE WHERE id=3; COMMIT;').success, true);
equal(query('SELECT b FROM flags WHERE id=3;'), [[false]]);
const compiled = run('CREATE TABLE compile_only(v BOOL DEFAULT TRUE); SELECT NOT v FROM compile_only;', 'compile');
equal(compiled.success, true); equal(compiled.stages.executor, 'notRun');
equal(compiled.plan.find(node=>node.kind==='Project').output.map(column=>[column.type,column.nullable]),[['bool',true]]);
const aggregatePlan=run('SELECT COUNT(b),MIN(b),MAX(b) FROM flags;','compile').plan.find(node=>node.kind==='Aggregate');
equal(aggregatePlan.output.map(column=>[column.type,column.nullable]),[['bigint',false],['bool',true],['bool',true]]);
equal(run('SELECT * FROM compile_only;').success, false);
equal(run('CREATE TABLE contextual(bool BOOL); INSERT INTO contextual VALUES(TRUE);').success, true);
equal(query('SELECT bool FROM contextual;'), [[true]]);
console.log(`${checks} BOOL column persistence checks passed`);

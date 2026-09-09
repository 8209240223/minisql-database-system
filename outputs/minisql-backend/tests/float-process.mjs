import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const database = join(mkdtempSync(join(tmpdir(), 'minisql-float-')), 'database.pages');
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
let checks = 0;
function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [database, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  assert.ok(child.status === 0 || child.status === 1, child.stderr);
  return JSON.parse(child.stdout);
}
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function query(sql) { const response = run(sql); assert.equal(response.success, true, JSON.stringify(response)); return response.results.at(-1); }
function rejected(sql) { assert.equal(run(sql).success, false, sql); ++checks; }

equal(run('CREATE TABLE f(id INT PRIMARY KEY,v FLOAT,n FLOAT,note VARCHAR);').success, true);
equal(run("INSERT INTO f VALUES(1,1.5,1e2,'a'),(2,-2.25,2.5E-1,'b'),(3,NULL,NULL,'c');").success, true);
equal(query('SELECT id,v,n FROM f ORDER BY id;').rows, [[1,1.5,100],[2,-2.25,0.25],[3,null,null]]);
equal(query('SELECT CAST(v AS FLOAT)+CAST(n AS FLOAT),v*n FROM f WHERE id=1;').rows, [[101.5,150]]);
equal(query("SELECT CAST('3.5e1' AS FLOAT),CAST(3 AS FLOAT),CAST(v AS VARCHAR) FROM f WHERE id=1;").rows, [[35,3,'1.5']]);
equal(query('SELECT SUM(v),AVG(v),MIN(v),MAX(v),COUNT(v) FROM f;').rows, [[-0.75,-0.375,-2.25,1.5,2]]);
equal(query('SELECT DISTINCT v FROM f ORDER BY v NULLS LAST;').rows, [[-2.25],[1.5],[null]]);
equal(run('UPDATE f SET v=CAST(10 AS FLOAT) WHERE id=2;').success, true);
equal(query('SELECT id,v FROM f ORDER BY id;').rows, [[1,1.5],[2,10],[3,null]]);
equal(query('SELECT v FROM f ORDER BY v DESC NULLS FIRST;').rows, [[null],[10],[1.5]]);
rejected('SELECT v+1 FROM f;');
rejected('SELECT v+1.5 FROM f;');
rejected("INSERT INTO f VALUES(4,1e309,1,'x');");
rejected("INSERT INTO f VALUES(4,CAST('NaN' AS FLOAT),1,'x');");
rejected("INSERT INTO f VALUES(4,CAST('Infinity' AS FLOAT),1,'x');");
rejected('SELECT CAST(v AS INT) FROM f WHERE id=1;');
rejected('SELECT CAST(1e308 AS FLOAT)*CAST(1e308 AS FLOAT);');
rejected('SELECT CAST(1 AS FLOAT)/CAST(0 AS FLOAT);');
rejected('CREATE TABLE bad(v FLOAT DEFAULT 1e309);');
equal(query('SELECT id,v,n FROM f ORDER BY id;').rows, [[1,1.5,100],[2,10,0.25],[3,null,null]]);
console.log(`${checks} FLOAT SQL, type, aggregate, CAST and persistence checks passed`);

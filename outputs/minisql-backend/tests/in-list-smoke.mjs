import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const db=join(mkdtempSync(join(tmpdir(),'minisql-in-')),'db.pages');
function run(sql) {
  const child=spawnSync('./build/windows/Release/minisql_database.exe',[db,'execute'],{input:sql,encoding:'utf8',windowsHide:true,timeout:15000});
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
function rows(sql) { const value=run(sql);assert.equal(value.success,true,JSON.stringify(value));return value.results.at(-1).rows; }
assert.equal(run('CREATE TABLE t(id INT); INSERT INTO t VALUES(1),(2),(NULL);').success,true);
assert.deepEqual(rows('SELECT id IN(1,NULL),id NOT IN(1,NULL) FROM t ORDER BY id NULLS LAST;'),[[true,false],[null,null],[null,null]]);
assert.deepEqual(rows('SELECT id FROM t WHERE id iN(1,2,2) ORDER BY id;'),[[1],[2]]);
assert.deepEqual(rows('SELECT id FROM t WHERE id NoT In(2,3);'),[[1]]);
assert.deepEqual(rows('SELECT id FROM t WHERE NOT id IN(2,3) AND id>0;'),[[1]]);
assert.deepEqual(rows('SELECT COUNT(*) FROM t WHERE id IN(1,1+1);'),[[2]]);
assert.equal(run('SELECT * FROM t WHERE id IN();').success,false);
assert.equal(run("SELECT * FROM t WHERE id IN('bad');").success,false);
// IN (subquery) is supported via semi-join (X09); UNKNOWN membership filters the row out.
assert.deepEqual(rows('SELECT id FROM t WHERE id IN(SELECT id FROM t) ORDER BY id;'),[[1],[2]]);
assert.equal(run('UPDATE t SET id=3 WHERE id IN(1,2);').success,true);
assert.deepEqual(rows('SELECT id FROM t WHERE id IN(3);'),[[3],[3]]);
assert.equal(run('DELETE FROM t WHERE id IN(3);').success,true);
assert.deepEqual(rows('SELECT * FROM t;'),[[null]]);
console.log('13 IN-list smoke checks passed');

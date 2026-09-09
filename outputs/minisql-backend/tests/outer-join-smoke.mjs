import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const executable = process.env.MINISQL_DATABASE_EXE ?? fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const database = join(mkdtempSync(join(tmpdir(), 'minisql-outer-')), 'database.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync(executable, [database, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  assert.equal(child.status, 0, child.stdout + child.stderr);
  const result = JSON.parse(child.stdout);
  assert.equal(result.success, true, child.stdout);
  return result;
}
const rows = sql => run(sql).results.at(-1).rows;
run('CREATE TABLE l(id INT NOT NULL); CREATE TABLE r(id INT NOT NULL);');
assert.deepEqual(rows('SELECT * FROM l FULL JOIN r ON l.id=r.id;'), []);
run('INSERT INTO r VALUES(2),(3);');
assert.deepEqual(rows('SELECT * FROM l FULL OUTER JOIN r ON l.id=r.id ORDER BY r.id;'), [[null,2],[null,3]]);
assert.deepEqual(rows('SELECT * FROM l RIGHT JOIN r ON l.id=r.id ORDER BY r.id;'), [[null,2],[null,3]]);
run('INSERT INTO l VALUES(1),(2),(2);');
assert.deepEqual(rows('SELECT * FROM l FULL JOIN r ON l.id=r.id ORDER BY l.id NULLS LAST,r.id;'), [[1,null],[2,2],[2,2],[null,3]]);
assert.deepEqual(rows('SELECT COUNT(*),COUNT(l.id),COUNT(r.id) FROM l FULL JOIN r ON l.id=r.id;'), [[4,3,3]]);
assert.deepEqual(rows('SELECT r.id FROM l FULL JOIN r ON l.id=r.id WHERE l.id IS NULL;'), [[3]]);
const compiled = run('SELECT * FROM l FULL JOIN r ON l.id=r.id;', 'compile');
assert.ok(compiled.plan.some(node => node.kind === 'FullJoin'));
run('DELETE FROM r;');
assert.deepEqual(rows('SELECT * FROM l FULL JOIN r ON l.id=r.id ORDER BY l.id;'), [[1,null],[2,null],[2,null]]);
assert.deepEqual(rows('SELECT * FROM l RIGHT JOIN r ON l.id=r.id;'), []);
console.log('9 outer-join smoke checks passed');

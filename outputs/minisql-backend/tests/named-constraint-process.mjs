import { mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { invoke } from './fuzz-process.mjs';
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/named-', import.meta.url)));
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const file = join(directory, 'database.pages');
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function run(sql, mode = 'execute') {
  const result = invoke(executable, [file, mode], sql);
  assert.ok(!result.category, JSON.stringify(result));return result.data;
}
function rejected(sql, name) {
  const result = run(sql);equal(result.success, false);
  assert.ok(result.error.message.includes(`[${name}]`), JSON.stringify(result));++checks;
}
equal(run('CREATE TABLE parent(id INT CONSTRAINT pk_parent PRIMARY KEY); INSERT INTO parent VALUES(1);').success, true);
equal(run('CREATE TABLE child(id INT CONSTRAINT pk_child PRIMARY KEY,pid INT CONSTRAINT fk_parent REFERENCES parent(id),score INT CONSTRAINT nn_score NOT NULL CONSTRAINT ck_score CHECK(score>=0),tag VARCHAR CONSTRAINT uq_tag UNIQUE);').success, true);
equal(run("INSERT INTO child VALUES(1,1,10,'first');").success, true);
rejected("INSERT INTO child VALUES(1,1,10,'other');", 'pk_child');
rejected("INSERT INTO child VALUES(2,1,10,'first');", 'uq_tag');
rejected("INSERT INTO child VALUES(2,99,10,'other');", 'fk_parent');
rejected("INSERT INTO child VALUES(2,1,-1,'other');", 'ck_score');
rejected("INSERT INTO child VALUES(2,1,NULL,'other');", 'nn_score');
rejected('UPDATE child SET score=CAST(NULL AS INT);', 'nn_score');
rejected("INSERT INTO child(id,pid,tag) VALUES(2,1,'other');", 'nn_score');
rejected('DELETE FROM parent;', 'fk_parent');
equal(run('SELECT * FROM child;').results[0].rows, [[1,1,10,'first']]);
equal(run('CREATE TABLE tuples(a INT,b INT,CONSTRAINT pk_tuple PRIMARY KEY(a,b)); INSERT INTO tuples VALUES(1,2);').success, true);
equal(run('CREATE TABLE linked(x INT,y INT,CONSTRAINT fk_tuple FOREIGN KEY(x,y) REFERENCES tuples(a,b),CONSTRAINT uq_pair UNIQUE(x,y),CONSTRAINT ck_pair CHECK(x<=y)); INSERT INTO linked VALUES(1,2);').success, true);
rejected('INSERT INTO linked VALUES(1,2);', 'uq_pair');
rejected('INSERT INTO linked VALUES(3,2);', 'ck_pair');
rejected('INSERT INTO linked VALUES(1,3);', 'fk_tuple');
rejected('UPDATE tuples SET b=3;', 'fk_tuple');
rejected('INSERT INTO tuples VALUES(1,2);', 'pk_tuple');
const compiled = run('CREATE TABLE draft(a INT CONSTRAINT positive CHECK(a>0),CONSTRAINT key_a UNIQUE(a));', 'compile');
equal(compiled.success, true);
equal(compiled.ast.constraintNames, [{name:'positive',kind:'check',index:0},{name:'key_a',kind:'key',index:0}]);
equal(compiled.plan[0].constraintNames, compiled.ast.constraintNames);
equal(run('SELECT * FROM draft;').success, false);
const catalog = spawnSync(executable, [file,'catalog'], {encoding:'utf8'});
equal(catalog.status, 0);
equal(JSON.parse(catalog.stdout).tables.find(t=>t.name==='linked').constraintNames,
  [{name:'fk_tuple',kind:'foreignKey',index:0},{name:'uq_pair',kind:'key',index:0},{name:'ck_pair',kind:'check',index:0}]);
equal(JSON.parse(catalog.stdout).tables.find(t=>t.name==='linked').checks[0].value, '<=');
for (const definition of [
  'a INT CONSTRAINT same UNIQUE,b INT CONSTRAINT SAME CHECK(b>0)',
  'a INT CONSTRAINT dangling',
  'a INT CONSTRAINT nn NULL',
  'a INT CONSTRAINT def DEFAULT 1',
  'CONSTRAINT name a INT',
  'a INT,CONSTRAINT x CHECK(a>0),CONSTRAINT x CHECK(a<10)',
]) equal(run(`CREATE TABLE invalid(${definition});`).success, false);
equal(run('CREATE TABLE other(id INT CONSTRAINT pk_parent PRIMARY KEY);').success, true);
equal(run('CREATE TABLE self_named(id INT CONSTRAINT pk_self PRIMARY KEY,parent INT CONSTRAINT fk_self REFERENCES self_named(id)); INSERT INTO self_named VALUES(1,1);').success, true);
rejected('UPDATE self_named SET id=2;', 'fk_self');
equal(run('UPDATE self_named SET id=2,parent=2;').success, true);
console.log(`${checks} named-constraint checks passed: column/table syntax, names, diagnostics, uniqueness and persistence`);

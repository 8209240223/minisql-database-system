import { mkdtempSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';
const db = join(mkdtempSync(join(tmpdir(), 'minisql-index-')), 'db.pages');
const session = await openSession('./build/windows/Release/minisql_database.exe', db);
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
try {
  equal((await session.request('execute', 'CREATE TABLE t(id INT,v INT); INSERT INTO t VALUES(1,10),(2,20),(3,20);')).success, true);
  equal((await session.request('execute', 'CREATE INDEX idx_v ON t(v);')).success, true);
  const catalog = await session.request('catalog');
  equal(catalog.tables[0].indexes[0].name, 'idx_v');
  const inspected = await session.request('indexInspect', undefined, { table: 't', index: 'idx_v' });
  equal(inspected.present, true);
  equal(inspected.storage, 'page-file');
  assert.equal(inspected.rootReachable, true); ++checks;
  const compiled = await session.request('compile', 'SELECT id FROM t WHERE v=20 ORDER BY id;');
  assert.ok(compiled.plan.some(node => node.kind === 'IndexScan')); ++checks;
  equal((await session.request('execute', 'SELECT id FROM t WHERE v=20 ORDER BY id;')).results.at(-1).rows, [[2],[3]]);
  const rangePlan = await session.request('compile', 'SELECT id FROM t WHERE v>=20 ORDER BY id;');
  assert.ok(rangePlan.plan.some(node => node.kind === 'IndexScan')); ++checks;
  equal((await session.request('execute', 'SELECT id FROM t WHERE v>=20 ORDER BY id;')).results.at(-1).rows, [[2],[3]]);
  equal((await session.request('execute', 'SELECT id FROM t WHERE v<20 ORDER BY id;')).results.at(-1).rows, [[1]]);
  equal((await session.request('execute', 'UPDATE t SET v=30 WHERE id=1;')).success, true);
  equal((await session.request('execute', 'DELETE FROM t WHERE id=3;')).success, true);
  equal((await session.request('execute', 'SELECT id,v FROM t ORDER BY id;')).results.at(-1).rows, [[1,30],[2,20]]);
  equal((await session.request('execute', 'CREATE UNIQUE INDEX uq_id ON t(id);')).success, true);
  equal((await session.request('execute', 'INSERT INTO t VALUES(2,40);')).success, false);
  equal((await session.request('execute', 'SELECT id,v FROM t ORDER BY id;')).results.at(-1).rows, [[1,30],[2,20]]);
  equal((await session.request('execute', 'CREATE TABLE tc(a INT,b INT,c INT); INSERT INTO tc VALUES(1,2,3),(1,3,4),(2,2,5); CREATE INDEX idx_ab ON tc(a,b);')).success, true);
  const compositePlan = await session.request('compile', 'SELECT c FROM tc WHERE a=1 AND b=2;');
  assert.ok(compositePlan.plan.some(node => node.kind === 'IndexScan' && node.indexColumns.length === 2)); ++checks;
  equal((await session.request('execute', 'SELECT c FROM tc WHERE a=1 AND b=2;')).results.at(-1).rows, [[3]]);
  equal((await session.request('execute', 'SELECT c FROM tc WHERE a=1 AND b>=3 ORDER BY c;')).results.at(-1).rows, [[4]]);
  equal((await session.request('execute', 'SELECT c FROM tc WHERE a=1 AND b<3 ORDER BY c;')).results.at(-1).rows, [[3]]);
  const compositeRangePlan = await session.request('compile', 'SELECT c FROM tc WHERE a=1 AND b>=3;');
  assert.ok(compositeRangePlan.plan.some(node => node.kind === 'IndexScan' && node.indexRangeOperator === '>=')); ++checks;
  equal((await session.request('execute', 'CREATE UNIQUE INDEX uq_ab ON tc(a,b);')).success, true);
  equal((await session.request('execute', 'INSERT INTO tc VALUES(1,2,9);')).success, false);
  const manyValues = Array.from({ length: 400 }, (_, index) => `(${index},${400 - index})`).join(',');
  equal((await session.request('execute', `CREATE TABLE many(id INT PRIMARY KEY,v INT); INSERT INTO many VALUES ${manyValues}; CREATE INDEX many_v ON many(v);`)).success, true);
  const manyIndex = (await session.request('catalog')).tables.find(table => table.name === 'many').indexes[0];
  equal(manyIndex.storage, 'page-file');
  assert.ok(manyIndex.pageCount >= 1, 'index page count must be exposed'); ++checks;
  assert.ok(manyIndex.height >= 2, 'multi-level tree must split beyond the root page'); ++checks;
  equal((await session.request('execute', 'SELECT id FROM many WHERE v=250;')).results.at(-1).rows, [[150]]);
  equal((await session.request('execute', 'SELECT id FROM many WHERE v>=250 AND v<=251 ORDER BY id;')).results.at(-1).rows, [[149],[150]]);
  equal((await session.request('execute', 'DELETE FROM many WHERE id>=300;')).success, true);
  console.log(`${checks} index checks passed`);
} finally {
  await session.close();
}
const sidecars = readdirSync(dirname(db)).filter(name => name.startsWith(basename(db) + '.idx.'));
equal(sidecars, []);
const reopened = await openSession('./build/windows/Release/minisql_database.exe', db);
try {
  const compiled = await reopened.request('compile', 'SELECT id FROM t WHERE v=20 ORDER BY id;');
  assert.ok(compiled.plan.some(node => node.kind === 'IndexScan')); ++checks;
  equal((await reopened.request('execute', 'SELECT id FROM t WHERE v=20 ORDER BY id;')).results.at(-1).rows, [[2]]);
  const reopenedMany = (await reopened.request('catalog')).tables.find(table => table.name === 'many').indexes[0];
  equal(reopenedMany.storage, 'page-file');
  assert.ok(reopenedMany.pageCount >= 1, 'page-backed index survives restart'); ++checks;
  assert.ok(reopenedMany.height >= 2, 'loaded multi-level tree keeps height'); ++checks;
  equal((await reopened.request('execute', 'SELECT id FROM many WHERE v=250;')).results.at(-1).rows, [[150]]);
  equal((await reopened.request('execute', 'SELECT id FROM many WHERE v=50;')).results.at(-1).rows, []);
  const composite = await reopened.request('compile', 'SELECT c FROM tc WHERE a=1 AND b=2;');
  assert.ok(composite.plan.some(node => node.kind === 'IndexScan' && node.indexColumns.length === 2)); ++checks;
  equal((await reopened.request('execute', 'DROP INDEX idx_ab;')).success, true);
  const dropped = await reopened.request('compile', 'SELECT c FROM tc WHERE a=1 AND b=2;');
  assert.ok(!dropped.plan.some(node => node.kind === 'IndexScan' && node.indexName === 'idx_ab')); ++checks;
  equal((await reopened.request('catalog')).tables.find(table => table.name === 'tc').indexes.some(index => index.name === 'idx_ab'), false);
  console.log(`${checks} index persistence checks passed`);
} finally {
  await reopened.close();
}
const rebuilt = await openSession('./build/windows/Release/minisql_database.exe', db, { env: { MINISQL_REBUILD_INDEXES: '1' } });
try {
  equal((await rebuilt.request('execute', 'SELECT id FROM many WHERE v=250;')).results.at(-1).rows, [[150]]);
  const rebuiltMany = (await rebuilt.request('catalog')).tables.find(table => table.name === 'many').indexes[0];
  equal(rebuiltMany.storage, 'page-file');
  assert.ok(rebuiltMany.pageCount >= 1, 'forced rebuild writes page-backed index'); ++checks;
  console.log(`${checks} index persistence checks passed`);
} finally {
  await rebuilt.close();
}

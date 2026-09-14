import { mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { openSession } from '../scripts/session-process.mjs';

const root = mkdtempSync(join(tmpdir(), 'minisql-session-stream-'));
const executable = process.env.MINISQL_DATABASE_EXE ?? fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
let session;
let checks = 0;
try {
  session = await openSession(executable, join(root, 'db.pages'), { timeoutMs: 30000 });
  const setup = await session.request('execute', 'CREATE TABLE t(id INT, value INT); INSERT INTO t VALUES(1,10),(2,20),(3,30),(4,40);');
  assert.equal(setup.success, true); ++checks;

  const frames = [];
  const streamed = await session.requestStream('executeStream', 'SELECT id FROM t WHERE id >= 2 ORDER BY id LIMIT 2;', {}, frame => frames.push(frame));
  assert.equal(streamed.type, 'complete'); ++checks;
  assert.equal(frames[0].type, 'meta'); ++checks;
  assert.deepEqual(frames.filter(frame => frame.type === 'row').map(frame => frame.row), [[2], [3]]); ++checks;
  assert.ok(frames.some(frame => frame.type === 'complete')); ++checks;

  const correlatedFrames = [];
  const correlated = await session.requestStream('executeStream',
    'SELECT x.id, (SELECT COUNT(*) FROM t y WHERE y.id <= x.id) AS c FROM t x ORDER BY x.id;',
    {}, frame => correlatedFrames.push(frame));
  assert.equal(correlated.type, 'complete'); ++checks;
  assert.deepEqual(correlatedFrames.filter(frame => frame.type === 'row').map(frame => frame.row), [[1,1],[2,2],[3,3],[4,4]]); ++checks;

  const invalidFrames = [];
  const invalid = await session.requestStream('executeStream', 'INSERT INTO t VALUES(5,50);', {}, frame => invalidFrames.push(frame));
  assert.equal(invalid.type, 'error'); ++checks;
  assert.equal(invalid.success, false); ++checks;
  assert.ok(String(invalid.error?.message ?? '').includes('one SELECT or EXPLAIN')); ++checks;
  assert.deepEqual(invalidFrames.map(frame => frame.type), ['error']); ++checks;

  await session.close();
  session = await openSession(executable, join(root, 'limited.pages'), {
    timeoutMs: 30000, env: { MINISQL_MAX_RESULT_ROWS: '2' },
  });
  assert.equal((await session.request('execute', 'CREATE TABLE limited(id INT); INSERT INTO limited VALUES(1),(2),(3);')).success, true); ++checks;
  const limitedFrames = [];
  const limited = await session.requestStream('executeStream', 'SELECT * FROM limited ORDER BY id;', {}, frame => limitedFrames.push(frame));
  assert.equal(limited.type, 'error'); ++checks;
  assert.equal(limited.error?.code, 5001); ++checks;
  assert.match(limited.error?.message ?? '', /row budget exceeded/i); ++checks;
  assert.deepEqual(limitedFrames.filter(frame => frame.type === 'row').map(frame => frame.row), [[1], [2]]); ++checks;

  console.log(`${checks} session stream checks passed`);
} finally {
  if (session) await session.close().catch(() => {});
}

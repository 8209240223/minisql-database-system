import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const root = mkdtempSync(join(tmpdir(), 'minisql-x25-stream-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(root, 'db.pages') },
  windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(server, 'exit');
let output = '', errors = '', url;
server.stdout.on('data', chunk => { output += chunk; });
server.stderr.on('data', chunk => { errors += chunk; });
const ready = new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error(errors || 'stream server startup timed out')), 10000);
  const check = () => {
    const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
    if (match) { clearTimeout(timer); url = match[0]; resolve(); }
  };
  server.stdout.on('data', check);
  server.once('exit', () => { clearTimeout(timer); reject(new Error(errors || 'stream server exited')); });
});
let checks = 0;
async function request(path, sql, method = 'POST', signal) {
  const response = await fetch(url + path, {
    method,
    headers: method === 'POST' ? { 'Content-Type': 'application/json' } : {},
    ...(method === 'POST' ? { body: JSON.stringify({ sql }) } : {}),
    signal,
  });
  return { status: response.status, response, data: method === 'POST' ? await response.json() : await response.json() };
}

try {
  await ready;
  const values = Array.from({ length: 160 }, (_, index) => `(${index})`).join(',');
  const seed = await request('/execute', `CREATE TABLE stream_rows(id INT); INSERT INTO stream_rows VALUES${values};`);
  assert.equal(seed.status, 200, JSON.stringify(seed.data)); ++checks;

  const streamed = await fetch(url + '/execute/stream', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql: 'SELECT id FROM stream_rows ORDER BY id;' }),
  });
  assert.equal(streamed.status, 200); ++checks;
  assert.match(streamed.headers.get('content-type') ?? '', /application\/x-ndjson/); ++checks;
  const chunks = [];
  for await (const chunk of streamed.body) chunks.push(chunk);
  const frames = Buffer.concat(chunks).toString('utf8').trim().split('\n').map(line => JSON.parse(line));
  assert.equal(frames[0].type, 'meta'); ++checks;
  assert.equal(frames[0].columns[0], 'id'); ++checks;
  assert.equal(frames.filter(frame => frame.type === 'row').length, 160); ++checks;
  assert.deepEqual(frames.filter(frame => frame.type === 'row').at(-1).values, [159]); ++checks;
  assert.equal(frames.at(-1).type, 'complete'); ++checks;
  assert.equal(frames.at(-1).rowCount, 160); ++checks;

  const opened = await fetch(url + '/sessions', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
  const session = await opened.json();
  assert.equal(opened.status, 201, JSON.stringify(session)); ++checks;
  const sessionStreamed = await fetch(url + '/sessions/' + session.sessionId + '/execute/stream', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql: 'SELECT id FROM stream_rows ORDER BY id LIMIT 2;' }),
  });
  assert.equal(sessionStreamed.status, 200); ++checks;
  const sessionChunks = [];
  for await (const chunk of sessionStreamed.body) sessionChunks.push(chunk);
  const sessionFrames = Buffer.concat(sessionChunks).toString('utf8').trim().split('\n').map(line => JSON.parse(line));
  assert.equal(sessionFrames[0].type, 'meta'); ++checks;
  assert.deepEqual(sessionFrames.filter(frame => frame.type === 'row').map(frame => frame.values), [[0], [1]]); ++checks;
  assert.equal(sessionFrames.at(-1).type, 'complete'); ++checks;
  assert.equal(sessionFrames.at(-1).rowCount, 2); ++checks;
  await fetch(url + '/sessions/' + session.sessionId + '/close', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });

  const writeAttempt = await fetch(url + '/execute/stream', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql: 'INSERT INTO stream_rows VALUES(999);' }),
  });
  assert.equal(writeAttempt.status, 400); ++checks;
  assert.deepEqual((await request('/execute', 'SELECT id FROM stream_rows WHERE id=999;')).data.rows, []); ++checks;

  const controller = new AbortController();
  const aborted = fetch(url + '/execute/stream', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql: 'SELECT id FROM stream_rows ORDER BY id;' }),
    signal: controller.signal,
  });
  const abortResponse = await aborted;
  const reader = abortResponse.body.getReader();
  await reader.read();
  const pendingRead = reader.read().catch(() => undefined);
  controller.abort();
  await pendingRead;
  await reader.cancel().catch(() => undefined);
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.deepEqual((await request('/execute', 'SELECT COUNT(*) FROM stream_rows;')).data.rows, [[160]]); ++checks;
  console.log(`${checks} X25 streaming HTTP checks passed: NDJSON frames, read-only guard, backpressure path and disconnect recovery`);
} finally {
  server.kill();
  await exited;
}

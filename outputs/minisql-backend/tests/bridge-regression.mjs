import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
const directory = mkdtempSync(join(tmpdir(), 'minisql-legacy-bridge-'));
const executable = process.env.MINISQL_DATABASE_EXE ?? fileURLToPath(new URL('../build/verification/Release/minisql_database.exe', import.meta.url));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DATABASE_EXE: executable, MINISQL_DB: join(directory, 'database.pages') },
  windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
});
const closed = once(server, 'exit');
let log = '';
server.stderr.on('data', chunk => { log += chunk; });
const timeout = setTimeout(() => server.kill(), 15000);
try {
  const url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.on('error', reject);
    server.on('exit', () => reject(new Error(`bridge exited: ${log}`)));
  });
  const post = (endpoint, body) => fetch(url + endpoint, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  const capabilities = await (await fetch(url + '/capabilities')).json();
  assert.equal(capabilities.engine, 'minisql-cpp');
  assert.equal(capabilities.execution, true);
  assert.equal(capabilities.persistence, true);
  assert.ok(capabilities.capabilities.includes('planRoundTrip'));
  const result = await post('/execute', { sql: "CREATE TABLE t(id INT,name VARCHAR); INSERT INTO t(name,id) VALUES('中文测试',1); SELECT name FROM t WHERE id=1;" });
  assert.equal(result.status, 200);
  const data = await result.json();
  assert.deepEqual(data.rows, [['中文测试']]);
  assert.equal((await (await fetch(url + '/catalog')).json()).tables[0].name, 't');
  assert.equal((await post('/compile', { sql: 'SELECT name FROM t;' })).status, 200);
  assert.equal((await post('/execute', { sql: 'DELETE FROM t;' })).status, 200);
  assert.equal((await post('/compile', { sql: 42 })).status, 400);
  assert.equal((await fetch(url + '/catalog', { headers: { Origin: 'https://untrusted.example' } })).status, 403);
  console.log('8 legacy bridge forwarding scenarios passed');
} finally {
  clearTimeout(timeout);
  server.kill();
  await closed;
}

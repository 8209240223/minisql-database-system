import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0' }, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
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
  assert.equal(capabilities.execution, false);
  assert.ok(capabilities.capabilities.includes('logicalPlan'));
  const result = await post('/compile', { sql: "CREATE TABLE t(id INT,name VARCHAR); INSERT INTO t(name,id) VALUES('中文测试',1); SELECT name FROM t WHERE id=1;" });
  assert.equal(result.status, 200);
  const data = await result.json();
  assert.equal(data.stages.planner, 'passed');
  assert.deepEqual(data.plan.find(p => p.kind === 'Insert').values, [1, '中文测试']);
  assert.deepEqual((await (await fetch(url + '/catalog')).json()).tables, []);
  assert.equal((await post('/compile', { sql: 'SELECT name FROM t;' })).status, 422);
  assert.equal((await post('/execute', { sql: 'DELETE FROM t;' })).status, 501);
  assert.equal((await post('/compile', { sql: 42 })).status, 400);
  assert.equal((await fetch(url + '/catalog', { headers: { Origin: 'https://untrusted.example' } })).status, 403);
  console.log('7 bridge integration scenarios passed');
} finally {
  clearTimeout(timeout);
  server.kill();
  await closed;
}

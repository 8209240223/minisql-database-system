// X24/C1 权限感知 CLI 契约：参数解析与真实 HTTP bridge 调用。
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseCliArgs, runCli } from '../scripts/minisql-cli.mjs';

let checks = 0;
const ok = (value, message) => { assert.ok(value, message); ++checks; };
const equal = (actual, expected, message) => { assert.deepEqual(actual, expected, message); ++checks; };

const parsed = parseCliArgs(['--execute', 'SELECT 1;', '--user', 'reader', '--password', 'secret', '--url', 'http://127.0.0.1:8081/api/']);
equal(parsed.mode, 'execute', 'execute mode parsed');
equal(parsed.user, 'reader', 'user parsed');
equal(parsed.password, 'secret', 'password parsed');
equal(parsed.url, 'http://127.0.0.1:8081/api', 'trailing slash removed');
equal(parsed.sql, 'SELECT 1;', 'inline SQL parsed');
const positional = parseCliArgs(['SELECT 2;', '--json']);
equal(positional.sql, 'SELECT 2;', 'positional SQL parsed');
ok(positional.json, 'JSON flag parsed');
assert.throws(() => parseCliArgs(['--execute', 'SELECT 1;', '--compile', 'SELECT 1;']), /不能同时使用/);
+checks;

const directory = mkdtempSync(join(tmpdir(), 'minisql-cli-'));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(directory, 'database.pages'), MINISQL_ACCESS_FILE: join(directory, 'access.catalog.json') },
  stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true,
});
let errors = '';
server.stderr.on('data', chunk => { errors += chunk; });
const exited = once(server, 'exit');
const deadline = setTimeout(() => server.kill(), 60000);
try {
  const url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.on('error', reject);
    server.on('exit', () => reject(new Error(errors || 'server exited')));
  });
  const config = { url, user: 'admin', password: '', mode: 'execute', sql: '' };
  const created = await runCli({ ...config, sql: 'CREATE TABLE cli_test(id INT PRIMARY KEY);' });
  ok(created.success, 'CLI creates table through HTTP bridge');
  const inserted = await runCli({ ...config, sql: 'INSERT INTO cli_test VALUES(7);' });
  equal(inserted.affectedRows, 1, 'CLI reports affected rows');
  const selected = await runCli({ ...config, sql: 'SELECT id FROM cli_test;' });
  equal(selected.rows, [[7]], 'CLI reads through a session-bound HTTP request');
  const compiled = await runCli({ ...config, mode: 'compile', sql: 'SELECT id FROM cli_test;' });
  ok(Array.isArray(compiled.plan), 'CLI compile returns a plan');
} finally {
  clearTimeout(deadline);
  server.kill();
  await exited.catch(() => {});
}

console.log(`${checks} permission-aware CLI checks passed`);

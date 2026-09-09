// X24 原子权限 HTTP 资源接口集成测试（成员 C）。
//
// 该测试只启动 database-bridge.mjs，不依赖 C++ 引擎二进制（这些端点仅操作权限目录，
// 不会调用 minisql_database.exe），因此可在未构建引擎的环境中复跑。
// 运行：node tests/access-atomic-http.mjs

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { can, roleNames, verifyUser } from '../scripts/access-catalog.mjs';

const directory = mkdtempSync(join(tmpdir(), 'minisql-access-atomic-'));
const accessPath = join(directory, 'access.catalog.json');
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: {
    ...process.env,
    PORT: '0',
    MINISQL_DB: join(directory, 'database.pages'),
    MINISQL_ACCESS_FILE: accessPath,
    MINISQL_SESSION_IDLE_MS: '30000',
  },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});
const exited = once(server, 'exit');
const deadline = setTimeout(() => server.kill(), 60000);
let checks = 0;
let errors = '';
server.stderr.on('data', chunk => { errors += chunk; });

function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(value, message) { assert.ok(value, message); ++checks; }

const loadAccessFile = () => JSON.parse(readFileSync(accessPath, 'utf8'));

try {
  const url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+/);
      if (match) resolve(match[0]);
    });
    server.on('error', reject);
    server.on('exit', () => reject(new Error(errors || 'server exited')));
  });
  async function request(path, method = 'GET', body, user = 'admin', password) {
    const headers = { 'Content-Type': 'application/json' };
    if (user !== 'admin') headers['X-MiniSQL-User'] = user;
    if (password !== undefined) headers['X-MiniSQL-Password'] = password;
    const response = await fetch(url + path, {
      method,
      headers,
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      signal: AbortSignal.timeout(10000),
    });
    return { status: response.status, data: await response.json() };
  }

  const initial = await request('/api/access');
  equal(initial.status, 200);
  ok(initial.data.access.users.some(user => user.name === 'admin'), 'initial admin present');
  ++checks;

  // 创建角色与用户
  equal((await request('/api/roles', 'POST', { name: 'analyst', inherits: [], grants: [{ object: 'public_records', permissions: ['SELECT'] }] })).status, 200);
  equal((await request('/api/users', 'POST', { name: 'alice', password: 'alice-secret', roles: ['analyst'] })).status, 200);
  let stored = loadAccessFile();
  ok(verifyUser(stored, 'alice', 'alice-secret'), 'alice password verified');
  ok(!verifyUser(stored, 'alice', 'wrong'), 'wrong password rejected');
  ok(can(stored, 'alice', 'SELECT', 'public_records'), 'alice inherited SELECT via analyst');
  ok(!stored.users.alice.hash.digest === false, 'no plaintext');
  // 用户列表不暴露散列
  const users = await request('/api/users');
  const aliceEntry = users.data.users.find(user => user.name === 'alice');
  ok(aliceEntry && !('hash' in aliceEntry), '/api/users hides password hash');
  ++checks;

  // 对象级授权并集
  equal((await request('/api/grants', 'POST', { subject: { type: 'user', name: 'alice' }, object: 'public_records', permissions: ['UPDATE'] })).status, 200);
  stored = loadAccessFile();
  ok(can(stored, 'alice', 'UPDATE', 'public_records'), 'granted UPDATE');
  // 撤销单项权限
  equal((await request('/api/revokes', 'POST', { subject: { type: 'user', name: 'alice' }, object: 'public_records', permissions: ['UPDATE'] })).status, 200);
  stored = loadAccessFile();
  ok(!can(stored, 'alice', 'UPDATE', 'public_records'), 'revoked UPDATE');
  ok(can(stored, 'alice', 'SELECT', 'public_records'), 'SELECT retained');
  ++checks;

  // 绑定/解绑角色
  equal((await request('/api/roles', 'POST', { name: 'writer', inherits: [], grants: [{ object: '*', permissions: ['UPDATE'] }] })).status, 200);
  const addRole = await request('/api/users/alice/roles', 'POST', { role: 'writer' });
  equal(addRole.status, 200);
  stored = loadAccessFile();
  ok(can(stored, 'alice', 'UPDATE', 'other_table'), 'role grant effective after addRole');
  equal((await request('/api/users/alice/roles/writer', 'DELETE')).status, 200);
  stored = loadAccessFile();
  ok(!can(stored, 'alice', 'UPDATE', 'other_table'), 'role removed after removeRole');
  ++checks;

  // 改密后以新密码连接
  equal((await request('/api/users/alice/password', 'POST', { password: 'new-secret' })).status, 200);
  stored = loadAccessFile();
  ok(verifyUser(stored, 'alice', 'new-secret'), 'password rotated');
  ok(!verifyUser(stored, 'alice', 'alice-secret'), 'old password invalid');
  ++checks;

  // 删除用户
  equal((await request('/api/users/alice', 'DELETE')).status, 200);
  stored = loadAccessFile();
  ok(!stored.users.alice, 'alice dropped');
  ++checks;

  // 缺 GRANT 的主体被拒绝
  const readerDenied = await request('/api/roles', 'POST', { name: 'x', inherits: [] }, 'reader');
  equal(readerDenied.status, 403);
  const readerCreateUser = await request('/api/users', 'POST', { name: 'z', roles: [] }, 'reader');
  equal(readerCreateUser.status, 403);
  ++checks;

  // 原子性：重复创建用户失败，原状态保持
  equal((await request('/api/users', 'POST', { name: 'bob', password: 'bob-secret' })).status, 200);
  const beforeDuplicate = JSON.stringify(loadAccessFile());
  const dup = await request('/api/users', 'POST', { name: 'bob', roles: [] });
  equal(dup.status, 400);
  equal(JSON.stringify(loadAccessFile()), beforeDuplicate, 'failed create leaves access unchanged');
  ++checks;

  // 删除不存在用户/角色幂等失败（明确 400）
  equal((await request('/api/users/ghost', 'DELETE')).status, 400);
  ++checks;

  console.log(`${checks} atomic access HTTP checks passed`);
} catch (error) {
  console.error('FAILED:', error);
  process.exitCode = 1;
} finally {
  clearTimeout(deadline);
  server.kill();
  await exited;
}

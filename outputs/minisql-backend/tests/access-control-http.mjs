import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/access-control-', import.meta.url)));
const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: {
    ...process.env,
    PORT: '0',
    MINISQL_DB: join(directory, 'database.pages'),
    MINISQL_ACCESS_FILE: join(directory, 'access.catalog.json'),
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

function equal(actual, expected) {
  assert.deepEqual(actual, expected);
  ++checks;
}

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
  async function request(path, sql, method = 'POST', user = 'admin', password) {
    const headers = { 'Content-Type': 'application/json' };
    if (user !== 'admin') headers['X-MiniSQL-User'] = user;
    if (password !== undefined) headers['X-MiniSQL-Password'] = password;
    const response = await fetch(url + path, {
      method,
      headers,
      ...(sql === undefined ? {} : { body: typeof sql === 'string' ? JSON.stringify({ sql }) : JSON.stringify(sql) }),
      signal: AbortSignal.timeout(10000),
    });
    return { status: response.status, data: await response.json() };
  }

  const adminSession = await request('/sessions');
  equal(adminSession.status, 201);
  const adminPrefix = '/sessions/' + adminSession.data.sessionId;
  equal((await request(adminPrefix + '/execute', 'CREATE TABLE public_records(id INT PRIMARY KEY);')).status, 200);
  equal((await request(adminPrefix + '/execute', 'CREATE TABLE secret_records(id INT PRIMARY KEY);')).status, 200);

  const initial = await request('/access', undefined, 'GET');
  equal(initial.status, 200);
  assert.ok(initial.data.access.users.some(user => user.name === 'reader'));
  ++checks;
  const current = initial.data.access;
  const configured = {
    version: current.version,
    users: [
      ...current.users,
      { name: 'alice', password: 'alice-secret', roles: ['analyst'] },
      { name: 'bob', password: 'bob-secret', roles: ['denied'] },
      { name: 'carol', password: 'carol-secret', roles: ['manager'] },
    ],
    roles: [
      ...current.roles,
      { name: 'analyst', inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ'] }, { object: 'public_records', permissions: ['SELECT', 'INSERT', 'UPDATE'] }] },
      { name: 'manager', inherits: ['analyst'], grants: [] },
      { name: 'denied', inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ'] }] },
    ],
  };
  const saved = await request('/access', { access: configured }, 'PUT');
  equal(saved.status, 200);
  const aliceRecord = saved.data.access.users.find(user => user.name === 'alice');
  equal(aliceRecord.passwordProtected, true);
  assert.equal(JSON.stringify(saved.data.access).includes('alice-secret'), false);
  ++checks;

  const aliceSession = await request('/sessions', undefined, 'POST', 'alice', 'alice-secret');
  equal(aliceSession.status, 201);
  const alicePrefix = '/sessions/' + aliceSession.data.sessionId;
  const bobSession = await request('/sessions', undefined, 'POST', 'bob', 'bob-secret');
  equal(bobSession.status, 201);
  const bobPrefix = '/sessions/' + bobSession.data.sessionId;

  equal((await request(alicePrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'alice', 'alice-secret')).status, 200);
  equal((await request(alicePrefix + '/execute', 'INSERT INTO public_records VALUES(1);', 'POST', 'alice', 'alice-secret')).status, 200);
  equal((await request(alicePrefix + '/execute', 'UPDATE public_records SET id = id + 1 WHERE id = 1;', 'POST', 'alice', 'alice-secret')).status, 200);
  equal((await request(alicePrefix + '/execute', 'DELETE FROM public_records;', 'POST', 'alice', 'alice-secret')).status, 403);
  equal((await request(bobPrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'bob', 'bob-secret')).status, 403);
  equal((await request(alicePrefix + '/execute', 'SELECT * FROM secret_records;', 'POST', 'alice', 'alice-secret')).status, 403);
  equal((await request(bobPrefix + '/diagnostics', 'SELECT * FROM public_records;', 'POST', 'bob', 'bob-secret')).status, 403);

  const aliceCatalog = await request(alicePrefix + '/catalog', undefined, 'GET', 'alice', 'alice-secret');
  equal(aliceCatalog.data.tables.map(table => table.name).sort(), ['public_records']);
  const bobCatalog = await request(bobPrefix + '/catalog', undefined, 'GET', 'bob', 'bob-secret');
  equal(bobCatalog.data.tables, []);

  const carolSession = await request('/sessions', undefined, 'POST', 'carol', 'carol-secret');
  equal(carolSession.status, 201);
  const carolPrefix = '/sessions/' + carolSession.data.sessionId;
  equal((await request(carolPrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'carol', 'carol-secret')).status, 200);
  equal((await request(carolPrefix + '/execute', 'INSERT INTO public_records VALUES(10);', 'POST', 'carol', 'carol-secret')).status, 200);

  const wrongPassword = await request(alicePrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'alice', 'wrong');
  equal(wrongPassword.status, 403);
  const crossIdentity = await request(alicePrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'bob', 'bob-secret');
  equal(crossIdentity.status, 403);

  const revoke = await request('/access', undefined, 'GET');
  const revokeUsers = revoke.data.access.users.map(user => {
    if (user.name === 'alice') return user;
    if (user.name === 'carol') return user;
    return user;
  });
  const revokeRoles = revoke.data.access.roles.map(role => {
    if (role.name === 'analyst') {
      return {
        ...role,
        grants: role.grants.map(grant => grant.object === 'public_records' ? { ...grant, permissions: ['SELECT'] } : grant),
      };
    }
    return role;
  });
  const revoked = await request('/access', { access: { version: revoke.data.access.version, users: revokeUsers, roles: revokeRoles } }, 'PUT');
  equal(revoked.status, 200);
  equal((await request(alicePrefix + '/execute', 'INSERT INTO public_records VALUES(3);', 'POST', 'alice', 'alice-secret')).status, 403);
  equal((await request(carolPrefix + '/execute', 'SELECT * FROM public_records;', 'POST', 'carol', 'carol-secret')).status, 200);

  const audit = await request('/audit?user=alice&object=public_records&limit=100', undefined, 'GET');
  equal(audit.status, 200);
  assert.ok(audit.data.entries.length > 0);
  assert.ok(audit.data.entries.every(entry => entry.user === 'alice'));
  assert.ok(audit.data.entries.every(entry => String(entry.object ?? '').split(',').includes('public_records')));
  assert.ok(audit.data.entries.every(entry => entry.errorCode !== 403 || entry.path.includes('/execute')));
  ++checks;

  const capabilities = await request('/capabilities', undefined, 'GET');
  equal(capabilities.data.permissionsModel, 'catalog-access');
  equal(capabilities.data.objectPermissions, true);
  equal(capabilities.data.roleInheritance, true);
  equal(capabilities.data.sessionIdentity, true);

  equal((await request(alicePrefix + '/close', undefined, 'POST', 'alice', 'alice-secret')).status, 200);
  equal((await request(bobPrefix + '/close', undefined, 'POST', 'bob', 'bob-secret')).status, 200);
  equal((await request(carolPrefix + '/close', undefined, 'POST', 'carol', 'carol-secret')).status, 200);
  equal((await request(adminPrefix + '/close')).status, 200);
  console.log(`${checks} access-control HTTP checks passed: roles, object grants, revoke, identity, metadata and audit`);
} catch (error) {
  console.error('server stderr:', errors);
  throw error;
} finally {
  clearTimeout(deadline);
  server.kill();
  await exited;
}

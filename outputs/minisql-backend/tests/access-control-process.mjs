// X24 C++ 引擎入口权限回归：验证页式权限目录被真实 session 与直连命令共同执行。
import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createUser, defaultAccess } from '../scripts/access-catalog.mjs';
import { pagesPathFor, writeStore } from '../scripts/access-store.mjs';
import { openSession } from '../scripts/session-process.mjs';

const executable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const directory = mkdtempSync(join(tmpdir(), 'minisql-access-process-'));
const database = join(directory, 'database.pages');
const accessJson = join(directory, 'access.catalog.json');
const accessPages = pagesPathFor(accessJson);
const env = { MINISQL_ACCESS_FILE: accessJson };
let checks = 0;

const equal = (actual, expected, message) => {
  assert.deepEqual(actual, expected, message);
  ++checks;
};

const access = createUser(defaultAccess(), {
  name: 'alice',
  password: 'alice-secret',
  roles: [],
});
access.users.alice.roles = ['limited-readers'];
access.roles['limited-readers'] = {
  inherits: [],
  grants: [
    { object: '*', permissions: ['CONNECT', 'READ'] },
    { object: 'public_records', permissions: ['SELECT', 'COMPILE'] },
  ],
};
writeStore(accessPages, access, { permissionVersion: 1 });

const admin = { sessionId: 'engine-admin', user: 'admin', password: '' };
const alice = { sessionId: 'engine-alice', user: 'alice', password: 'alice-secret' };
const wrongPassword = { sessionId: 'engine-alice', user: 'alice', password: 'wrong' };

const session = await openSession(executable, database, { env });
try {
  equal((await session.request('execute', 'CREATE TABLE public_records(id INT PRIMARY KEY);', admin)).success, true,
    'administrator can create a table through the session process');
  equal((await session.request('execute', 'INSERT INTO public_records VALUES(1);', admin)).success, true,
    'administrator can insert through the session process');
  equal((await session.request('execute', 'CREATE TABLE secret_records(id INT);', admin)).success, true,
    'administrator can create a second object for nested authorization checks');
  equal((await session.request('execute', 'SELECT * FROM public_records;', alice)).success, true,
    'reader can select an object through the session process');

  const deniedNestedRead = await session.request('execute', 'SELECT * FROM public_records WHERE id IN (SELECT id FROM secret_records);', alice);
  equal(deniedNestedRead.success, false, 'nested subquery object without SELECT permission is denied');
  equal(deniedNestedRead.error.code, 7001, 'nested subquery denial is a PermissionError');
  const deniedDerivedRead = await session.request('execute', 'SELECT * FROM (SELECT id FROM secret_records) AS hidden;', alice);
  equal(deniedDerivedRead.success, false, 'derived-table object without SELECT permission is denied');
  equal(deniedDerivedRead.error.code, 7001, 'derived-table denial is a PermissionError');
  equal((await session.request('execute', "SELECT * FROM public_records WHERE 'FROM secret_records' = 'x';", alice)).success, true,
    'object names inside string literals do not trigger authorization');

  const deniedWrite = await session.request('execute', 'INSERT INTO public_records VALUES(2);', alice);
  equal(deniedWrite.success, false, 'reader write is denied inside the C++ process');
  equal(deniedWrite.error.code, 7001, 'reader write reports PermissionError');

  const deniedPassword = await session.request('execute', 'SELECT * FROM public_records;', wrongPassword);
  equal(deniedPassword.success, false, 'wrong password is denied inside the C++ process');
  equal(deniedPassword.error.code, 7001, 'wrong password reports PermissionError');

  equal((await session.request('close', undefined, admin)).success, true,
    'authenticated session can close cleanly');
  await session.closed;
} finally {
  await session.terminate();
}

function direct(sql, user, password) {
  const result = spawnSync(executable, [database, 'execute'], {
    input: sql,
    encoding: 'utf8',
    windowsHide: true,
    timeout: 10000,
    maxBuffer: 8 * 1024 * 1024,
    env: { ...process.env, ...env, MINISQL_USER: user, MINISQL_PASSWORD: password },
  });
  assert.ifError(result.error);
  return { code: result.status, data: JSON.parse(result.stdout.trim()) };
}

const directSelect = direct('SELECT * FROM public_records;', 'alice', 'alice-secret');
equal(directSelect.code, 0, 'direct authenticated select exits successfully');
equal(directSelect.data.success, true, 'direct authenticated select succeeds');

const directWrite = direct('INSERT INTO public_records VALUES(2);', 'alice', 'alice-secret');
equal(directWrite.code, 1, 'direct unauthorized write exits with failure');
equal(directWrite.data.error.code, 7001, 'direct unauthorized write reports PermissionError');

const directAnonymous = direct('SELECT * FROM public_records;', '', '');
equal(directAnonymous.code, 1, 'direct anonymous request exits with failure');
equal(directAnonymous.data.error.code, 7001, 'direct anonymous request reports PermissionError');

console.log(`${checks} C++ access-control process checks passed: session credentials, password verification, object authorization and direct-entry enforcement`);

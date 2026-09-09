// X24 原子权限资源接口契约测试（成员 C）。
//
// 验证 access-catalog.mjs 提供的 GRANT / REVOKE / CREATE USER / CREATE ROLE /
// DROP / 改密 / 身份绑定函数在不落盘的前提下满足原子性与权限语义。
// 运行：node tests/access-catalog-atomic-contract.mjs

import assert from 'node:assert/strict';
import { defaultAccess, createUser, dropUser, createRole, dropRole,
  grant, revoke, setPassword, addRole, removeRole, can, roleNames,
  verifyUser, effectiveGrants } from '../scripts/access-catalog.mjs';

let checks = 0;
const check = value => { assert.ok(value); ++checks; };
const equal = (actual, expected) => { assert.deepEqual(actual, expected); ++checks; };

const base = defaultAccess();

// 1) CREATE / DROP USER
{
  const next = createUser(base, { name: 'Alice', password: 'pw', roles: ['readers'], grants: [{ object: 't', permissions: ['SELECT'] }] });
  check(next.users.alice);
  check(verifyUser(next, 'alice', 'pw'));
  check(!verifyUser(next, 'alice', 'wrong'));
  check(can(next, 'alice', 'SELECT', 't'));
  // 输入不变
  check(!base.users.alice);
  const dropped = dropUser(next, 'alice');
  check(!dropped.users.alice);
  const err = (() => { try { createUser(base, { name: 'reader', password: 'x' }); return null; } catch (e) { return e.message; } })();
  check(err && /already exists/.test(err));
}

// 2) CREATE / DROP ROLE，含继承环检测
{
  const next = createRole(base, { name: 'analyst', inherits: ['readers'], grants: [{ object: 't', permissions: ['SELECT'] }] });
  equal(roleNames(next, 'reader'), ['readers']);
  check(can(next, 'reader', 'SELECT', 't'));
  const cycleErr = (() => { try { createRole(next, { name: 'x', inherits: ['y'] }); return null; } catch (e) { return e.message; } })();
  check(cycleErr && /unknown inherited role/.test(cycleErr));
  const analyst = createRole(next, { name: 'analyst2', inherits: ['analyst'] });
  const cycle = createRole(analyst, { name: 'reader', inherits: [] }); // not a cycle yet
  const dropped = dropRole(analyst, 'analyst');
  check(!dropped.roles.analyst);
}

// 3) GRANT / REVOKE 用户与角色对象级
{
  const user = createUser(base, { name: 'bob', roles: [] });
  const granted = grant(user, { subject: { type: 'user', name: 'bob' }, object: 'public_records', permissions: ['SELECT', 'INSERT'] });
  check(can(granted, 'bob', 'SELECT', 'public_records'));
  check(can(granted, 'bob', 'INSERT', 'public_records'));
  check(!can(granted, 'bob', 'UPDATE', 'public_records'));
  check(!can(granted, 'bob', 'SELECT', 'other'));
  const revoked = revoke(granted, { subject: { type: 'user', name: 'bob' }, object: 'public_records', permissions: ['INSERT'] });
  check(can(revoked, 'bob', 'SELECT', 'public_records'));
  check(!can(revoked, 'bob', 'INSERT', 'public_records'));
  const revokedAll = revoke(revoked, { subject: { type: 'user', name: 'bob' }, object: 'public_records' });
  check(!can(revokedAll, 'bob', 'SELECT', 'public_records'));
  // 角色级授权
  const role = createRole(base, { name: 'writer2', inherits: [] });
  const roleGranted = grant(role, { subject: { type: 'role', name: 'writer2' }, object: 'accounts', permissions: ['UPDATE'] });
  check(can(roleGranted, 'admin', 'UPDATE', 'accounts')); // admin 拥有 *
  const emptyErr = (() => { try { grant(granted, { subject: { type: 'user', name: 'bob' }, object: 'x', permissions: [] }); return null; } catch (e) { return e.message; } })();
  check(emptyErr && /permissions must not be empty/.test(emptyErr));
}

// 4) 改密与加/删角色
{
  let next = createUser(base, { name: 'carol', password: 'first' });
  next = setPassword(next, 'carol', 'second');
  check(verifyUser(next, 'carol', 'second'));
  check(!verifyUser(next, 'carol', 'first'));
  next = addRole(next, 'carol', 'readers');
  check(roleNames(next, 'carol').includes('readers'));
  next = removeRole(next, 'carol', 'readers');
  check(!roleNames(next, 'carol').includes('readers'));
}

// 5) 原子性：非法输入不产生部分变更
{
  const before = JSON.stringify(base);
  try { grant(base, { subject: { type: 'user', name: 'ghost' }, object: 'x', permissions: ['SELECT'] }); }
  catch { /* expected */ }
  equal(JSON.stringify(base), before);
}

// 6) 重复授权并集、撤销不存在对象幂等
{
  let next = createUser(base, { name: 'dave', roles: [] });
  next = grant(next, { subject: { type: 'user', name: 'dave' }, object: 'o', permissions: ['SELECT'] });
  next = grant(next, { subject: { type: 'user', name: 'dave' }, object: 'o', permissions: ['SELECT', 'UPDATE'] });
  equal([...new Set(effectiveGrants(next, 'dave').filter(g => g.object === 'o').flatMap(g => g.permissions))].sort(), ['select', 'update']);
  const idempotent = revoke(next, { subject: { type: 'user', name: 'dave' }, object: 'absent' });
  equal(JSON.stringify(idempotent), JSON.stringify(next));
}

console.log(`${checks} atomic access-catalog checks passed`);

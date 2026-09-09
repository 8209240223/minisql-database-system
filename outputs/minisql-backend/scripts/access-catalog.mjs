import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import { existsSync, readFileSync, renameSync, writeFileSync } from 'node:fs';

const ALL_PERMISSIONS = new Set(['*', 'connect', 'read', 'select', 'insert', 'update', 'delete', 'create', 'drop', 'transaction', 'checkpoint', 'compile', 'grant', 'audit']);

function clone(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function normalizeName(name) {
  return String(name ?? '').trim().toLowerCase();
}

function normalizePermissions(raw) {
  const permissions = new Set();
  for (const item of Array.isArray(raw) ? raw : []) {
    const name = normalizeName(item);
    if (ALL_PERMISSIONS.has(name)) permissions.add(name);
  }
  return [...permissions];
}

function normalizeGrants(raw) {
  return (Array.isArray(raw) ? raw : []).map(grant => ({
    object: normalizeName(grant.object ?? '*'),
    permissions: normalizePermissions(grant.permissions),
  })).filter(grant => grant.permissions.length > 0);
}

export const defaultAccess = () => ({
  version: 1,
  users: {
    admin: { hash: null, roles: ['administrators'], grants: [] },
    reader: { hash: null, roles: ['readers'], grants: [] },
    writer: { hash: null, roles: ['writers'], grants: [] },
  },
  roles: {
    administrators: { inherits: [], grants: [{ object: '*', permissions: ['*'] }] },
    readers: { inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ', 'SELECT', 'COMPILE'] }] },
    writers: { inherits: ['readers'], grants: [{ object: '*', permissions: ['INSERT', 'UPDATE', 'DELETE', 'CREATE', 'DROP', 'TRANSACTION', 'CHECKPOINT'] }] },
  },
});

export function hashPassword(password) {
  const salt = randomBytes(16).toString('hex');
  const digest = createHash('sha256').update(salt + ':' + password).digest('hex');
  return { scheme: 'sha256-salted', salt, digest };
}

export function verifyUser(access, name, password) {
  const user = access?.users?.[normalizeName(name)];
  if (!user) return false;
  if (!user.hash) return password == null || password === '';
  if (!user.hash.salt || !user.hash.digest) return false;
  const digest = Buffer.from(createHash('sha256').update(user.hash.salt + ':' + (password ?? '')).digest('hex'), 'hex');
  const expected = Buffer.from(user.hash.digest, 'hex');
  return digest.length === expected.length && timingSafeEqual(digest, expected);
}

export function roleNames(access, name, seen = new Set()) {
  const user = access?.users?.[normalizeName(name)];
  if (!user) return [];
  const roles = [];
  const visitRole = role => {
    const normalized = normalizeName(role);
    if (seen.has(normalized)) return;
    seen.add(normalized);
    roles.push(normalized);
    for (const parent of access.roles?.[normalized]?.inherits ?? []) visitRole(parent);
  };
  for (const role of user.roles ?? []) visitRole(role);
  return roles;
}

export function effectiveGrants(access, name) {
  const normalized = normalizeName(name);
  if (!access?.users?.[normalized]) return [];
  const grants = [];
  const seenRoles = new Set();
  const visitRole = role => {
    if (seenRoles.has(role)) return;
    seenRoles.add(role);
    const definition = access.roles?.[role];
    if (!definition) return;
    grants.push(...normalizeGrants(definition.grants));
    for (const parent of definition.inherits ?? []) visitRole(normalizeName(parent));
  };
  grants.push(...normalizeGrants(access.users[normalized].grants));
  for (const role of roleNames(access, normalized)) visitRole(role);
  return grants;
}

export function can(access, name, permission, object = '*') {
  const normalizedUser = normalizeName(name);
  if (!access?.users?.[normalizedUser]) return false;
  const permissionName = normalizeName(permission);
  const objectName = object === undefined || object === null ? '*' : normalizeName(object);
  return effectiveGrants(access, normalizedUser).some(grant => {
    if (grant.object !== '*' && grant.object !== objectName) return false;
    return grant.permissions.includes('*') || grant.permissions.includes(permissionName);
  });
}

export function canConnect(access, name, password) {
  return verifyUser(access, name, password) && can(access, name, 'CONNECT');
}

export function hasRole(access, name, role) {
  return roleNames(access, name).includes(normalizeName(role));
}

export function normalizeAccess(raw, previous) {
  if (!raw || typeof raw !== 'object') throw new Error('Access catalog must be an object');
  const users = {};
  const usersRaw = raw.users && typeof raw.users === 'object' && !Array.isArray(raw.users) ? Object.entries(raw.users)
    : Array.isArray(raw.users) ? raw.users.map(user => [user?.name, user]) : [];
  for (const [nameValue, value] of usersRaw) {
    const name = normalizeName(nameValue ?? value?.name);
    if (!name || name.length > 64) throw new Error('Invalid user name');
    const previousUser = previous?.users?.[name];
    let hash = previousUser?.hash ?? null;
    if (value && typeof value.password === 'string' && value.password.length > 0) hash = hashPassword(value.password);
    else if (value && value.hash && typeof value.hash === 'object') hash = value.hash;
    users[name] = {
      hash,
      roles: [...new Set((value?.roles ?? []).map(normalizeName).filter(Boolean))],
      grants: normalizeGrants(value?.grants),
    };
  }
  const roles = {};
  const rolesRaw = raw.roles && typeof raw.roles === 'object' && !Array.isArray(raw.roles) ? Object.entries(raw.roles)
    : Array.isArray(raw.roles) ? raw.roles.map(role => [role?.name, role]) : [];
  for (const [nameValue, value] of rolesRaw) {
    const name = normalizeName(nameValue ?? value?.name);
    if (!name || name.length > 64) throw new Error('Invalid role name');
    roles[name] = {
      inherits: [...new Set((value?.inherits ?? []).map(normalizeName).filter(Boolean))],
      grants: normalizeGrants(value?.grants),
    };
  }
  for (const [name, role] of Object.entries(roles)) {
    const seen = new Set();
    const stack = [name];
    while (stack.length) {
      const current = stack.pop();
      if (seen.has(current)) throw new Error(`Role inheritance cycle at ${current}`);
      seen.add(current);
      const definition = roles[current];
      for (const parent of definition.inherits) {
        if (!roles[parent]) throw new Error(`Missing inherited role ${parent}`);
        stack.push(parent);
      }
    }
  }
  return { version: 1, users, roles };
}

export function loadAccess(path) {
  if (!existsSync(path)) {
    const value = defaultAccess();
    saveAccess(path, value);
    return value;
  }
  const parsed = JSON.parse(readFileSync(path, 'utf8'));
  return normalizeAccess(parsed, parsed);
}

export function saveAccess(path, access) {
  const serialized = JSON.stringify(access, null, 2);
  const temporary = path + '.tmp';
  writeFileSync(temporary, serialized, 'utf8');
  renameSync(temporary, path);
}

export function publicAccess(access) {
  return {
    version: access.version,
    users: Object.entries(access.users).map(([name, user]) => ({
      name,
      roles: [...user.roles],
      grants: clone(user.grants) ?? [],
      passwordProtected: Boolean(user.hash),
    })),
    roles: Object.entries(access.roles).map(([name, role]) => ({
      name,
      inherits: [...role.inherits],
      grants: clone(role.grants) ?? [],
    })),
  };
}

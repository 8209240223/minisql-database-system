// X24 统一鉴权入口。
//
// CLI 与 HTTP bridge 必须经由本模块判定权限，任何调用方都不允许直接访问执行器或
// 自行拼装权限判断。所有拒绝都返回同一条 `Permission denied`（错误码 7001），
// 不区分"对象不存在"和"对象无权访问"，避免通过错误信息探测数据库结构。
//
// 权限版本（permissionVersion）在每次成功的授权变更后自增。调用方在编译阶段记录
// 版本号，执行前若版本已变化必须重新鉴权，从而保证"撤权后旧计划下一次执行立即失败"。

import {
  ALL_PERMISSIONS, can, defaultAccess, effectiveGrants, hashPassword,
  normalizeAccess, publicAccess, roleNames, verifyUser,
} from './access-catalog.mjs';
import { openStore, pagesPathFor, readHeader, writeStore } from './access-store.mjs';

export const PERMISSION_DENIED_CODE = 7001;

export class AccessDeniedError extends Error {
  constructor(reason = 'denied') {
    super('Permission denied');
    this.name = 'AccessDeniedError';
    this.status = 403;
    this.code = PERMISSION_DENIED_CODE;
    // reason 只写审计日志，不回传给调用方。
    this.reason = reason;
  }
}

export class AccessRequestError extends Error {
  constructor(message, status = 400) {
    super(message);
    this.name = 'AccessRequestError';
    this.status = status;
  }
}

const normalizeName = value => String(value ?? '').trim().toLowerCase();
const NAME_PATTERN = /^[a-z_][a-z0-9_]{0,63}$/;
const OBJECT_PATTERN = /^(\*|[a-z_][a-z0-9_]{0,63})$/;

function requireName(kind, value) {
  const name = normalizeName(value);
  if (!NAME_PATTERN.test(name)) throw new AccessRequestError(`Invalid ${kind} name`);
  return name;
}

function requireObject(value) {
  const object = value === undefined || value === null || value === '' ? '*' : normalizeName(value);
  if (!OBJECT_PATTERN.test(object)) throw new AccessRequestError('Invalid object name');
  return object;
}

/** 显式 GRANT/REVOKE 必须拒绝未知权限，不能像批量导入那样静默丢弃。 */
function requirePermissions(raw) {
  const list = Array.isArray(raw) ? raw : [raw];
  if (!list.length) throw new AccessRequestError('At least one permission is required');
  const permissions = [];
  for (const item of list) {
    const name = normalizeName(item);
    if (!ALL_PERMISSIONS.has(name)) throw new AccessRequestError(`Unknown permission ${JSON.stringify(String(item))}`);
    if (!permissions.includes(name)) permissions.push(name);
  }
  return permissions;
}

// ---------------------------------------------------------------------------
// SQL → 权限检查项映射（CLI 与 HTTP 共用，保证两条路径判定完全一致）
// ---------------------------------------------------------------------------

export function firstKeyword(sql) {
  return String(sql ?? '').replace(/^\s+|\s+$/g, '')
    .match(/^(?:\/\*[\s\S]*?\*\/\s*|--[^\r\n]*\r?\n\s*)*([a-zA-Z]+)/)?.[1]?.toUpperCase() ?? '';
}

export function tableReferences(sql, keyword) {
  const stripped = String(sql ?? '').replace(/^EXPLAIN(?:\s+ANALYZE)?/i, '');
  const tables = [];
  const add = pattern => {
    const expression = new RegExp(pattern, 'gi');
    let match;
    while ((match = expression.exec(stripped))) {
      const name = match[1]?.toLowerCase();
      if (name && !tables.includes(name)) tables.push(name);
    }
  };
  if (keyword === 'SELECT') {
    add('\\b(?:FROM|JOIN)\\s+([A-Za-z_][A-Za-z0-9_]*)');
    add('\\bUPDATE\\s+([A-Za-z_][A-Za-z0-9_]*)');
  } else if (keyword === 'INSERT') add('\\bINTO\\s+([A-Za-z_][A-Za-z0-9_]*)');
  else if (keyword === 'UPDATE') add('\\bUPDATE\\s+([A-Za-z_][A-Za-z0-9_]*)');
  else if (keyword === 'DELETE') add('\\bFROM\\s+([A-Za-z_][A-Za-z0-9_]*)');
  else if (keyword === 'DROP') add('\\bTABLE\\s+([A-Za-z_][A-Za-z0-9_]*)');
  else if (keyword === 'CREATE') {
    add('\\bTABLE\\s+([A-Za-z_][A-Za-z0-9_]*)');
    add('\\bON\\s+([A-Za-z_][A-Za-z0-9_]*)');
  }
  return tables;
}

export function sqlPermissionChecks(mode, sql) {
  if (mode === 'catalog' || mode === 'statistics' || mode === 'buffer') return [{ permission: 'read', object: '*' }];
  if (mode === 'health' || mode === 'audit' || mode === 'capabilities') return [{ permission: 'read', object: '*' }];
  if (mode === 'close') return [{ permission: 'connect', object: '*' }];
  const keyword = firstKeyword(sql);
  const objects = tableReferences(sql, keyword);
  let permission = 'compile';
  if (keyword === 'BEGIN' || keyword === 'COMMIT' || keyword === 'ROLLBACK' || keyword === 'CHECKPOINT') permission = 'transaction';
  else if (keyword === 'CREATE') permission = 'create';
  else if (keyword === 'DROP') permission = 'drop';
  else if (keyword === 'SELECT' || keyword === 'INSERT' || keyword === 'UPDATE' || keyword === 'DELETE') permission = keyword.toLowerCase();
  if (!objects.length) return [{ permission, object: '*' }];
  return objects.map(object => ({ permission, object }));
}

// ---------------------------------------------------------------------------
// 审计脱敏
// ---------------------------------------------------------------------------

const SENSITIVE_KEYS = /^(password|newPassword|oldPassword|hash|digest|salt|secret|token)$/i;

/** 把 SQL 或请求体里的口令字面量替换掉，审计日志永远不落明文口令或散列。 */
export function redactSql(sql) {
  return String(sql ?? '')
    .replace(/(\b(?:IDENTIFIED\s+BY|PASSWORD)\s*=?\s*)('(?:[^']|'')*'|"(?:[^"]|"")*"|\S+)/gi, '$1<redacted>')
    .slice(0, 4096);
}

export function redactValue(value, depth = 0) {
  if (depth > 6 || value === null || typeof value !== 'object') return value;
  if (Array.isArray(value)) return value.map(item => redactValue(item, depth + 1));
  const output = {};
  for (const [key, item] of Object.entries(value)) {
    if (SENSITIVE_KEYS.test(key)) output[key] = item === undefined || item === null ? item : '<redacted>';
    else output[key] = redactValue(item, depth + 1);
  }
  return output;
}

// ---------------------------------------------------------------------------
// Authorizer
// ---------------------------------------------------------------------------

export class Authorizer {
  /**
   * @param {string} accessPath 旧 JSON 路径；页式文件由 pagesPathFor 推导
   * @param {{ onChange?: (state) => void }} [options]
   */
  constructor(accessPath, options = {}) {
    this.accessPath = accessPath;
    this.pagesFile = pagesPathFor(accessPath);
    const opened = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    this.access = opened.catalog;
    this.permissionVersion = opened.permissionVersion;
    this.catalogVersion = opened.catalogVersion;
    this.migratedFromJson = opened.migrated;
    this.storeSource = opened.source;
    this.onChange = options.onChange;
  }

  /** 供 capabilities/诊断使用的存储描述。 */
  describeStore() {
    return {
      model: 'paged-access-catalog',
      pageSize: 4096,
      file: this.pagesFile,
      permissionVersion: this.permissionVersion,
      catalogVersion: this.catalogVersion,
      migratedFromJson: this.migratedFromJson,
      source: this.storeSource,
    };
  }

  /** 从磁盘 META 页读取版本号，用于多进程（CLI 与 bridge 并存）时检测外部改动。 */
  reloadIfStale() {
    let header;
    try { header = readHeader(this.pagesFile); }
    catch { return false; }
    if (header.permissionVersion === this.permissionVersion) return false;
    const opened = openStore(this.accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    this.access = opened.catalog;
    this.permissionVersion = opened.permissionVersion;
    this.catalogVersion = opened.catalogVersion;
    return true;
  }

  // -- 判定 ---------------------------------------------------------------

  authenticate(user, password) {
    const name = normalizeName(user);
    if (!this.access.users[name]) return { ok: false, reason: 'unknown-user' };
    if (!verifyUser(this.access, name, password ?? null)) return { ok: false, reason: 'bad-password' };
    if (!can(this.access, name, 'CONNECT')) return { ok: false, reason: 'no-connect' };
    return { ok: true, user: name, permissionVersion: this.permissionVersion };
  }

  /** 认证失败时抛出与授权失败完全相同的错误，避免区分"用户不存在"与"口令错误"。 */
  requireIdentity(user, password) {
    const outcome = this.authenticate(user, password);
    if (!outcome.ok) throw new AccessDeniedError(outcome.reason);
    return outcome.user;
  }

  can(user, permission, object = '*') {
    return can(this.access, user, permission, object);
  }

  /** @param {{permission: string, object?: string}[]} checks 全部通过才算通过。 */
  authorize(user, checks) {
    for (const check of checks) {
      if (!can(this.access, user, check.permission, check.object ?? '*')) {
        throw new AccessDeniedError(`missing ${check.permission} on ${check.object ?? '*'}`);
      }
    }
    return this.permissionVersion;
  }

  authorizeSql(user, mode, sql) {
    return this.authorize(user, sqlPermissionChecks(mode, sql));
  }

  /** 目录/统计响应过滤：无 SELECT 权限的表不得出现在任何面向用户的列表里。 */
  visibleTables(user, tables) {
    return (Array.isArray(tables) ? tables : []).filter(table => can(this.access, user, 'SELECT', table?.name));
  }

  roles(user) { return roleNames(this.access, normalizeName(user)); }
  grants(user) { return effectiveGrants(this.access, normalizeName(user)); }

  publicState() {
    return { ...publicAccess(this.access), permissionVersion: this.permissionVersion, store: this.describeStore().model };
  }

  /** 单个主体的有效权限视图，供工作台"为什么被拒绝"面板使用。 */
  describeSubject(user) {
    const name = normalizeName(user);
    if (!this.access.users[name]) throw new AccessDeniedError('unknown-user');
    return {
      name,
      roles: this.roles(name),
      directGrants: this.access.users[name].grants,
      effectiveGrants: this.grants(name),
      passwordProtected: Boolean(this.access.users[name].hash),
      permissionVersion: this.permissionVersion,
    };
  }

  // -- 原子变更 -----------------------------------------------------------

  /**
   * 在克隆上应用变更，规范化（含角色环检测）后再落盘。
   * 落盘失败时回滚内存状态，保证内存与页文件始终一致。
   */
  #commit(mutate, description) {
    const draft = JSON.parse(JSON.stringify(this.access));
    mutate(draft);
    const normalized = normalizeAccess(draft, draft);
    // 保底检查：任何变更都不允许把数据库改成"没有人能再管理权限"的状态。
    const administrators = Object.keys(normalized.users)
      .filter(name => can(normalized, name, 'GRANT') && can(normalized, name, 'CONNECT'));
    if (!administrators.length) throw new AccessRequestError('Refusing a change that would leave no user able to manage permissions', 409);
    const previousAccess = this.access;
    const previousVersion = this.permissionVersion;
    const nextVersion = previousVersion + 1;
    if (nextVersion > 0xffffffff) throw new AccessRequestError('Permission version space exhausted', 500);
    this.access = normalized;
    this.permissionVersion = nextVersion;
    try {
      writeStore(this.pagesFile, normalized, { permissionVersion: nextVersion, catalogVersion: this.catalogVersion });
    } catch (error) {
      this.access = previousAccess;
      this.permissionVersion = previousVersion;
      throw error;
    }
    const state = { permissionVersion: nextVersion, change: description };
    this.onChange?.(state);
    return state;
  }

  createUser(actor, { name, password, roles = [] }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', name);
    if (this.access.users[user]) throw new AccessRequestError(`User ${user} already exists`, 409);
    const roleList = (Array.isArray(roles) ? roles : []).map(value => requireName('role', value));
    for (const role of roleList) if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`);
    if (password !== undefined && password !== null && typeof password !== 'string')
      throw new AccessRequestError('Password must be a string');
    if (typeof password === 'string' && (password.length < 1 || password.length > 256))
      throw new AccessRequestError('Password must be 1..256 characters');
    return {
      ...this.#commit(draft => {
        draft.users[user] = { hash: typeof password === 'string' ? hashPassword(password) : null, roles: roleList, grants: [] };
      }, { action: 'CREATE USER', user }),
      user,
    };
  }

  dropUser(actor, { name }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', name);
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    if (user === normalizeName(actor)) throw new AccessRequestError('Refusing to drop the acting user', 409);
    return { ...this.#commit(draft => { delete draft.users[user]; }, { action: 'DROP USER', user }), user };
  }

  setPassword(actor, { name, password }) {
    const user = requireName('user', name);
    // 用户可以改自己的口令；改别人的口令需要 GRANT。
    if (user !== normalizeName(actor)) this.authorize(actor, [{ permission: 'grant' }]);
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    if (password !== null && (typeof password !== 'string' || password.length < 1 || password.length > 256))
      throw new AccessRequestError('Password must be a 1..256 character string or null');
    return {
      ...this.#commit(draft => {
        draft.users[user] = { ...draft.users[user], hash: password === null ? null : hashPassword(password) };
      }, { action: 'ALTER USER PASSWORD', user }),
      user,
    };
  }

  createRole(actor, { name, inherits = [] }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const role = requireName('role', name);
    if (this.access.roles[role]) throw new AccessRequestError(`Role ${role} already exists`, 409);
    const parents = (Array.isArray(inherits) ? inherits : []).map(value => requireName('role', value));
    for (const parent of parents) if (!this.access.roles[parent]) throw new AccessRequestError(`Unknown role ${parent}`);
    return {
      ...this.#commit(draft => { draft.roles[role] = { inherits: parents, grants: [] }; }, { action: 'CREATE ROLE', role }),
      role,
    };
  }

  dropRole(actor, { name }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const role = requireName('role', name);
    if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`, 404);
    const heldBy = Object.entries(this.access.users).filter(([, user]) => user.roles.includes(role)).map(([name]) => name);
    const inheritedBy = Object.entries(this.access.roles).filter(([, value]) => value.inherits.includes(role)).map(([name]) => name);
    if (heldBy.length || inheritedBy.length)
      throw new AccessRequestError(`Role ${role} is still referenced by ${[...heldBy, ...inheritedBy].join(', ')}`, 409);
    return { ...this.#commit(draft => { delete draft.roles[role]; }, { action: 'DROP ROLE', role }), role };
  }

  grantRole(actor, { user: userName, role: roleName }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', userName);
    const role = requireName('role', roleName);
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`, 404);
    return {
      ...this.#commit(draft => {
        const roles = new Set(draft.users[user].roles);
        roles.add(role);
        draft.users[user].roles = [...roles];
      }, { action: 'GRANT ROLE', user, role }),
      user, role,
    };
  }

  revokeRole(actor, { user: userName, role: roleName }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', userName);
    const role = requireName('role', roleName);
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    return {
      ...this.#commit(draft => {
        draft.users[user].roles = draft.users[user].roles.filter(value => value !== role);
      }, { action: 'REVOKE ROLE', user, role }),
      user, role,
    };
  }

  #subject(draft, kind, name) {
    if (kind === 'user') {
      if (!draft.users[name]) throw new AccessRequestError(`Unknown user ${name}`, 404);
      return draft.users[name];
    }
    if (!draft.roles[name]) throw new AccessRequestError(`Unknown role ${name}`, 404);
    return draft.roles[name];
  }

  /** 逐项 GRANT：合并到 (主体, 对象) 现有权限集合，不覆盖其他对象的授权。 */
  grant(actor, { subject = 'user', name, object, permissions }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const kind = subject === 'role' ? 'role' : 'user';
    const target = requireName(kind, name);
    const objectName = requireObject(object);
    const list = requirePermissions(permissions);
    this.#subject(this.access, kind, target);
    return {
      ...this.#commit(draft => {
        const holder = this.#subject(draft, kind, target);
        const existing = holder.grants.find(grant => grant.object === objectName);
        if (existing) existing.permissions = [...new Set([...existing.permissions, ...list])];
        else holder.grants.push({ object: objectName, permissions: list });
      }, { action: 'GRANT', subject: kind, name: target, object: objectName, permissions: list }),
      subject: kind, name: target, object: objectName, permissions: list,
    };
  }

  /** 逐项 REVOKE：只移除指定权限；集合清空后删除该对象条目。 */
  revoke(actor, { subject = 'user', name, object, permissions }) {
    this.authorize(actor, [{ permission: 'grant' }]);
    const kind = subject === 'role' ? 'role' : 'user';
    const target = requireName(kind, name);
    const objectName = requireObject(object);
    const list = requirePermissions(permissions);
    this.#subject(this.access, kind, target);
    return {
      ...this.#commit(draft => {
        const holder = this.#subject(draft, kind, target);
        holder.grants = holder.grants
          .map(grant => grant.object !== objectName ? grant
            : { ...grant, permissions: grant.permissions.filter(value => !list.includes(value)) })
          .filter(grant => grant.permissions.length > 0);
      }, { action: 'REVOKE', subject: kind, name: target, object: objectName, permissions: list }),
      subject: kind, name: target, object: objectName, permissions: list,
    };
  }

  /** 整表替换（保留旧的 PUT /api/access 语义），同样走版本自增和管理员保底检查。 */
  replace(actor, raw) {
    this.authorize(actor, [{ permission: 'grant' }]);
    return this.#commit(draft => {
      const next = normalizeAccess(raw, this.access);
      draft.users = next.users;
      draft.roles = next.roles;
    }, { action: 'REPLACE ACCESS CATALOG' });
  }
}

export { ALL_PERMISSIONS };

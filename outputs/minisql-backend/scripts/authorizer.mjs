// X24 统一鉴权入口。
//
// CLI 与 HTTP bridge 必须经由本模块判定权限，任何调用方都不允许直接访问执行器或
// 自行拼装权限判断。所有拒绝都返回同一条 `Permission denied`（错误码 7001），
// 不区分"对象不存在"和"对象无权访问"，避免通过错误信息探测数据库结构。
//
// 权限版本（permissionVersion）在每次成功的授权变更后自增。调用方在编译阶段记录
// 版本号，执行前若版本已变化必须重新鉴权，从而保证"撤权后旧计划下一次执行立即失败"。

import {
// 从权限目录模块引入权限模型相关的全部工具。
  ALL_PERMISSIONS, can, defaultAccess, effectiveGrants, hashPassword,
  // 权限白名单、单点判定、默认目录、有效授权汇总、口令哈希。
  normalizeAccess, publicAccess, roleNames, verifyUser,
  // 目录规范化、对外视图、角色展开、口令校验。
} from './access-catalog.mjs';
// 权限模型的唯一来源。
import { openStore, pagesPathFor, readHeader, writeStore } from './access-store.mjs';
// 页式持久化层的入口。
import { firstKeyword, tableReferences } from './sql-object-references.mjs';
// SQL 取对象名（词法兜底）的两个函数，用于把 SQL 映射成待检查的对象。

export { firstKeyword, tableReferences };
// 原样再导出，方便调用方只依赖本模块即可拿到这两件事。

export const PERMISSION_DENIED_CODE = 7001;
// 统一的权限拒绝错误码。调用方（CLI/HTTP）按它判断"这是权限问题"。

export class AccessDeniedError extends Error {
// 权限拒绝专用错误。
  constructor(reason = 'denied') {
  // reason 是内部原因，用于写审计日志。
    super('Permission denied');
    // 对外信息固定为这一句，不透露具体原因。
    this.name = 'AccessDeniedError';
    // 固定名字，便于日志与测试识别。
    this.status = 403;
    // HTTP 层直接映射成 403。
    this.code = PERMISSION_DENIED_CODE;
    // 业务错误码。
    // reason 只写审计日志，不回传给调用方。
    // 也就是说：外面看到"没有权限"就够，不需要知道是哪个对象导致的。
    this.reason = reason;
    // 内部原因挂在这里，仅供服务端自己使用。
  }
}

export class AccessRequestError extends Error {
// 请求本身不合法（名字格式错、权限名不认识等）时用的错误。
  constructor(message, status = 400) {
  // 这类错误可以给出具体信息，因为问题出在调用方自己的请求上。
    super(message);
    // 用调用方给的信息。
    this.name = 'AccessRequestError';
    // 固定名字。
    this.status = status;
    // 默认 400，调用方可以指定成别的（例如 409 冲突）。
  }
}

const normalizeName = value => String(value ?? '').trim().toLowerCase();
// 名字规范化的简写版本（与权限目录模块里的规则保持一致）。
const NAME_PATTERN = /^[a-z_][a-z0-9_]{0,63}$/;
// 用户名/角色名的合法格式：字母或下划线开头，后接字母数字下划线，总长不超过 64。
const OBJECT_PATTERN = /^(\*|[a-z_][a-z0-9_]{0,63})$/;
// 对象名的合法格式：通配 '*' 或一个合法标识符。

function requireName(kind, value) {
// 校验并返回合法名字；kind 用于拼出"是用户名还是角色名"的错误信息。
  const name = normalizeName(value);
  // 规范化。
  if (!NAME_PATTERN.test(name)) throw new AccessRequestError(`Invalid ${kind} name`);
  // 不符合格式就拒绝，避免把奇怪字符写进权限目录。
  return name;
  // 返回规范名。
}

function requireObject(value) {
// 校验并返回合法对象名；空值按通配 '*' 处理。
  const object = value === undefined || value === null || value === '' ? '*' : normalizeName(value);
  // 三种"没给"的情形统一成通配。
  if (!OBJECT_PATTERN.test(object)) throw new AccessRequestError('Invalid object name');
  // 不符合格式就拒绝。
  return object;
  // 返回规范对象名。
}

/** 显式 GRANT/REVOKE 必须拒绝未知权限，不能像批量导入那样静默丢弃。 */
// 这里的行为刻意与 normalizeAccess 不同：导入历史数据时可以宽容，
// 但用户在命令行显式敲了一个不存在的权限名，必须报错而不是当作没写。
function requirePermissions(raw) {
// 把入参整理成合法的权限数组。
  const list = Array.isArray(raw) ? raw : [raw];
  // 允许传单个权限或数组，统一成数组处理。
  if (!list.length) throw new AccessRequestError('At least one permission is required');
  // 一个都不给就拒绝。
  const permissions = [];
  // 结果。
  for (const item of list) {
  // 逐项校验。
    const name = normalizeName(item);
    // 规范化。
    if (!ALL_PERMISSIONS.has(name)) throw new AccessRequestError(`Unknown permission ${JSON.stringify(String(item))}`);
    // 不认识就报错，并把原始写法回显出来（用 JSON.stringify 包住，避免特殊字符干扰日志）。
    if (!permissions.includes(name)) permissions.push(name);
    // 去重后再收。
  }
  return permissions;
  // 返回权限列表。
}

// ---------------------------------------------------------------------------
// SQL → 权限检查项映射（CLI 与 HTTP 共用，保证两条路径判定完全一致）
// ---------------------------------------------------------------------------

export function sqlPermissionChecks(mode, sql) {
// 把"操作 + SQL"翻译成一串待检查的权限项，CLI 与 HTTP 两条路径共用这一份映射。
  if (mode === 'catalog' || mode === 'statistics' || mode === 'buffer') return [{ permission: 'read', object: '*' }];
  // 目录、统计、缓冲池状态：只读，且不针对具体对象。
  if (mode === 'health' || mode === 'audit' || mode === 'capabilities') return [{ permission: 'read', object: '*' }];
  // 健康检查、审计查询、能力查询同样是只读。
  if (mode === 'close') return [{ permission: 'connect', object: '*' }];
  // 关闭会话需要连接权限。
  const keyword = firstKeyword(sql);
  // 取出 SQL 的第一个关键字（跳过注释与空白）。
  const objects = tableReferences(sql, keyword);
  // 保守地抽出这条语句涉及的表名。
  let permission = 'compile';
  // 默认权限是 compile：只编译不执行的路径按它算。
  if (keyword === 'BEGIN' || keyword === 'COMMIT' || keyword === 'ROLLBACK' || keyword === 'CHECKPOINT') permission = 'transaction';
  // 事务控制语句需要事务权限。
  else if (keyword === 'CREATE') permission = 'create';
  // 建对象需要 create 权限。
  else if (keyword === 'DROP') permission = 'drop';
  // 删对象需要 drop 权限。
  else if (keyword === 'SELECT' || keyword === 'INSERT' || keyword === 'UPDATE' || keyword === 'DELETE') permission = keyword.toLowerCase();
  // 四种数据语句各自对应同名权限（关键字本来就是大写，转小写后与权限名一致）。
  if (!objects.length) return [{ permission, object: '*' }];
  // 一个对象都没识别出来时退化成对通配对象的检查，
  // 这样"解析不出来"只会更严格，不会绕过鉴权。
  return objects.map(object => ({ permission, object }));
  // 否则对每个对象分别生成一条待检查项。
}

// ---------------------------------------------------------------------------
// 审计脱敏
// ---------------------------------------------------------------------------

const SENSITIVE_KEYS = /^(password|newPassword|oldPassword|hash|digest|salt|secret|token)$/i;
// 需要脱敏的字段名（大小写不敏感）：各种口令、哈希、摘要、盐、密钥与令牌。

/** 把 SQL 或请求体里的口令字面量替换掉，审计日志永远不落明文口令或散列。 */
// 注意这里处理的是"文本里出现的口令字面量"，与下面按字段名脱敏是两种不同场景。
export function redactSql(sql) {
// 对 SQL 文本做脱敏。
  return String(sql ?? '')
    // 容错处理空输入。
    .replace(/(\b(?:IDENTIFIED\s+BY|PASSWORD)\s*=?\s*)('(?:[^']|'')*'|"(?:[^"]|"")*"|\S+)/gi, '$1<redacted>')
    // 匹配"IDENTIFIED BY / PASSWORD 后面跟的值"，值可以是单引号串、双引号串或不含空白的词；
    // 保留关键字本身，只把值换成 <redacted>，这样日志仍能看出用户做了什么操作。
    .slice(0, 4096);
    // 再截断到 4096 字符，避免超长 SQL 把日志撑爆。
}

export function redactValue(value, depth = 0) {
// 对一个 JSON 结构做脱敏：按字段名判断，敏感字段整段替换。
  if (depth > 6 || value === null || typeof value !== 'object') return value;
  // 超过 6 层就不再深入（防止深层结构或循环引用把递归拖垮），非对象直接返回。
  if (Array.isArray(value)) return value.map(item => redactValue(item, depth + 1));
  // 数组逐项递归。
  const output = {};
  // 结果对象。
  for (const [key, item] of Object.entries(value)) {
  // 逐字段处理。
    if (SENSITIVE_KEYS.test(key)) output[key] = item === undefined || item === null ? item : '<redacted>';
    // 敏感字段：保留键与"是否为空"这一信息，但把值抹掉。
    else output[key] = redactValue(item, depth + 1);
    // 其它字段递归处理。
  }
  return output;
  // 返回脱敏后的对象。
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
  // 构造鉴权器：打开（必要时迁移）权限存储，并记下版本与来源。
    this.accessPath = accessPath;
    // 保留旧 JSON 路径，供迁移与诊断使用。
    this.pagesFile = pagesPathFor(accessPath);
    // 推出页式文件路径。
    const opened = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    // 打开存储：优先页式文件，其次从旧 JSON 迁移，最后用默认目录初始化。
    this.access = opened.catalog;
    // 当前生效的权限目录。
    this.permissionVersion = opened.permissionVersion;
    // 权限版本号：鉴权时要用它判断"编译后是否被撤权"。
    this.catalogVersion = opened.catalogVersion;
    // 结构版本号。
    this.migratedFromJson = opened.migrated;
    // 本次是否发生过从 JSON 的迁移。
    this.storeSource = opened.source;
    // 数据来源：pages / json / defaults。
    this.onChange = options.onChange;
    // 变更回调：每次权限变更后由调用方决定如何处理（例如通知会话重新鉴权）。
  }

  /** 供 capabilities/诊断使用的存储描述。 */
  describeStore() {
  // 返回存储的描述信息，供 capabilities 与诊断接口展示。
    return {
    // 返回一个纯数据对象。
      model: 'paged-access-catalog',
      // 存储模型的标识，前端据此判断是页式权限目录。
      pageSize: 4096,
      // 页大小。
      file: this.pagesFile,
      // 实际使用的文件路径。
      permissionVersion: this.permissionVersion,
      // 当前权限版本。
      catalogVersion: this.catalogVersion,
      // 当前结构版本。
      migratedFromJson: this.migratedFromJson,
      // 是否从旧 JSON 迁移而来。
      source: this.storeSource,
      // 数据来源。
    };
  }

  /** 从磁盘 META 页读取版本号，用于多进程（CLI 与 bridge 并存）时检测外部改动。 */
  reloadIfStale() {
  // 检查磁盘上的权限版本是否已经变化；变了就整体重载，返回是否发生了重载。
    let header;
    // 元数据头。
    try { header = readHeader(this.pagesFile); }
    // 只读 META 页，成本很低。
    catch { return false; }
    // 读不到（文件被删或损坏）就当作"没变化"，把问题留给真正的读取路径去报错。
    if (header.permissionVersion === this.permissionVersion) return false;
    // 版本一致说明没有外部改动，无需重载。
    const opened = openStore(this.accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    // 版本变了就完整重载一次（走同样的打开流程，含校验）。
    this.access = opened.catalog;
    // 换上新目录。
    this.permissionVersion = opened.permissionVersion;
    // 换上新版本号。
    this.catalogVersion = opened.catalogVersion;
    // 同步结构版本。
    return true;
    // 告诉调用方"确实发生了外部改动"。
  }

  // -- 判定 ---------------------------------------------------------------

  authenticate(user, password) {
  // 认证：判断"这个人能不能登录"，返回结论与原因（原因只用于审计）。
    const name = normalizeName(user);
    // 规范化用户名。
    if (!this.access.users[name]) return { ok: false, reason: 'unknown-user' };
    // 用户不存在。
    if (!verifyUser(this.access, name, password ?? null)) return { ok: false, reason: 'bad-password' };
    // 口令不对。
    if (!can(this.access, name, 'CONNECT')) return { ok: false, reason: 'no-connect' };
    // 口令对了但该用户连 connect 权限都没有。
    return { ok: true, user: name, permissionVersion: this.permissionVersion };
    // 通过时把规范用户名与当时的权限版本一起带出去。
  }

  /** 认证失败时抛出与授权失败完全相同的错误，避免区分"用户不存在"与"口令错误"。 */
  requireIdentity(user, password) {
  // 认证失败就抛错，成功时返回规范用户名。
    const outcome = this.authenticate(user, password);
    // 先走认证。
    if (!outcome.ok) throw new AccessDeniedError(outcome.reason);
    // 失败时抛的是同一个 AccessDeniedError，原因只在内部保留。
    return outcome.user;
    // 返回规范用户名供后续鉴权使用。
  }

  can(user, permission, object = '*') {
  // 单点权限判断的薄封装。
    return can(this.access, user, permission, object);
    // 直接转给权限目录模块的判定函数。
  }

  /** @param {{permission: string, object?: string}[]} checks 全部通过才算通过。 */
  authorize(user, checks) {
  // 逐条检查权限，全部通过才返回；任一条不过就抛错。
    for (const check of checks) {
    // 逐项处理。
      if (!can(this.access, user, check.permission, check.object ?? '*')) {
      // 缺权限。
        throw new AccessDeniedError(`missing ${check.permission} on ${check.object ?? '*'}`);
        // 内部原因写清缺什么权限、在哪个对象上（便于审计定位）。
      }
    }
    // 全部检查完毕。
    return this.permissionVersion;
    // 返回"本次鉴权所依据的权限版本"，调用方执行前要拿它再比一次。
  }

  authorizeSql(user, mode, sql) {
  // 按操作与 SQL 做鉴权（内部先用 sqlPermissionChecks 生成待查项）。
    return this.authorize(user, sqlPermissionChecks(mode, sql));
    // 返回权限版本。
  }

  /** 目录/统计响应过滤：无 SELECT 权限的表不得出现在任何面向用户的列表里。 */
  visibleTables(user, tables) {
  // 过滤目录/统计响应：没有 SELECT 权限的表不该出现在任何面向用户的列表里。
    return (Array.isArray(tables) ? tables : []).filter(table => can(this.access, user, 'SELECT', table?.name));
    // 注意这里查的是"对该表有没有 SELECT 权限"，而不是单纯隐藏敏感字段；
    // 目的是让无权用户连"库里有这张表"都看不到。
  }

  roles(user) { return roleNames(this.access, normalizeName(user)); }
  // 查询用户拥有的角色（含继承展开）。
  grants(user) { return effectiveGrants(this.access, normalizeName(user)); }
  // 查询用户实际生效的全部授权。

  publicState() {
  // 对外展示的权限状态（脱敏后的目录 + 版本号 + 存储模型）。
    return { ...publicAccess(this.access), permissionVersion: this.permissionVersion, store: this.describeStore().model };
    // publicAccess 已经保证不会泄露口令哈希，这里再补上版本与存储模型标识。
  }

  /** 单个主体的有效权限视图，供工作台"为什么被拒绝"面板使用。 */
  describeSubject(user) {
  // 给出某个主体的完整权限视图，供工作台的"为什么被拒绝"面板解释判定依据。
    const name = normalizeName(user);
    // 规范化用户名。
    if (!this.access.users[name]) throw new AccessDeniedError('unknown-user');
    // 用户不存在时同样按权限拒绝处理，不泄露"有没有这个用户"。
    return {
    // 返回视图对象。
      name,
      // 规范用户名。
      roles: this.roles(name),
      // 拥有的角色（含继承展开）。
      directGrants: this.access.users[name].grants,
      // 直接授予的授权（未展开角色）。
      effectiveGrants: this.grants(name),
      // 实际生效的全部授权（个人 + 角色）。
      passwordProtected: Boolean(this.access.users[name].hash),
      // 是否设了口令——只给布尔值，不给哈希。
      permissionVersion: this.permissionVersion,
      // 这份视图对应的权限版本，便于前端判断是否需要刷新。
    };
  }

  // -- 原子变更 -----------------------------------------------------------

  /**
   * 在克隆上应用变更，规范化（含角色环检测）后再落盘。
   * 落盘失败时回滚内存状态，保证内存与页文件始终一致。
   */
  #commit(mutate, description) {
  // 私有方法：在副本上应用变更 → 规范化与安全检查 → 落盘 → 更新内存状态。
  // 这是所有权限变更的唯一出口，保证它们的行为完全一致。
    const draft = JSON.parse(JSON.stringify(this.access));
    // 先深拷贝出一份草稿，改动只发生在草稿上。
    mutate(draft);
    // 应用调用方给的变更函数。
    const normalized = normalizeAccess(draft, draft);
    // 规范化：这一步会做名字/权限清洗、角色继承的环检查等。
    // 保底检查：任何变更都不允许把数据库改成"没有人能再管理权限"的状态。
    // 这是防止把自己锁在门外：例如把最后一个管理员的 GRANT 权限收掉之后，
    // 就再也没有任何人能改权限了，这种变更必须拒绝。
    const administrators = Object.keys(normalized.users)
      // 遍历所有用户名，
      .filter(name => can(normalized, name, 'GRANT') && can(normalized, name, 'CONNECT'));
      // 留下同时具备"授予权限"与"连接"能力的人。
    if (!administrators.length) throw new AccessRequestError('Refusing a change that would leave no user able to manage permissions', 409);
    // 一个都没有就拒绝（409 冲突，表示与当前状态约束冲突）。
    const previousAccess = this.access;
    // 记下旧目录，落盘失败时要回滚。
    const previousVersion = this.permissionVersion;
    // 记下旧版本号。
    const nextVersion = previousVersion + 1;
    // 每次成功变更都把权限版本加一——这是"撤权后旧计划立即失效"机制的支点。
    if (nextVersion > 0xffffffff) throw new AccessRequestError('Permission version space exhausted', 500);
    // 版本号是 32 位无符号数，用尽就明确报错而不是回绕（回绕会让旧版本号重新有效）。
    this.access = normalized;
    // 先把内存状态换上新目录。
    this.permissionVersion = nextVersion;
    // 再更新版本号。
    try {
    // 尝试落盘。
      writeStore(this.pagesFile, normalized, { permissionVersion: nextVersion, catalogVersion: this.catalogVersion });
      // 写入页式文件（内部是"先写临时文件再改名"的原子替换）。
    } catch (error) {
    // 落盘失败。
      this.access = previousAccess;
      // 回滚内存目录。
      this.permissionVersion = previousVersion;
      // 回滚版本号，保证内存与磁盘始终一致。
      throw error;
      // 把错误继续抛给调用方。
    }
    // 落盘成功。
    const state = { permissionVersion: nextVersion, change: description };
    // 组装变更结果。
    this.onChange?.(state);
    // 通知订阅者（例如让所有会话重新鉴权）。
    return state;
  }
  // #commit 结束。

  createUser(actor, { name, password, roles = [] }) {
  // 创建用户（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    // 先鉴权：只有能授予权限的人才能建用户。
    const user = requireName('user', name);
    // 校验用户名格式。
    if (this.access.users[user]) throw new AccessRequestError(`User ${user} already exists`, 409);
    // 已存在就报冲突。
    const roleList = (Array.isArray(roles) ? roles : []).map(value => requireName('role', value));
    // 校验并规范化初始角色列表。
    for (const role of roleList) if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`);
    // 角色必须都已存在。
    if (password !== undefined && password !== null && typeof password !== 'string')
    // 给了口令但它不是字符串。
      throw new AccessRequestError('Password must be a string');
      // 拒绝。
    if (typeof password === 'string' && (password.length < 1 || password.length > 256))
    // 口令长度限制在 1..256。
      throw new AccessRequestError('Password must be 1..256 characters');
      // 拒绝。
    return {
    // 返回变更结果（把用户名一并带上）。
      ...this.#commit(draft => {
      // 用私有提交方法落盘。
        draft.users[user] = { hash: typeof password === 'string' ? hashPassword(password) : null, roles: roleList, grants: [] };
        // 写入新用户：给了口令就生成哈希，否则记 null（表示不设口令）。
      }, { action: 'CREATE USER', user }),
      // 变更描述会写进审计与变更通知。
      user,
      // 额外带上用户名。
    };
  }

  dropUser(actor, { name }) {
  // 删除用户（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', name);
    // 校验用户名格式。
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    // 不存在就 404。
    if (user === normalizeName(actor)) throw new AccessRequestError('Refusing to drop the acting user', 409);
    // 不允许删除"正在操作的人自己"：这在多步操作里容易把自己锁在外面。
    return { ...this.#commit(draft => { delete draft.users[user]; }, { action: 'DROP USER', user }), user };
    // 提交变更；变更描述会记录成 DROP USER。
  }

  setPassword(actor, { name, password }) {
  // 改密：自己改自己不需要额外权限，改别人需要 grant。
    const user = requireName('user', name);
    // 校验目标用户名格式。
    // 用户可以改自己的口令；改别人的口令需要 GRANT。
    // 这条规则的具体实现就在下面这一行。
    if (user !== normalizeName(actor)) this.authorize(actor, [{ permission: 'grant' }]);
    // 目标不是自己时才鉴权。
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    // 目标用户必须存在。
    if (password !== null && (typeof password !== 'string' || password.length < 1 || password.length > 256))
    // 口令允许是 null（表示清除口令），否则必须是 1..256 字符的字符串。
      throw new AccessRequestError('Password must be a 1..256 character string or null');
      // 不满足就拒绝。
    return {
    // 返回变更结果。
      ...this.#commit(draft => {
      // 提交变更。
        draft.users[user] = { ...draft.users[user], hash: password === null ? null : hashPassword(password) };
        // 保留其它字段，只替换 hash：传 null 就清空口令，否则重新生成盐与哈希。
      }, { action: 'ALTER USER PASSWORD', user }),
      // 审计描述里不包含口令内容。
      user,
      // 带上用户名。
    };
  }

  createRole(actor, { name, inherits = [] }) {
  // 创建角色（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    const role = requireName('role', name);
    // 校验角色名。
    if (this.access.roles[role]) throw new AccessRequestError(`Role ${role} already exists`, 409);
    // 已存在就冲突。
    const parents = (Array.isArray(inherits) ? inherits : []).map(value => requireName('role', value));
    // 校验并规范化要继承的父角色。
    for (const parent of parents) if (!this.access.roles[parent]) throw new AccessRequestError(`Unknown role ${parent}`);
    // 父角色必须已存在。
    return {
      ...this.#commit(draft => { draft.roles[role] = { inherits: parents, grants: [] }; }, { action: 'CREATE ROLE', role }),
      // 新角色初始没有授权；继承关系交给 normalizeAccess 做环检查。
      role,
      // 带上角色名。
    };
  }

  dropRole(actor, { name }) {
  // 删除角色（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    const role = requireName('role', name);
    // 校验角色名。
    if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`, 404);
    // 不存在就 404。
    const heldBy = Object.entries(this.access.users).filter(([, user]) => user.roles.includes(role)).map(([name]) => name);
    // 找出还持有该角色的用户。
    const inheritedBy = Object.entries(this.access.roles).filter(([, value]) => value.inherits.includes(role)).map(([name]) => name);
    // 找出还继承该角色的其它角色。
    if (heldBy.length || inheritedBy.length)
    // 只要有引用就不允许删。
      throw new AccessRequestError(`Role ${role} is still referenced by ${[...heldBy, ...inheritedBy].join(', ')}`, 409);
      // 冲突错误里列出具体是谁在引用，方便用户先解绑。
    return { ...this.#commit(draft => { delete draft.roles[role]; }, { action: 'DROP ROLE', role }), role };
    // 提交删除。
  }

  grantRole(actor, { user: userName, role: roleName }) {
  // 给用户授予角色（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', userName);
    // 校验用户名。
    const role = requireName('role', roleName);
    // 校验角色名。
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    // 用户必须存在。
    if (!this.access.roles[role]) throw new AccessRequestError(`Unknown role ${role}`, 404);
    // 角色必须存在。
    return {
      ...this.#commit(draft => {
      // 提交变更。
        const roles = new Set(draft.users[user].roles);
        // 用集合去重。
        roles.add(role);
        // 加入角色（本来就有也不会重复）。
        draft.users[user].roles = [...roles];
        // 写回数组。
      }, { action: 'GRANT ROLE', user, role }),
      // 审计描述带上用户与角色。
      user, role,
      // 返回值里也带上。
    };
  }

  revokeRole(actor, { user: userName, role: roleName }) {
  // 取消用户的角色（需要 grant 权限）。
    this.authorize(actor, [{ permission: 'grant' }]);
    const user = requireName('user', userName);
    // 校验用户名。
    const role = requireName('role', roleName);
    // 校验角色名。
    if (!this.access.users[user]) throw new AccessRequestError(`Unknown user ${user}`, 404);
    // 用户必须存在。
    return {
      ...this.#commit(draft => {
      // 提交变更。
        draft.users[user].roles = draft.users[user].roles.filter(value => value !== role);
        // 过滤掉该角色（幂等）。
      }, { action: 'REVOKE ROLE', user, role }),
      // 审计描述。
      user, role,
    };
  }

  #subject(draft, kind, name) {
  // 私有工具：按主体类型取出用户或角色定义，不存在就 404。
    if (kind === 'user') {
    // 用户分支。
      if (!draft.users[name]) throw new AccessRequestError(`Unknown user ${name}`, 404);
      // 不存在就报 404。
      return draft.users[name];
      // 返回用户定义。
    }
    // 用户分支结束。
    if (!draft.roles[name]) throw new AccessRequestError(`Unknown role ${name}`, 404);
    // 角色不存在同样 404。
    return draft.roles[name];
    // 返回角色定义。
  }
  // #subject 结束。

  /** 逐项 GRANT：合并到 (主体, 对象) 现有权限集合，不覆盖其他对象的授权。 */
  grant(actor, { subject = 'user', name, object, permissions }) {
  // 逐项授予权限；subject 可以是 user 或 role。
    this.authorize(actor, [{ permission: 'grant' }]);
    const kind = subject === 'role' ? 'role' : 'user';
    // 只认这两种主体类型，其它一律按 user 处理。
    const target = requireName(kind, name);
    // 校验主体名。
    const objectName = requireObject(object);
    // 校验对象名（没给就按通配）。
    const list = requirePermissions(permissions);
    // 校验权限清单：这里必须是已知权限，未知的直接报错。
    this.#subject(this.access, kind, target);
    // 先确认主体存在（在真实目录上检查，避免为不存在的目标走一遍提交）。
    return {
      ...this.#commit(draft => {
      // 提交变更。
        const holder = this.#subject(draft, kind, target);
        // 在草稿上再取一次主体（要改的是草稿里的对象）。
        const existing = holder.grants.find(grant => grant.object === objectName);
        // 找该对象上已有的授权条目。
        if (existing) existing.permissions = [...new Set([...existing.permissions, ...list])];
        // 有就取并集——注意只影响这一个对象，其它对象的授权不受影响。
        else holder.grants.push({ object: objectName, permissions: list });
        // 没有就新增一条。
      }, { action: 'GRANT', subject: kind, name: target, object: objectName, permissions: list }),
      // 审计描述里带上主体、对象与权限清单。
      subject: kind, name: target, object: objectName, permissions: list,
    };
  }

  /** 逐项 REVOKE：只移除指定权限；集合清空后删除该对象条目。 */
  revoke(actor, { subject = 'user', name, object, permissions }) {
  // 逐项撤销权限。
    this.authorize(actor, [{ permission: 'grant' }]);
    const kind = subject === 'role' ? 'role' : 'user';
    // 主体类型。
    const target = requireName(kind, name);
    // 校验主体名。
    const objectName = requireObject(object);
    // 校验对象名。
    const list = requirePermissions(permissions);
    // 校验权限清单。
    this.#subject(this.access, kind, target);
    // 确认主体存在。
    return {
      ...this.#commit(draft => {
      // 提交变更。
        const holder = this.#subject(draft, kind, target);
        // 取草稿上的主体。
        holder.grants = holder.grants
          // 重建授权数组：
          .map(grant => grant.object !== objectName ? grant
            // 不是目标对象的条目原样保留，
            : { ...grant, permissions: grant.permissions.filter(value => !list.includes(value)) })
            // 是目标对象的就过滤掉要撤销的权限。
          .filter(grant => grant.permissions.length > 0);
          // 最后把权限被清空的条目整条删掉，不留空壳。
      }, { action: 'REVOKE', subject: kind, name: target, object: objectName, permissions: list }),
      // 审计描述。
      subject: kind, name: target, object: objectName, permissions: list,
    };
  }

  /** 整表替换（保留旧的 PUT /api/access 语义），同样走版本自增和管理员保底检查。 */
  // 即：即使是一次性整体替换，也受"不能把最后的管理员删掉"这条约束保护。
  replace(actor, raw) {
  // 整体替换权限目录。
    this.authorize(actor, [{ permission: 'grant' }]);
    return this.#commit(draft => {
    // 提交变更。
      const next = normalizeAccess(raw, this.access);
      // 先把新目录规范化；第二个参数传当前目录，这样没给新口令的用户会沿用旧哈希。
      draft.users = next.users;
      // 覆盖用户表。
      draft.roles = next.roles;
      // 覆盖角色表。
    }, { action: 'REPLACE ACCESS CATALOG' });
    // 审计描述。
  }
}

export { ALL_PERMISSIONS };
// 再导出权限白名单，方便调用方做前端提示与校验。

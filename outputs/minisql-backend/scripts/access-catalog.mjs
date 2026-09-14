import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
// 哈希、随机盐、常数时间比较，都来自 Node 内置加密库。
import { existsSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
// 判断存在、读文件、原子改名、写文件——同样是"先写临时文件再改名"的写法。

export const ALL_PERMISSIONS = new Set(['*', 'connect', 'read', 'select', 'insert', 'update', 'delete', 'create', 'drop', 'transaction', 'checkpoint', 'compile', 'grant', 'audit']);
// 全部合法权限名。任何不在这个集合里的权限都会被规范化时丢弃，
// 这样即使权限文件被手工改过，也不会凭空多出一种可被授予的权限。

function clone(value) {
// 深拷贝：用 JSON 往返实现，够用于本文件里这些纯数据对象。
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
  // undefined 直接返回（JSON 无法表示它），其余走序列化再反序列化。
}

function normalizeName(name) {
// 统一名字规范：转字符串、去首尾空白、转小写，保证比较大小写不敏感。
  return String(name ?? '').trim().toLowerCase();
  // null/undefined 会被当成空串，避免调用方传空值时抛错。
}

function normalizePermissions(raw) {
// 把一组权限名规范化：非法权限被丢掉，重复的自动去重。
  const permissions = new Set();
  // 用集合去重。
  for (const item of Array.isArray(raw) ? raw : []) {
  // 不是数组就当作空列表处理（容错，不让脏数据把整次加载打断）。
    const name = normalizeName(item);
    // 规范化这一项。
    if (ALL_PERMISSIONS.has(name)) permissions.add(name);
    // 只保留白名单里的权限。
  }
  return [...permissions];
  // 转成数组返回，顺序是首次出现的顺序。
}

function normalizeGrants(raw) {
// 规范化授权列表：每条授权保留"作用对象 + 权限清单"，并丢掉权限为空的条目。
  return (Array.isArray(raw) ? raw : []).map(grant => ({
  // 逐条映射。
    object: normalizeName(grant.object ?? '*'),
    // 作用对象：缺省是 '*'（对所有对象生效）。
    permissions: normalizePermissions(grant.permissions),
    // 权限清单走上面的规范化。
  })).filter(grant => grant.permissions.length > 0);
  // 过滤掉规范化后没有权限的条目：留着它们只会让权限文件显得很长却没有实际意义。
}

export const defaultAccess = () => ({
// 默认权限目录工厂函数（每次调用返回新对象，避免调用方改到共享状态）。
  version: 1,
  // 结构版本。
  users: {
  // 三个内置用户。
    admin: { hash: null, roles: ['administrators'], grants: [] },
    // admin：口令为空（hash 为 null），属于 administrators 角色，没有个人额外授权。
    reader: { hash: null, roles: ['readers'], grants: [] },
    // reader：只读用户。
    writer: { hash: null, roles: ['writers'], grants: [] },
    // writer：可写用户。
  },
  // 用户定义结束。
  roles: {
  // 角色定义。
    administrators: { inherits: [], grants: [{ object: '*', permissions: ['*'] }] },
    // 管理员：不继承别的角色，直接对所有对象拥有所有权限。
    readers: { inherits: [], grants: [{ object: '*', permissions: ['CONNECT', 'READ', 'SELECT', 'COMPILE'] }] },
    // 只读角色：连接、读目录、查询、只编译。
    writers: { inherits: ['readers'], grants: [{ object: '*', permissions: ['INSERT', 'UPDATE', 'DELETE', 'CREATE', 'DROP', 'TRANSACTION', 'CHECKPOINT'] }] },
    // 可写角色：继承只读角色，额外拥有增删改、建删对象、事务与检查点权限。
  },
  // 角色定义结束。
});

export function hashPassword(password) {
// 把口令哈希成可存储的记录（加盐 + SHA-256）。
  const salt = randomBytes(16).toString('hex');
  // 每个口令生成独立的 16 字节随机盐（十六进制表示）。
  const digest = createHash('sha256').update(salt + ':' + password).digest('hex');
  // 对"盐 + ':' + 口令"求摘要；加冒号是为了避免盐与口令拼接歧义。
  return { scheme: 'sha256-salted', salt, digest };
  // 连方案名一起返回，便于以后更换算法时按方案分支校验。
}

export function verifyUser(access, name, password) {
// 校验用户与口令是否匹配。
  const user = access?.users?.[normalizeName(name)];
  // 取出该用户（可选链保证目录结构缺失时不会抛异常）。
  if (!user) return false;
  // 用户不存在就是认证失败。
  if (!user.hash) return password == null || password === '';
  // 记录为 null 表示该用户不设口令，此时只接受空口令。
  if (!user.hash.salt || !user.hash.digest) return false;
  // 有 hash 记录但字段不全，说明记录损坏，保守判失败。
  const digest = Buffer.from(createHash('sha256').update(user.hash.salt + ':' + (password ?? '')).digest('hex'), 'hex');
  // 用同样的拼接方式重算摘要；口令缺失时按空串处理。
  const expected = Buffer.from(user.hash.digest, 'hex');
  // 取出期望摘要。
  return digest.length === expected.length && timingSafeEqual(digest, expected);
  // 先比长度（长度不同 timingSafeEqual 会抛错），再用常数时间比较，
  // 避免通过比较耗时反推口令内容。
}

export function roleNames(access, name, seen = new Set()) {
// 列出一个用户实际拥有的全部角色（含通过继承间接得到的）。
  const user = access?.users?.[normalizeName(name)];
  // 找用户。
  if (!user) return [];
  // 用户不存在就没有角色。
  const roles = [];
  // 结果列表。
  const visitRole = role => {
  // 递归访问一个角色及其父角色。
    const normalized = normalizeName(role);
    // 规范化角色名。
    if (seen.has(normalized)) return;
    // 已经访问过就返回，这样既能去重，也能在角色继承成环时自动停下。
    seen.add(normalized);
    // 标记已访问。
    roles.push(normalized);
    // 收进结果。
    for (const parent of access.roles?.[normalized]?.inherits ?? []) visitRole(parent);
    // 递归父角色；inherits 缺失时按空数组处理。
  };
  for (const role of user.roles ?? []) visitRole(role);
  // 从该用户直接拥有的角色出发逐个展开。
  return roles;
  // 返回角色列表。
}

export function effectiveGrants(access, name) {
// 汇总一个用户"实际生效"的全部授权：个人授权 + 所有角色（含继承）的授权。
  const normalized = normalizeName(name);
  // 规范化用户名。
  if (!access?.users?.[normalized]) return [];
  // 用户不存在，没有任何授权。
  const grants = [];
  // 结果列表。
  const seenRoles = new Set();
  // 记录已经处理过的角色，防止继承成环时无限递归。
  const visitRole = role => {
  // 收集一个角色带来的授权。
    if (seenRoles.has(role)) return;
    // 已经处理过就直接返回。
    seenRoles.add(role);
    // 标记已处理。
    const definition = access.roles?.[role];
    // 取出角色定义。
    if (!definition) return;
    // 角色不存在就跳过（引用了不存在的角色不算致命错误，只是不产生授权）。
    grants.push(...normalizeGrants(definition.grants));
    // 把该角色的授权规范化后并入结果。
    for (const parent of definition.inherits ?? []) visitRole(normalizeName(parent));
    // 递归父角色，实现权限继承。
  };
  grants.push(...normalizeGrants(access.users[normalized].grants));
  // 先加用户自身的授权。
  for (const role of roleNames(access, normalized)) visitRole(role);
  // 再把该用户所有角色（roleNames 已经展开过继承）的授权加进来。
  return grants;
  // 返回汇总后的授权列表。
}

export function can(access, name, permission, object = '*') {
// 判断某个用户对某个对象是否拥有指定权限。
  const normalizedUser = normalizeName(name);
  // 规范化用户名。
  if (!access?.users?.[normalizedUser]) return false;
  // 用户不存在，一律不通过。
  const permissionName = normalizeName(permission);
  // 规范化权限名。
  const objectName = object === undefined || object === null ? '*' : normalizeName(object);
  // 规范化对象名；没给对象就按通配 '*' 处理。
  return effectiveGrants(access, normalizedUser).some(grant => {
  // 只要有一条授权命中就算通过。
    if (grant.object !== '*' && grant.object !== objectName) return false;
    // 授权的对象要么是通配，要么正好是目标对象。
    return grant.permissions.includes('*') || grant.permissions.includes(permissionName);
    // 权限要么是通配，要么正好包含所需权限。
  });
}

export function canConnect(access, name, password) {
// 能否建立连接：既要通过认证，又要有 connect 权限。
  return verifyUser(access, name, password) && can(access, name, 'CONNECT');
  // 两个条件都必须成立；注意顺序是先认证后鉴权。
}

export function hasRole(access, name, role) {
// 判断用户是否拥有某个角色（含通过继承得到的）。
  return roleNames(access, name).includes(normalizeName(role));
  // 直接在展开后的角色列表里查找。
}

export function normalizeAccess(raw, previous) {
// 把任意来源的权限数据整理成规范结构；previous 是"上一次的目录"，用于沿用已存的密码哈希。
  if (!raw || typeof raw !== 'object') throw new Error('Access catalog must be an object');
  // 顶层必须是对象，否则直接拒绝。
  const users = {};
  // 规范化后的用户表。
  const usersRaw = raw.users && typeof raw.users === 'object' && !Array.isArray(raw.users) ? Object.entries(raw.users)
    // 形式一：对象映射（用户名 → 定义），转成 [名, 定义] 数组统一处理。
    : Array.isArray(raw.users) ? raw.users.map(user => [user?.name, user]) : [];
    // 形式二：数组形式（每项自带 name 字段）；其它情况按空列表处理。
  for (const [nameValue, value] of usersRaw) {
  // 逐个用户处理。
    const name = normalizeName(nameValue ?? value?.name);
    // 用户名可以来自键名，也可以来自项内的 name 字段。
    if (!name || name.length > 64) throw new Error('Invalid user name');
    // 用户名为空或超过 64 字符都拒绝，防止写出无法处理的目录。
    const previousUser = previous?.users?.[name];
    // 找上一次同名的用户记录。
    let hash = previousUser?.hash ?? null;
    // 默认沿用旧的密码哈希（这样只改授权时不会把口令弄丢）。
    if (value && typeof value.password === 'string' && value.password.length > 0) hash = hashPassword(value.password);
    // 如果这次给的是明文密码，就现场生成新的盐与哈希（覆盖旧值）。
    else if (value && value.hash && typeof value.hash === 'object') hash = value.hash;
    // 否则若给了现成的哈希记录，就直接采用（迁移场景）。
    users[name] = {
    // 写入规范化后的用户记录。
      hash,
      // 口令记录。
      roles: [...new Set((value?.roles ?? []).map(normalizeName).filter(Boolean))],
      // 角色列表：规范化 → 去掉空值 → 用集合去重，再转回数组。
      grants: normalizeGrants(value?.grants),
      // 个人授权走统一的规范化。
    };
    // 记录写入结束。
  }
  // 用户处理结束。
  const roles = {};
  // 规范化后的角色表。
  const rolesRaw = raw.roles && typeof raw.roles === 'object' && !Array.isArray(raw.roles) ? Object.entries(raw.roles)
    // 与用户表一样，兼容"对象映射"写法。
    : Array.isArray(raw.roles) ? raw.roles.map(role => [role?.name, role]) : [];
    // 以及"数组"写法。
  for (const [nameValue, value] of rolesRaw) {
  // 逐个角色处理。
    const name = normalizeName(nameValue ?? value?.name);
    // 角色名同样可以来自键名或项内字段。
    if (!name || name.length > 64) throw new Error('Invalid role name');
    // 名字非法就拒绝。
    roles[name] = {
    // 写入角色记录。
      inherits: [...new Set((value?.inherits ?? []).map(normalizeName).filter(Boolean))],
      // 继承的父角色列表，同样去重。
      grants: normalizeGrants(value?.grants),
      // 角色授权。
    };
    // 记录写入结束。
  }
  // 角色处理结束。
  for (const [name, role] of Object.entries(roles)) {
  // 逐个角色做一致性检查。
    const seen = new Set();
    // 本轮遍历过的角色。
    const stack = [name];
    // 用显式栈做深度优先遍历（比递归更安全，不会因为继承链过长爆栈）。
    while (stack.length) {
    // 栈非空就继续。
      const current = stack.pop();
      // 取一个角色。
      if (seen.has(current)) throw new Error(`Role inheritance cycle at ${current}`);
      // 同一轮里又遇到同一个角色，说明继承关系成环，直接报错。
      seen.add(current);
      // 标记已访问。
      const definition = roles[current];
      // 取角色定义。
      for (const parent of definition.inherits) {
      // 遍历它继承的父角色。
        if (!roles[parent]) throw new Error(`Missing inherited role ${parent}`);
        // 父角色不存在就报错：这种悬空引用会让权限继承静默失效，必须显式暴露。
        stack.push(parent);
        // 父角色入栈继续检查。
      }
      // 父角色遍历结束。
    }
    // 检查结束。
  }
  // 全部角色检查结束。
  return { version: 1, users, roles };
  // 返回规范结构（版本号固定为 1）。
}

export function loadAccess(path) {
// 读取权限目录文件；文件不存在时用默认目录初始化一份并落盘。
  if (!existsSync(path)) {
  // 首次运行的情况。
    const value = defaultAccess();
    // 造默认目录。
    saveAccess(path, value);
    // 立刻写盘，保证下次启动能读到同一份。
    return value;
    // 返回默认目录。
  }
  // 存在性判断结束。
  const parsed = JSON.parse(readFileSync(path, 'utf8'));
  // 按 UTF-8 读并解析。
  return normalizeAccess(parsed, parsed);
  // 规范化；第二个参数也传 parsed，表示"没有更早的版本可参照"。
}

export function saveAccess(path, access) {
// 保存权限目录（同样是"先写临时文件再改名"的原子写法）。
  const serialized = JSON.stringify(access, null, 2);
  // 缩进 2 空格输出，方便人工查看与用 git 看差异。
  const temporary = path + '.tmp';
  // 临时文件路径。
  writeFileSync(temporary, serialized, 'utf8');
  // 先写临时文件。
  renameSync(temporary, path);
  // 再原子改名替换正式文件，避免读端看到写了一半的 JSON。
}

// ---------------------------------------------------------------------------
// X24 原子资源接口：GRANT / REVOKE / CREATE USER / CREATE ROLE / DROP / 改密 / 身份绑定。
// 全部为纯函数：先克隆并规范化，再校验，最后返回新对象；任何校验失败都抛错且不产生
// 部分变更，由调用方决定是否落盘（access-store.writeStore 写入页式目录并递增权限版本）。
// subject 形如 { type: 'user' | 'role', name }。
// ---------------------------------------------------------------------------

function cloneAccess(access) { return normalizeAccess(access, access); }
// 克隆一份权限目录：借规范化流程做过一次深拷贝，调用方改副本不会影响原对象。

function requireUser(access, name) {
// 要求用户存在，返回规范化后的用户名（不存在就抛错）。
  const normalized = normalizeName(name ?? '');
  // 规范化输入。
  if (!access.users?.[normalized]) throw new Error(`unknown user ${name}`);
  // 查不到就抛"未知用户"，避免后续静默地对不存在的用户操作。
  return normalized;
  // 返回规范名。
}

function requireRole(access, name) {
// 要求角色存在，返回规范化后的角色名。
  const normalized = normalizeName(name ?? '');
  // 规范化输入。
  if (!access.roles?.[normalized]) throw new Error(`unknown role ${name}`);
  // 查不到就抛错。
  return normalized;
  // 返回规范名。
}

function assertNoRoleCycle(roles, name) {
// 检查从 name 出发的角色继承关系里有没有环、有没有悬空父角色。
  const seen = new Set();
  // 本轮访问过的角色。
  const stack = [name];
  // 显式栈，避免继承链过长时递归爆栈。
  while (stack.length) {
  // 栈非空就继续。
    const current = stack.pop();
    // 取一个角色。
    if (seen.has(current)) throw new Error(`role inheritance cycle at ${current}`);
    // 重复遇到说明成环，直接抛错。
    seen.add(current);
    // 标记已访问。
    for (const parent of roles[current]?.inherits ?? []) {
    // 遍历父角色。
      if (!roles[parent]) throw new Error(`missing inherited role ${parent}`);
      // 父角色不存在也抛错（悬空引用会让权限继承静默失效）。
      stack.push(parent);
      // 入栈继续。
    }
    // 父角色遍历结束。
  }
  // 检查结束。
}

export function createUser(access, { name, password, roles = [], grants = [] } = {}) {
// 新建用户：返回一份新的权限目录，原对象不变。
  const next = cloneAccess(access);
  // 先克隆，后续所有改动都作用在副本上。
  const normalized = normalizeName(name ?? '');
  // 规范化用户名。
  if (!normalized || normalized.length > 64) throw new Error('invalid user name');
  // 用户名为空或过长都拒绝。
  if (next.users[normalized]) throw new Error(`user ${normalized} already exists`);
  // 已存在同名用户就拒绝（不覆盖，避免误删已有授权）。
  const roleList = [...new Set(roles.map(normalizeName).filter(Boolean))];
  // 角色列表去重并丢掉空值。
  for (const role of roleList) if (!next.roles[role]) throw new Error(`unknown role ${role}`);
  // 引用了不存在的角色就报错：静默忽略会让用户以为权限生效了其实没有。
  next.users[normalized] = {
  // 写入新用户。
    hash: typeof password === 'string' && password.length ? hashPassword(password) : null,
    // 给了非空口令就生成哈希；否则记 null（表示不设口令）。
    roles: roleList,
    // 角色列表。
    grants: normalizeGrants(grants),
    // 个人授权。
  };
  // 写入结束。
  return next;
  // 返回新目录。
}

export function dropUser(access, name) {
// 删除用户。
  const next = cloneAccess(access);
  const normalized = requireUser(next, name);
  // 要求用户存在（顺带拿到规范名）。
  delete next.users[normalized];
  // 从用户表里删掉。
  return next;
  // 返回新目录。
}

export function createRole(access, { name, inherits = [], grants = [] } = {}) {
// 新建角色。
  const next = cloneAccess(access);
  const normalized = normalizeName(name ?? '');
  // 规范化角色名。
  if (!normalized || normalized.length > 64) throw new Error('invalid role name');
  // 名字非法就拒绝。
  if (next.roles[normalized]) throw new Error(`role ${normalized} already exists`);
  // 同名角色已存在就拒绝。
  const parents = [...new Set(inherits.map(normalizeName).filter(Boolean))];
  // 父角色列表去重。
  for (const parent of parents) if (!next.roles[parent]) throw new Error(`unknown inherited role ${parent}`);
  // 父角色必须真实存在。
  next.roles[normalized] = { inherits: parents, grants: normalizeGrants(grants) };
  // 写入角色定义。
  assertNoRoleCycle(next.roles, normalized);
  // 这一步很关键：新加的继承关系可能让原有的继承链成环（例如 A 继承 B，而 B 又继承 A），
  // 所以写完之后要重新做一次环检查。
  return next;
  // 返回新目录。
}

export function dropRole(access, name) {
// 删除角色，同时把它从所有引用处摘掉。
  const next = cloneAccess(access);
  const normalized = requireRole(next, name);
  // 要求角色存在。
  for (const user of Object.values(next.users)) user.roles = user.roles.filter(role => role !== normalized);
  // 把该角色从每个用户的角色列表里移除。
  for (const role of Object.values(next.roles)) role.inherits = role.inherits.filter(parent => parent !== normalized);
  // 也从其它角色的继承列表里移除，避免留下悬空引用。
  delete next.roles[normalized];
  // 最后删掉角色本身。
  for (const role of Object.keys(next.roles)) assertNoRoleCycle(next.roles, role);
  // 再对剩余的每个角色做一次环检查（删除本身不会造环，但这样能一次性发现目录里原有的问题）。
  return next;
  // 返回新目录。
}

export function setPassword(access, name, password) {
// 改密：重新生成盐与哈希。
  const next = cloneAccess(access);
  const normalized = requireUser(next, name);
  // 要求用户存在。
  next.users[normalized] = { ...next.users[normalized], hash: hashPassword(String(password ?? '')) };
  // 保留其它字段，只替换 hash；口令缺失时按空串处理（等于设置成"空口令"）。
  return next;
  // 返回新目录。
}

export function addRole(access, name, role) {
// 给用户加角色。
  const next = cloneAccess(access);
  const normalized = requireUser(next, name);
  // 用户必须存在。
  const roleName = requireRole(next, role);
  // 角色必须存在。
  if (!next.users[normalized].roles.includes(roleName)) next.users[normalized].roles = [...next.users[normalized].roles, roleName];
  // 已经有了就不重复添加（幂等）。
  return next;
  // 返回新目录。
}

export function removeRole(access, name, role) {
// 取消用户的某个角色。
  const next = cloneAccess(access);
  const normalized = requireUser(next, name);
  // 用户必须存在。
  const roleName = requireRole(next, role);
  // 角色必须存在。
  next.users[normalized].roles = next.users[normalized].roles.filter(item => item !== roleName);
  // 过滤掉该角色；本来就没有也照样返回新目录（幂等）。
  return next;
  // 返回新目录。
}

function grantsRef(access, subject) {
// 拿到某个主体（用户或角色）的授权数组引用，供 grant/revoke 直接改写。
  const type = normalizeName(subject?.type ?? 'user');
  // 主体类型，默认是 user。
  const name = normalizeName(subject?.name ?? '');
  // 主体名。
  if (!name) throw new Error('subject.name is required');
  // 名字必填。
  if (type === 'role') { requireRole(access, name); return access.roles[name].grants; }
  // 角色：确认存在后返回它的授权数组。
  if (type === 'user') { requireUser(access, name); return access.users[name].grants; }
  // 用户：同理。
  throw new Error(`invalid subject type ${type}`);
  // 其它类型一律拒绝，避免写到一个不存在的容器上。
}

// 授予：把权限并集到主体（用户或角色）的指定对象授权上。
// 这是"并集"语义：重复授予同一权限不会出错，也不会产生重复项。
export function grant(access, { subject, object = '*', permissions = [] }) {
// 给某个主体授予权限。
  const next = cloneAccess(access);
  const normalizedObject = normalizeName(object);
  // 规范化作用对象。
  const permissionList = [...new Set(normalizePermissions(permissions))];
  // 权限清单规范化并去重。
  if (!permissionList.length) throw new Error('permissions must not be empty');
  // 空权限列表没有意义，直接拒绝而不是悄悄什么都不做。
  const list = grantsRef(next, subject);
  // 拿到主体的授权数组。
  const existing = list.find(item => item.object === normalizedObject);
  // 找该对象上已有的授权条目。
  if (existing) existing.permissions = [...new Set([...existing.permissions, ...permissionList])];
  // 已存在就取并集（用集合去重后写回）。
  else list.push({ object: normalizedObject, permissions: permissionList });
  // 不存在就新增一条。
  return next;
  // 返回新目录。
}

// 撤销：从主体指定对象授权中移除权限；permissions 为空表示撤销该对象上的全部授权。
// 注意"列表为空"和"列表里都是非法权限名"是两种不同意图，这里按"撤销全部"处理后者之外的情况：
// 前者清空该对象的授权，后者（规范化后为空）也会走到清空分支。
export function revoke(access, { subject, object = '*', permissions = [] }) {
// 从某个主体撤销权限。
  const next = cloneAccess(access);
  const normalizedObject = normalizeName(object);
  // 规范化作用对象。
  const permissionList = normalizePermissions(permissions);
  // 规范化要撤销的权限清单。
  const list = grantsRef(next, subject);
  // 拿到主体的授权数组。
  const existing = list.find(item => item.object === normalizedObject);
  // 找该对象的授权条目。
  if (!existing) return next;
  // 本来就没有该对象的授权，直接返回新目录（幂等）。
  if (!permissionList.length) {
  // 没指定要撤销哪些权限，理解为"撤销该对象上的全部授权"。
    list.splice(list.indexOf(existing), 1);
    // 整条删掉。
    return next;
    // 返回。
  }
  // 指定了具体权限的情况。
  const remaining = existing.permissions.filter(permission => !permissionList.includes(permission));
  // 保留不在撤销清单里的权限。
  if (remaining.length) existing.permissions = remaining;
  // 还有剩余权限就写回。
  else list.splice(list.indexOf(existing), 1);
  // 否则权限被清空，整条授权也一并删掉（不留空壳条目）。
  return next;
  // 返回新目录。
}

export function publicAccess(access) {
// 生成"可以对外展示"的权限视图。
  return {
  // 返回一个新对象。
    version: access.version,
    // 结构版本。
    users: Object.entries(access.users).map(([name, user]) => ({
    // 用户列表转成数组形式。
      name,
      // 用户名。
      roles: [...user.roles],
      // 角色列表（复制一份，调用方改它不会影响原目录）。
      grants: clone(user.grants) ?? [],
      // 授权同样深拷贝。
      passwordProtected: Boolean(user.hash),
      // 关键点：只暴露"是否设了口令"这个布尔值，绝不把 salt 与 digest 交给前端。
    })),
    // 用户映射结束。
    roles: Object.entries(access.roles).map(([name, role]) => ({
    // 角色列表。
      name,
      // 角色名。
      inherits: [...role.inherits],
      // 继承的父角色。
      grants: clone(role.grants) ?? [],
      // 授权深拷贝。
    })),
    // 角色映射结束。
  };
}

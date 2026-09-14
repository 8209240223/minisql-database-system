import http from 'node:http';
// HTTP 服务端。
import { spawn } from 'node:child_process';
// 启动数据库进程（这里只用于早期探活，正式会话走 session-process.mjs）。
import { appendFileSync, closeSync, copyFileSync, existsSync, ftruncateSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, renameSync, statSync, unlinkSync, writeFileSync, writeSync } from 'node:fs';
// 文件系统操作：审计日志追加、快照复制、目录遍历、原子改名等。
import { dirname, resolve } from 'node:path';
// 取目录名与把相对路径解析成绝对路径。
import { fileURLToPath } from 'node:url';
// 把 import.meta.url 转成文件系统路径，用于定位可执行文件与数据文件。
import { createHash, randomUUID } from 'node:crypto';
// 会话标识用随机 UUID；取消令牌用哈希。
import { openSession } from './session-process.mjs';
// 会话客户端：负责与数据库进程通信。
import { can, canConnect, defaultAccess, normalizeAccess, publicAccess,
  // 权限判定与权限目录的读取。
  createUser, dropUser, createRole, dropRole, setPassword, addRole, removeRole, grant, revoke } from './access-catalog.mjs';
  // 以及权限目录的全部变更操作（都是纯函数）。
import { openStore, readHeader, writeStore } from './access-store.mjs';
// 页式权限存储的入口。

function cliOption(name) {
// 从进程参数里取形如 --port 8081 的选项值。
  const index = process.argv.indexOf(name);
  // 找到选项名的位置。
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : undefined;
  // 存在且后面还有值才返回；否则返回 undefined 让调用方走默认值。
}
const bridgePort = Number(cliOption('--port') ?? process.env.PORT ?? 8081);
// 服务端口：命令行 > 环境变量 > 默认 8081。
if (!Number.isInteger(bridgePort) || bridgePort < 0 || bridgePort > 65535) throw new Error('Invalid bridge port');
// 端口必须是 0..65535 的整数，否则直接拒绝启动。
const releaseExecutable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
// 优先使用的数据库可执行文件：CMake 构建产物。
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(releaseExecutable) ? releaseExecutable : fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url)));
// 选择顺序：环境变量指定 > Release 构建产物 > 手写的 bin 目录产物。
const database = resolve(cliOption('--database') ?? process.env.MINISQL_DB ?? fileURLToPath(new URL('../data/workbench.pages', import.meta.url)));
// 数据库文件路径：命令行 > 环境变量 > 仓库内默认位置。
mkdirSync(dirname(database), { recursive: true });
// 确保数据目录存在（recursive 相当于 mkdir -p）。
const accessPath = resolve(cliOption('--access-file') ?? process.env.MINISQL_ACCESS_FILE ?? resolve(dirname(database), 'access.catalog.json'));
// 权限目录的"旧 JSON 路径"；实际使用的页式文件由 openStore 从这个路径推导。
const openedAccess = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
// 打开（必要时迁移）权限存储。
const accessPagesFile = openedAccess.pagesFile;
// 实际使用的页式文件路径。
let access = openedAccess.catalog;
// 内存里的权限目录。
let permissionVersion = openedAccess.permissionVersion;
// 权限版本号。
let accessCatalogVersion = openedAccess.catalogVersion;
// 结构版本号。
function reloadAccessIfStale() {
// 检查磁盘上的权限版本是否被别的进程改过（例如同时开着 CLI），变了就重载。
  try {
  // 读取可能失败（文件被删或损坏），这里不抛出去。
    const header = readHeader(accessPagesFile);
    // 只读元数据页。
    if (header.permissionVersion === permissionVersion) return;
    // 版本一致，无需重载。
    const reopened = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    // 完整重开一次（含全部校验）。
    access = reopened.catalog;
    // 换上新目录。
    permissionVersion = reopened.permissionVersion;
    // 换上新版本号。
    accessCatalogVersion = reopened.catalogVersion;
    // 同步结构版本。
  } catch { /* Corrupt storage is rejected by the operation that needs it. */ }
  // 注释说明：这里故意吞掉异常，因为"损坏"应该由真正需要使用权限的那个操作去报错，
  // 而不是在每次轮询检查时就把服务打断。
}
function persistAccess() {
// 把当前权限目录写盘，并把版本号加一。
  const nextVersion = permissionVersion + 1;
  // 新版本号。
  if (nextVersion > 0xffffffff) throw new Error('Permission version space exhausted');
  // 32 位版本号用尽就报错（回绕会让旧版本号重新有效）。
  writeStore(accessPagesFile, access, { permissionVersion: nextVersion, catalogVersion: accessCatalogVersion });
  // 原子写盘。
  permissionVersion = nextVersion;
  // 内存版本号同步。
}
if (process.env.MINISQL_ADMIN_PASSWORD && !access.users.admin?.hash) {
// 启动时如果给了初始管理员口令，且 admin 目前还没有口令，就设置它。
  access = normalizeAccess({ ...access, users: { ...access.users, admin: { ...access.users.admin, password: process.env.MINISQL_ADMIN_PASSWORD } } }, access);
  // 用 password 字段触发重新生成盐与哈希；第二个参数传旧目录，其它用户的口令保持不变。
  persistAccess();
  // 立刻落盘，保证之后启动的进程也看得到。
}
// 初始管理员口令处理结束。
const allowedOrigins = new Set((process.env.MINISQL_ORIGINS ?? 'http://127.0.0.1:4173,http://localhost:4173').split(','));
// CORS 白名单；默认只允许本地工作台的默认端口。
let queue = Promise.resolve();
// 串行化所有需要独占数据库的操作（保证同一时刻只有一个在跑）。
let queued = 0;
// 排队中的操作数，用于限流。
let quarantined = false;
// 隔离标志：一旦发生"提交状态未知"的严重故障，就停止接受新操作，避免在不确定状态上继续写。
const sessions = new Map();
// 会话编号 → 会话对象的映射。
let engine;
// 当前打开的数据库会话（惰性创建）。
let engineOpening;
// 正在进行的打开操作（避免并发重复打开）。
let transactionOwner;
// 当前持有事务的会话编号。
let turnOwner;
// 当前持有"执行权"的会话编号（保证多步操作不被插队）。
let activeOperation;
// 正在执行的操作描述，用于诊断接口。
const transactionWaiters = [];
// 等待事务释放的排队者。
const turnWaiters = [];
// 等待执行权释放的排队者。
// 授权判定完全交给 C++ 绑定结果：bridge 不再读取或扫描 SQL 文本。
// 会话内取绑定供只读判定与审计用（不用于授权决策）。
async function bindAccessForSql(sql, sessionRoute, user, password) {
  if (sessionRoute) {
    const session = sessions.get(sessionRoute[1]);
    if (!session) throw httpError(404, 'Session not found or expired');
    if (session.user !== user) throw httpError(403, 'Permission denied');
    return enqueue(async () => {
      const currentEngine = await ensureEngine();
      return currentEngine.worker.request('bindAccess', sql, { sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password ?? '' });
    });
  }
  return callDatabase('bindAccess', sql, {}, { user, password });
}
const READ_ONLY_ACTIONS = new Set(['select', 'read', 'compile']);
// 流式入口只读兜底：同样只看绑定结果，不看 SQL 文本。
function readOnlyBinding(binding) {
  if (!binding || binding.success === false) return { allowed: false, message: binding?.error?.message ?? 'Permission denied' };
  const allowed = binding.bound === true && READ_ONLY_ACTIONS.has(String(binding.statementAction ?? '').toLowerCase()) &&
    (binding.objects ?? []).every(object => READ_ONLY_ACTIONS.has(String(object.action ?? '').toLowerCase()));
  if (!allowed) return { allowed: false, message: 'Streaming endpoint accepts read-only SELECT or EXPLAIN SQL' };
  return { allowed: true, objects: [...new Set((binding.objects ?? []).map(object => String(object.object).toLowerCase()))] };
}
const backupDirectory = resolve(process.env.MINISQL_BACKUP_DIR ?? resolve(dirname(database), 'backups'));
// 备份目录：环境变量可覆盖，默认放在数据库文件旁边的 backups 子目录。
mkdirSync(backupDirectory, { recursive: true });
// 确保目录存在。
const cancelDirectory = resolve(dirname(database), 'cancellation');
// 取消令牌目录：数据库进程通过"某个文件是否存在"来感知取消请求。
mkdirSync(cancelDirectory, { recursive: true });
// 确保目录存在。
function sessionCancelFile(id) { return resolve(cancelDirectory, `${id}.cancel`); }
// 由会话编号推出它的取消令牌文件路径。
// 序列化计划的受权对象来自已解析的计划节点本身，不做任何 SQL 文本扫描。
function collectPlanTables(document) {
  const rows = Array.isArray(document) ? document
    : document && typeof document === 'object' && Array.isArray(document.plans) ? document.plans : [];
  const tables = new Set();
  for (const row of rows) {
    if (row && typeof row === 'object' && typeof row.table === 'string' && row.table) tables.add(row.table.toLowerCase());
  }
  return [...tables];
}
function clearCancelFile(file) { if (file) { try { unlinkSync(file); } catch { /* The token may already be absent. */ } } }
// 删除取消令牌文件；文件本来就不存在不算错误（注释里写明了这一点）。
function backupName(raw) {
// 规范化备份文件名（用于旧式整文件备份）。
  const name = String(raw ?? '').replace(/[^a-zA-Z0-9._-]/g, '');
  // 只保留字母数字与 . _ -，其余字符一律剔除：这是防目录穿越的第一道闸。
  if (!name || name === '.' || name === '..') throw httpError(400, 'Invalid backup name');
  // 清洗后为空或者是相对路径里的特殊名字，直接拒绝。
  return name.endsWith('.pages') ? name : name + '.pages';
  // 统一补上 .pages 后缀。
}
function backupFile(raw) {
// 把备份名解析成"目录内的绝对路径"，并确认没有逃出备份目录。
  const name = backupName(raw);
  // 规范化名字。
  const file = resolve(backupDirectory, name);
  // 拼出绝对路径。
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  // 最终父目录必须正好是备份目录；这是防目录穿越的第二道闸（纵深防御）。
  return { name, file };
  // 返回名字与路径。
}
function sha256(file) { return createHash('sha256').update(readFileSync(file)).digest('hex'); }
// 算一个文件的 SHA-256 十六进制摘要，用于备份校验。
function pageFormatVersion(file) {
// 读页文件头，返回它的格式版本。
  const descriptor = openSync(file, 'r');
  // 只读打开，拿文件描述符（比 readFileSync 更省内存，只读前 12 字节）。
  try {
  // 用 try/finally 保证描述符一定被关闭。
    const header = Buffer.alloc(12);
    // 头部长 12 字节。
    if (readSync(descriptor, header, 0, header.length, 0) !== header.length || header.readUInt32LE(0) !== 0x4644534d) throw httpError(422, 'Invalid page file header');
    // 必须完整读到 12 字节，且魔数正确（0x4644534d 小端读出即 "MSDF"）。
    const version = header.readUInt32LE(8);
    // 读版本号。
    if (version !== 1 && version !== 2) throw httpError(422, `Unsupported page format version ${version}`);
    // 只认版本 1 和 2；其它版本明确拒绝，避免按错误布局解读。
    return version;
    // 返回版本。
  } finally { closeSync(descriptor); }
  // 无论成功失败都关闭描述符。
}
const backupPageSize = 4096;
// 备份文件的页大小常量。
function sanitizeBackupName(raw) {
// 新版备份（整份或增量）的名字清洗：去掉后缀，只保留安全字符。
  let value = String(raw ?? '').replace(/[^a-zA-Z0-9._-]/g, '');
  // 先剔除不安全字符。
  if (value.endsWith('.pages')) value = value.slice(0, -'.pages'.length);
  // 去掉 .pages 后缀。
  if (value.endsWith('.delta')) value = value.slice(0, -'.delta'.length);
  // 去掉 .delta 后缀。
  if (!value || value === '.' || value === '..') throw httpError(400, 'Invalid backup name');
  // 清洗后为空或特殊名字就拒绝。
  return value;
  // 返回纯名字（不带后缀）。
}
function backupArtifactFile(raw) {
// 按名字找出可用的备份工件：优先增量，其次整份。
  const name = sanitizeBackupName(raw);
  // 清洗名字。
  const full = resolve(backupDirectory, name + '.pages');
  // 整份备份的文件路径。
  const delta = resolve(backupDirectory, name + '.delta');
  // 增量备份的文件路径。
  if (existsSync(delta) && existsSync(delta + '.json')) return { kind: 'incremental', name: name + '.delta', file: delta, manifest: delta + '.json' };
  // 增量文件与其清单都在，就返回增量（优先级更高，因为它是更新的状态）。
  if (existsSync(full) && existsSync(full + '.json')) return { kind: 'full', name: name + '.pages', file: full, manifest: full + '.json' };
  // 否则看整份备份是否齐全。
  return undefined;
  // 都没有就返回 undefined，交给调用方报 404。
}
function fullBackupFile(raw) {
// 要求整份备份必须存在（同时校验它没逃出备份目录）。
  const name = sanitizeBackupName(raw);
  // 清洗名字。
  const file = resolve(backupDirectory, name + '.pages');
  // 文件路径。
  const manifest = file + '.json';
  // 清单路径（与备份文件同名，加 .json）。
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  // 防目录穿越。
  if (!existsSync(file) || !existsSync(manifest)) throw httpError(404, 'Full backup not found');
  // 文件或清单缺失就 404。
  return { name: name + '.pages', file, manifest };
  // 返回三项信息。
}
function deltaBackupFile(raw) {
// 给出增量备份的目标路径（用于写入，不要求文件已存在）。
  const name = sanitizeBackupName(raw);
  // 清洗名字。
  const file = resolve(backupDirectory, name + '.delta');
  // 增量文件路径。
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  // 防目录穿越。
  return { name: name + '.delta', file, manifest: file + '.json' };
  // 返回名字、文件与清单路径。
}
function normalizeFullManifest(metadata, file, manifest) {
// 校验整份备份的清单并与实际文件对账，必要时把老版本清单就地升级。
  if (metadata.sha256 !== sha256(file)) throw httpError(422, 'Backup checksum mismatch');
  // 清单里记录的整体摘要必须与实际文件一致。
  if (metadata.pageChecksum !== undefined && metadata.pageChecksum !== sha256(file)) throw httpError(422, 'Backup page checksum mismatch');
  // 如果清单里还有另一份页校验值，也要一致（兼容老格式里两个字段同时存在的情况）。
  const detectedPageVersion = pageFormatVersion(file);
  // 从文件头读出真实的页格式版本。
  if (metadata.version === 1) {
  // 清单版本 1：老格式，就地升级成版本 2。
    const migrated = { ...metadata, version: 2, pageFormatVersion: detectedPageVersion, walBytes: 0 };
    // 补上页格式版本，并把 WAL 字节数记为 0（老备份没有 WAL 概念）。
    writeFileSync(manifest, JSON.stringify(migrated), 'utf8');
    // 把升级后的清单写回磁盘。
    return migrated;
    // 返回升级后的清单。
  }
  // 版本 1 处理结束。
  if (metadata.version !== 2 && metadata.version !== 4)
  // 只认 2 和 4 两个版本。
    throw httpError(422, 'Backup manifest version or page format mismatch');
    // 其它版本拒绝。
  if (metadata.pageFormatVersion !== detectedPageVersion) throw httpError(422, 'Backup page format mismatch');
  // 清单里记的页格式必须与文件头一致，否则说明两者不是同一次写出来的。
  if (metadata.version === 2 && metadata.walBytes !== 0) throw httpError(422, 'Backup WAL prefix is not supported');
  // 版本 2 不支持带 WAL 前缀，出现非零值就拒绝（避免恢复出不一致的状态）。
  if (metadata.version === 4 && (metadata.walCutoffBytes === undefined || metadata.committedSequence === undefined))
  // 版本 4 是快照格式，必须带 WAL 截止位置与已提交序号。
    throw httpError(422, 'Snapshot manifest is incomplete');
    // 缺字段说明清单不完整，拒绝恢复。
  return metadata;
  // 校验通过，原样返回。
}
function deltaHeader(basePages, finalPages, records) {
// 构造增量备份的 64 字节文件头。
  const header = Buffer.alloc(64);
  // 固定 64 字节。
  header.write('MISQLDLT', 0, 'ascii');
  // 偏移 0：魔数（ASCII）。
  header.writeUInt32LE(1, 8);
  // 偏移 8：增量格式版本。
  header.writeUInt32LE(backupPageSize, 12);
  // 偏移 12：页大小，用于读取时确认双方约定一致。
  header.writeBigUInt64LE(BigInt(basePages), 16);
  // 偏移 16：基线（被增量依赖的那份备份）的页数。
  header.writeBigUInt64LE(BigInt(finalPages), 24);
  // 偏移 24：应用增量后应有的页数。
  header.writeBigUInt64LE(BigInt(records), 32);
  // 偏移 32：本文件里包含多少条"页号 + 整页内容"记录。
  return header;
  // 返回头部。
}
function parseDeltaHeader(buffer) {
// 解析并校验增量备份的文件头。
  if (buffer.length < 64 || buffer.toString('ascii', 0, 8) !== 'MISQLDLT') throw httpError(422, 'Invalid incremental backup header');
  // 长度不足或魔数不符都拒绝。
  if (buffer.readUInt32LE(8) !== 1 || buffer.readUInt32LE(12) !== backupPageSize) throw httpError(422, 'Unsupported incremental backup version');
  // 版本必须为 1，页大小必须与本进程一致（否则页边界都对不上）。
  return {
  // 返回解析出的三个字段。
    basePages: Number(buffer.readBigUInt64LE(16)),
    // 基线页数。
    finalPages: Number(buffer.readBigUInt64LE(24)),
    // 最终页数。
    records: Number(buffer.readBigUInt64LE(32)),
    // 记录条数。
  };
}
function changedPageIds(base, current) {
// 比较两份页文件，列出所有发生变化的页号（含新增的尾部页）。
  const basePages = Math.floor(base.length / backupPageSize);
  // 基线页数。
  const currentPages = Math.floor(current.length / backupPageSize);
  // 当前页数。
  const ids = [];
  // 变化页号列表。
  for (let id = 0; id < Math.min(basePages, currentPages); ++id) {
  // 只比较两者都存在的那些页。
    const left = base.subarray(id * backupPageSize, (id + 1) * backupPageSize);
    // 基线上的这一页。
    const right = current.subarray(id * backupPageSize, (id + 1) * backupPageSize);
    // 当前的这一页。
    if (!left.equals(right)) ids.push(id);
    // 不同就记下页号。逐页整页比较，简单且不会漏。
  }
  // 共同部分比较结束。
  for (let id = basePages; id < currentPages; ++id) ids.push(id);
  // 新增长出来的页全部算变化。
  return ids;
  // 返回变化页号列表。
}
async function reconstructBackup(name, depth = 0) {
// 把一份备份（整份或增量链）还原成完整的内存缓冲；depth 用于限制链长。
  const artifact = backupArtifactFile(name);
  // 找到可用的备份工件。
  if (!artifact) throw httpError(404, 'Backup not found');
  // 找不到就 404。
  if (artifact.kind === 'full') {
  // 整份备份：直接读出来，并把它依赖的 WAL / ckpt 附加文件一起带上。
    const metadata = normalizeFullManifest(JSON.parse(readFileSync(artifact.manifest, 'utf8')), artifact.file, artifact.manifest);
    // 校验清单（顺带把老版本清单升级）。
    const walBuffer = metadata.version === 4 && existsSync(artifact.file + '.wal') ? readFileSync(artifact.file + '.wal') : Buffer.alloc(0);
    // 快照格式才可能带 WAL 段；没有就给空缓冲。
    const ckptBuffer = metadata.version === 4 && existsSync(artifact.file + '.ckpt') ? readFileSync(artifact.file + '.ckpt') : Buffer.alloc(0);
    // 同理带上 ckpt 附加文件。
    return { buffer: readFileSync(artifact.file), pages: Math.floor(statSync(artifact.file).size / backupPageSize),
      // 返回页文件内容与页数，
      manifest: metadata, walBuffer, ckptBuffer };
      // 以及清单与两个附加缓冲。
  }
  // 整份备份分支结束。
  if (depth > 8) throw httpError(422, 'Backup chain too deep');
  // 增量链最长 8 层：再深就不像是正常使用，更可能是构造出来的攻击或错误状态。
  const delta = readFileSync(artifact.file);
  // 读出增量文件。
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  // 读出它的清单。
  if (metadata.sha256 !== sha256(artifact.file) || metadata.version !== 3 || metadata.kind !== 'incremental')
  // 三项校验：文件摘要、清单版本 3、类型为 incremental。
    throw httpError(422, 'Incremental backup checksum or version mismatch');
    // 任一不符都拒绝。
  const header = parseDeltaHeader(delta);
  // 解析增量头。
  if (metadata.pageFormatVersion === undefined || metadata.base === undefined) throw httpError(422, 'Incremental manifest incomplete');
  // 清单必须说明基线与页格式版本，否则无法确定该以谁为底。
  const base = await reconstructBackup(metadata.base, depth + 1);
  // 递归还原基线（基线本身可能又是一份增量）。
  const buffer = Buffer.alloc(header.finalPages * backupPageSize);
  // 按最终页数分配结果缓冲。
  const copied = Math.min(base.pages, header.finalPages);
  // 先拷贝基线里能对应上的部分。
  base.buffer.copy(buffer, 0, 0, copied * backupPageSize);
  // 逐页整体拷贝。
  let offset = 64;
  // 记录区从头部之后开始。
  for (let index = 0; index < header.records; ++index) {
  // 逐条读取"页号 + 整页"记录。
    if (offset + 8 > delta.length) throw httpError(422, 'Incremental backup truncated');
    // 连页号都读不全说明文件被截断。
    const id = Number(delta.readBigUInt64LE(offset));
    // 读出目标页号。
    offset += 8;
    // 游标跳过页号。
    if (id >= header.finalPages || offset + backupPageSize > delta.length) throw httpError(422, 'Incremental backup page out of range');
    // 页号必须在范围内，且后面必须还有一整页数据。
    delta.copy(buffer, id * backupPageSize, offset, offset + backupPageSize);
    // 用增量里的内容覆盖对应页。
    offset += backupPageSize;
    // 游标跳过这一页。
  }
  // 记录处理结束。
  if (offset !== delta.length) throw httpError(422, 'Incremental backup trailing data');
  // 处理完必须正好用光文件；有剩余说明文件里多塞了内容，拒绝而不是忽略。
  if (header.basePages !== base.pages) throw httpError(422, 'Incremental base page count mismatch');
  // 头部声明的基线页数必须与实际还原出的基线一致，否则说明链被替换过。
  return { buffer, pages: header.finalPages, manifest: metadata,
    // 返回还原结果，
    walBuffer: base.walBuffer ?? Buffer.alloc(0), ckptBuffer: base.ckptBuffer ?? Buffer.alloc(0) };
    // 以及从基线一路带下来的 WAL / ckpt 缓冲。
}

async function materializeBackup(name, target, depth = 0) {
// 把一份备份直接还原到 target 文件上（与 reconstructBackup 的区别：这里不整体读进内存）。
  const artifact = backupArtifactFile(name);
  // 找到备份工件。
  if (!artifact) throw httpError(404, 'Backup not found');
  // 找不到就 404。
  if (artifact.kind === 'full') {
  // 整份备份：直接复制文件。
    const metadata = normalizeFullManifest(JSON.parse(readFileSync(artifact.manifest, 'utf8')), artifact.file, artifact.manifest);
    // 校验清单。
    copyFileSync(artifact.file, target);
    // 复制页文件到目标位置。
    for (const suffix of ['.wal', '.ckpt']) {
    // 两个附加文件要按清单决定复制还是删除。
      const source = artifact.file + suffix;
      // 备份里的附加文件。
      const destination = target + suffix;
      // 目标位置的附加文件。
      if (metadata.version === 4 && existsSync(source)) copyFileSync(source, destination);
      // 快照格式且备份里确实有，就复制过去。
      else if (existsSync(destination)) unlinkSync(destination);
      // 否则如果目标位置有残留的旧附加文件，要删掉——不然它会被误当成这份备份的一部分。
    }
    // 附加文件处理结束。
    return { pages: Math.floor(statSync(target).size / backupPageSize), manifest: metadata };
    // 返回最终页数与清单。
  }
  // 整份备份分支结束。
  if (depth > 8) throw httpError(422, 'Backup chain too deep');
  // 增量链深度上限，与 reconstructBackup 保持一致。
  const delta = readFileSync(artifact.file);
  // 读出增量文件。
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  // 读出清单。
  if (metadata.sha256 !== sha256(artifact.file) || metadata.version !== 3 || metadata.kind !== 'incremental')
  // 摘要、版本、类型三项校验。
    throw httpError(422, 'Incremental backup checksum or version mismatch');
    // 不符就拒绝。
  const header = parseDeltaHeader(delta);
  // 解析增量头。
  if (metadata.pageFormatVersion === undefined || metadata.base === undefined) throw httpError(422, 'Incremental manifest incomplete');
  // 清单必须完整。
  const base = await materializeBackup(metadata.base, target, depth + 1);
  // 先把基线落到目标文件上（递归，基线也可能是增量）。
  if (header.basePages !== base.pages) throw httpError(422, 'Incremental base page count mismatch');
  // 基线的实际页数必须与增量头声明的一致。
  const descriptor = openSync(target, 'r+');
  // 以可读写方式打开目标文件，准备原地打补丁。
  try {
  // 用 try/finally 保证关闭。
    let offset = 64;
    // 记录区从头部之后开始。
    for (let index = 0; index < header.records; ++index) {
    // 逐条应用记录。
      if (offset + 8 > delta.length) throw httpError(422, 'Incremental backup truncated');
      // 读不全页号说明文件被截断。
      const id = Number(delta.readBigUInt64LE(offset));
      // 目标页号。
      offset += 8;
      // 跳过页号。
      if (id >= header.finalPages || offset + backupPageSize > delta.length) throw httpError(422, 'Incremental backup page out of range');
      // 页号必须在范围内，且后面要有整页数据。
      writeSync(descriptor, delta, offset, backupPageSize, id * backupPageSize);
      // 用增量里的这一页覆盖目标文件的对应位置（按绝对偏移写，相当于直接在文件里打补丁）。
      offset += backupPageSize;
      // 游标跳过这一页。
    }
    // 记录处理结束。
    if (offset !== delta.length) throw httpError(422, 'Incremental backup trailing data');
    // 必须正好用光文件，多余数据一律拒绝。
    ftruncateSync(descriptor, header.finalPages * backupPageSize);
    // 把文件截断到最终页数：如果新版本比基线短，这一步负责去掉尾部多余的页。
  } finally { closeSync(descriptor); }
  // 无论如何关闭描述符。
  return { pages: header.finalPages, manifest: metadata };
  // 返回最终页数与清单。
}

function backupChainDepth(name, seen = new Set()) {
// 算出一条备份链有多少层（用于诊断与展示）。
  const artifact = backupArtifactFile(name);
  // 取工件。
  if (!artifact || artifact.kind === 'full') return 1;
  // 找不到就按 1 算；整份备份本身就是 1 层。
  if (seen.has(artifact.name)) throw httpError(422, 'Backup chain cycle detected');
  // 同一次计算里重复遇到同一个备份，说明链上出现了环（基线指向自己或互相指向）。
  seen.add(artifact.name);
  // 标记已访问。
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  // 读清单。
  if (!metadata.base) return 1;
  // 没有基线字段就当作 1 层。
  return 1 + backupChainDepth(metadata.base, seen);
  // 否则递归累加。
}

function safeChainDepth(name) {
// 容错版本：算不出来就返回 1，不把异常抛给展示层。
  try { return backupChainDepth(name); } catch { return 1; }
  // 链有环或文件损坏时，展示层只想知道一个大概的层数，没必要因此报错。
}
function writeDatabaseAtomically(buffer, walBuffer, ckptBuffer) {
// 把还原结果原子地写回数据库文件与它的两个附加文件。
  const temporary = database + '.restore.tmp';
  // 临时文件名特意带了明确的用途标记。
  writeFileSync(temporary, buffer);
  // 先写临时文件。
  renameSync(temporary, database);
  // 原子改名替换正式数据库文件。
  if (walBuffer && walBuffer.length) {
  // 有 WAL 内容时。
    const walTemporary = database + '.wal.restore.tmp';
    // WAL 的临时文件。
    writeFileSync(walTemporary, walBuffer);
    // 先写临时文件。
    renameSync(walTemporary, database + '.wal');
    // 再原子替换。
  } else if (existsSync(database + '.wal')) unlinkSync(database + '.wal');
  // 没有 WAL 内容时必须把旧的 .wal 删掉，否则残留的日志会和还原后的数据不一致。
  if (ckptBuffer && ckptBuffer.length) {
  // 有 ckpt 内容时（与上面 WAL 的处理完全对称）。
    const ckptTemporary = database + '.ckpt.restore.tmp';
    // 临时文件。
    writeFileSync(ckptTemporary, ckptBuffer);
    // 先写。
    renameSync(ckptTemporary, database + '.ckpt');
    // 再原子替换。
  } else if (existsSync(database + '.ckpt')) unlinkSync(database + '.ckpt');
  // 没有 ckpt 内容就删掉残留文件，避免旧检查点被误用。
}

function createRestoreRollback() {
// 还原之前先把当前数据库整份备份下来，作为"还原失败时回退"的保险。
  const rollbackDirectory = resolve(backupDirectory, 'rollback');
  // 回退快照统一放在 backups/rollback 下。
  mkdirSync(rollbackDirectory, { recursive: true });
  // 确保目录存在。
  const rollbackPath = resolve(rollbackDirectory, `restore-${Date.now()}`);
  // 每次还原用时间戳建一个独立子目录，互不覆盖。
  mkdirSync(rollbackPath, { recursive: true });
  // 建这次的回退目录。
  copyFileSync(database, resolve(rollbackPath, 'db.pages'));
  // 复制当前数据库文件。
  for (const suffix of ['.wal', '.ckpt']) {
  // 两个附加文件如果存在也一起复制。
    if (existsSync(database + suffix)) copyFileSync(database + suffix, resolve(rollbackPath, 'db.pages' + suffix));
    // 命名与主文件保持一致的后缀，方便原样还原。
  }
  // 附加文件处理结束。
  return rollbackPath;
  // 返回回退目录路径，稍后如果还原失败就用它恢复。
}

function restoreRollbackDirectory(rollbackPath) {
// 把数据库恢复到之前保存的回退快照；返回是否真的恢复了。
  const pages = resolve(rollbackPath, 'db.pages');
  // 快照里的数据库文件。
  if (!existsSync(pages)) return false;
  // 快照不存在（或已被清理）就返回 false，让调用方知道没能回退。
  copyFileSync(pages, database);
  // 复制回去。
  for (const suffix of ['.wal', '.ckpt']) {
  // 附加文件同样处理。
    const source = resolve(rollbackPath, 'db.pages' + suffix);
    // 快照里的附加文件。
    if (existsSync(source)) copyFileSync(source, database + suffix);
    // 有就复制回去。
    else if (existsSync(database + suffix)) unlinkSync(database + suffix);
    // 没有就删掉当前的——否则当前那份会与回退后的主文件不匹配。
  }
  return true;
  // 恢复成功。
}
const auditPath = process.env.MINISQL_AUDIT_LOG ?? resolve(dirname(database), 'audit.log');
// 审计日志路径：环境变量可覆盖，默认与数据库同目录。
const auditLimitBytes = 16 * 1024 * 1024;
// 审计日志上限 16 MiB。
function appendAudit(entry) {
// 追加一条审计记录。
  try {
  // 审计写入绝不能影响数据库语义，所以整体包在 try 里。
    if (existsSync(auditPath) && statSync(auditPath).size > auditLimitBytes) return;
    // 超过上限就不再写：宁可丢审计，也不能把磁盘写满导致数据库不可用。
    appendFileSync(auditPath, JSON.stringify(entry) + '\n', 'utf8');
    // 一行一条 JSON，便于流式分析与用命令行工具过滤。
  } catch { /* Audit failures must not change database semantics. */ }
  // 注释重申：审计失败不改变数据库行为。
}
function readAudit(limit, filters = {}) {
// 读审计并按条件过滤，返回最后 limit 条。
  try {
  // 读取失败（文件不存在等）就返回空数组。
    const lines = readFileSync(auditPath, 'utf8').split(/\r?\n/).filter(Boolean);
    // 按行切分并丢掉空行。
    const entries = lines.map(line => { try { return JSON.parse(line); } catch { return { malformed: true }; } });
    // 逐行解析；解析失败的行不丢弃，而是标成 malformed 保留下来，
    // 这样"日志里出现了坏行"这件事本身是可观测的。
    return entries.filter(entry => {
    // 按过滤条件筛选。
      if (filters.user && entry.user !== filters.user) return false;
      // 按用户过滤。
      if (filters.sessionId && entry.sessionId !== filters.sessionId) return false;
      // 按会话过滤。
      if (filters.object && !String(entry.object ?? '').split(',').includes(filters.object)) return false;
      // 按对象过滤：审计记录里的 object 可能是逗号分隔的多个对象，所以先拆开再判断。
      if (filters.from && new Date(entry.at).getTime() < new Date(filters.from).getTime()) return false;
      // 起始时间过滤。
      if (filters.to && new Date(entry.at).getTime() > new Date(filters.to).getTime()) return false;
      // 结束时间过滤。
      return true;
      // 全部条件通过。
    }).slice(-limit);
    // 只取最后 limit 条（最近的记录最有价值）。
  } catch { return []; }
  // 异常时返回空。
}
const sessionIdleMs = Number(process.env.MINISQL_SESSION_IDLE_MS ?? 300000);
// 会话空闲多久后视为过期，默认 5 分钟。
if (!Number.isInteger(sessionIdleMs) || sessionIdleMs < 100 || sessionIdleMs > 3600000) throw new Error('Invalid session idle timeout');
// 必须在 100 毫秒到 1 小时之间，避免配出"几乎立刻过期"或"永不过期"的极端值。
const maxSessions = Number(process.env.MINISQL_MAX_SESSIONS ?? 16);
// 最多允许多少个会话。
const transactionLockTimeoutMs = Number(process.env.MINISQL_TRANSACTION_LOCK_TIMEOUT_MS ?? 30000);
// 等事务锁的超时时间。
const engineRequestTimeoutMs = Number(process.env.MINISQL_ENGINE_REQUEST_TIMEOUT_MS ?? 30000);
// 向数据库进程发一次请求的超时时间。
if (!Number.isInteger(maxSessions) || maxSessions < 1 || maxSessions > 128) throw new Error('Invalid maximum session count');
// 会话数上限必须在 1..128。
if (!Number.isInteger(transactionLockTimeoutMs) || transactionLockTimeoutMs < 100 || transactionLockTimeoutMs > 3600000) throw new Error('Invalid transaction lock timeout');
// 事务锁超时在 100 毫秒到 1 小时之间。
if (!Number.isInteger(engineRequestTimeoutMs) || engineRequestTimeoutMs < 1000 || engineRequestTimeoutMs > 3600000) throw new Error('Invalid engine request timeout');
// 引擎请求超时至少 1 秒（再短会把正常的慢查询误杀），最多 1 小时。
const httpError = (status, message) => Object.assign(new Error(message), { status });
// 造一个带 HTTP 状态码的 Error；上层统一按 error.status 决定响应码。
function notifyWaiters(waiters) {
// 唤醒某个等待队列里的所有等待者。
  const current = waiters.splice(0);
  // 一次性把队列清空取出（splice 返回被移除的元素），避免唤醒过程中队列被并发修改。
  for (const waiter of current) {
  // 逐个唤醒。
    clearTimeout(waiter.timer);
    // 清掉它的超时定时器。
    waiter.resolve();
    // 兑现它的 Promise（唤醒后由调用方重新抢锁）。
  }
  // 唤醒结束。
}
function removeWaiter(waiters, waiter) {
// 从等待队列里移除一个等待者（通常在它超时的时候）。
  const index = waiters.indexOf(waiter);
  // 找位置。
  if (index >= 0) waiters.splice(index, 1);
  // 找到了就删掉；没找到说明已经被唤醒或已移除，不算错误。
}
function waitForTurn(waiters, session) {
// 进入某个等待队列并等待被唤醒或超时。
  return new Promise((resolve, reject) => {
  // 返回一个 Promise，靠 resolve/reject 决定结果。
    const waiter = { resolve, timer: undefined, session };
    // 等待者记录：兑现回调、超时定时器、所属会话。
    waiter.timer = setTimeout(() => {
    // 超时处理。
      removeWaiter(waiters, waiter);
      // 把自己从队列里摘掉，避免之后被"唤醒"到一个已经失败的等待者。
      reject(httpError(409, `Session lock wait exceeded ${transactionLockTimeoutMs}ms`));
      // 以 409 拒绝，并把等待时长写进信息，便于用户判断是不是锁竞争。
    }, transactionLockTimeoutMs);
    // 超时时长取配置值。
    waiters.push(waiter);
    // 入队。
  });
  // Promise 构造结束。
}
async function acquireTurn(session) {
// 获取"执行权"：拿到之后才能向数据库发请求。
  for (;;) {
  // 循环抢锁，抢不到就等，醒来后重试。
    if (session.cancelRequested) {
    // 等待期间用户请求了取消。
      session.cancelRequested = false;
      // 清掉标记，避免影响下一次请求。
      throw Object.assign(new Error('Cancellation requested'), { status: 409, code: 5002 });
      // 抛 5002：调用方按这个码识别"是取消，不是失败"。
    }
    // 取消检查结束。
    if (quarantined) throw httpError(503, 'Database requires inspection after an uncertain failure');
    // 服务处于隔离状态时拒绝新操作（提交状态未知，继续写会更糟）。
    if (transactionOwner && transactionOwner !== session.id) {
    // 别的会话正持有事务。
      session.waiting = true;
      // 标记自己在等（诊断接口会用到）。
      await waitForTurn(transactionWaiters, session);
      // 等事务队列。
      session.waiting = false;
      // 醒来后清掉标记。
    } else if (turnOwner && turnOwner !== session.id) {
    // 事务无人持有，但执行权在别的会话手里。
      session.waiting = true;
      // 标记等待。
      await waitForTurn(turnWaiters, session);
      // 等执行权队列。
      session.waiting = false;
      // 清标记。
    } else if (session.activeRequest) {
    // 同一个会话已经有请求在跑。
      session.waiting = true;
      // 标记等待。
      await waitForTurn(turnWaiters, session);
      // 也排进执行权队列（保证同一会话的请求串行）。
      session.waiting = false;
      // 清标记。
    } else {
    // 三条阻塞条件都不成立，可以拿锁了。
      session.waiting = false;
      // 明确标记不在等待。
      turnOwner = session.id;
      // 把执行权交给自己。
      return;
      // 返回，调用方可以发请求了。
    }
    // 分支结束。
  }
  // 循环继续。
}
function releaseTurn(session) {
// 释放执行权。
  if (turnOwner !== session.id) return;
  // 不是自己持有的就不动（防止误释放别人的锁）。
  turnOwner = undefined;
  // 清空持有者。
  notifyWaiters(turnWaiters);
  // 唤醒所有等待执行权的会话。
}
function applyTransactionResult(session, result) {
// 根据一次请求的结果更新"事务归属"状态。
  const state = result?.transactionState ?? session.transactionState;
  // 优先用数据库进程回报的状态；没回报就沿用会话里记的。
  session.transactionState = state;
  // 记进会话。
  if (state === 'IDLE') {
  // 空闲表示这个会话没有活动事务了。
    if (transactionOwner === session.id) {
    // 如果事务原本就归它。
      transactionOwner = undefined;
      // 释放事务归属。
      notifyWaiters(transactionWaiters);
      // 唤醒等事务的其它会话。
    }
    // 归属判断结束。
  } else transactionOwner = session.id;
  // 非空闲（ACTIVE/ABORTED）说明事务归这个会话。
}
function clearAllSessions() {
// 清空所有会话：关定时器、删取消令牌文件、从表里移除。
  for (const [id, session] of sessions) {
  // 逐个会话处理。
    clearTimeout(session.timer);
    // 清掉空闲超时定时器。
    clearCancelFile(session.cancelFile);
    // 删掉它的取消令牌文件，避免残留文件影响后续同名会话。
    sessions.delete(id);
    // 从会话表移除。
  }
  transactionOwner = undefined;
  // 清空事务归属。
  turnOwner = undefined;
  // 清空执行权归属。
  activeOperation = undefined;
  // 清掉"正在执行的操作"描述。
  notifyWaiters(transactionWaiters);
  // 唤醒所有等事务的会话（让它们重新判断并报错，而不是永远挂着）。
  notifyWaiters(turnWaiters);
  // 唤醒所有等执行权的会话。
}
async function startEngine() {
// 启动数据库进程（惰性，且并发安全）。
  if (engine) return engine;
  // 已经开着就直接返回。
  if (engineOpening) return engineOpening;
  // 正在开着就把那次的 Promise 返回给调用方，避免重复启动两个进程。
  const cancelFile = sessionCancelFile('engine');
  // 引擎级取消令牌文件（会话名叫 engine）。
  clearCancelFile(cancelFile);
  // 启动前先清掉可能残留的旧文件。
  engineOpening = openSession(executable, database, { timeoutMs: engineRequestTimeoutMs, env: { MINISQL_SESSION_ID: 'bridge', MINISQL_CANCEL_FILE: cancelFile } })
  // 打开会话，并把会话标识与取消文件路径通过环境变量传给子进程。
    .then(worker => {
    // 打开成功。
      const value = { worker, cancelFile, closing: false };
      // 记下工人对象、取消文件与"是否正在主动关闭"标志。
      engine = value;
      // 挂到全局。
      worker.closed.then(() => {
      // 子进程退出时的处理。
        if (engine !== value) return;
        // 说明这个 engine 已经被换掉了（比如重启过），旧实例的退出直接忽略。
        const wasClosing = value.closing;
        // 先记下是不是我们主动关的。
        engine = undefined;
        // 清空全局引用。
        clearCancelFile(cancelFile);
        // 清掉取消文件。
        if (!wasClosing) quarantined = true;
        // 不是主动关闭却退出了，说明进程是崩的——提交状态可能未知，
        // 因此进入隔离状态，拒绝后续操作并提示需要人工检查。
        clearAllSessions();
        // 进程没了，所有会话都失效，一并清掉。
      });
      // 退出处理结束。
      return value;
      // 返回新引擎。
    })
    // 成功分支结束。
    .catch(error => {
    // 启动失败。
      clearCancelFile(cancelFile);
      // 清掉取消文件。
      throw error;
      // 继续抛出。
    })
    // 失败分支结束。
    .finally(() => { engineOpening = undefined; });
    // 无论成功失败都清掉"正在打开"的标记。
  return engineOpening;
  // 返回这一次的打开过程。
}
async function ensureEngine() {
// 需要引擎时调用：隔离状态下直接拒绝。
  if (quarantined) throw httpError(503, 'Database requires inspection after an uncertain failure');
  // 503 提示调用方：服务还在，但因为上次故障需要人工介入。
  return startEngine();
  // 否则正常启动（或复用）。
}
async function closeEngineIfIdle(context = {}) {
// 没有会话在用时，主动关掉引擎进程，释放资源。
  if (!engine || engine.closing || sessions.size) return;
  // 没引擎、正在关、或者还有会话在用，都不关。
  const current = engine;
  // 记下要关的那一个（关闭过程中全局 engine 可能变化）。
  current.closing = true;
  // 标记为主动关闭，这样它的退出不会被误判成崩溃而触发隔离。
  try { return await current.worker.close(context); }
  // 优雅关闭并返回结果。
  finally {
  // 不管关闭成功还是失败，都要做收尾。
    if (engine === current) engine = undefined;
    // 只有全局引用还指向这个实例时才清空（避免误清掉刚刚新建的引擎）。
    clearCancelFile(current.cancelFile);
    // 清掉它的取消令牌文件。
  }
  // 收尾结束。
}
async function closeSession(session) {
// 关闭一个会话：必要时先回滚事务，再清理定时器与令牌文件。
  if (session.closing) return { success: true, transactionState: 'IDLE', transactionRolledBack: false };
  // 已经在关就直接返回成功（幂等，重复调用不会出问题）。
  session.closing = true;
  // 标记正在关闭，后续请求会被拒绝。
  clearTimeout(session.timer);
  // 停掉空闲回收定时器。
  sessions.delete(session.id);
  // 从会话表里移除。
  try {
  // 主体逻辑。
    let rolledBack = false;
    // 记录是否真的做了回滚（返回给调用方）。
    if (session.transactionState !== 'IDLE') {
    // 还有未结束的事务。
      const result = await runSessionOperation(session, 'execute', 'ROLLBACK;', undefined);
      // 发一条 ROLLBACK 把事务收掉，避免下一个会话继承一个半截事务。
      rolledBack = result.success !== false;
      // 记下回滚结果。
      if (result.success === false) return result;
      // 回滚失败就把失败结果直接返回给调用方（不要假装关闭成功）。
    }
    // 事务处理结束。
    return { success: true, transactionState: 'IDLE', transactionRolledBack: rolledBack };
    // 返回关闭结果。
  } finally {
  // 无论如何都要做的收尾。
    clearCancelFile(session.cancelFile);
    // 删掉取消令牌文件。
    await closeEngineIfIdle({ user: session.user, password: session.password });
    // 如果已经没有别的会话，顺手把引擎进程关掉以释放资源。
  }
  // 收尾结束。
}
function touchSession(session) {
// 刷新会话的活跃时间并重设空闲回收定时器。
  clearTimeout(session.timer);
  // 先停掉旧的定时器。
  session.lastActiveAt = new Date().toISOString();
  // 记下最近活跃时间（诊断与展示用）。
  const epoch = Symbol();
  // 用一个新的 Symbol 作为"这一轮定时器"的标记。
  session.epoch = epoch;
  // 写进会话。
  session.timer = setTimeout(() => {
  // 空闲超时后的处理。
    if (session.epoch !== epoch || !sessions.has(session.id) || session.closing) return;
    // 三种情况直接放弃：这一轮已经被刷新过、会话已经不在表里、已经在关闭。
    // 用 epoch 比较是为了防止"旧定时器在新一轮刷新之后才触发"导致误关。
    // 长请求、锁等待和当前轮次都不能被空闲回收中断；事务持有者则由 closeSession 回滚后回收。
    // 这条注释说明了下面这个判断的理由：正在干活的会话不能因为"看起来空闲"被回收，
    // 否则一个跑了两分钟的查询会被误杀。
    // 长请求、锁等待和当前轮次都不能被空闲回收中断；事务持有者则由 closeSession 回滚后回收。
    if (session.activeRequest || session.waiting || turnOwner === session.id) {
    // 三种"其实很忙"的状态。
      touchSession(session);
      // 直接续期，等它干完再说。
      return;
      // 本轮结束。
    }
    // 忙碌判断结束。
    closeSession(session).catch(() => { quarantined = true; });
    // 真正回收；如果回收本身失败（通常是回滚失败），就进入隔离状态，
    // 因为这意味着数据库里可能留着一个状态不明的事务。
  }, sessionIdleMs);
  // 空闲时长取配置值。
}

function callDatabase(mode, sql = '', extraEnv = {}, authorization = null) {
  return new Promise((resolveResult, reject) => {
  // 返回 Promise，由子进程结果决定兑现或拒绝。
    // authorization 为空表示 bridge 内部调用（备份/恢复/健康检查）自己承担授权；
    // 否则把身份透传给引擎，由引擎按绑定结果判定对象权限。
    const env = { ...process.env, ...extraEnv, MINISQL_ACCESS_FILE: accessPagesFile };
    if (authorization) {
      env.MINISQL_USER = authorization.user ?? '';
      env.MINISQL_PASSWORD = authorization.password ?? '';
    } else {
      env.MINISQL_AUTH_BYPASS = '1';
    }
    const child = spawn(executable, [database, mode], { windowsHide: true, env });
    const output = [];
    // 累积标准输出。
    let length = 0, stopped = false;
    // 已读字节数；是否已被我们主动杀掉。
    const stop = () => { stopped = true; child.kill(); };
    // 统一的"停掉子进程"动作。
    const timer = setTimeout(stop, engineRequestTimeoutMs);
    // 超时就杀掉，避免一条慢语句把 HTTP 连接一直挂着。
    child.stdin.on('error', () => {});
    // 忽略写入错误（子进程可能已退出）。
    child.stderr.resume();
    // 把标准错误读掉，避免管道写满导致子进程卡住。
    child.stdout.on('data', chunk => {
    // 累积输出。
      length += chunk.length;
      // 累计大小。
      if (length > 32 * 1024 * 1024) stop();
      // 超过 32 MiB 就停掉：这是一个防止内存被撑爆的硬上限。
      else output.push(chunk);
      // 没超就收集。
    });
    // 数据回调结束。
    child.on('error', error => { clearTimeout(timer); reject(error); });
    // 子进程根本起不来时清掉定时器并拒绝。
    child.on('close', code => {
    // 子进程结束时。
      clearTimeout(timer);
      // 清掉超时定时器。
      try {
      // 下面的解析与校验可能失败，统一走 catch。
        if (stopped || (code !== 0 && code !== 1)) throw new Error('Database process stopped unexpectedly');
        // 被我们杀掉、或者退出码既不是 0 也不是 1，都算异常结束。
        const data = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(output)));
        // 严格按 UTF-8 解码再解析 JSON（非法字节直接抛错，不静默替换）。
        if (code === 1 && data.success !== false) throw new Error('Invalid database exit status');
        // 约定：退出码 1 必须伴随 success:false，否则视为协议不一致。
        if (data.error?.code === 4001 || data.error?.code === 9999) quarantined = true;
        // 4001/9999 表示"提交状态未知"这类严重错误，直接把服务置为隔离状态。
        resolveResult(data);
        // 校验通过，把结果交给调用方。
      } catch (error) {
      // 解析失败或校验不过。
        quarantined = true;
        // 连子进程的输出都无法信任，说明状态不明，置为隔离。
        reject(error);
        // 拒绝调用方。
      }
    });
    child.stdin.end(sql);
    // 把 SQL 写进子进程标准输入并关闭管道。
  });
  // Promise 构造结束。
}
// 所有读写共用队列；无事务锁时禁止多个进程同时打开同一数据库。
// 这句话是下面这个队列存在的核心理由：同一个数据库文件不允许被两个进程同时打开，
// 所以任何会碰到文件的操作用一条全局队列串起来。

// 所有读写共用队列；无事务锁时禁止多个进程同时打开同一数据库。
function enqueue(operation, allowQuarantined = false) {
// 把操作排进全局队列。
  if (queued >= 64) return Promise.reject(new Error('Request queue full'));
  // 排队超过 64 个就拒绝，避免无限堆积把内存吃光。
  ++queued;
  // 计数加一。
  const result = queue.then(() => {
  // 排到队尾。
    if (quarantined && !allowQuarantined) throw new Error('Database requires inspection after an uncertain failure');
    // 隔离状态下默认拒绝新操作；只有少数"检查/诊断"类操作可以显式放行。
    return operation();
    // 真正的操作。
  });
  // 队列挂接结束。
  queue = result.catch(() => {}).finally(() => { --queued; });
  // 推进队列：吞掉错误（已交给调用方）并复位计数。
  return result;
  // 返回给调用方。
}

async function runSessionOperation(session, mode, sql, res, context = {}) {
// 执行一次会话操作：拿执行权 → 进队列 → 发请求 → 处理取消与隔离。
  await acquireTurn(session);
  // 先抢执行权（可能等待）。
  try {
  // 用 try 保证最后一定释放执行权。
    const result = await enqueue(async () => {
    // 再进全局队列。
      if (res?.destroyed) throw httpError(499, 'Request disconnected before execution');
      // 轮到自己时客户端已经断开，就别白跑一趟了（499 是"客户端关闭连接"的约定码）。
      const currentEngine = await ensureEngine();
      // 确保引擎进程已就绪（隔离状态下这里会抛 503）。
      clearCancelFile(session.cancelFile);
      // 清掉上一次可能残留的取消令牌。
      session.activeRequest = true;
      // 标记这个会话有请求在跑（空闲回收与并发判断都会读它）。
      activeOperation = { sessionId: session.id, cancelFile: session.cancelFile };
      // 记录当前操作，供诊断接口展示"谁在跑什么"。
      const disconnected = () => {
      // 客户端断开时的处理。
        if (!res?.writableEnded) {
        // 响应还没写完，说明是被中断的。
          try { writeFileSync(session.cancelFile, 'disconnect\n', 'utf8'); }
          // 写一个取消令牌文件：数据库进程会看到它并主动停止当前语句。
          catch {
          // 连令牌都写不出去。
            quarantined = true;
            // 无法通知数据库停止 → 状态不明，进入隔离。
            void currentEngine.worker.terminate();
            // 直接终止引擎进程，避免它继续跑一条没人要的语句。
          }
          // 写入异常处理结束。
        }
      };
      // disconnected 定义结束。
      res?.once('close', disconnected);
      // 挂在响应流的 close 事件上。
      try {
      // 请求主体。
        const { stream: streamRequested, onFrame, ...sessionContext } = context;
        // 把上下文里"流式标记"与"逐帧回调"拆出来，其余字段透传给数据库进程。
        const value = streamRequested
          // 流式执行走 requestStream（会多次回调 onFrame），
          ? await currentEngine.worker.requestStream('executeStream', sql, {
              // 上下文里带上会话号、取消文件与凭据。
              sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, ...sessionContext,
            }, onFrame)
            // 以及逐帧回调。
          : await currentEngine.worker.request(mode, sql, {
              // 普通请求走 request。
              sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, ...sessionContext,
            });
            // 同样带上会话上下文。
        if (value.error?.code === 5002) value.cancelled = true;
        // 5002 是"被取消"，这里补一个更直白的 cancelled 标记给上层用。
        if (value.commitState === 'unknown' || value.error?.code === 4001 || value.error?.code === 9999) quarantined = true;
        // 三种"提交状态不可知"的信号都要进入隔离：显式标记、4001、9999。
        return value;
      } catch (error) {
      // 请求过程中抛错。
        quarantined = true;
        // 这属于非预期失败，状态不明，隔离。
        throw error;
        // 继续抛给上层。
      } finally {
      // 收尾。
        session.activeRequest = false;
        // 清掉"有请求在跑"的标记。
        activeOperation = undefined;
        // 清掉当前操作描述。
        clearCancelFile(session.cancelFile);
        // 删掉取消令牌，避免影响下一次请求。
        res?.off('close', disconnected);
        // 摘掉断开监听，防止它在本次请求之后仍然触发。
        if (!session.closing) touchSession(session);
        // 只要会话没在关闭，就续期空闲定时器（这次操作算一次活跃）。
      }
    });
    applyTransactionResult(session, result);
    // 按结果更新事务归属。
    return result;
    // 把结果返回给调用方。
  } finally {
    releaseTurn(session);
    // 无论成功失败都要释放执行权，否则其它会话会被永久阻塞。
  }
}

async function runSnapshotOperation(session, target) {
// 执行快照操作：与普通会话操作类似，但不参与事务/执行权那套协调。
  return enqueue(async () => {
  // 进全局队列（快照要读整个数据库文件，必须独占）。
    const currentEngine = await ensureEngine();
    // 确保引擎就绪。
    clearCancelFile(session.cancelFile);
    // 清掉旧取消令牌。
    const value = await currentEngine.worker.request('snapshot', '', {
    // 发快照请求。
      sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, target,
    });
    if (value.error?.code === 5002) value.cancelled = true;
    // 取消标记。
    if (value.commitState === 'unknown' || value.error?.code === 4001 || value.error?.code === 9999) quarantined = true;
    // 与普通请求相同的隔离判定。
    return value;
  });
}

function queryResult(data, durationMs) {
// 把数据库进程返回的原始结果整理成 HTTP 响应用的形状。
  const results = data.results ?? [];
  // 语句结果列表（多语句执行时会有多条）。
  const last = results.at(-1);
  // 取最后一条：面向用户的"结果集"通常来自最后一条查询。
  return {
  // 返回整理后的对象。
    ...data, schemaVersion: 1, engine: 'minisql-cpp', durationMs,
    // 原样带上数据库的字段，再补协议版本、引擎标识与本次耗时。
    rows: last?.rows ?? [], columns: last?.columns ?? [],
    // 把最后一组行与列提到顶层，方便前端直接用。
    columnTypes: last?.columnTypes ?? [],
    // 列类型同样提上来。
    affectedRows: results.reduce((sum, result) => sum + (result.commitState === 'rolledBack' ? 0 : result.affectedRows ?? 0), 0),
    // 影响行数：把所有语句的影响行数加起来，但被回滚的语句记 0（它的修改最终没生效）。
    statements: data.statements ?? data.completedStatements ?? 0,
    // 执行了多少条语句；两个字段名都兼容一下。
    plan: data.plan ?? last?.plan ?? [],
    // 逻辑计划：优先用整体结果里的，其次用最后一条语句的。
    optimizedPlan: data.optimizedPlan ?? last?.optimizedPlan,
    // 优化后的计划。
    optimizationRules: data.optimizationRules ?? last?.optimizationRules,
    // 命中的优化规则。
    executionStats: data.executionStats ?? last?.executionStats,
    // 执行统计（每个算子的耗时/行数等）。
    resourceUsage: last?.resourceUsage ?? data.resourceUsage,
    // 资源用量（内存、溢出到磁盘的字节数等）。
  };
}

const server = http.createServer(async (req, res) => {
// HTTP 服务主处理函数：整个 bridge 的所有接口都在这里分发。
  const auditStarted = performance.now();
  // 记下请求开始时间，用于统计耗时。
  const auditId = randomUUID();
  // 每次请求生成一个审计编号。
  let requestId = randomUUID();
  // 响应里回显的请求编号（有的分支会覆盖它）。
  let auditSql = '';
  // 本次请求涉及的 SQL（脱敏后写审计）。
  let requestUser = req.headers['x-minisql-user'] ?? 'admin';
  // 调用方身份：优先取请求头，缺省按 admin（权限目录未启用时的兼容行为）。
  const requestPassword = req.headers['x-minisql-password'];
  // 口令（仅内存中使用，绝不落日志）。
  let auditSessionId = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)/)?.[1] ?? '';
  // 从 URL 里提取会话号，供审计记录。
  const origin = req.headers.origin;
  // 请求来源，用于 CORS 判断。
  let auditObjects = [];
  // 本次访问的对象列表（审计用）。
  let streamOutput = null;
  // 流式响应用到的输出对象（只在流式接口里赋值）。
  const send = (status, data) => {
  // 统一的响应工具。
    const payload = data && typeof data === 'object' && !Array.isArray(data)
      // 只有对象（非数组）才补协议字段；
      ? {
          // 补上的统一外壳：
          protocolVersion: 1,
          // 协议版本。
          requestId,
          // 请求编号，便于把响应与日志对应起来。
          success: data.success !== false && !data.error,
          // 由业务字段推导出的成功标记。
          stages: data.stages ?? {},
          // 各阶段状态。
          diagnostics: data.diagnostics ?? [],
          // 诊断信息。
          ...data,
          // 最后铺开业务数据（让它能覆盖上面的默认值）。
        }
        // 对象分支结束。
      : data;
      // 非对象（字符串/数组）直接原样返回。
    if (!res.destroyed && !res.writableEnded) {
    // 只有连接还在、响应还没写完时才写，避免在已断开的连接上报错。
      appendAudit({
      // 写审计记录。
        id: auditId,
        // 审计编号。
        at: new Date().toISOString(),
        // 发生时间。
        method: req.method,
        // HTTP 方法。
        path: req.url,
        // 请求路径。
        user: requestUser,
        // 调用方身份。
        sessionId: auditSessionId,
        // 会话号。
        sql: auditSql,
        // 脱敏后的 SQL。
        object: auditObjects.length ? auditObjects.join(',') : undefined,
        // 涉及的对象（多个用逗号连接；没有就不写这个字段）。
        status,
        // 响应状态码。
        success: payload?.success !== false,
        // 这次请求在业务上是否成功。
        durationMs: performance.now() - auditStarted,
        // 端到端耗时。
        affectedRows: payload?.affectedRows ?? 0,
        // 影响行数。
        errorCode: payload?.error?.code,
        // 错误码（成功时为 undefined）。
        transactionState: payload?.transactionState,
        // 请求结束时的会话事务状态。
        quarantined,
        // 当时服务是否处于隔离状态——审计里记下它，事后排查能区分"正常拒绝"与"故障期拒绝"。
      });
      // 审计写入结束。
    }
    // 连接检查结束。
    if (res.destroyed || res.writableEnded) return;
    // 再一次确认连接可用（写审计期间客户端可能又断开了）。
    const headers = { 'Content-Type': 'application/json; charset=utf-8', 'Vary': 'Origin' };
    // 基本响应头；Vary: Origin 告诉缓存"响应随 Origin 变化"，避免缓存串味。
    if (origin && allowedOrigins.has(origin)) headers['Access-Control-Allow-Origin'] = origin;
    // 只有白名单里的来源才回 CORS 头（不是无条件回显，避免变成开放代理）。
    headers['Access-Control-Allow-Headers'] = 'Content-Type, X-MiniSQL-User, X-MiniSQL-Password';
    // 允许的自定义头：内容类型与身份头。
    headers['Access-Control-Allow-Methods'] = 'GET, POST, DELETE, OPTIONS';
    // 允许的方法。
    res.writeHead(status, headers);
    // 写状态行与响应头。
    res.end(JSON.stringify(payload));
    // 输出 JSON 正文并结束响应。
  };
  const beginStream = (status = 200) => {
  // 开始一个流式响应（NDJSON：一行一条 JSON）。
    if (res.destroyed || res.writableEnded) return;
    // 连接不可用就直接返回 undefined，调用方据此放弃。
    const headers = { 'Content-Type': 'application/x-ndjson; charset=utf-8', 'Cache-Control': 'no-store', 'Vary': 'Origin' };
    // 流式响应的内容类型是 NDJSON；明确禁止缓存。
    if (origin && allowedOrigins.has(origin)) headers['Access-Control-Allow-Origin'] = origin;
    // 同样只在白名单内回 CORS 头。
    headers['Access-Control-Allow-Headers'] = 'Content-Type, X-MiniSQL-User, X-MiniSQL-Password';
    // 允许的头。
    headers['Access-Control-Allow-Methods'] = 'GET, POST, DELETE, OPTIONS';
    // 允许的方法。
    res.writeHead(status, headers);
    // 写响应头。
    const write = value => new Promise((resolve, reject) => {
    // 写一条消息，返回 Promise 以便调用方 await（实现背压控制）。
      // Closing a streaming response is the normal client-cancellation path. The
      // response close listener signals the engine through its cancel file.
      if (res.destroyed || res.writableEnded) { resolve(false); return; }
      const finish = error => {
      // 本次写入结束的收尾：摘掉监听器。
        res.off('drain', onDrain);
        // 摘掉 drain 监听。
        res.off('error', onError);
        // 摘掉 error 监听。
        if (error) reject(error); else resolve(true);
      };
      const onDrain = () => finish();
      // 缓冲区排空时算写完。
      const onError = error => finish(error);
      // 出错时带着错误结束。
      res.once('error', onError);
      // 先挂上错误监听。
      if (res.write(JSON.stringify({ protocolVersion: 1, requestId, ...value }) + '\n')) finish();
      // 统一补上协议版本与请求编号再写出一行；write 返回 true 表示缓冲区没满，可以立即算写完。
      else res.once('drain', onDrain);
      // 返回 false 说明缓冲区已满，要等 drain 事件才能认为这一条写完了（这就是背压）。
    });
    // write 定义结束。
    return { write, end() { if (!res.destroyed && !res.writableEnded) res.end(); } };
    // 返回写入器：write 逐条写，end 结束响应。
  };
  const sendStream = async (status, data) => {
  // 把一份完整结果以流式形式发出去（先 meta，再逐行，最后 complete）。
    const output = beginStream(status);
    // 开启流。
    if (!output) return;
    // 连接不可用就返回。
    const { write } = output;
    // 取出写入器。
    const rows = Array.isArray(data.rows) ? data.rows : [];
    // 结果行（容错处理成数组）。
    if (data.success === false) {
    // 失败时只发一条 error 消息。
      await write({ type: 'error', success: false, error: data.error, commitState: data.commitState, transactionState: data.transactionState });
      // 把错误与提交状态一起发出去——commitState 是调用方判断"要不要人工检查"的关键。
    } else {
    // 成功时按顺序发三类消息。
      await write({ type: 'meta', success: true, schemaVersion: data.schemaVersion, engine: data.engine,
        // 先发元数据：协议版本、引擎标识，
        columns: data.columns, columnTypes: data.columnTypes, statements: data.statements,
        // 列定义、列类型、语句条数，
        affectedRows: data.affectedRows, durationMs: data.durationMs });
        // 影响行数与耗时。
      for (let index = 0; index < rows.length; ++index) await write({ type: 'row', index, values: rows[index] });
      // 逐行发；每条都 await，这样客户端读得慢时会自然放慢发送节奏。
      await write({ type: 'complete', success: true, rowCount: rows.length, affectedRows: data.affectedRows,
        // 最后发 complete：总行数、影响行数、
        statements: data.statements, transactionState: data.transactionState, durationMs: data.durationMs });
        // 语句条数、事务状态与总耗时。
    }
    // 分支结束。
    output.end();
    // 关闭响应。
  };
  if (origin && !allowedOrigins.has(origin)) { send(403, { error: { message: 'Origin not allowed' } }); return; }
  // 来源不在白名单里，直接拒绝——这一步在鉴权之前，避免非白名单站点触发任何业务逻辑。
  if (req.method === 'OPTIONS') { send(204, {}); return; }
  // 预检请求回 204，让浏览器继续。
  reloadAccessIfStale();
  // 每次请求前检查权限目录是否被别的进程改过。
  if (!canConnect(access, requestUser, requestPassword ?? null)) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
  // 认证与"连接权限"检查；失败统一回 7001，不区分用户不存在与口令错误。
  if (req.method === 'GET' && req.url === '/api/users') {
  // 列用户清单：有 GRANT 或 READ 任一权限即可查看。
    if (!can(access, requestUser, 'GRANT') && !can(access, requestUser, 'READ')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 两个权限都没有就拒绝。
    const publicState = publicAccess(access);
    // 取脱敏后的视图（只有 passwordProtected 布尔值，没有哈希）。
    send(200, { success: true, current: requestUser, users: publicState.users });
    // 返回用户列表，并告诉调用方"你是谁"（current），方便前端高亮。
    return;
  }
  if (req.method === 'GET' && req.url === '/api/access') {
  // 读取整份权限目录：需要 GRANT 权限。
    if (!can(access, requestUser, 'GRANT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 没权限就拒绝。
    send(200, { success: true, access: publicAccess(access) });
    // 返回脱敏后的完整目录。
    return;
  }
  if (req.method === 'PUT' && req.url === '/api/access') {
  // 整表替换权限目录：同样需要 GRANT。
    if (!can(access, requestUser, 'GRANT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 没权限就拒绝。
    try {
    // 读请求体与替换都可能失败。
      const chunks = [];
      // 收集请求体分片。
      for await (const chunk of req) chunks.push(chunk);
      // 异步迭代读完整个请求体。
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      // 严格按 UTF-8 解码再解析 JSON。
      access = normalizeAccess(body.access ?? body, access);
      // 规范化新目录；把旧目录作为 previous，这样没重新提供口令的用户会保留原哈希。
      persistAccess();
      // 落盘并递增权限版本。
      send(200, { success: true, access: publicAccess(access) });
      // 返回替换后的脱敏目录。
    } catch (error) { send(400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    // 任何失败（JSON 非法、目录不合法）都回 400 并带上原因。
    return;
  }
  // X24 原子权限资源接口：每个端点对应一次原子变更，失败不产生部分状态。

  // X24 原子权限资源接口：每个端点对应一次原子变更，失败不产生部分状态。
  const readJson = async () => {
  // 读并解析请求体的公共工具。
    const chunks = [];
    // 分片缓冲。
    for await (const chunk of req) chunks.push(chunk);
    // 异步迭代读完整个请求体。
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
    // 严格按 UTF-8 解码后解析 JSON。
  };
  const requireGrant = () => { if (!can(access, requestUser, 'GRANT')) throw httpError(403, 'Permission denied'); };
  // 统一的"需要授予权限"检查；失败就抛，由外层 catch 转成响应。
  const commitAccess = () => { persistAccess(); send(200, { success: true, access: publicAccess(access) }); };
  // 统一的提交动作：落盘（版本自增）+ 返回新的脱敏目录。
  const badRequest = error => send(error?.status ?? 400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } });
  // 统一的错误响应：优先用错误自带的 status（例如 404/409），否则按 400 处理。

  if (req.method === 'POST' && req.url === '/api/users') {
  // 建用户。
    try { requireGrant(); access = createUser(access, await readJson()); commitAccess(); }
    // 鉴权 → 读请求体 → 用纯函数生成新目录 → 提交。
    catch (error) { badRequest(error); }
    // 任何失败都转成响应。
    return;
  }
  const userDelete = req.url?.match(/^\/api\/users\/([^/]+)$/);
  // 匹配 /api/users/<名字>。
  if (req.method === 'DELETE' && userDelete) {
  // 删用户。
    try { requireGrant(); access = dropUser(access, decodeURIComponent(userDelete[1])); commitAccess(); }
    // 路径里的用户名要 decodeURIComponent，以支持非 ASCII 名字。
    catch (error) { badRequest(error); }
    return;
  }
  const passwordRoute = req.url?.match(/^\/api\/users\/([^/]+)\/password$/);
  // 匹配改密路由。
  if (req.method === 'POST' && passwordRoute) {
  // 改密。
    try { requireGrant(); const body = await readJson(); access = setPassword(access, decodeURIComponent(passwordRoute[1]), body.password); commitAccess(); }
    // 鉴权 → 读体 → 设置口令 → 提交。
    catch (error) { badRequest(error); }
    return;
  }
  const addRoleRoute = req.url?.match(/^\/api\/users\/([^/]+)\/roles$/);
  // 匹配"给用户加角色"。
  if (req.method === 'POST' && addRoleRoute) {
  // 加角色。
    try { requireGrant(); const body = await readJson(); access = addRole(access, decodeURIComponent(addRoleRoute[1]), body.role); commitAccess(); }
    // 与上面同构：鉴权 → 读体 → 变更 → 提交。
    catch (error) { badRequest(error); }
    return;
  }
  const removeRoleRoute = req.url?.match(/^\/api\/users\/([^/]+)\/roles\/([^/]+)$/);
  // 匹配"取消用户的角色"：路径里有用户名与角色名两段。
  if (req.method === 'DELETE' && removeRoleRoute) {
  // 取消角色。
    try { requireGrant(); access = removeRole(access, decodeURIComponent(removeRoleRoute[1]), decodeURIComponent(removeRoleRoute[2])); commitAccess(); }
    // 两段路径参数都要解码。
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/roles') {
  // 建角色。
    try { requireGrant(); access = createRole(access, await readJson()); commitAccess(); }
    // 与建用户同构。
    catch (error) { badRequest(error); }
    return;
  }
  const roleDelete = req.url?.match(/^\/api\/roles\/([^/]+)$/);
  // 匹配删除角色。
  if (req.method === 'DELETE' && roleDelete) {
  // 删角色。
    try { requireGrant(); access = dropRole(access, decodeURIComponent(roleDelete[1])); commitAccess(); }
    // 还有引用时 dropRole 会抛 409，由 badRequest 转成对应状态码。
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/grants') {
  // 授权。
    try { requireGrant(); access = grant(access, await readJson()); commitAccess(); }
    // 请求体里说明授给谁、哪个对象、哪些权限。
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/revokes') {
  // 撤权。
    try { requireGrant(); access = revoke(access, await readJson()); commitAccess(); }
    // 与授权对称。
    catch (error) { badRequest(error); }
    return;
  }

  if (req.method === 'GET' && req.url?.startsWith('/api/audit')) {
  // 查询审计日志：需要 AUDIT 权限。
    if (!can(access, requestUser, 'AUDIT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 没权限就拒绝——审计日志里含用户行为，属于敏感数据。
    const query = new URL(req.url, 'http://127.0.0.1');
    // 借 URL 解析查询串（这里的基址只是为了满足构造要求）。
    const requested = Number(query.searchParams.get('limit') ?? 200);
    // 取 limit 参数，默认 200。
    const limit = Number.isInteger(requested) ? Math.max(1, Math.min(1000, requested)) : 200;
    // 夹在 1..1000 之间：既防止一次拉太多，也避免 0 或负数导致语义奇怪。
    const filters = {
    // 组装过滤条件。
      user: query.searchParams.get('user') ?? undefined,
      // 按用户。
      sessionId: query.searchParams.get('sessionId') ?? undefined,
      // 按会话。
      object: query.searchParams.get('object') ?? undefined,
      // 按对象。
      from: query.searchParams.get('from') ?? undefined,
      // 起始时间。
      to: query.searchParams.get('to') ?? undefined,
      // 结束时间。
    };
    // 条件组装结束。
    const entries = readAudit(limit, filters);
    // 读日志。
    send(200, { success: true, entries, count: entries.length, limit });
    // 返回记录、条数与实际生效的 limit（便于调用方确认）。
    return;
  }
  if (req.method === 'GET' && req.url === '/api/health') {
  // 健康检查：不需要鉴权（已经通过 canConnect），用于探活。
    send(200, { status: quarantined ? 'degraded' : 'ok', engine: 'minisql-cpp', execution: true, persistence: true, quarantined });
    // 隔离状态下回报 degraded 而不是 ok，让监控能区分"活着但从上次故障中恢复不过来"。
    return;
  }
  if (req.method === 'GET' && req.url === '/api/capabilities') {
  // 能力声明：前端据此决定哪些界面元素该显示、哪些功能该禁用。
    send(200, { engine: 'minisql-cpp', execution: true, persistence: true, serializedRequests: true,
      // 引擎标识、可执行、可持久化、请求串行化。
      quarantined, transactions: true, sessionTransactions: true, multiSession: true, sessionRegistry: true,
      // 当前隔离状态、事务支持、会话级事务、多会话与会话注册表。
      maxSessions, sessionIdleMs, engineRequestTimeoutMs, concurrencyModel: 'serialized-two-phase-database-lock',
      // 三项上限值与并发模型名称（"串行的两阶段数据库锁"）。
      transactionLock: 'exclusive-database', transactionLockTimeoutMs,
      // 事务锁粒度是整库独占，以及等待超时。
      scriptTransactions: true, statementAtomicity: true, cancellation: true, cancellationMode: 'cancel-file', cancellationScope: 'request-and-session', cancelledErrorCode: 5002, autoCheckpoint: true,
      // 脚本级事务、语句原子性、取消机制（用文件传递）、取消范围与错误码，以及自动检查点。
      autoCheckpointWrites: Number(process.env.MINISQL_AUTO_CHECKPOINT_WRITES ?? 0), autoCheckpointWalBytes: Number(process.env.MINISQL_AUTO_CHECKPOINT_WAL_BYTES ?? 0),
      // 触发自动检查点的写语句数与 WAL 字节阈值。
      autoCheckpointDirtyPages: Number(process.env.MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES ?? 0), autoCheckpointDirtyRatio: Number(process.env.MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO ?? 0),
      // 脏页数与脏页比例阈值。
      autoCheckpointIntervalMs: Number(process.env.MINISQL_AUTO_CHECKPOINT_INTERVAL_MS ?? 0), autoCheckpointWalBasis: 'committed-journal-bytes',
      // 时间间隔阈值，以及"WAL 以已提交日志字节数计"这一口径说明。
      autoCheckpointEvaluation: 'after-successful-commit', streamingResults: true, streamingFormat: 'ndjson', streamingReadOnly: true,
      // 自动检查点的评估时机（成功提交之后），以及流式结果的格式与"只读"约束。
      maxResultRows: Number(process.env.MINISQL_MAX_RESULT_ROWS ?? 100000),
      queryMemoryBytes: Number(process.env.MINISQL_QUERY_MEMORY_BYTES ?? 64 * 1024 * 1024),
      tempDiskBytes: Number(process.env.MINISQL_TEMP_DISK_BYTES ?? 1024 * 1024 * 1024),
      externalSort: true, sortSpill: true, sortSpillEncoding: 'jsonl', sortArtifactIdentity: 'session-query-sort', sortChecksum: 'fnv1a64',
      externalAggregate: true, aggregateSpill: true, aggregateSpillEncoding: 'jsonl',
      distinctSpill: true, joinSpill: true, queryResourceManager: true,
      integerEncoding: 'safe-number-or-decimal-string',
      // 大整数编码约定：安全范围内用 JSON number，超出则用十进制字符串。
      decimalExpressions: true, decimalColumns: true, decimalEncoding: 'fixed-scale-string',
      // 定点数表达式与列均支持，编码为"定标度字符串"（避免浮点误差）。
      floatColumns: true, floatEncoding: 'json-number-finite-only',
      // 浮点列支持，编码只允许有限值（NaN/Infinity 不能进 JSON）。
      boolColumns: true, booleanEncoding: 'json-boolean',
      // 布尔列与编码。
      dateColumns: true, dateEncoding: 'iso-date-string',
      // 日期列，编码为 ISO 日期字符串。
      boundedVarchar: true, varcharLengthUnit: 'unicode-code-point',
      // 变长字符串有长度上限，且长度按 Unicode 码点计数（不是字节数）。
      castTargets: ['int', 'bigint', 'float', 'varchar', 'varchar(n)', 'decimal(p,s)', 'bool', 'date'],
      // CAST 支持的八种目标类型。
      audit: true, permissions: true, backupRestore: true, backupManifestVersion: 2, backupManifestVersions: [2, 3],
      // 审计、权限、备份恢复能力；当前清单版本 2，可读版本列表是 2 和 3。
      backupIncremental: true, backupChain: true, backupMigration: 'v1-to-v2',
      // 增量备份、备份链，以及清单迁移路径 v1→v2。
      permissionsModel: 'catalog-access', accessCatalogVersion: 1, accessCatalogStore: 'paged-access-catalog', engineAccessCatalogStore: 'persistent-catalog-system-table', accessCatalogPermissionVersion: permissionVersion,
      // 权限模型名、权限目录的结构版本、两种存储位置（bridge 侧页式文件 / 引擎侧系统表），以及当前权限版本。
      engineAuthorization: true, directBinaryAuth: true, accessCatalogHotReload: true,
      // 引擎侧鉴权、直接运行二进制时的认证、权限目录热加载。
      objectPermissions: true, roleInheritance: true,
      // 按对象的授权与角色继承。
      atomicPermissionEndpoints: true, permissionEndpoints: ['POST /users', 'DELETE /users/:name', 'POST /users/:name/password', 'POST /users/:name/roles', 'DELETE /users/:name/roles/:role', 'POST /roles', 'DELETE /roles/:name', 'POST /grants', 'POST /revokes'],
      // 权限相关的九个原子端点（每个要么整体成功要么整体失败）。
      passwordHashing: 'sha256-salted', auditFiltering: true, sessionIdentity: true,
      // 口令方案、审计过滤、会话身份绑定。
      indexPageStorage: true,
      // 索引页级存储。
      indexVerify: true, indexRebuild: true, indexConsistencyCheck: true,
      uniqueIndexBuildPhases: ['build', 'validate', 'publish'], indexIncrementalMaintenance: true,
      capabilities: ['backupRestore', 'backupIncremental', 'backupChain', 'backupMigration', 'permissions', 'audit', 'create', 'insert', 'multiRowInsert', 'select', 'delete', 'update', 'arithmetic', 'projection', 'tableAlias', 'innerJoin', 'leftJoin', 'null', 'notNull', 'bigint', 'float', 'default', 'primaryKey', 'unique', 'compositeKey', 'distinct', 'orderBy', 'limit', 'groupBy', 'having', 'count', 'sum', 'min', 'max', 'avg', 'compile', 'diagnostics', 'inSubquery', 'existsSubquery', 'scalarSubquery', 'correlatedSubquery', 'astRoundTrip', 'planRoundTrip', 'hashJoin', 'predicatePushdown', 'pruneColumns', 'statistics', 'createIndex', 'indexScan', 'uniqueIndex', 'indexPersistence', 'indexSnapshots', 'indexPageStorage', 'checkpoint', 'nodeStatistics', 'optimizer', 'storageStats', 'externalSort', 'sortSpill', 'externalAggregate', 'aggregateSpill', 'distinctSpill', 'joinSpill', 'queryResourceManager', 'cancellation', 'streamingResults', 'autoCheckpoint', 'multiSession', 'sessionRegistry', 'health'] });
    return;
  }
  // /api/capabilities 分支结束。
  // 第十九章 REQ-UI-010 列出的是 GET /api/storage/stats；/api/storage 为既有兼容路径。
  if (req.method === 'GET' && (req.url === '/api/storage' || req.url === '/api/storage/stats')) {
    try {
    // 文件可能不存在。
      const bytes = statSync(database).size;
      // 取文件字节数。
      // 未接通的能力明确标记不可用，不伪造零值；缓存明细走 /api/sessions/:id/buffer。
      send(200, { pageSize: 4096, fileBytes: bytes, allocatedPages: Math.ceil(bytes / 4096),
        buffer: { available: false, reason: 'backend-not-exposed', endpoint: '/api/sessions/:id/buffer' },
        policy: 'backend-not-exposed' });
    } catch (error) { send(503, { error: { message: error instanceof Error ? error.message : String(error) } }); }
    // 读不到文件时回 503（服务暂时无法提供该信息）。
    return;
  }
  // /api/storage 分支结束。
  if (req.method === 'POST' && req.url === '/api/sessions') {
  // 开一个新会话。
    req.resume();
    // 开会话不需要请求体，但必须把流读掉，否则连接不会正常结束。
    try {
    // 开会话可能因为权限或数量上限失败。
      if (!can(access, requestUser, 'CONNECT')) throw httpError(403, 'Permission denied');
      // 检查连接权限。
      if (sessions.size >= maxSessions) throw httpError(409, `Session limit reached (${maxSessions})`);
      // 会话数达上限就回 409，并在信息里说明上限是多少。
      await ensureEngine();
      // 确保引擎就绪（这里可能就是第一次启动它）。
      const id = randomUUID();
      // 生成会话编号。
      const cancelFile = sessionCancelFile(id);
      // 推出该会话的取消令牌文件路径。
      clearCancelFile(cancelFile);
      // 清掉可能残留的旧令牌。
      const session = {
      // 组装会话对象。
        id, cancelFile, closing: false, timer: undefined, epoch: undefined,
        // 编号、令牌文件、关闭标记、空闲定时器与其轮次标记。
        transactionState: 'IDLE', activeRequest: false, waiting: false, cancelRequested: false,
        // 初始事务状态为空闲；没有在跑的请求、没有等待、没有取消请求。
        lastActiveAt: new Date().toISOString(), user: requestUser, password: requestPassword ?? '',
        // 最近活跃时间以及该会话绑定的身份（后续请求都用这份身份，避免中途换人）。
      };
      // 会话对象组装结束。
      sessions.set(id, session);
      // 登记进会话表。
      touchSession(session);
      // 启动空闲回收定时器。
      auditSessionId = id;
      // 让本次请求的审计记录带上会话号。
      if (res.destroyed) { await closeSession(session); return; }
      // 客户端在开会话的过程中就断开了，那就别留下孤儿会话，直接关掉。
      send(201, { success: true, sessionId: id, transactionState: 'IDLE', idleTimeoutMs: sessionIdleMs, maxSessions });
      // 回 201（资源已创建），并带上会话号、超时与上限，方便前端提示。
    } catch (error) { send(error.status ?? 503, { success: false, error: { message: error.message } }); }
    // 失败时用错误自带的状态码；没有就按 503（服务暂时不可用）。
    return;
  }
  // POST /api/sessions 分支结束。
  if (req.method === 'GET' && req.url === '/api/sessions') {
  // 列出当前会话：需要 READ 权限。
    if (!can(access, requestUser, 'READ')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 没权限就拒绝。
    const entries = [...sessions.values()].map(session => ({
    // 把会话映射精简成"可对外展示"的字段（不暴露口令等内部状态）。
      sessionId: session.id,
      // 会话编号。
      user: session.user,
      // 该会话绑定的身份。
      transactionState: session.transactionState,
      // 事务状态。
      activeRequest: session.activeRequest,
      // 是否有请求在跑。
      waitingForLock: session.waiting,
      // 是否在等锁。
      ownsTransactionLock: transactionOwner === session.id,
      // 是否持有整库事务锁。
      lastActiveAt: session.lastActiveAt,
      // 最近活跃时间。
    }));
    // 映射结束。
    send(200, { success: true, entries, count: entries.length, maxSessions, transactionOwner, transactionLockTimeoutMs });
    // 返回列表、条数、上限，以及"当前谁持有事务锁"与锁等待超时，便于诊断锁竞争。
    return;
  }
  if (req.method === 'GET' && req.url === '/api/backups') {
  // 列出备份：扫描备份目录里的页文件与增量文件。
    try {
    // 目录读取与清单解析都可能失败。
      const entries = readdirSync(backupDirectory).filter(name => name.endsWith('.pages') || name.endsWith('.delta')).map(name => {
      // 只看这两种后缀的文件（清单是 .json，不单独列出）。
        const file = resolve(backupDirectory, name);
        // 备份文件路径。
        const manifest = resolve(backupDirectory, name + '.json');
        // 对应的清单路径。
        const metadata = existsSync(manifest) ? JSON.parse(readFileSync(manifest, 'utf8')) : {};
        // 读清单；缺失时用空对象，这样仍然能列出文件（至少让用户看到有个孤儿文件）。
        return {
        // 组装一条备份信息。
          name,
          // 文件名。
          kind: metadata.kind ?? 'full',
          // 类型：没有 kind 字段的老备份按整份处理。
          base: metadata.base,
          // 增量备份的基线名（整份备份为 undefined）。
          bytes: statSync(file).size,
          // 文件大小。
          createdAt: metadata.createdAt,
          // 创建时间。
          manifestVersion: metadata.version,
          // 清单版本。
          pageFormatVersion: metadata.pageFormatVersion,
          // 页格式版本。
          walBytes: metadata.walBytes,
          // 附带的 WAL 字节数。
          snapshotLsn: metadata.committedSequence,
          // 快照对应的已提交序号。
          chainDepth: metadata.kind === 'incremental' ? (metadata.chainDepth ?? safeChainDepth(name)) : 1,
          // 增量备份显示链深（清单里没有就现算，算不出来按 1）；整份备份固定 1。
          pageChecksum: metadata.pageChecksum,
          // 页校验值。
          migrationState: metadata.version === 1 ? 'pending' : metadata.version >= 2 && metadata.version <= 4 ? 'ready' : 'unknown',
          // 迁移状态：清单版本 1 表示"待迁移"，2..4 表示可直接用，其它为未知。
        };
      });
      // 映射结束。
      send(200, { success: true, entries });
      // 返回列表。
    } catch (error) { send(503, { success: false, error: { message: error.message } }); }
    // 失败回 503。
    return;
  }
  if (req.method === 'POST' && req.url === '/api/backup/validate') {
  // 校验备份（不还原）：把备份完整读一遍并检查所有校验值。
    try {
    // 读体与校验都可能失败。
      const chunks = [];
      // 请求体分片。
      for await (const chunk of req) chunks.push(chunk);
      // 读完。
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      // 解析出备份名。
      if (sessions.size) throw httpError(409, 'Database reserved by active sessions');
      // 还有会话在用数据库时拒绝：校验要独占访问，避免与正在跑的查询互相干扰。
      const result = await enqueue(async () => {
      // 进全局队列后执行。
        const artifact = backupArtifactFile(body?.name);
        // 定位备份工件。
        if (!artifact) throw httpError(404, 'Backup not found');
        // 找不到就 404。
        const before = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
        // 先记下校验前的清单（下面要判断是否发生了迁移）。
        const reconstructed = await reconstructBackup(body.name);
        // 完整还原一遍（这一步会跑完所有校验）。
        return {
        // 返回校验结果。
          name: artifact.name,
          // 备份名。
          kind: artifact.kind,
          migrated: before.version === 1 && reconstructed.manifest.version === 2,
          // 是否在校验过程中发生了清单迁移（v1 → v2）。
          manifestVersion: reconstructed.manifest.version,
          // 迁移后的清单版本。
          pageFormatVersion: reconstructed.manifest.pageFormatVersion,
          // 页格式版本。
          pages: reconstructed.pages,
          // 还原出来的页数——这个值对得上，就说明整条备份链是自洽的。
        };
      });
      send(200, { success: true, operation: 'backup-migration', phase: 'migration', validation: result });
      // 返回校验结果；operation/phase 字段让前端能确认这是"迁移校验"而不是普通备份。
    } catch (error) {
    // 校验失败。
      send(error.status ?? 422, { success: false, operation: 'backup-migration', phase: 'migration',
        // 用错误自带的 status，默认 422（内容不合法）。
        error: { code: 'BACKUP_MIGRATION_FAILED', message: error instanceof Error ? error.message : String(error) } });
        // 错误码固定为 BACKUP_MIGRATION_FAILED，便于前端精确提示。
    }
    return;
  }
  if ((req.method === 'POST' && req.url === '/api/backup') || (req.method === 'POST' && req.url === '/api/restore')) {
  // 备份与还原共用一个分支：它们要读的请求体与独占性检查是一样的。
    try {
    // 读体与执行都可能失败。
      const chunks = [];
      // 分片缓冲。
      for await (const chunk of req) chunks.push(chunk);
      // 读完请求体。
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      // 解析 JSON。
      const online = req.url === '/api/backup' && (body.mode === 'online' || body.kind === 'online');
      // 判断是不是"在线备份"：只有备份接口支持，且要显式声明 online。
      if (sessions.size && !online) throw httpError(409, 'Database reserved by active sessions');
      // 非在线模式必须独占数据库；在线模式允许有会话在跑。
      if (req.url === '/api/backup') {
      // 备份分支。
        const requestedName = typeof body?.name === 'string' && body.name.trim() ? body.name : `backup-${Date.now()}`;
        // 备份名用调用方给的，没给就用时间戳生成一个。
        if (online) {
        // 在线备份：靠数据库进程自己做快照，不需要停掉会话。
          if (!can(access, requestUser, 'CHECKPOINT')) throw httpError(403, 'Permission denied');
          // 在线备份等价于做检查点，需要 CHECKPOINT 权限。
          const { name, file } = backupFile(requestedName);
          // 解析出规范备份名与目标文件路径（含目录穿越检查）。
          const cancelFile = sessionCancelFile(`backup-${auditId}`);
          // 给这次备份造一个独立的取消令牌文件。
          clearCancelFile(cancelFile);
          // 先清掉可能的残留。
          const ephemeral = {
          // 造一个临时会话对象：复用会话通道发快照请求，但它不登记进 sessions 表。
            id: `backup-${auditId}`, cancelFile, closing: false, timer: undefined, epoch: undefined,
            // 编号借审计编号，保证唯一；不设空闲定时器。
            transactionState: 'IDLE', activeRequest: false, waiting: false, cancelRequested: false,
            // 初始状态与普通会话一致。
            user: requestUser, password: requestPassword ?? '',
            // 用请求方的身份（引擎侧仍会做一次鉴权）。
          };
          // 临时会话组装结束。
          auditSql = `SNAPSHOT ${name}`;
          // 审计里记下这是一次快照操作（不含 SQL 原文）。
          const result = await runSnapshotOperation(ephemeral, file);
          // 执行快照。
          clearCancelFile(cancelFile);
          // 清掉令牌文件。
          await closeEngineIfIdle({ user: requestUser, password: requestPassword ?? '' });
          // 如果已经没有别的会话，顺手关掉引擎。
          if (result.success === false) {
          // 快照失败。
            send(quarantined ? 503 : 422, result);
            // 隔离状态下回 503（服务不可用），否则回 422（请求本身有问题）。
            return;
            // 结束。
          }
          // 失败分支结束。
          const metadata = {
          // 组装快照清单。
            version: 4, kind: 'snapshot', name, createdAt: new Date().toISOString(),
            // 清单版本 4 表示快照格式；记录类型、名字与创建时间。
            bytes: statSync(file).size, sha256: sha256(file), pageFormatVersion: pageFormatVersion(file),
            // 文件大小、整体摘要与页格式版本。
            walBytes: Number(result.walBytes ?? 0), walCutoffBytes: Number(result.walCutoffBytes ?? 0),
            // 快照对应的 WAL 字节数与截止位置（恢复时要从这里续）。
            committedSequence: Number(result.committedSequence ?? 0), catalogVersion: Number(result.catalogVersion ?? 0),
            // 已提交序号与当时的目录版本。
            indexVersion: Number(result.indexVersion ?? 0),
            // 索引版本。
            pageChecksum: sha256(file),
            // 页校验值（与整体摘要同值，兼容老读端字段名）。
          };
          // 清单组装结束。
          writeFileSync(file + '.json', JSON.stringify(metadata), 'utf8');
          // 把清单写到备份文件旁边。
          send(200, { success: true, backup: name, kind: 'snapshot', bytes: metadata.bytes,
            // 返回结果，
            sha256: metadata.sha256, walBytes: metadata.walBytes, committedSequence: metadata.committedSequence });
            // 附上摘要、WAL 字节数与提交序号，调用方可以据此核对。
          return;
        }
        // 在线备份分支结束。
        const incremental = body.kind === 'incremental' || Boolean(body.base);
        // 判断是不是增量备份：显式写了 kind，或者给了基线名。
        if (incremental) {
        // 增量备份分支。
          if (!backupArtifactFile(body.base)) throw httpError(404, 'Base backup not found');
          // 基线必须真实存在，否则算不出差异。
          const target = deltaBackupFile(requestedName);
          // 算出增量文件的目标路径。
          await enqueue(async () => {
          // 进全局队列，独占数据库。
            await callDatabase('execute', 'CHECKPOINT;');
            // 先做一次检查点：把内存里的脏页全部落盘，
            // 否则"当前文件"和"备份里的文件"比较出来的差异会缺少尚未落盘的部分。
            const current = readFileSync(database);
            // 读入当前数据库文件。
            const baseReconstructed = await reconstructBackup(body.base);
            // 把基线完整还原成内存缓冲（可能是一条增量链）。
            const baseBuffer = baseReconstructed.buffer;
            // 取出基线的页内容。
            const ids = changedPageIds(baseBuffer, current);
            // 算出两边的差异页号。
            const fileBuffer = Buffer.alloc(64 + ids.length * (8 + backupPageSize));
            // 按"头 64 字节 + 每页 8 字节页号 + 一整页内容"预分配增量文件。
            deltaHeader(Math.floor(baseBuffer.length / backupPageSize), Math.floor(current.length / backupPageSize), ids.length).copy(fileBuffer);
            // 写文件头：基线页数、最终页数、记录条数。
            let offset = 64;
            // 记录区从头部之后开始。
            for (const id of ids) {
            // 逐页写入差异。
              fileBuffer.writeBigUInt64LE(BigInt(id), offset);
              // 写页号（64 位，为了和读端一致）。
              offset += 8;
              // 前移。
              current.copy(fileBuffer, offset, id * backupPageSize, (id + 1) * backupPageSize);
              // 把当前文件里的这一整页拷进增量。
              offset += backupPageSize;
              // 前移一整页。
            }
            // 差异写完了。
            writeFileSync(target.file, fileBuffer);
            // 落盘增量文件。
            writeFileSync(target.manifest, JSON.stringify({
            // 写增量清单。
              version: 3,
              // 清单版本 3 = 增量格式。
              kind: 'incremental',
              name: target.name,
              // 文件名。
              base: backupArtifactFile(body.base).name,
              // 基线名（存规范化后的名字，而不是用户原样的输入）。
              createdAt: new Date().toISOString(),
              // 创建时间。
              bytes: statSync(target.file).size,
              // 大小。
              sha256: sha256(target.file),
              // 整体摘要，读取时会校验。
              pageFormatVersion: baseReconstructed.manifest.pageFormatVersion,
              // 页格式版本沿用基线——增量必须和基线的页布局一致。
              walBytes: 0,
              // 增量本身不带 WAL。
              chainDepth: safeChainDepth(body.base) + 1,
              // 链深 = 基线链深 + 1（提前算好，展示时不必再递归）。
            }), 'utf8');
            // 清单写入结束。
          });
          send(200, { success: true, backup: target.name, kind: 'incremental', base: backupArtifactFile(body.base).name, bytes: statSync(target.file).size, sha256: sha256(target.file) });
          // 返回增量备份信息（含基线名、大小与摘要）。
        } else {
        // 整份备份分支（非在线）。
          const { name, file } = backupFile(requestedName);
          // 解析目标路径。
          await enqueue(async () => {
          // 独占数据库。
            await callDatabase('execute', 'CHECKPOINT;');
            // 同样先做检查点，保证备份里的数据是完整的。
            copyFileSync(database, file);
            // 直接把数据库文件复制成备份。
          writeFileSync(file + '.json', JSON.stringify({ version: 2, name, createdAt: new Date().toISOString(), bytes: statSync(file).size, sha256: sha256(file), pageFormatVersion: pageFormatVersion(file), walBytes: 0, pageChecksum: sha256(file) }), 'utf8');
          // 写清单：版本 2（整份格式），记录名字、时间、大小、摘要、页格式版本。
          // 注意 walBytes 写 0：这是停机备份，不携带 WAL。
          });
          send(200, { success: true, backup: name, bytes: statSync(file).size, sha256: sha256(file) });
        }
        // 备份分支结束。
      } else {
      // 还原分支（/api/restore）。
        let rollbackUsed;
        // 记录回退目录，便于失败时恢复。
        await enqueue(async () => {
        // 独占数据库。
          try {
          // 还原过程出错要回退到操作前的状态。
            const temporaryPages = database + '.restore.tmp';
            // 先把备份还原到一个临时文件，而不是直接覆盖正式数据库。
            if (existsSync(temporaryPages)) unlinkSync(temporaryPages);
            // 清掉上次可能残留的临时文件。
            await materializeBackup(body.name, temporaryPages);
            // 还原到临时文件。
            const rollbackPath = createRestoreRollback();
            // 在动正式文件之前，先给当前数据库留一份回退快照。
            rollbackUsed = rollbackPath;
            // 记下回退目录。
            renameSync(temporaryPages, database);
            // 原子替换正式数据库文件——到这一步才是"真正切换"。
            for (const suffix of ['.wal', '.ckpt']) {
            // 两个附加文件要跟着一起切换。
              const source = temporaryPages + suffix;
              // 还原过程产生的附加文件（materializeBackup 会写到目标旁边）。
              if (existsSync(source)) renameSync(source, database + suffix);
              // 有就替换过去。
              else if (existsSync(database + suffix)) unlinkSync(database + suffix);
              // 没有就删掉当前的，避免旧日志/检查点与新数据不匹配。
            }
            // 附加文件处理结束。
            await callDatabase('catalog');
            // 用一次目录查询验证还原后的数据库确实能打开、结构可读——
            // 这比"文件复制成功"更能说明还原成功。
          } catch (error) {
          // 还原过程中出错。
            if (rollbackUsed && restoreRollbackDirectory(rollbackUsed)) {
            // 有回退快照并且恢复成功。
              await callDatabase('catalog');
              // 再验证一次回退后的数据库可用。
              throw Object.assign(new Error(`Restore failed and original database was restored: ${error.message}`), { status: 422 });
              // 抛出时明确告诉调用方"还原失败，但原库已恢复"，状态码 422。
              // 这句话很重要：让用户知道数据没丢，只是这次还原没做成。
            }
            // 回退分支结束。
            throw error;
            // 连回退都没做成，只能把原始错误抛出去。
          }
        });
        send(200, { success: true, restored: body.name, bytes: statSync(database).size, rollback: rollbackUsed });
        // 返回还原结果，并把回退目录一并告知（用户可据此自行处理）。
      }
      // 还原分支结束。
    } catch (error) { send(error.status ?? 503, { success: false, error: { message: error.message } }); }
    // 读体或整体流程失败：用错误自带状态码，默认 503。
    return;
  }
  // 备份/还原分支结束。
  const cancelRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/cancel$/);
  // 匹配取消路由。
  if (cancelRoute) {
  // 取消处理。
    if (req.method !== 'POST') { send(405, { success: false, error: { code: 405, message: 'Cancellation requires POST' } }); return; }
    // 取消必须用 POST（它会产生副作用，不能被浏览器预取或缓存）。
    try {
    // 请求体可选。
      const body = await readJson();
      // 读体以取可选的 requestId。
      if (typeof body.requestId === 'string' && body.requestId.length > 0 && body.requestId.length <= 128) requestId = body.requestId;
      // 调用方可以指定要取消哪个请求；长度限制在 128 以内防止异常输入。
    } catch { /* Cancellation remains valid without a JSON body. */ }
    // 注释说明：不带请求体也允许取消（这样命令行 curl 也能直接用）。
    const session = sessions.get(cancelRoute[1]);
    // 找会话。
    if (!session) { send(404, { success: false, error: { code: 404, message: 'Session not found or expired' } }); return; }
    // 会话不存在或已过期。
    if (session.user !== requestUser) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 只允许本人取消自己的会话（否则任何人都能打断别人）。
    try {
    // 写取消文件可能失败。
      if (activeOperation?.sessionId === session.id) {
      // 这个会话正好有操作在跑。
        writeFileSync(activeOperation.cancelFile, 'cancel\n', 'utf8');
        // 写取消令牌：数据库进程会轮询到它并主动停止当前语句。
        send(202, { success: false, cancelled: true, commitState: 'unknown', transactionState: session.transactionState,
          error: { code: 5002, message: 'Cancellation requested' } });
        return;
      }
      // 运行中分支结束。
      if (session.waiting) {
      // 会话在等锁（还没真正开始跑）。
        session.cancelRequested = true;
        // 打标记，等它抢到锁时立刻抛取消（见 acquireTurn 开头）。
        for (const waiters of [transactionWaiters, turnWaiters]) {
        // 两个等待队列都要处理。
          const index = waiters.findIndex(waiter => waiter.session?.id === session.id);
          // 找到它在队列里的位置。
          if (index < 0) continue;
          // 不在这个队列就跳过。
          const [waiter] = waiters.splice(index, 1);
          // 摘出来。
          clearTimeout(waiter.timer);
          // 清掉它的超时定时器。
          waiter.resolve();
          // 立刻唤醒，让它重新走 acquireTurn 从而看到取消标记。
        }
        // 队列处理结束。
        send(202, { success: false, cancelled: true, transactionState: session.transactionState,
          error: { code: 5002, message: 'Cancellation requested' } });
        return;
      }
      // 等待分支结束。
      send(409, { success: false, error: { code: 409, message: 'No query is running' } });
      // 既没在跑也没在等，没什么可取消的。
    } catch (error) { send(503, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    // 写取消文件失败等异常回 503。
    return;
  }
  // 取消路由分支结束。
  const indexRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/index-(inspect|verify|rebuild)$/);
  if (indexRoute) {
    req.resume();
    // 先把请求流读掉再检查方法，避免连接因为未消费的流而挂住。
    const action = indexRoute[2];
    if (req.method !== 'POST') { send(405, { success: false, error: { code: 405, message: `Index ${action} requires POST` } }); return; }
    const session = sessions.get(indexRoute[1]);
    if (!session) { send(404, { success: false, error: { code: 404, message: 'Session not found or expired' } }); return; }
    // 会话不存在。
    if (session.user !== requestUser) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    // 只允许本人操作自己的会话。
    try {
    // 读体与执行都可能失败。
      const chunks = [];
      // 分片缓冲。
      for await (const chunk of req) chunks.push(chunk);
      // 读完请求体。
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      // 严格解码后解析。
      if (typeof body.table !== 'string' || typeof body.index !== 'string') throw httpError(400, 'Expected table and index strings');
      // 表和索引都必须是字符串。
      // 结构检查只读；在线重建改写索引页，按对象写权限（UPDATE）收紧。
      const permission = action === 'rebuild' ? 'UPDATE' : 'READ';
      if (!can(access, requestUser, permission, body.table)) throw httpError(403, 'Permission denied');
      auditSql = `INDEX ${action.toUpperCase()} ${body.table}.${body.index}`;
      auditObjects = [body.table.toLowerCase()];
      // 记下访问的对象（统一小写，便于按对象过滤审计）。
      const operation = action === 'inspect' ? 'indexInspect' : action === 'verify' ? 'indexVerify' : 'indexRebuild';
      const data = await runSessionOperation(session, operation, '', res, { table: body.table, index: body.index });
      send(data.success === false ? (data.error?.code === 7001 ? 403 : quarantined ? 503 : 422) : 200, data);
    } catch (error) { send(error.status ?? 400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    return;
  }
  // 第十七章 REQ-CORE-002：执行已序列化的计划文档；指纹失效由引擎返回 PLAN_STALE_SCHEMA。
  const planRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/execute-plan$/);
  if (planRoute) {
    if (req.method !== 'POST') { send(405, { success: false, error: { code: 405, message: 'Plan execution requires POST' } }); return; }
    const session = sessions.get(planRoute[1]);
    if (!session) { send(404, { success: false, error: { code: 404, message: 'Session not found or expired' } }); return; }
    if (session.user !== requestUser) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    try {
      const chunks = [];
      let length = 0;
      for await (const chunk of req) {
        length += chunk.length;
        if (length > 8 * 1024 * 1024) { send(413, { error: { message: 'Request exceeds 8 MiB' } }); return; }
        chunks.push(chunk);
      }
      let body;
      try { body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks))); }
      catch { send(400, { success: false, error: { code: 400, message: 'Invalid JSON body' } }); return; }
      if (!body || body.plan === undefined || body.plan === null) {
        send(400, { success: false, error: { code: 400, message: 'Expected a serialized plan document' } }); return;
      }
      // 形状先在这里判为请求不合法（400）；引擎侧仍会 fail-closed 兜底。
      const planArray = Array.isArray(body.plan);
      const planWrapped = !planArray && typeof body.plan === 'object' && Array.isArray(body.plan.plans);
      if (!planArray && !planWrapped) {
        send(400, { success: false, error: { code: 400, message: 'Expected a serialized plan document (node array or { plans: [...] })' } }); return;
      }
      auditSql = 'EXECUTE PLAN';
      const tables = collectPlanTables(body.plan);
      if (tables.length) auditObjects = tables;
      const data = await runSessionOperation(session, 'executePlan', '', res, { plan: body.plan });
      send(data.success === false
        ? (data.error?.code === 7001 ? 403 : data.error?.code === 9001 ? 501 : quarantined ? 503 : 422)
        : 200, data);
    } catch (error) { send(error.status ?? 400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    // 异常时用错误自带状态码，默认 400。
    return;
  }
  const sessionRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/(execute|compile|diagnostics|statistics|catalog|close|buffer)(\/stream)?$/);
  // 会话级路由：路径里带会话号与操作名，末尾可选 /stream。
  const streamed = req.url === '/api/execute/stream' || Boolean(sessionRoute?.[3]);
  // 判断是否是流式请求：要么走无会话的 /api/execute/stream，要么路径带 /stream。
  let mode;
  // 最终确定的操作模式。
  if (sessionRoute && (((sessionRoute[2] === 'catalog' || sessionRoute[2] === 'statistics') && req.method === 'GET') || (sessionRoute[2] !== 'catalog' && sessionRoute[2] !== 'statistics' && req.method === 'POST'))) mode = sessionRoute[2];
  // 会话路由的方法约束：catalog/statistics 用 GET，其余用 POST。
  else if (req.method === 'GET' && req.url === '/api/catalog') mode = 'catalog';
  // 无会话的目录查询。
  else if (req.method === 'POST' && req.url === '/api/compile') mode = 'compile';
  // 只编译。
  else if (req.method === 'POST' && req.url === '/api/diagnostics') mode = 'diagnostics';
  // 诊断。
  else if (req.method === 'GET' && req.url === '/api/statistics') mode = 'statistics';
  // 统计。
  else if (req.method === 'POST' && (req.url === '/api/execute' || req.url === '/api/execute/stream')) mode = 'execute';
  // 执行（含流式）。
  else { send(404, { error: { message: 'Not found' } }); return; }
  // 都不匹配就是 404。
  if (streamed && sessionRoute && sessionRoute[2] !== 'execute') { send(404, { error: { message: 'Streaming is available for execute only' } }); return; }
  // 注意：/stream 只对 execute 有意义；其它操作带 /stream 直接拒绝，
  // 而不是悄悄当成普通请求处理（否则调用方会以为自己在拿流式结果）。
  try {
  // 读体与执行都可能失败。
    let sql = '';
    // 本请求的 SQL。
    if (mode !== 'catalog' && mode !== 'statistics' && mode !== 'close') {
    // 这三个模式不需要 SQL，其余都要。
      const chunks = [];
      // 分片缓冲。
      let length = 0;
      // 累计长度。
      for await (const chunk of req) {
      // 逐片读取。
        length += chunk.length;
        // 累加。
        if (length > 8 * 1024 * 1024) { send(413, { error: { message: 'Request exceeds 8 MiB' } }); return; }
        // 超过 8 MiB 直接回 413 并放弃（不再继续读，避免内存被撑）。
        chunks.push(chunk);
        // 收集。
      }
      // 读完了。
      try {
      // 解析与校验。
        const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
        // 严格解码后解析。
        if (typeof body.sql !== 'string') throw new Error();
        // 必须有 sql 字符串字段。
        if (typeof body.requestId === 'string' && body.requestId.length > 0 && body.requestId.length <= 128) requestId = body.requestId;
        // 调用方可以指定请求编号（用于与取消接口对应），限制长度防止异常输入。
        sql = body.sql;
        // 记下 SQL。
        auditSql = sql.slice(0, 4096);
        // 审计里只记前 4096 个字符（防止超长 SQL 把审计撑爆）。
      } catch { send(400, { error: { message: 'Expected UTF-8 JSON with a sql string' } }); return; }
      // 解析失败回 400。
      if (streamed) {
        // 只读兜底也只看绑定结果：一旦出现写动作，就在开始分帧前拒绝。
        let binding;
        try { binding = await bindAccessForSql(sql, sessionRoute, requestUser, requestPassword); }
        catch (error) { send(error.status ?? 400, { success: false, error: { message: error.message } }); return; }
        const readOnly = readOnlyBinding(binding);
        if (!readOnly.allowed) { send(400, { success: false, error: { code: 400, message: readOnly.message } }); return; }
        if (readOnly.objects.length) auditObjects = readOnly.objects;
      }
    }
    // 读体分支结束。
    const started = performance.now();
    // 记下开始时间，用于回报耗时。
    const data = await (async () => {
    // 用一个立即执行的异步函数包住两条执行路径，让下面的公共处理只写一遍。
      if (!sessionRoute) {
      // 分支一：没有会话的无状态请求。
        if (sessions.size) throw httpError(409, 'Database reserved by active sessions');
        // 还有会话在用数据库时拒绝无状态请求——无状态路径会另起一个进程，
        // 而同一个数据库文件不允许被两个进程同时打开。
        return await enqueue(async () => {
        // 进全局队列。
          const cancelFile = resolve(cancelDirectory, `request-${auditId}.cancel`);
          // 这次请求专用的取消令牌文件。
          clearCancelFile(cancelFile);
          // 清掉可能的残留文件。
          const disconnected = () => {
          // 客户端断开时的处理。
            if (!res.writableEnded) {
            // 响应还没写完，说明是真的被中断了。
              try { writeFileSync(cancelFile, 'disconnect\n', 'utf8'); }
              // 写取消令牌让子进程停止。
              catch { /* The child process timeout remains the fallback. */ }
              // 注释说明：写不出去也没关系，子进程自身的超时是第二道保险。
            }
          };
          // disconnected 定义结束。
          res.once('close', disconnected);
          // 监听断开。
          try { return await callDatabase(mode, sql, { MINISQL_CANCEL_FILE: cancelFile }, { user: requestUser, password: requestPassword ?? '' }); }
          finally { res.off('close', disconnected); clearCancelFile(cancelFile); }
          // 收尾：摘掉监听并删掉令牌文件。
        });
      }
      // 无状态分支结束。
      const session = sessions.get(sessionRoute[1]);
      // 分支二：有会话的请求，先找会话。
      if (!session) throw httpError(404, 'Session not found or expired');
      // 会话不存在或已过期。
      if (session.user !== requestUser) throw httpError(403, 'Permission denied');
      // 只允许会话的归属者使用它。
      session.password = requestPassword ?? '';
      // 更新会话里记的口令：调用方可能在会话期间改了口令，这里以最新一次请求为准。
      if (res.destroyed && mode !== 'close') throw httpError(499, 'Request disconnected before execution');
      // 客户端已断开且不是关闭操作，就不必白跑（close 例外：关闭必须执行）。
      if (mode === 'close') return closeSession(session);
      // 关闭会话走专门的流程（含必要的事务回滚）。
      const streamSession = streamed && mode === 'execute';
      // 只有"流式 + 执行"才真正走流式通道。
      return runSessionOperation(session, mode, sql, res, streamSession ? {
      // 流式时带上逐帧回调。
        stream: true,
        // 标记这是流式请求。
        onFrame: async frame => {
        // 每收到一帧就转发给客户端。
          streamOutput ??= beginStream(200);
          // 第一帧到达时才真正开流（避免"还没算就直接开流又立刻报错"）。
          if (frame.type === 'meta') await streamOutput.write({ type: 'meta', success: true, ...frame.meta });
          // 元数据帧。
          else if (frame.type === 'row') await streamOutput.write({ type: 'row', success: true, values: frame.row });
          // 数据行帧。
          else if (frame.type === 'complete') await streamOutput.write({ type: 'complete', success: true,
            // 完成帧。
            rowCount: frame.rows, resourceUsage: frame.resourceUsage, transactionState: session.transactionState });
            // 带上总行数、资源用量与当前事务状态。
          else if (frame.type === 'error') await streamOutput.write({ type: 'error', success: false,
            // 错误帧。
            error: frame.error, transactionState: session.transactionState });
        },
        // 回调结束。
      } : {});
      // 非流式时上下文为空对象。
    })();
    // 执行结束。
    if ((mode === 'catalog' || mode === 'statistics') && data && Array.isArray(data.tables)) {
    // 目录/统计响应要按权限过滤。
      data.tables = data.tables.filter(table => can(access, requestUser, 'SELECT', table.name));
      // 没有 SELECT 权限的表不出现在列表里——连"库里有这张表"都不让看到。
    }
    // 过滤结束。
    const resourceLimited = data.error?.code === 5001 && /budget exceeded/i.test(data.error?.message ?? '');
    // 审计对象来自引擎绑定结果；权限/只读错误按语义映射到 403/400。
    if (Array.isArray(data?.accessObjects) && data.accessObjects.length)
      auditObjects = data.accessObjects.map(object => String(object).toLowerCase());
    const permissionDenied = data.error?.code === 7001;
    const readOnlyViolation = !permissionDenied && /read-?only/i.test(data.error?.message ?? '');
    // 第十七章：未实现的能力不得伪装成成功，统一按 501 返回。
    const notImplemented = !permissionDenied && data.error?.code === 9001;
    const status = data.success === false
      ? (permissionDenied ? 403 : readOnlyViolation ? 400 : notImplemented ? 501 : quarantined ? 503 : resourceLimited ? 413 : 422)
      : 200;
    const response = mode === 'catalog' || mode === 'buffer' || mode === 'diagnostics' || mode === 'statistics' ? data : queryResult(data, performance.now() - started);
    // 这四种模式的结果本身就是最终形态；其余模式要经过 queryResult 整理。
    if (data.success === false && data.completedStatements > 0) {
    // 多语句执行中途失败时，补一段"已经发生了什么"的说明。
      const committed = (data.results ?? []).filter(result => result.commitState === 'committed').length;
      // 统计已经提交的语句数。
      const rolledBack = (data.results ?? []).filter(result => result.commitState === 'rolledBack').length;
      // 统计已回滚的语句数。
      const suffix = [committed ? `此前 ${committed} 条语句已成功，未自动回滚。` : '',
        // 有已提交的语句就说明它们不会自动撤销——这一点必须让用户知道。
        rolledBack ? `事务内 ${rolledBack} 条已执行语句已回滚。` : ''].filter(Boolean).join('');
        // 有回滚的语句也一并说明。
      response.error = { ...data.error, message: `${data.error.message}${suffix ? '；' + suffix : ''}` };
      // 把说明拼进错误信息，用户不必自己去数结果集。
    }
    // 失败说明处理结束。
    if (streamOutput) streamOutput.end();
    // 已经开过流（数据通过 onFrame 发出去了），这里只需要结束响应。
    else if (streamed) await sendStream(status, response); else send(status, response);
    // 没开过流时：流式请求走 sendStream，普通请求走 send。
  } catch (error) {
  // 整个执行过程抛出异常。
    const failure = { success: false, commitState: !error.status && mode === 'execute' ? 'unknown' : undefined,
      // 关键判断：没有 status 说明不是我们主动构造的"业务错误"，
      // 而是执行过程中意外崩了，此时执行类的提交状态必须标成 unknown。
      transactionState: sessionRoute ? sessions.get(sessionRoute[1])?.transactionState : undefined,
      // 尽量回报当前事务状态。
      error: { message: error.message, suggestion: 'Do not automatically retry writes; inspect database state.' } };
      // 明确提示调用方：不要自动重试写操作，先人工看数据库状态。
    if (streamOutput) {
    // 已经开过流。
      await streamOutput.write({ type: 'error', ...failure });
      // 用一条错误帧收尾。
      streamOutput.end();
      // 结束流。
    } else send(error.status ?? 503, failure);
    // 否则按普通响应回错误（默认 503）。
  }
});
// HTTP 处理函数结束。
// 启动预热：尝试加载一次引擎 Catalog。若引擎二进制缺失或损坏，服务器仍进入降级
// 启动时先试着读一次目录：
// 模式（健康检查报 degraded、引擎路由返回 503），使 access/audit/capabilities 等
// 即使失败也只进入降级模式（健康检查报 degraded、引擎相关路由返回 503），
// 不依赖引擎的管理端点保持可用（供管理员恢复）。
// 这样 access/audit/capabilities 这些不依赖引擎的管理端点仍然可用，管理员才有机会修复。
// 启动预热：尝试加载一次引擎 Catalog。若引擎二进制缺失或损坏，服务器仍进入降级
// 模式（健康检查报 degraded、引擎路由返回 503），使 access/audit/capabilities 等
// 不依赖引擎的管理端点保持可用（供管理员恢复）。
try { await callDatabase('catalog'); }
// 试读目录。
catch { quarantined = true; }
// 失败就进入隔离状态（也就是上面说的降级模式）。
server.listen(bridgePort, '127.0.0.1', () => {
// 只监听回环地址，不对外网开放。
  console.log(`MiniSQL database API http://127.0.0.1:${server.address().port}/api`);
  // 打印实际监听地址（端口为 0 时会显示系统分配的真实端口）。
});
// 启动流程结束。

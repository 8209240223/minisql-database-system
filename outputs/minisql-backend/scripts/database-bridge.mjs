import http from 'node:http';
import { spawn } from 'node:child_process';
import { appendFileSync, closeSync, copyFileSync, existsSync, ftruncateSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, renameSync, statSync, unlinkSync, writeFileSync, writeSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash, randomUUID } from 'node:crypto';
import { openSession } from './session-process.mjs';
import { can, canConnect, defaultAccess, normalizeAccess, publicAccess,
  createUser, dropUser, createRole, dropRole, setPassword, addRole, removeRole, grant, revoke } from './access-catalog.mjs';
import { openStore, readHeader, writeStore } from './access-store.mjs';

function cliOption(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : undefined;
}
const bridgePort = Number(cliOption('--port') ?? process.env.PORT ?? 8081);
if (!Number.isInteger(bridgePort) || bridgePort < 0 || bridgePort > 65535) throw new Error('Invalid bridge port');
const releaseExecutable = fileURLToPath(new URL('../build/windows/Release/minisql_database.exe', import.meta.url));
const executable = process.env.MINISQL_DATABASE_EXE ?? (existsSync(releaseExecutable) ? releaseExecutable : fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url)));
const database = resolve(cliOption('--database') ?? process.env.MINISQL_DB ?? fileURLToPath(new URL('../data/workbench.pages', import.meta.url)));
mkdirSync(dirname(database), { recursive: true });
const accessPath = resolve(cliOption('--access-file') ?? process.env.MINISQL_ACCESS_FILE ?? resolve(dirname(database), 'access.catalog.json'));
const openedAccess = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
const accessPagesFile = openedAccess.pagesFile;
let access = openedAccess.catalog;
let permissionVersion = openedAccess.permissionVersion;
let accessCatalogVersion = openedAccess.catalogVersion;
function reloadAccessIfStale() {
  try {
    const header = readHeader(accessPagesFile);
    if (header.permissionVersion === permissionVersion) return;
    const reopened = openStore(accessPath, { defaults: defaultAccess, normalize: normalizeAccess });
    access = reopened.catalog;
    permissionVersion = reopened.permissionVersion;
    accessCatalogVersion = reopened.catalogVersion;
  } catch { /* Corrupt storage is rejected by the operation that needs it. */ }
}
function persistAccess() {
  const nextVersion = permissionVersion + 1;
  if (nextVersion > 0xffffffff) throw new Error('Permission version space exhausted');
  writeStore(accessPagesFile, access, { permissionVersion: nextVersion, catalogVersion: accessCatalogVersion });
  permissionVersion = nextVersion;
}
if (process.env.MINISQL_ADMIN_PASSWORD && !access.users.admin?.hash) {
  access = normalizeAccess({ ...access, users: { ...access.users, admin: { ...access.users.admin, password: process.env.MINISQL_ADMIN_PASSWORD } } }, access);
  persistAccess();
}
const allowedOrigins = new Set((process.env.MINISQL_ORIGINS ?? 'http://127.0.0.1:4173,http://localhost:4173').split(','));
let queue = Promise.resolve();
let queued = 0;
let quarantined = false;
const sessions = new Map();
let engine;
let engineOpening;
let transactionOwner;
let turnOwner;
let activeOperation;
const transactionWaiters = [];
const turnWaiters = [];
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
mkdirSync(backupDirectory, { recursive: true });
const cancelDirectory = resolve(dirname(database), 'cancellation');
mkdirSync(cancelDirectory, { recursive: true });
function sessionCancelFile(id) { return resolve(cancelDirectory, `${id}.cancel`); }
function clearCancelFile(file) { if (file) { try { unlinkSync(file); } catch { /* The token may already be absent. */ } } }
function backupName(raw) {
  const name = String(raw ?? '').replace(/[^a-zA-Z0-9._-]/g, '');
  if (!name || name === '.' || name === '..') throw httpError(400, 'Invalid backup name');
  return name.endsWith('.pages') ? name : name + '.pages';
}
function backupFile(raw) {
  const name = backupName(raw);
  const file = resolve(backupDirectory, name);
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  return { name, file };
}
function sha256(file) { return createHash('sha256').update(readFileSync(file)).digest('hex'); }
function pageFormatVersion(file) {
  const descriptor = openSync(file, 'r');
  try {
    const header = Buffer.alloc(12);
    if (readSync(descriptor, header, 0, header.length, 0) !== header.length || header.readUInt32LE(0) !== 0x4644534d) throw httpError(422, 'Invalid page file header');
    const version = header.readUInt32LE(8);
    if (version !== 1 && version !== 2) throw httpError(422, `Unsupported page format version ${version}`);
    return version;
  } finally { closeSync(descriptor); }
}
const backupPageSize = 4096;
function sanitizeBackupName(raw) {
  let value = String(raw ?? '').replace(/[^a-zA-Z0-9._-]/g, '');
  if (value.endsWith('.pages')) value = value.slice(0, -'.pages'.length);
  if (value.endsWith('.delta')) value = value.slice(0, -'.delta'.length);
  if (!value || value === '.' || value === '..') throw httpError(400, 'Invalid backup name');
  return value;
}
function backupArtifactFile(raw) {
  const name = sanitizeBackupName(raw);
  const full = resolve(backupDirectory, name + '.pages');
  const delta = resolve(backupDirectory, name + '.delta');
  if (existsSync(delta) && existsSync(delta + '.json')) return { kind: 'incremental', name: name + '.delta', file: delta, manifest: delta + '.json' };
  if (existsSync(full) && existsSync(full + '.json')) return { kind: 'full', name: name + '.pages', file: full, manifest: full + '.json' };
  return undefined;
}
function fullBackupFile(raw) {
  const name = sanitizeBackupName(raw);
  const file = resolve(backupDirectory, name + '.pages');
  const manifest = file + '.json';
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  if (!existsSync(file) || !existsSync(manifest)) throw httpError(404, 'Full backup not found');
  return { name: name + '.pages', file, manifest };
}
function deltaBackupFile(raw) {
  const name = sanitizeBackupName(raw);
  const file = resolve(backupDirectory, name + '.delta');
  if (dirname(file) !== backupDirectory) throw httpError(400, 'Backup path escapes backup directory');
  return { name: name + '.delta', file, manifest: file + '.json' };
}
function normalizeFullManifest(metadata, file, manifest) {
  if (metadata.sha256 !== sha256(file)) throw httpError(422, 'Backup checksum mismatch');
  if (metadata.pageChecksum !== undefined && metadata.pageChecksum !== sha256(file)) throw httpError(422, 'Backup page checksum mismatch');
  const detectedPageVersion = pageFormatVersion(file);
  if (metadata.version === 1) {
    const migrated = { ...metadata, version: 2, pageFormatVersion: detectedPageVersion, walBytes: 0 };
    writeFileSync(manifest, JSON.stringify(migrated), 'utf8');
    return migrated;
  }
  if (metadata.version !== 2 && metadata.version !== 4)
    throw httpError(422, 'Backup manifest version or page format mismatch');
  if (metadata.pageFormatVersion !== detectedPageVersion) throw httpError(422, 'Backup page format mismatch');
  if (metadata.version === 2 && metadata.walBytes !== 0) throw httpError(422, 'Backup WAL prefix is not supported');
  if (metadata.version === 4 && (metadata.walCutoffBytes === undefined || metadata.committedSequence === undefined))
    throw httpError(422, 'Snapshot manifest is incomplete');
  return metadata;
}
function deltaHeader(basePages, finalPages, records) {
  const header = Buffer.alloc(64);
  header.write('MISQLDLT', 0, 'ascii');
  header.writeUInt32LE(1, 8);
  header.writeUInt32LE(backupPageSize, 12);
  header.writeBigUInt64LE(BigInt(basePages), 16);
  header.writeBigUInt64LE(BigInt(finalPages), 24);
  header.writeBigUInt64LE(BigInt(records), 32);
  return header;
}
function parseDeltaHeader(buffer) {
  if (buffer.length < 64 || buffer.toString('ascii', 0, 8) !== 'MISQLDLT') throw httpError(422, 'Invalid incremental backup header');
  if (buffer.readUInt32LE(8) !== 1 || buffer.readUInt32LE(12) !== backupPageSize) throw httpError(422, 'Unsupported incremental backup version');
  return {
    basePages: Number(buffer.readBigUInt64LE(16)),
    finalPages: Number(buffer.readBigUInt64LE(24)),
    records: Number(buffer.readBigUInt64LE(32)),
  };
}
function changedPageIds(base, current) {
  const basePages = Math.floor(base.length / backupPageSize);
  const currentPages = Math.floor(current.length / backupPageSize);
  const ids = [];
  for (let id = 0; id < Math.min(basePages, currentPages); ++id) {
    const left = base.subarray(id * backupPageSize, (id + 1) * backupPageSize);
    const right = current.subarray(id * backupPageSize, (id + 1) * backupPageSize);
    if (!left.equals(right)) ids.push(id);
  }
  for (let id = basePages; id < currentPages; ++id) ids.push(id);
  return ids;
}
async function reconstructBackup(name, depth = 0) {
  const artifact = backupArtifactFile(name);
  if (!artifact) throw httpError(404, 'Backup not found');
  if (artifact.kind === 'full') {
    const metadata = normalizeFullManifest(JSON.parse(readFileSync(artifact.manifest, 'utf8')), artifact.file, artifact.manifest);
    const walBuffer = metadata.version === 4 && existsSync(artifact.file + '.wal') ? readFileSync(artifact.file + '.wal') : Buffer.alloc(0);
    const ckptBuffer = metadata.version === 4 && existsSync(artifact.file + '.ckpt') ? readFileSync(artifact.file + '.ckpt') : Buffer.alloc(0);
    return { buffer: readFileSync(artifact.file), pages: Math.floor(statSync(artifact.file).size / backupPageSize),
      manifest: metadata, walBuffer, ckptBuffer };
  }
  if (depth > 8) throw httpError(422, 'Backup chain too deep');
  const delta = readFileSync(artifact.file);
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  if (metadata.sha256 !== sha256(artifact.file) || metadata.version !== 3 || metadata.kind !== 'incremental')
    throw httpError(422, 'Incremental backup checksum or version mismatch');
  const header = parseDeltaHeader(delta);
  if (metadata.pageFormatVersion === undefined || metadata.base === undefined) throw httpError(422, 'Incremental manifest incomplete');
  const base = await reconstructBackup(metadata.base, depth + 1);
  const buffer = Buffer.alloc(header.finalPages * backupPageSize);
  const copied = Math.min(base.pages, header.finalPages);
  base.buffer.copy(buffer, 0, 0, copied * backupPageSize);
  let offset = 64;
  for (let index = 0; index < header.records; ++index) {
    if (offset + 8 > delta.length) throw httpError(422, 'Incremental backup truncated');
    const id = Number(delta.readBigUInt64LE(offset));
    offset += 8;
    if (id >= header.finalPages || offset + backupPageSize > delta.length) throw httpError(422, 'Incremental backup page out of range');
    delta.copy(buffer, id * backupPageSize, offset, offset + backupPageSize);
    offset += backupPageSize;
  }
  if (offset !== delta.length) throw httpError(422, 'Incremental backup trailing data');
  if (header.basePages !== base.pages) throw httpError(422, 'Incremental base page count mismatch');
  return { buffer, pages: header.finalPages, manifest: metadata,
    walBuffer: base.walBuffer ?? Buffer.alloc(0), ckptBuffer: base.ckptBuffer ?? Buffer.alloc(0) };
}

async function materializeBackup(name, target, depth = 0) {
  const artifact = backupArtifactFile(name);
  if (!artifact) throw httpError(404, 'Backup not found');
  if (artifact.kind === 'full') {
    const metadata = normalizeFullManifest(JSON.parse(readFileSync(artifact.manifest, 'utf8')), artifact.file, artifact.manifest);
    copyFileSync(artifact.file, target);
    for (const suffix of ['.wal', '.ckpt']) {
      const source = artifact.file + suffix;
      const destination = target + suffix;
      if (metadata.version === 4 && existsSync(source)) copyFileSync(source, destination);
      else if (existsSync(destination)) unlinkSync(destination);
    }
    return { pages: Math.floor(statSync(target).size / backupPageSize), manifest: metadata };
  }
  if (depth > 8) throw httpError(422, 'Backup chain too deep');
  const delta = readFileSync(artifact.file);
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  if (metadata.sha256 !== sha256(artifact.file) || metadata.version !== 3 || metadata.kind !== 'incremental')
    throw httpError(422, 'Incremental backup checksum or version mismatch');
  const header = parseDeltaHeader(delta);
  if (metadata.pageFormatVersion === undefined || metadata.base === undefined) throw httpError(422, 'Incremental manifest incomplete');
  const base = await materializeBackup(metadata.base, target, depth + 1);
  if (header.basePages !== base.pages) throw httpError(422, 'Incremental base page count mismatch');
  const descriptor = openSync(target, 'r+');
  try {
    let offset = 64;
    for (let index = 0; index < header.records; ++index) {
      if (offset + 8 > delta.length) throw httpError(422, 'Incremental backup truncated');
      const id = Number(delta.readBigUInt64LE(offset));
      offset += 8;
      if (id >= header.finalPages || offset + backupPageSize > delta.length) throw httpError(422, 'Incremental backup page out of range');
      writeSync(descriptor, delta, offset, backupPageSize, id * backupPageSize);
      offset += backupPageSize;
    }
    if (offset !== delta.length) throw httpError(422, 'Incremental backup trailing data');
    ftruncateSync(descriptor, header.finalPages * backupPageSize);
  } finally { closeSync(descriptor); }
  return { pages: header.finalPages, manifest: metadata };
}

function backupChainDepth(name, seen = new Set()) {
  const artifact = backupArtifactFile(name);
  if (!artifact || artifact.kind === 'full') return 1;
  if (seen.has(artifact.name)) throw httpError(422, 'Backup chain cycle detected');
  seen.add(artifact.name);
  const metadata = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
  if (!metadata.base) return 1;
  return 1 + backupChainDepth(metadata.base, seen);
}

function safeChainDepth(name) {
  try { return backupChainDepth(name); } catch { return 1; }
}
function writeDatabaseAtomically(buffer, walBuffer, ckptBuffer) {
  const temporary = database + '.restore.tmp';
  writeFileSync(temporary, buffer);
  renameSync(temporary, database);
  if (walBuffer && walBuffer.length) {
    const walTemporary = database + '.wal.restore.tmp';
    writeFileSync(walTemporary, walBuffer);
    renameSync(walTemporary, database + '.wal');
  } else if (existsSync(database + '.wal')) unlinkSync(database + '.wal');
  if (ckptBuffer && ckptBuffer.length) {
    const ckptTemporary = database + '.ckpt.restore.tmp';
    writeFileSync(ckptTemporary, ckptBuffer);
    renameSync(ckptTemporary, database + '.ckpt');
  } else if (existsSync(database + '.ckpt')) unlinkSync(database + '.ckpt');
}

function createRestoreRollback() {
  const rollbackDirectory = resolve(backupDirectory, 'rollback');
  mkdirSync(rollbackDirectory, { recursive: true });
  const rollbackPath = resolve(rollbackDirectory, `restore-${Date.now()}`);
  mkdirSync(rollbackPath, { recursive: true });
  copyFileSync(database, resolve(rollbackPath, 'db.pages'));
  for (const suffix of ['.wal', '.ckpt']) {
    if (existsSync(database + suffix)) copyFileSync(database + suffix, resolve(rollbackPath, 'db.pages' + suffix));
  }
  return rollbackPath;
}

function restoreRollbackDirectory(rollbackPath) {
  const pages = resolve(rollbackPath, 'db.pages');
  if (!existsSync(pages)) return false;
  copyFileSync(pages, database);
  for (const suffix of ['.wal', '.ckpt']) {
    const source = resolve(rollbackPath, 'db.pages' + suffix);
    if (existsSync(source)) copyFileSync(source, database + suffix);
    else if (existsSync(database + suffix)) unlinkSync(database + suffix);
  }
  return true;
}
const auditPath = process.env.MINISQL_AUDIT_LOG ?? resolve(dirname(database), 'audit.log');
const auditLimitBytes = 16 * 1024 * 1024;
function appendAudit(entry) {
  try {
    if (existsSync(auditPath) && statSync(auditPath).size > auditLimitBytes) return;
    appendFileSync(auditPath, JSON.stringify(entry) + '\n', 'utf8');
  } catch { /* Audit failures must not change database semantics. */ }
}
function readAudit(limit, filters = {}) {
  try {
    const lines = readFileSync(auditPath, 'utf8').split(/\r?\n/).filter(Boolean);
    const entries = lines.map(line => { try { return JSON.parse(line); } catch { return { malformed: true }; } });
    return entries.filter(entry => {
      if (filters.user && entry.user !== filters.user) return false;
      if (filters.sessionId && entry.sessionId !== filters.sessionId) return false;
      if (filters.object && !String(entry.object ?? '').split(',').includes(filters.object)) return false;
      if (filters.from && new Date(entry.at).getTime() < new Date(filters.from).getTime()) return false;
      if (filters.to && new Date(entry.at).getTime() > new Date(filters.to).getTime()) return false;
      return true;
    }).slice(-limit);
  } catch { return []; }
}
const sessionIdleMs = Number(process.env.MINISQL_SESSION_IDLE_MS ?? 300000);
if (!Number.isInteger(sessionIdleMs) || sessionIdleMs < 100 || sessionIdleMs > 3600000) throw new Error('Invalid session idle timeout');
const maxSessions = Number(process.env.MINISQL_MAX_SESSIONS ?? 16);
const transactionLockTimeoutMs = Number(process.env.MINISQL_TRANSACTION_LOCK_TIMEOUT_MS ?? 30000);
const engineRequestTimeoutMs = Number(process.env.MINISQL_ENGINE_REQUEST_TIMEOUT_MS ?? 30000);
if (!Number.isInteger(maxSessions) || maxSessions < 1 || maxSessions > 128) throw new Error('Invalid maximum session count');
if (!Number.isInteger(transactionLockTimeoutMs) || transactionLockTimeoutMs < 100 || transactionLockTimeoutMs > 3600000) throw new Error('Invalid transaction lock timeout');
if (!Number.isInteger(engineRequestTimeoutMs) || engineRequestTimeoutMs < 1000 || engineRequestTimeoutMs > 3600000) throw new Error('Invalid engine request timeout');
const httpError = (status, message) => Object.assign(new Error(message), { status });
function notifyWaiters(waiters) {
  const current = waiters.splice(0);
  for (const waiter of current) {
    clearTimeout(waiter.timer);
    waiter.resolve();
  }
}
function removeWaiter(waiters, waiter) {
  const index = waiters.indexOf(waiter);
  if (index >= 0) waiters.splice(index, 1);
}
function waitForTurn(waiters, session) {
  return new Promise((resolve, reject) => {
    const waiter = { resolve, timer: undefined, session };
    waiter.timer = setTimeout(() => {
      removeWaiter(waiters, waiter);
      reject(httpError(409, `Session lock wait exceeded ${transactionLockTimeoutMs}ms`));
    }, transactionLockTimeoutMs);
    waiters.push(waiter);
  });
}
async function acquireTurn(session) {
  for (;;) {
    if (session.cancelRequested) {
      session.cancelRequested = false;
      throw Object.assign(new Error('Cancellation requested'), { status: 409, code: 5002 });
    }
    if (quarantined) throw httpError(503, 'Database requires inspection after an uncertain failure');
    if (transactionOwner && transactionOwner !== session.id) {
      session.waiting = true;
      await waitForTurn(transactionWaiters, session);
      session.waiting = false;
    } else if (turnOwner && turnOwner !== session.id) {
      session.waiting = true;
      await waitForTurn(turnWaiters, session);
      session.waiting = false;
    } else if (session.activeRequest) {
      session.waiting = true;
      await waitForTurn(turnWaiters, session);
      session.waiting = false;
    } else {
      session.waiting = false;
      turnOwner = session.id;
      return;
    }
  }
}
function releaseTurn(session) {
  if (turnOwner !== session.id) return;
  turnOwner = undefined;
  notifyWaiters(turnWaiters);
}
function applyTransactionResult(session, result) {
  const state = result?.transactionState ?? session.transactionState;
  session.transactionState = state;
  if (state === 'IDLE') {
    if (transactionOwner === session.id) {
      transactionOwner = undefined;
      notifyWaiters(transactionWaiters);
    }
  } else transactionOwner = session.id;
}
function clearAllSessions() {
  for (const [id, session] of sessions) {
    clearTimeout(session.timer);
    clearCancelFile(session.cancelFile);
    sessions.delete(id);
  }
  transactionOwner = undefined;
  turnOwner = undefined;
  activeOperation = undefined;
  notifyWaiters(transactionWaiters);
  notifyWaiters(turnWaiters);
}
async function startEngine() {
  if (engine) return engine;
  if (engineOpening) return engineOpening;
  const cancelFile = sessionCancelFile('engine');
  clearCancelFile(cancelFile);
  engineOpening = openSession(executable, database, { timeoutMs: engineRequestTimeoutMs, env: { MINISQL_SESSION_ID: 'bridge', MINISQL_CANCEL_FILE: cancelFile } })
    .then(worker => {
      const value = { worker, cancelFile, closing: false };
      engine = value;
      worker.closed.then(() => {
        if (engine !== value) return;
        const wasClosing = value.closing;
        engine = undefined;
        clearCancelFile(cancelFile);
        if (!wasClosing) quarantined = true;
        clearAllSessions();
      });
      return value;
    })
    .catch(error => {
      clearCancelFile(cancelFile);
      throw error;
    })
    .finally(() => { engineOpening = undefined; });
  return engineOpening;
}
async function ensureEngine() {
  if (quarantined) throw httpError(503, 'Database requires inspection after an uncertain failure');
  return startEngine();
}
async function closeEngineIfIdle(context = {}) {
  if (!engine || engine.closing || sessions.size) return;
  const current = engine;
  current.closing = true;
  try { return await current.worker.close(context); }
  finally {
    if (engine === current) engine = undefined;
    clearCancelFile(current.cancelFile);
  }
}
async function closeSession(session) {
  if (session.closing) return { success: true, transactionState: 'IDLE', transactionRolledBack: false };
  session.closing = true;
  clearTimeout(session.timer);
  sessions.delete(session.id);
  try {
    let rolledBack = false;
    if (session.transactionState !== 'IDLE') {
      const result = await runSessionOperation(session, 'execute', 'ROLLBACK;', undefined);
      rolledBack = result.success !== false;
      if (result.success === false) return result;
    }
    return { success: true, transactionState: 'IDLE', transactionRolledBack: rolledBack };
  } finally {
    clearCancelFile(session.cancelFile);
    await closeEngineIfIdle({ user: session.user, password: session.password });
  }
}
function touchSession(session) {
  clearTimeout(session.timer);
  session.lastActiveAt = new Date().toISOString();
  const epoch = Symbol();
  session.epoch = epoch;
  session.timer = setTimeout(() => {
    if (session.epoch !== epoch || !sessions.has(session.id) || session.closing) return;
    // 长请求、锁等待和当前轮次都不能被空闲回收中断；事务持有者则由 closeSession 回滚后回收。
    if (session.activeRequest || session.waiting || turnOwner === session.id) {
      touchSession(session);
      return;
    }
    closeSession(session).catch(() => { quarantined = true; });
  }, sessionIdleMs);
}

function callDatabase(mode, sql = '', extraEnv = {}, authorization = null) {
  return new Promise((resolveResult, reject) => {
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
    let length = 0, stopped = false;
    const stop = () => { stopped = true; child.kill(); };
    const timer = setTimeout(stop, engineRequestTimeoutMs);
    child.stdin.on('error', () => {});
    child.stderr.resume();
    child.stdout.on('data', chunk => {
      length += chunk.length;
      if (length > 32 * 1024 * 1024) stop();
      else output.push(chunk);
    });
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.on('close', code => {
      clearTimeout(timer);
      try {
        if (stopped || (code !== 0 && code !== 1)) throw new Error('Database process stopped unexpectedly');
        const data = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(output)));
        if (code === 1 && data.success !== false) throw new Error('Invalid database exit status');
        if (data.error?.code === 4001 || data.error?.code === 9999) quarantined = true;
        resolveResult(data);
      } catch (error) {
        quarantined = true;
        reject(error);
      }
    });
    child.stdin.end(sql);
  });
}

// 所有读写共用队列；无事务锁时禁止多个进程同时打开同一数据库。
function enqueue(operation, allowQuarantined = false) {
  if (queued >= 64) return Promise.reject(new Error('Request queue full'));
  ++queued;
  const result = queue.then(() => {
    if (quarantined && !allowQuarantined) throw new Error('Database requires inspection after an uncertain failure');
    return operation();
  });
  queue = result.catch(() => {}).finally(() => { --queued; });
  return result;
}

async function runSessionOperation(session, mode, sql, res, context = {}) {
  await acquireTurn(session);
  try {
    const result = await enqueue(async () => {
      if (res?.destroyed) throw httpError(499, 'Request disconnected before execution');
      const currentEngine = await ensureEngine();
      clearCancelFile(session.cancelFile);
      session.activeRequest = true;
      activeOperation = { sessionId: session.id, cancelFile: session.cancelFile };
      const disconnected = () => {
        if (!res?.writableEnded) {
          try { writeFileSync(session.cancelFile, 'disconnect\n', 'utf8'); }
          catch {
            quarantined = true;
            void currentEngine.worker.terminate();
          }
        }
      };
      res?.once('close', disconnected);
      try {
        const { stream: streamRequested, onFrame, ...sessionContext } = context;
        const value = streamRequested
          ? await currentEngine.worker.requestStream('executeStream', sql, {
              sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, ...sessionContext,
            }, onFrame)
          : await currentEngine.worker.request(mode, sql, {
              sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, ...sessionContext,
            });
        if (value.error?.code === 5002) value.cancelled = true;
        if (value.commitState === 'unknown' || value.error?.code === 4001 || value.error?.code === 9999) quarantined = true;
        return value;
      } catch (error) {
        quarantined = true;
        throw error;
      } finally {
        session.activeRequest = false;
        activeOperation = undefined;
        clearCancelFile(session.cancelFile);
        res?.off('close', disconnected);
        if (!session.closing) touchSession(session);
      }
    });
    applyTransactionResult(session, result);
    return result;
  } finally {
    releaseTurn(session);
  }
}

async function runSnapshotOperation(session, target) {
  return enqueue(async () => {
    const currentEngine = await ensureEngine();
    clearCancelFile(session.cancelFile);
    const value = await currentEngine.worker.request('snapshot', '', {
      sessionId: session.id, cancelFile: session.cancelFile, user: session.user, password: session.password, target,
    });
    if (value.error?.code === 5002) value.cancelled = true;
    if (value.commitState === 'unknown' || value.error?.code === 4001 || value.error?.code === 9999) quarantined = true;
    return value;
  });
}

function queryResult(data, durationMs) {
  const results = data.results ?? [];
  const last = results.at(-1);
  return {
    ...data, schemaVersion: 1, engine: 'minisql-cpp', durationMs,
    rows: last?.rows ?? [], columns: last?.columns ?? [],
    columnTypes: last?.columnTypes ?? [],
    affectedRows: results.reduce((sum, result) => sum + (result.commitState === 'rolledBack' ? 0 : result.affectedRows ?? 0), 0),
    statements: data.statements ?? data.completedStatements ?? 0,
    plan: data.plan ?? last?.plan ?? [],
    optimizedPlan: data.optimizedPlan ?? last?.optimizedPlan,
    optimizationRules: data.optimizationRules ?? last?.optimizationRules,
    executionStats: data.executionStats ?? last?.executionStats,
    resourceUsage: last?.resourceUsage ?? data.resourceUsage,
  };
}

const server = http.createServer(async (req, res) => {
  const auditStarted = performance.now();
  const auditId = randomUUID();
  let requestId = randomUUID();
  let auditSql = '';
  let requestUser = req.headers['x-minisql-user'] ?? 'admin';
  const requestPassword = req.headers['x-minisql-password'];
  let auditSessionId = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)/)?.[1] ?? '';
  const origin = req.headers.origin;
  let auditObjects = [];
  let streamOutput = null;
  const send = (status, data) => {
    const payload = data && typeof data === 'object' && !Array.isArray(data)
      ? {
          protocolVersion: 1,
          requestId,
          success: data.success !== false && !data.error,
          stages: data.stages ?? {},
          diagnostics: data.diagnostics ?? [],
          ...data,
        }
      : data;
    if (!res.destroyed && !res.writableEnded) {
      appendAudit({
        id: auditId,
        at: new Date().toISOString(),
        method: req.method,
        path: req.url,
        user: requestUser,
        sessionId: auditSessionId,
        sql: auditSql,
        object: auditObjects.length ? auditObjects.join(',') : undefined,
        status,
        success: payload?.success !== false,
        durationMs: performance.now() - auditStarted,
        affectedRows: payload?.affectedRows ?? 0,
        errorCode: payload?.error?.code,
        transactionState: payload?.transactionState,
        quarantined,
      });
    }
    if (res.destroyed || res.writableEnded) return;
    const headers = { 'Content-Type': 'application/json; charset=utf-8', 'Vary': 'Origin' };
    if (origin && allowedOrigins.has(origin)) headers['Access-Control-Allow-Origin'] = origin;
    headers['Access-Control-Allow-Headers'] = 'Content-Type, X-MiniSQL-User, X-MiniSQL-Password';
    headers['Access-Control-Allow-Methods'] = 'GET, POST, DELETE, OPTIONS';
    res.writeHead(status, headers);
    res.end(JSON.stringify(payload));
  };
  const beginStream = (status = 200) => {
    if (res.destroyed || res.writableEnded) return;
    const headers = { 'Content-Type': 'application/x-ndjson; charset=utf-8', 'Cache-Control': 'no-store', 'Vary': 'Origin' };
    if (origin && allowedOrigins.has(origin)) headers['Access-Control-Allow-Origin'] = origin;
    headers['Access-Control-Allow-Headers'] = 'Content-Type, X-MiniSQL-User, X-MiniSQL-Password';
    headers['Access-Control-Allow-Methods'] = 'GET, POST, DELETE, OPTIONS';
    res.writeHead(status, headers);
    const write = value => new Promise((resolve, reject) => {
      if (res.destroyed || res.writableEnded) { reject(httpError(499, 'Stream client disconnected')); return; }
      const finish = error => {
        res.off('drain', onDrain);
        res.off('error', onError);
        if (error) reject(error); else resolve();
      };
      const onDrain = () => finish();
      const onError = error => finish(error);
      res.once('error', onError);
      if (res.write(JSON.stringify({ protocolVersion: 1, requestId, ...value }) + '\n')) finish();
      else res.once('drain', onDrain);
    });
    return { write, end() { if (!res.destroyed && !res.writableEnded) res.end(); } };
  };
  const sendStream = async (status, data) => {
    const output = beginStream(status);
    if (!output) return;
    const { write } = output;
    const rows = Array.isArray(data.rows) ? data.rows : [];
    if (data.success === false) {
      await write({ type: 'error', success: false, error: data.error, commitState: data.commitState, transactionState: data.transactionState });
    } else {
      await write({ type: 'meta', success: true, schemaVersion: data.schemaVersion, engine: data.engine,
        columns: data.columns, columnTypes: data.columnTypes, statements: data.statements,
        affectedRows: data.affectedRows, durationMs: data.durationMs });
      for (let index = 0; index < rows.length; ++index) await write({ type: 'row', index, values: rows[index] });
      await write({ type: 'complete', success: true, rowCount: rows.length, affectedRows: data.affectedRows,
        statements: data.statements, transactionState: data.transactionState, durationMs: data.durationMs });
    }
    output.end();
  };
  if (origin && !allowedOrigins.has(origin)) { send(403, { error: { message: 'Origin not allowed' } }); return; }
  if (req.method === 'OPTIONS') { send(204, {}); return; }
  reloadAccessIfStale();
  if (!canConnect(access, requestUser, requestPassword ?? null)) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
  if (req.method === 'GET' && req.url === '/api/users') {
    if (!can(access, requestUser, 'GRANT') && !can(access, requestUser, 'READ')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    const publicState = publicAccess(access);
    send(200, { success: true, current: requestUser, users: publicState.users });
    return;
  }
  if (req.method === 'GET' && req.url === '/api/access') {
    if (!can(access, requestUser, 'GRANT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    send(200, { success: true, access: publicAccess(access) });
    return;
  }
  if (req.method === 'PUT' && req.url === '/api/access') {
    if (!can(access, requestUser, 'GRANT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    try {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      access = normalizeAccess(body.access ?? body, access);
      persistAccess();
      send(200, { success: true, access: publicAccess(access) });
    } catch (error) { send(400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    return;
  }

  // X24 原子权限资源接口：每个端点对应一次原子变更，失败不产生部分状态。
  const readJson = async () => {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
  };
  const requireGrant = () => { if (!can(access, requestUser, 'GRANT')) throw httpError(403, 'Permission denied'); };
  const commitAccess = () => { persistAccess(); send(200, { success: true, access: publicAccess(access) }); };
  const badRequest = error => send(error?.status ?? 400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } });

  if (req.method === 'POST' && req.url === '/api/users') {
    try { requireGrant(); access = createUser(access, await readJson()); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  const userDelete = req.url?.match(/^\/api\/users\/([^/]+)$/);
  if (req.method === 'DELETE' && userDelete) {
    try { requireGrant(); access = dropUser(access, decodeURIComponent(userDelete[1])); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  const passwordRoute = req.url?.match(/^\/api\/users\/([^/]+)\/password$/);
  if (req.method === 'POST' && passwordRoute) {
    try { requireGrant(); const body = await readJson(); access = setPassword(access, decodeURIComponent(passwordRoute[1]), body.password); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  const addRoleRoute = req.url?.match(/^\/api\/users\/([^/]+)\/roles$/);
  if (req.method === 'POST' && addRoleRoute) {
    try { requireGrant(); const body = await readJson(); access = addRole(access, decodeURIComponent(addRoleRoute[1]), body.role); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  const removeRoleRoute = req.url?.match(/^\/api\/users\/([^/]+)\/roles\/([^/]+)$/);
  if (req.method === 'DELETE' && removeRoleRoute) {
    try { requireGrant(); access = removeRole(access, decodeURIComponent(removeRoleRoute[1]), decodeURIComponent(removeRoleRoute[2])); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/roles') {
    try { requireGrant(); access = createRole(access, await readJson()); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  const roleDelete = req.url?.match(/^\/api\/roles\/([^/]+)$/);
  if (req.method === 'DELETE' && roleDelete) {
    try { requireGrant(); access = dropRole(access, decodeURIComponent(roleDelete[1])); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/grants') {
    try { requireGrant(); access = grant(access, await readJson()); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/revokes') {
    try { requireGrant(); access = revoke(access, await readJson()); commitAccess(); }
    catch (error) { badRequest(error); }
    return;
  }

  if (req.method === 'GET' && req.url?.startsWith('/api/audit')) {
    if (!can(access, requestUser, 'AUDIT')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    const query = new URL(req.url, 'http://127.0.0.1');
    const requested = Number(query.searchParams.get('limit') ?? 200);
    const limit = Number.isInteger(requested) ? Math.max(1, Math.min(1000, requested)) : 200;
    const filters = {
      user: query.searchParams.get('user') ?? undefined,
      sessionId: query.searchParams.get('sessionId') ?? undefined,
      object: query.searchParams.get('object') ?? undefined,
      from: query.searchParams.get('from') ?? undefined,
      to: query.searchParams.get('to') ?? undefined,
    };
    const entries = readAudit(limit, filters);
    send(200, { success: true, entries, count: entries.length, limit });
    return;
  }
  if (req.method === 'GET' && req.url === '/api/health') {
    send(200, { status: quarantined ? 'degraded' : 'ok', engine: 'minisql-cpp', execution: true, persistence: true, quarantined });
    return;
  }
  if (req.method === 'GET' && req.url === '/api/capabilities') {
    send(200, { engine: 'minisql-cpp', execution: true, persistence: true, serializedRequests: true,
      quarantined, transactions: true, sessionTransactions: true, multiSession: true, sessionRegistry: true,
      maxSessions, sessionIdleMs, engineRequestTimeoutMs, concurrencyModel: 'serialized-two-phase-database-lock',
      transactionLock: 'exclusive-database', transactionLockTimeoutMs,
      scriptTransactions: true, statementAtomicity: true, cancellation: true, cancellationMode: 'cancel-file', cancellationScope: 'request-and-session', cancelledErrorCode: 5002, autoCheckpoint: true,
      autoCheckpointWrites: Number(process.env.MINISQL_AUTO_CHECKPOINT_WRITES ?? 0), autoCheckpointWalBytes: Number(process.env.MINISQL_AUTO_CHECKPOINT_WAL_BYTES ?? 0),
      autoCheckpointDirtyPages: Number(process.env.MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES ?? 0), autoCheckpointDirtyRatio: Number(process.env.MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO ?? 0),
      autoCheckpointIntervalMs: Number(process.env.MINISQL_AUTO_CHECKPOINT_INTERVAL_MS ?? 0), autoCheckpointWalBasis: 'committed-journal-bytes',
      autoCheckpointEvaluation: 'after-successful-commit', streamingResults: true, streamingFormat: 'ndjson', streamingReadOnly: true,
      maxResultRows: Number(process.env.MINISQL_MAX_RESULT_ROWS ?? 0),
      queryMemoryBytes: Number(process.env.MINISQL_QUERY_MEMORY_BYTES ?? 64 * 1024 * 1024),
      tempDiskBytes: Number(process.env.MINISQL_TEMP_DISK_BYTES ?? 1024 * 1024 * 1024),
      externalSort: true, sortSpill: true, sortSpillEncoding: 'jsonl', sortArtifactIdentity: 'session-query-sort', sortChecksum: 'fnv1a64',
      externalAggregate: true, aggregateSpill: true, aggregateSpillEncoding: 'jsonl',
      distinctSpill: true, joinSpill: true, queryResourceManager: true,
      integerEncoding: 'safe-number-or-decimal-string',
      decimalExpressions: true, decimalColumns: true, decimalEncoding: 'fixed-scale-string',
      floatColumns: true, floatEncoding: 'json-number-finite-only',
      boolColumns: true, booleanEncoding: 'json-boolean',
      dateColumns: true, dateEncoding: 'iso-date-string',
      boundedVarchar: true, varcharLengthUnit: 'unicode-code-point',
      castTargets: ['int', 'bigint', 'float', 'varchar', 'varchar(n)', 'decimal(p,s)', 'bool', 'date'],
      audit: true, permissions: true, backupRestore: true, backupManifestVersion: 2, backupManifestVersions: [2, 3],
      backupIncremental: true, backupChain: true, backupMigration: 'v1-to-v2',
      permissionsModel: 'catalog-access', accessCatalogVersion: 1, accessCatalogStore: 'paged-access-catalog', engineAccessCatalogStore: 'persistent-catalog-system-table', accessCatalogPermissionVersion: permissionVersion,
      engineAuthorization: true, directBinaryAuth: true, accessCatalogHotReload: true,
      objectPermissions: true, roleInheritance: true,
      atomicPermissionEndpoints: true, permissionEndpoints: ['POST /users', 'DELETE /users/:name', 'POST /users/:name/password', 'POST /users/:name/roles', 'DELETE /users/:name/roles/:role', 'POST /roles', 'DELETE /roles/:name', 'POST /grants', 'POST /revokes'],
      passwordHashing: 'sha256-salted', auditFiltering: true, sessionIdentity: true,
      indexPageStorage: true,
      indexVerify: true, indexRebuild: true, indexConsistencyCheck: true,
      uniqueIndexBuildPhases: ['build', 'validate', 'publish'], indexIncrementalMaintenance: true,
      capabilities: ['backupRestore', 'backupIncremental', 'backupChain', 'backupMigration', 'permissions', 'audit', 'create', 'insert', 'multiRowInsert', 'select', 'delete', 'update', 'arithmetic', 'projection', 'tableAlias', 'innerJoin', 'leftJoin', 'null', 'notNull', 'bigint', 'float', 'default', 'primaryKey', 'unique', 'compositeKey', 'distinct', 'orderBy', 'limit', 'groupBy', 'having', 'count', 'sum', 'min', 'max', 'avg', 'compile', 'diagnostics', 'inSubquery', 'existsSubquery', 'scalarSubquery', 'correlatedSubquery', 'astRoundTrip', 'planRoundTrip', 'hashJoin', 'predicatePushdown', 'pruneColumns', 'statistics', 'createIndex', 'indexScan', 'uniqueIndex', 'indexPersistence', 'indexSnapshots', 'indexPageStorage', 'checkpoint', 'nodeStatistics', 'optimizer', 'storageStats', 'externalSort', 'sortSpill', 'externalAggregate', 'aggregateSpill', 'distinctSpill', 'joinSpill', 'queryResourceManager', 'cancellation', 'streamingResults', 'autoCheckpoint', 'multiSession', 'sessionRegistry', 'health'] });
    return;
  }
  if (req.method === 'GET' && req.url === '/api/storage') {
    try {
      const bytes = statSync(database).size;
      send(200, { pageSize: 4096, fileBytes: bytes, allocatedPages: Math.ceil(bytes / 4096), buffer: { available: false }, policy: 'backend-not-exposed' });
    } catch (error) { send(503, { error: { message: error instanceof Error ? error.message : String(error) } }); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/sessions') {
    req.resume();
    try {
      if (!can(access, requestUser, 'CONNECT')) throw httpError(403, 'Permission denied');
      if (sessions.size >= maxSessions) throw httpError(409, `Session limit reached (${maxSessions})`);
      await ensureEngine();
      const id = randomUUID();
      const cancelFile = sessionCancelFile(id);
      clearCancelFile(cancelFile);
      const session = {
        id, cancelFile, closing: false, timer: undefined, epoch: undefined,
        transactionState: 'IDLE', activeRequest: false, waiting: false, cancelRequested: false,
        lastActiveAt: new Date().toISOString(), user: requestUser, password: requestPassword ?? '',
      };
      sessions.set(id, session);
      touchSession(session);
      auditSessionId = id;
      if (res.destroyed) { await closeSession(session); return; }
      send(201, { success: true, sessionId: id, transactionState: 'IDLE', idleTimeoutMs: sessionIdleMs, maxSessions });
    } catch (error) { send(error.status ?? 503, { success: false, error: { message: error.message } }); }
    return;
  }
  if (req.method === 'GET' && req.url === '/api/sessions') {
    if (!can(access, requestUser, 'READ')) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    const entries = [...sessions.values()].map(session => ({
      sessionId: session.id,
      user: session.user,
      transactionState: session.transactionState,
      activeRequest: session.activeRequest,
      waitingForLock: session.waiting,
      ownsTransactionLock: transactionOwner === session.id,
      lastActiveAt: session.lastActiveAt,
    }));
    send(200, { success: true, entries, count: entries.length, maxSessions, transactionOwner, transactionLockTimeoutMs });
    return;
  }
  if (req.method === 'GET' && req.url === '/api/backups') {
    try {
      const entries = readdirSync(backupDirectory).filter(name => name.endsWith('.pages') || name.endsWith('.delta')).map(name => {
        const file = resolve(backupDirectory, name);
        const manifest = resolve(backupDirectory, name + '.json');
        const metadata = existsSync(manifest) ? JSON.parse(readFileSync(manifest, 'utf8')) : {};
        return {
          name,
          kind: metadata.kind ?? 'full',
          base: metadata.base,
          bytes: statSync(file).size,
          createdAt: metadata.createdAt,
          manifestVersion: metadata.version,
          pageFormatVersion: metadata.pageFormatVersion,
          walBytes: metadata.walBytes,
          snapshotLsn: metadata.committedSequence,
          chainDepth: metadata.kind === 'incremental' ? (metadata.chainDepth ?? safeChainDepth(name)) : 1,
          pageChecksum: metadata.pageChecksum,
          migrationState: metadata.version === 1 ? 'pending' : metadata.version >= 2 && metadata.version <= 4 ? 'ready' : 'unknown',
        };
      });
      send(200, { success: true, entries });
    } catch (error) { send(503, { success: false, error: { message: error.message } }); }
    return;
  }
  if (req.method === 'POST' && req.url === '/api/backup/validate') {
    try {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      if (sessions.size) throw httpError(409, 'Database reserved by active sessions');
      const result = await enqueue(async () => {
        const artifact = backupArtifactFile(body?.name);
        if (!artifact) throw httpError(404, 'Backup not found');
        const before = JSON.parse(readFileSync(artifact.manifest, 'utf8'));
        const reconstructed = await reconstructBackup(body.name);
        return {
          name: artifact.name,
          kind: artifact.kind,
          migrated: before.version === 1 && reconstructed.manifest.version === 2,
          manifestVersion: reconstructed.manifest.version,
          pageFormatVersion: reconstructed.manifest.pageFormatVersion,
          pages: reconstructed.pages,
        };
      });
      send(200, { success: true, operation: 'backup-migration', phase: 'migration', validation: result });
    } catch (error) {
      send(error.status ?? 422, { success: false, operation: 'backup-migration', phase: 'migration',
        error: { code: 'BACKUP_MIGRATION_FAILED', message: error instanceof Error ? error.message : String(error) } });
    }
    return;
  }
  if ((req.method === 'POST' && req.url === '/api/backup') || (req.method === 'POST' && req.url === '/api/restore')) {
    try {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      const online = req.url === '/api/backup' && (body.mode === 'online' || body.kind === 'online');
      if (sessions.size && !online) throw httpError(409, 'Database reserved by active sessions');
      if (req.url === '/api/backup') {
        const requestedName = typeof body?.name === 'string' && body.name.trim() ? body.name : `backup-${Date.now()}`;
        if (online) {
          if (!can(access, requestUser, 'CHECKPOINT')) throw httpError(403, 'Permission denied');
          const { name, file } = backupFile(requestedName);
          const cancelFile = sessionCancelFile(`backup-${auditId}`);
          clearCancelFile(cancelFile);
          const ephemeral = {
            id: `backup-${auditId}`, cancelFile, closing: false, timer: undefined, epoch: undefined,
            transactionState: 'IDLE', activeRequest: false, waiting: false, cancelRequested: false,
            user: requestUser, password: requestPassword ?? '',
          };
          auditSql = `SNAPSHOT ${name}`;
          const result = await runSnapshotOperation(ephemeral, file);
          clearCancelFile(cancelFile);
          await closeEngineIfIdle({ user: requestUser, password: requestPassword ?? '' });
          if (result.success === false) {
            send(quarantined ? 503 : 422, result);
            return;
          }
          const metadata = {
            version: 4, kind: 'snapshot', name, createdAt: new Date().toISOString(),
            bytes: statSync(file).size, sha256: sha256(file), pageFormatVersion: pageFormatVersion(file),
            walBytes: Number(result.walBytes ?? 0), walCutoffBytes: Number(result.walCutoffBytes ?? 0),
            committedSequence: Number(result.committedSequence ?? 0), catalogVersion: Number(result.catalogVersion ?? 0),
            indexVersion: Number(result.indexVersion ?? 0),
            pageChecksum: sha256(file),
          };
          writeFileSync(file + '.json', JSON.stringify(metadata), 'utf8');
          send(200, { success: true, backup: name, kind: 'snapshot', bytes: metadata.bytes,
            sha256: metadata.sha256, walBytes: metadata.walBytes, committedSequence: metadata.committedSequence });
          return;
        }
        const incremental = body.kind === 'incremental' || Boolean(body.base);
        if (incremental) {
          if (!backupArtifactFile(body.base)) throw httpError(404, 'Base backup not found');
          const target = deltaBackupFile(requestedName);
          await enqueue(async () => {
            await callDatabase('execute', 'CHECKPOINT;');
            const current = readFileSync(database);
            const baseReconstructed = await reconstructBackup(body.base);
            const baseBuffer = baseReconstructed.buffer;
            const ids = changedPageIds(baseBuffer, current);
            const fileBuffer = Buffer.alloc(64 + ids.length * (8 + backupPageSize));
            deltaHeader(Math.floor(baseBuffer.length / backupPageSize), Math.floor(current.length / backupPageSize), ids.length).copy(fileBuffer);
            let offset = 64;
            for (const id of ids) {
              fileBuffer.writeBigUInt64LE(BigInt(id), offset);
              offset += 8;
              current.copy(fileBuffer, offset, id * backupPageSize, (id + 1) * backupPageSize);
              offset += backupPageSize;
            }
            writeFileSync(target.file, fileBuffer);
            writeFileSync(target.manifest, JSON.stringify({
              version: 3,
              kind: 'incremental',
              name: target.name,
              base: backupArtifactFile(body.base).name,
              createdAt: new Date().toISOString(),
              bytes: statSync(target.file).size,
              sha256: sha256(target.file),
              pageFormatVersion: baseReconstructed.manifest.pageFormatVersion,
              walBytes: 0,
              chainDepth: safeChainDepth(body.base) + 1,
            }), 'utf8');
          });
          send(200, { success: true, backup: target.name, kind: 'incremental', base: backupArtifactFile(body.base).name, bytes: statSync(target.file).size, sha256: sha256(target.file) });
        } else {
          const { name, file } = backupFile(requestedName);
          await enqueue(async () => {
            await callDatabase('execute', 'CHECKPOINT;');
            copyFileSync(database, file);
          writeFileSync(file + '.json', JSON.stringify({ version: 2, name, createdAt: new Date().toISOString(), bytes: statSync(file).size, sha256: sha256(file), pageFormatVersion: pageFormatVersion(file), walBytes: 0, pageChecksum: sha256(file) }), 'utf8');
          });
          send(200, { success: true, backup: name, bytes: statSync(file).size, sha256: sha256(file) });
        }
      } else {
        let rollbackUsed;
        await enqueue(async () => {
          try {
            const temporaryPages = database + '.restore.tmp';
            if (existsSync(temporaryPages)) unlinkSync(temporaryPages);
            await materializeBackup(body.name, temporaryPages);
            const rollbackPath = createRestoreRollback();
            rollbackUsed = rollbackPath;
            renameSync(temporaryPages, database);
            for (const suffix of ['.wal', '.ckpt']) {
              const source = temporaryPages + suffix;
              if (existsSync(source)) renameSync(source, database + suffix);
              else if (existsSync(database + suffix)) unlinkSync(database + suffix);
            }
            await callDatabase('catalog');
          } catch (error) {
            if (rollbackUsed && restoreRollbackDirectory(rollbackUsed)) {
              await callDatabase('catalog');
              throw Object.assign(new Error(`Restore failed and original database was restored: ${error.message}`), { status: 422 });
            }
            throw error;
          }
        });
        send(200, { success: true, restored: body.name, bytes: statSync(database).size, rollback: rollbackUsed });
      }
    } catch (error) { send(error.status ?? 503, { success: false, error: { message: error.message } }); }
    return;
  }
  const cancelRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/cancel$/);
  if (cancelRoute) {
    if (req.method !== 'POST') { send(405, { success: false, error: { code: 405, message: 'Cancellation requires POST' } }); return; }
    try {
      const body = await readJson();
      if (typeof body.requestId === 'string' && body.requestId.length > 0 && body.requestId.length <= 128) requestId = body.requestId;
    } catch { /* Cancellation remains valid without a JSON body. */ }
    const session = sessions.get(cancelRoute[1]);
    if (!session) { send(404, { success: false, error: { code: 404, message: 'Session not found or expired' } }); return; }
    if (session.user !== requestUser) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    try {
      if (activeOperation?.sessionId === session.id) {
        writeFileSync(activeOperation.cancelFile, 'cancel\n', 'utf8');
        send(202, { success: false, cancelled: true, commitState: 'unknown', error: { code: 5002, message: 'Cancellation requested' } });
        return;
      }
      if (session.waiting) {
        session.cancelRequested = true;
        for (const waiters of [transactionWaiters, turnWaiters]) {
          const index = waiters.findIndex(waiter => waiter.session?.id === session.id);
          if (index < 0) continue;
          const [waiter] = waiters.splice(index, 1);
          clearTimeout(waiter.timer);
          waiter.resolve();
        }
        send(202, { success: false, cancelled: true, error: { code: 5002, message: 'Cancellation requested' } });
        return;
      }
      send(409, { success: false, error: { code: 409, message: 'No query is running' } });
    } catch (error) { send(503, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    return;
  }
  const indexRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/index-(inspect|verify|rebuild)$/);
  if (indexRoute) {
    req.resume();
    const action = indexRoute[2];
    if (req.method !== 'POST') { send(405, { success: false, error: { code: 405, message: `Index ${action} requires POST` } }); return; }
    const session = sessions.get(indexRoute[1]);
    if (!session) { send(404, { success: false, error: { code: 404, message: 'Session not found or expired' } }); return; }
    if (session.user !== requestUser) { send(403, { success: false, error: { code: 7001, message: 'Permission denied' } }); return; }
    try {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
      if (typeof body.table !== 'string' || typeof body.index !== 'string') throw httpError(400, 'Expected table and index strings');
      // 结构检查只读；在线重建改写索引页，按对象写权限（UPDATE）收紧。
      const permission = action === 'rebuild' ? 'UPDATE' : 'READ';
      if (!can(access, requestUser, permission, body.table)) throw httpError(403, 'Permission denied');
      auditSql = `INDEX ${action.toUpperCase()} ${body.table}.${body.index}`;
      auditObjects = [body.table.toLowerCase()];
      const operation = action === 'inspect' ? 'indexInspect' : action === 'verify' ? 'indexVerify' : 'indexRebuild';
      const data = await runSessionOperation(session, operation, '', res, { table: body.table, index: body.index });
      send(data.success === false ? (data.error?.code === 7001 ? 403 : quarantined ? 503 : 422) : 200, data);
    } catch (error) { send(error.status ?? 400, { success: false, error: { message: error instanceof Error ? error.message : String(error) } }); }
    return;
  }
  const sessionRoute = req.url?.match(/^\/api\/sessions\/([a-zA-Z0-9-]+)\/(execute|compile|diagnostics|statistics|catalog|close|buffer)(\/stream)?$/);
  const streamed = req.url === '/api/execute/stream' || Boolean(sessionRoute?.[3]);
  let mode;
  if (sessionRoute && (((sessionRoute[2] === 'catalog' || sessionRoute[2] === 'statistics') && req.method === 'GET') || (sessionRoute[2] !== 'catalog' && sessionRoute[2] !== 'statistics' && req.method === 'POST'))) mode = sessionRoute[2];
  else if (req.method === 'GET' && req.url === '/api/catalog') mode = 'catalog';
  else if (req.method === 'POST' && req.url === '/api/compile') mode = 'compile';
  else if (req.method === 'POST' && req.url === '/api/diagnostics') mode = 'diagnostics';
  else if (req.method === 'GET' && req.url === '/api/statistics') mode = 'statistics';
  else if (req.method === 'POST' && (req.url === '/api/execute' || req.url === '/api/execute/stream')) mode = 'execute';
  else { send(404, { error: { message: 'Not found' } }); return; }
  if (streamed && sessionRoute && sessionRoute[2] !== 'execute') { send(404, { error: { message: 'Streaming is available for execute only' } }); return; }
  try {
    let sql = '';
    if (mode !== 'catalog' && mode !== 'statistics' && mode !== 'close') {
      const chunks = [];
      let length = 0;
      for await (const chunk of req) {
        length += chunk.length;
        if (length > 8 * 1024 * 1024) { send(413, { error: { message: 'Request exceeds 8 MiB' } }); return; }
        chunks.push(chunk);
      }
      try {
        const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
        if (typeof body.sql !== 'string') throw new Error();
        if (typeof body.requestId === 'string' && body.requestId.length > 0 && body.requestId.length <= 128) requestId = body.requestId;
        sql = body.sql;
        auditSql = sql.slice(0, 4096);
      } catch { send(400, { error: { message: 'Expected UTF-8 JSON with a sql string' } }); return; }
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
    const started = performance.now();
    const data = await (async () => {
      if (!sessionRoute) {
        if (sessions.size) throw httpError(409, 'Database reserved by active sessions');
        return await enqueue(async () => {
          const cancelFile = resolve(cancelDirectory, `request-${auditId}.cancel`);
          clearCancelFile(cancelFile);
          const disconnected = () => {
            if (!res.writableEnded) {
              try { writeFileSync(cancelFile, 'disconnect\n', 'utf8'); }
              catch { /* The child process timeout remains the fallback. */ }
            }
          };
          res.once('close', disconnected);
          try { return await callDatabase(mode, sql, { MINISQL_CANCEL_FILE: cancelFile }, { user: requestUser, password: requestPassword ?? '' }); }
          finally { res.off('close', disconnected); clearCancelFile(cancelFile); }
        });
      }
      const session = sessions.get(sessionRoute[1]);
      if (!session) throw httpError(404, 'Session not found or expired');
      if (session.user !== requestUser) throw httpError(403, 'Permission denied');
      session.password = requestPassword ?? '';
      if (res.destroyed && mode !== 'close') throw httpError(499, 'Request disconnected before execution');
      if (mode === 'close') return closeSession(session);
      const streamSession = streamed && mode === 'execute';
      return runSessionOperation(session, mode, sql, res, streamSession ? {
        stream: true,
        onFrame: async frame => {
          streamOutput ??= beginStream(200);
          if (frame.type === 'meta') await streamOutput.write({ type: 'meta', success: true, ...frame.meta });
          else if (frame.type === 'row') await streamOutput.write({ type: 'row', success: true, values: frame.row });
          else if (frame.type === 'complete') await streamOutput.write({ type: 'complete', success: true,
            rowCount: frame.rows, resourceUsage: frame.resourceUsage, transactionState: session.transactionState });
          else if (frame.type === 'error') await streamOutput.write({ type: 'error', success: false,
            error: frame.error, commitState: frame.commitState, transactionState: session.transactionState });
        },
      } : {});
    })();
    if ((mode === 'catalog' || mode === 'statistics') && data && Array.isArray(data.tables)) {
      data.tables = data.tables.filter(table => can(access, requestUser, 'SELECT', table.name));
    }
    const resourceLimited = data.error?.code === 5001 && /budget exceeded/i.test(data.error?.message ?? '');
    // 审计对象来自引擎绑定结果；权限/只读错误按语义映射到 403/400。
    if (Array.isArray(data?.accessObjects) && data.accessObjects.length)
      auditObjects = data.accessObjects.map(object => String(object).toLowerCase());
    const permissionDenied = data.error?.code === 7001;
    const readOnlyViolation = !permissionDenied && /read-?only/i.test(data.error?.message ?? '');
    const status = data.success === false
      ? (permissionDenied ? 403 : readOnlyViolation ? 400 : quarantined ? 503 : resourceLimited ? 413 : 422)
      : 200;
    const response = mode === 'catalog' || mode === 'buffer' || mode === 'diagnostics' || mode === 'statistics' ? data : queryResult(data, performance.now() - started);
    if (data.success === false && data.completedStatements > 0) {
      const committed = (data.results ?? []).filter(result => result.commitState === 'committed').length;
      const rolledBack = (data.results ?? []).filter(result => result.commitState === 'rolledBack').length;
      const suffix = [committed ? `此前 ${committed} 条语句已成功，未自动回滚。` : '',
        rolledBack ? `事务内 ${rolledBack} 条已执行语句已回滚。` : ''].filter(Boolean).join('');
      response.error = { ...data.error, message: `${data.error.message}${suffix ? '；' + suffix : ''}` };
    }
    if (streamOutput) streamOutput.end();
    else if (streamed) await sendStream(status, response); else send(status, response);
  } catch (error) {
    const failure = { success: false, commitState: !error.status && mode === 'execute' ? 'unknown' : undefined,
      transactionState: sessionRoute ? sessions.get(sessionRoute[1])?.transactionState : undefined,
      error: { message: error.message, suggestion: 'Do not automatically retry writes; inspect database state.' } };
    if (streamOutput) {
      await streamOutput.write({ type: 'error', ...failure });
      streamOutput.end();
    } else send(error.status ?? 503, failure);
  }
});
// 启动预热：尝试加载一次引擎 Catalog。若引擎二进制缺失或损坏，服务器仍进入降级
// 模式（健康检查报 degraded、引擎路由返回 503），使 access/audit/capabilities 等
// 不依赖引擎的管理端点保持可用（供管理员恢复）。
try { await callDatabase('catalog'); }
catch { quarantined = true; }
server.listen(bridgePort, '127.0.0.1', () => {
  console.log(`MiniSQL database API http://127.0.0.1:${server.address().port}/api`);
});

import type { AccessState, AuditEntry, BackupEntry, BackupValidation, Capabilities, Connection, QueryResult, SessionEntry, Table } from './types';

function requestHeaders(connection: Connection, initial?: HeadersInit) {
  const headers = new Headers(initial);
  headers.set('X-MiniSQL-User', connection.user);
  if (connection.password) headers.set('X-MiniSQL-Password', connection.password);
  else headers.delete('X-MiniSQL-Password');
  return headers;
}

async function api(connection: Connection, path: string, options?: RequestInit) {
  const url = `${connection.url.replace(/\/$/, '')}${path}`;
  const response = await fetch(url, { ...options, headers: requestHeaders(connection, options?.headers) });
  const data = await response.json().catch(() => { throw new Error(`API 返回非 JSON 响应 (${response.status})`); });
  if (!response.ok || data.error) throw Object.assign(new Error(data.error?.message || data.message || `HTTP ${response.status}`), data.error, {
    completedStatements: data.completedStatements, results: data.results, commitState: data.commitState,
    transactionState: data.transactionState, status: response.status, response: data,
  });
  return data;
}

async function streamApi(connection: Connection, path: string, sql: string, signal: AbortSignal) {
  const requestId = crypto.randomUUID();
  const response = await fetch(`${connection.url.replace(/\/$/, '')}${path}`, {
    method: 'POST', headers: requestHeaders(connection, { 'Content-Type': 'application/json' }), body: JSON.stringify({ sql, requestId }), signal,
  });
  const reader = response.body?.getReader();
  if (!reader) throw new Error(`API 未返回可读取的流 (${response.status})`);
  const decoder = new TextDecoder();
  let buffer = '';
  const data: any = { columns: [], rows: [], plan: [], affectedRows: 0, statements: 1, durationMs: 0 };
  const consume = (frame: any) => {
    if (frame.type === 'meta') Object.assign(data, frame);
    else if (frame.type === 'row') data.rows.push(frame.values);
    else if (frame.type === 'complete') Object.assign(data, frame);
    else if (frame.type === 'error') {
      throw Object.assign(new Error(frame.error?.message ?? '流式查询失败'), frame.error ?? {}, {
        status: response.status, response: frame, commitState: frame.commitState, transactionState: frame.transactionState,
      });
    }
  };
  try {
    for (;;) {
      const part = await reader.read();
      buffer += decoder.decode(part.value ?? new Uint8Array(), { stream: !part.done });
      let newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline).trim(); buffer = buffer.slice(newline + 1);
        if (line) consume(JSON.parse(line));
      }
      if (part.done) break;
    }
    const tail = buffer.trim();
    if (tail) consume(JSON.parse(tail));
  } finally { reader.releaseLock(); }
  if (!response.ok) throw Object.assign(new Error(data.error?.message ?? `HTTP ${response.status}`), data.error ?? {}, { status: response.status, response: data });
  return data;
}

function sessionPath(connection: Connection, path: string) {
  return connection.sessionId ? `/sessions/${encodeURIComponent(connection.sessionId)}${path}` : path;
}

export async function openApiSession(connection: Connection): Promise<{ sessionId: string; transactionState: string }> {
  return api(connection, '/sessions', { method: 'POST' });
}

export async function closeApiSession(connection: Connection) {
  if (connection.sessionId) return api(connection, sessionPath(connection, '/close'), { method: 'POST' });
}

export function releaseApiSession(connection: Connection) {
  if (connection.sessionId) void fetch(`${connection.url.replace(/\/$/, '')}${sessionPath(connection, '/close')}`, {
    method: 'POST', keepalive: true, headers: requestHeaders(connection),
  }).catch(() => {});
}

export async function getCatalog(connection: Connection): Promise<Table[]> {
  const data = await api(connection, sessionPath(connection, '/catalog'));
  if (!Array.isArray(data.tables)) throw new Error('Catalog 响应缺少 tables 数组。');
  return data.tables;
}

export interface BufferStatistics {
  available: boolean;
  policy: string;
  capacity: number;
  residentPages: number;
  hits: number;
  misses: number;
  hitRate: number;
  diskReads: number;
  diskWrites: number;
  ioErrors: number;
  stagedPageReads: number;
  stagedPageWrites: number;
  evictions: { sequence: number; policy: string; pageId: number; generation: number; dirty: boolean; writeBack?: string }[];
}

export interface StorageStatistics {
  pageSize: number;
  fileBytes: number;
  allocatedPages: number;
  buffer?: BufferStatistics;
}

export async function configureBuffer(connection: Connection, action: 'LRU' | 'FIFO' | 'RESET'): Promise<BufferStatistics> {
  if (!connection.sessionId) throw new Error('需要真实数据库会话。');
  const data = await api(connection, sessionPath(connection, '/buffer'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql: action }),
  });
  return data.buffer;
}

export async function getStorage(connection: Connection): Promise<StorageStatistics> {
  const storage = await api(connection, '/storage');
  if (!connection.sessionId) return { ...storage, buffer: undefined };
  const catalog = await api(connection, sessionPath(connection, '/catalog'));
  return { ...storage, buffer: catalog.buffer?.available ? catalog.buffer : undefined };
}

export async function getHealth(connection: Connection): Promise<{ status: string; engine: string }> {
  return api(connection, '/health');
}
// X24 权限、审计与多会话面板客户端。
const ACCESS_PERMISSIONS = ['*', 'connect', 'read', 'select', 'insert', 'update', 'delete', 'create', 'drop', 'transaction', 'checkpoint', 'compile', 'grant', 'audit'];

export async function getAccess(connection: Connection): Promise<AccessState> {
  const data = await api(connection, '/access');
  if (!data.access) throw new Error('权限响应缺少 access 字段。');
  return data.access;
}
export async function getUsers(connection: Connection): Promise<AccessState['users']> {
  const data = await api(connection, '/users');
  if (!Array.isArray(data.users)) throw new Error('用户响应缺少 users 数组。');
  return data.users;
}
export async function createUser(connection: Connection, body: { name: string; password?: string; roles?: string[]; grants?: { object: string; permissions: string[] }[] }): Promise<AccessState> {
  const data = await api(connection, '/users', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  return data.access;
}
export async function dropUser(connection: Connection, name: string): Promise<AccessState> {
  const data = await api(connection, `/users/${encodeURIComponent(name)}`, { method: 'DELETE' });
  return data.access;
}
export async function setUserPassword(connection: Connection, name: string, password: string): Promise<AccessState> {
  const data = await api(connection, `/users/${encodeURIComponent(name)}/password`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password }) });
  return data.access;
}
export async function addUserRole(connection: Connection, name: string, role: string): Promise<AccessState> {
  const data = await api(connection, `/users/${encodeURIComponent(name)}/roles`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ role }) });
  return data.access;
}
export async function removeUserRole(connection: Connection, name: string, role: string): Promise<AccessState> {
  const data = await api(connection, `/users/${encodeURIComponent(name)}/roles/${encodeURIComponent(role)}`, { method: 'DELETE' });
  return data.access;
}
export async function createRole(connection: Connection, body: { name: string; inherits?: string[]; grants?: { object: string; permissions: string[] }[] }): Promise<AccessState> {
  const data = await api(connection, '/roles', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  return data.access;
}
export async function dropRole(connection: Connection, name: string): Promise<AccessState> {
  const data = await api(connection, `/roles/${encodeURIComponent(name)}`, { method: 'DELETE' });
  return data.access;
}
export async function grantAccess(connection: Connection, body: { subject: { type: 'user' | 'role'; name: string }; object?: string; permissions: string[] }): Promise<AccessState> {
  const data = await api(connection, '/grants', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  return data.access;
}
export async function revokeAccess(connection: Connection, body: { subject: { type: 'user' | 'role'; name: string }; object?: string; permissions?: string[] }): Promise<AccessState> {
  const data = await api(connection, '/revokes', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  return data.access;
}
export async function getAudit(connection: Connection, filters: { user?: string; object?: string; limit?: number } = {}): Promise<{ entries: AuditEntry[]; count: number }> {
  const params = new URLSearchParams();
  if (filters.user) params.set('user', filters.user);
  if (filters.object) params.set('object', filters.object);
  if (filters.limit) params.set('limit', String(filters.limit));
  const query = params.toString();
  const data = await api(connection, `/audit${query ? '?' + query : ''}`);
  if (!Array.isArray(data.entries)) throw new Error('审计响应缺少 entries 数组。');
  return { entries: data.entries, count: data.count ?? data.entries.length };
}
export async function getSessions(connection: Connection): Promise<SessionEntry[]> {
  const data = await api(connection, '/sessions');
  if (!Array.isArray(data.entries)) throw new Error('会话响应缺少 entries 数组。');
  return data.entries;
}

export async function cancelSession(connection: Connection, sessionId: string) {
  const requestId = crypto.randomUUID();
  const response = await fetch(`${connection.url.replace(/\/$/, '')}/sessions/${encodeURIComponent(sessionId)}/cancel`, {
    method: 'POST', headers: requestHeaders(connection, { 'Content-Type': 'application/json' }), body: JSON.stringify({ requestId }), signal: AbortSignal.timeout(10000),
  });
  const data = await response.json().catch(() => { throw new Error(`取消请求返回非 JSON 响应 (${response.status})`); });
  if (response.status === 202 && data.cancelled) return data;
  throw Object.assign(new Error(data.error?.message ?? `HTTP ${response.status}`), data.error ?? {}, { status: response.status, response: data });
}

export async function getCapabilities(connection: Connection): Promise<Capabilities> {
  return api(connection, '/capabilities');
}

export async function getBackups(connection: Connection): Promise<BackupEntry[]> {
  const data = await api(connection, '/backups');
  if (!Array.isArray(data.entries)) throw new Error('备份响应缺少 entries 数组。');
  return data.entries;
}

export async function createBackup(connection: Connection, body: { name?: string; kind?: 'full' | 'incremental'; base?: string } = {}) {
  return api(connection, '/backup', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
}

export async function restoreBackup(connection: Connection, name: string) {
  return api(connection, '/restore', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name }) });
}

export async function validateBackup(connection: Connection, name: string): Promise<BackupValidation> {
  const data = await api(connection, '/backup/validate', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name }),
  });
  if (!data.validation) throw new Error('备份迁移校验响应缺少 validation 字段。');
  return data.validation;
}
export interface IndexPageInspect {
  page: { id: number; generation: number };
  leaf: boolean;
  height: number;
  keyCount: number;
  parent: { id: number; generation: number };
  left: { id: number; generation: number };
  right: { id: number; generation: number };
}
export interface IndexInspect {
  kind: string;
  table: string;
  index: string;
  present: boolean;
  root: { id: number; generation: number };
  height: number;
  nodeCount: number;
  leafCount: number;
  rowCount: number;
  leafChainLength: number;
  rootReachable: boolean;
  leafChainLinked: boolean;
  parentLinksValid: boolean;
  storage?: string;
  message?: string;
  problems: string[];
  pages: IndexPageInspect[];
}
export async function inspectIndex(connection: Connection, table: string, index: string): Promise<IndexInspect> {
  if (connection.mode !== 'api' || !connection.sessionId) throw new Error('需要真实数据库会话。');
  return api(connection, sessionPath(connection, '/index-inspect'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ table, index }),
  });
}

export async function runSql(connection: Connection, sql: string, compile: boolean, signal: AbortSignal): Promise<QueryResult> {
  if (!compile && /^\s*SELECT\b/i.test(sql)) {
    const data = await streamApi(connection, sessionPath(connection, '/execute/stream'), sql, signal);
    if (!Array.isArray(data.rows) || !Array.isArray(data.columns)) throw new Error('流式 API 响应不符合 QueryResult 契约。');
    return { ...data, plan: data.plan ?? [], durationMs: data.durationMs ?? 0, affectedRows: data.affectedRows ?? 0, statements: data.statements ?? 1 };
  }
  const data = await api(connection, sessionPath(connection, compile ? '/compile' : '/execute'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql, requestId: crypto.randomUUID() }), signal,
  });
  if (!Array.isArray(data.rows) || !Array.isArray(data.columns)) throw new Error('API 响应不符合 QueryResult 契约。');
  return { ...data, plan: data.plan ?? [], durationMs: data.durationMs ?? 0, affectedRows: data.affectedRows ?? 0, statements: data.statements ?? 1 };
}

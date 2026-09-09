import type { Connection, QueryResult, Table } from './types';

const DB_KEY = 'minisql-studio-db-v1';
let worker: Worker | undefined;
let sequence = 0;
let ready: Promise<{ tables: Table[] }> | undefined;
const pending = new Map<number, { resolve: (v: any) => void; reject: (e: Error) => void }>();
let snapshot: Uint8Array | undefined;

function save(bytes: Uint8Array) {
  snapshot = bytes;
  try { localStorage.setItem(DB_KEY, btoa(Array.from(bytes, b => String.fromCharCode(b)).join(''))); } catch { /* Session still works when storage is unavailable. */ }
}
function request(action: string, extra: Record<string, unknown> = {}): Promise<any> {
  return new Promise((resolve, reject) => {
    const id = ++sequence;
    pending.set(id, { resolve, reject });
    worker!.postMessage({ id, action, ...extra });
  });
}
function ensureWorker() {
  if (ready) return ready;
  if (!snapshot) {
    try { const stored = localStorage.getItem(DB_KEY); if (stored) snapshot = Uint8Array.from(atob(stored), c => c.charCodeAt(0)); } catch { snapshot = undefined; }
  }
  worker = new Worker(new URL('./engine.worker.ts', import.meta.url), { type: 'module' });
  worker.onmessage = ({ data }) => {
    const promise = pending.get(data.id);
    if (!promise) return;
    pending.delete(data.id);
    if (data.error) promise.reject(Object.assign(new Error(data.error.message), data.error));
    else {
      if (data.snapshot || data.data.snapshot) save(data.snapshot || data.data.snapshot);
      promise.resolve(data.data);
    }
  };
  worker.onerror = () => cancelDemo('本地数据库引擎加载失败。');
  ready = request('init', { snapshot });
  return ready!;
}
export function cancelDemo(message = '执行已取消，本地数据库已恢复到上次完成的状态。') {
  worker?.terminate(); worker = undefined; ready = undefined;
  pending.forEach(p => p.reject(new Error(message))); pending.clear();
}
async function api(connection: Connection, path: string, options?: RequestInit) {
  const url = `${connection.url.replace(/\/$/, '')}${path}`;
  const response = await fetch(url, options);
  const data = await response.json().catch(() => { throw new Error(`API 返回非 JSON 响应 (${response.status})`); });
  if (!response.ok || data.error) throw Object.assign(new Error(data.error?.message || data.message || `HTTP ${response.status}`), data.error, {
    completedStatements: data.completedStatements, results: data.results, commitState: data.commitState,
    transactionState: data.transactionState, status: response.status, response: data,
  });
  return data;
}
async function streamApi(connection: Connection, path: string, sql: string, signal: AbortSignal) {
  const response = await fetch(`${connection.url.replace(/\/$/, '')}${path}`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }), signal,
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
  if (connection.sessionId) void fetch(`${connection.url.replace(/\/$/, '')}${sessionPath(connection, '/close')}`, { method: 'POST', keepalive: true }).catch(() => {});
}
export async function getCatalog(connection: Connection): Promise<Table[]> {
  if (connection.mode === 'api') {
    const data = await api(connection, sessionPath(connection, '/catalog'));
    if (!Array.isArray(data.tables)) throw new Error('Catalog 响应缺少 tables 数组。');
    return data.tables;
  }
  await ensureWorker();
  return (await request('catalog')).tables;
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
  if (connection.mode !== 'api' || !connection.sessionId) throw new Error('需要真实数据库会话。');
  const data = await api(connection, sessionPath(connection, '/buffer'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql: action }),
  });
  return data.buffer;
}
export async function getStorage(connection: Connection): Promise<StorageStatistics> {
  if (connection.mode !== 'api') throw new Error('本地演示引擎未提供文件存储统计。');
  const storage = await api(connection, '/storage');
  if (!connection.sessionId) return { ...storage, buffer: undefined };
  const catalog = await api(connection, sessionPath(connection, '/catalog'));
  return { ...storage, buffer: catalog.buffer?.available ? catalog.buffer : undefined };
}
export async function getHealth(connection: Connection): Promise<{ status: string; engine: string }> {
  if (connection.mode !== 'api') throw new Error('本地演示引擎未提供服务健康检查。');
  return api(connection, '/health');
}
export async function runSql(connection: Connection, sql: string, compile: boolean, signal: AbortSignal): Promise<QueryResult> {
  if (connection.mode === 'demo') {
    await ensureWorker();
    if (signal.aborted) throw new Error('执行已取消。');
    const cancel = () => cancelDemo();
    signal.addEventListener('abort', cancel, { once: true });
    try { return await request(compile ? 'compile' : 'execute', { sql }); }
    finally { signal.removeEventListener('abort', cancel); }
  }
  if (!compile && /^\s*SELECT\b/i.test(sql)) {
    const data = await streamApi(connection, sessionPath(connection, '/execute/stream'), sql, signal);
    if (!Array.isArray(data.rows) || !Array.isArray(data.columns)) throw new Error('流式 API 响应不符合 QueryResult 契约。');
    return { ...data, plan: data.plan ?? [], durationMs: data.durationMs ?? 0, affectedRows: data.affectedRows ?? 0, statements: data.statements ?? 1 };
  }
  const data = await api(connection, sessionPath(connection, compile ? '/compile' : '/execute'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }), signal,
  });
  if (!Array.isArray(data.rows) || !Array.isArray(data.columns)) throw new Error('API 响应不符合 QueryResult 契约。');
  return { ...data, plan: data.plan ?? [], durationMs: data.durationMs ?? 0, affectedRows: data.affectedRows ?? 0, statements: data.statements ?? 1 };
}

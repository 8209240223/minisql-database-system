import type { HistoryItem } from './types';

export const HISTORY_KEY = 'minisql-studio-history-v1';
export const MAX_HISTORY_SQL = 65536;
const MAX_HISTORY_TEXT = 262144;

export function limitHistory(items: HistoryItem[]): HistoryItem[] {
  let length = 0;
  const result: HistoryItem[] = [];
  for (const item of items) {
    const size = JSON.stringify(item).length;
    if (item.sql.length > MAX_HISTORY_SQL || result.length === 50 || length + size > MAX_HISTORY_TEXT) break;
    result.push(item); length += size;
  }
  return result;
}

export function parseHistory(raw: string | null): HistoryItem[] {
  if (!raw || raw.length > MAX_HISTORY_TEXT + 1024) return [];
  try {
    const value: unknown = JSON.parse(raw);
    if (!Array.isArray(value) || value.length > 50) return [];
    const result: HistoryItem[] = [];
    const ids = new Set<string>();
    for (const item of value) {
      if (!item || typeof item.id !== 'string' || item.id.length > 100 || ids.has(item.id) ||
          typeof item.sql !== 'string' || item.sql.length > MAX_HISTORY_SQL ||
          !Number.isFinite(item.at) || item.at < 0 || item.at > 8640000000000000 ||
          !Number.isFinite(item.durationMs) || item.durationMs < 0 ||
          !Number.isSafeInteger(item.rows) || item.rows < 0 ||
          !['demo', 'api'].includes(item.mode) || !['compile', 'execute'].includes(item.action) ||
          typeof item.connection !== 'string' || item.connection.length > 100 ||
          (item.error !== undefined && (typeof item.error !== 'string' || item.error.length > 1000))) return [];
      ids.add(item.id);
      // 仅恢复允许字段，不保留会话标识或其他附带数据。
      result.push({ id: item.id, sql: item.sql, at: item.at, durationMs: item.durationMs,
        rows: item.rows, mode: item.mode, action: item.action, connection: item.connection,
        ...(item.error === undefined ? {} : { error: item.error }) });
    }
    return limitHistory(result);
  } catch { return []; }
}

export function restoreHistory(): HistoryItem[] {
  try { return parseHistory(localStorage.getItem(HISTORY_KEY)); } catch { return []; }
}

export function filterHistory(items: HistoryItem[], query: string): HistoryItem[] {
  const needle = query.trim().toLowerCase();
  return items.filter(item => `${item.sql}\n${item.connection}\n${item.error ?? ''}\n${item.mode === 'demo' ? '演示' : '真实'}\n${item.action === 'compile' ? '编译' : '执行'}`.toLowerCase().includes(needle));
}

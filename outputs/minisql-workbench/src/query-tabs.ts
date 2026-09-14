import type { Diagnostic, QueryResult, QueryTab } from './types';
import type { DiagnosticLocation } from './diagnostic-location';

export const DRAFT_KEY = 'minisql-studio-tabs-v1';
export interface QueryView {
  result: QueryResult | null;
  output: 'results' | 'plan' | 'ast' | 'tokens' | 'diagnostics' | 'inspect';
  notice: string;
  source?: string;
  outcome?: string;
  selection?: boolean;
  diagnostic?: DiagnosticLocation;
  diagnostics?: Diagnostic[];
  diagnosticFrom?: number;
  diagnosticTo?: number;
}
export const emptyView: QueryView = { result: null, output: 'results', notice: '' };
export function newQueryTab(value: Pick<QueryTab, 'id' | 'name' | 'sql'> & Partial<QueryTab>): QueryTab {
  return { ...value, editVersion: value.editVersion ?? 0, transactionState: value.transactionState ?? 'IDLE', running: value.running ?? false };
}
export function serializeTabs(tabs: QueryTab[]): string {
  return JSON.stringify(tabs.map(tab => ({
    id: tab.id, name: tab.name, sql: tab.sql, dirty: tab.dirty, editVersion: tab.editVersion,
    ...(tab.connection ? { connection: {
      mode: tab.connection.mode, kind: tab.connection.kind, name: tab.connection.name,
      url: tab.connection.url, user: tab.connection.user,
    } } : {}),
  })));
}
export function restoreTabs(fallback: QueryTab[]): QueryTab[] {
  try {
    const value: unknown = JSON.parse(localStorage.getItem(DRAFT_KEY) ?? 'null');
    if (!Array.isArray(value) || !value.length || value.length > 50) return fallback;
    const ids = new Set<string>();
    for (const tab of value) {
      if (!tab || typeof tab.id !== 'string' || !/^(?:q\d+|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/i.test(tab.id) || ids.has(tab.id) ||
          typeof tab.name !== 'string' || !tab.name.trim() || tab.name.length > 100 ||
          typeof tab.sql !== 'string' || tab.sql.length > 8 * 1024 * 1024) return fallback;
      ids.add(tab.id);
    }
    return value.map(tab => {
      const saved = tab as Partial<QueryTab>;
      const profile = saved.connection;
      const connection = profile && profile.mode === 'api' && ['native', 'demo'].includes(profile.kind) &&
        typeof profile.name === 'string' && typeof profile.url === 'string' && typeof profile.user === 'string'
        ? { mode: profile.mode, kind: profile.kind, name: profile.name, url: profile.url, user: profile.user, password: '' }
        : undefined;
      return newQueryTab({ id: tab.id, name: tab.name, sql: tab.sql, dirty: true,
        editVersion: Number.isSafeInteger(saved.editVersion) && (saved.editVersion ?? -1) >= 0 ? saved.editVersion : 0,
        connection });
    });
  } catch { return fallback; }
}

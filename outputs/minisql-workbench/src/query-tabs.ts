import type { QueryResult, QueryTab } from './types';
import type { DiagnosticLocation } from './diagnostic-location';

export const DRAFT_KEY = 'minisql-studio-tabs-v1';
export interface QueryView {
  result: QueryResult | null;
  output: 'results' | 'plan' | 'ast' | 'tokens';
  notice: string;
  source?: string;
  outcome?: string;
  selection?: boolean;
  diagnostic?: DiagnosticLocation;
}
export const emptyView: QueryView = { result: null, output: 'results', notice: '' };
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
    return value.map(tab => ({ id: tab.id, name: tab.name, sql: tab.sql, dirty: true }));
  } catch { return fallback; }
}

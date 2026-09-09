export type Cell = string | number | boolean | null;
export interface Column { name: string; type: string; primaryKey: boolean; nullable: boolean; defaultValue?: string | null; unique?: boolean; references?: { table: string; column: string } | null }
export interface Table { name: string; columns: Column[]; indexes?: { name: string; columns: string[]; unique: boolean }[]; rowCount: number; keys?: { primary: boolean; columns: string[] }[]; foreignKeys?: { columns: string[]; table: string; referencedColumns: string[] }[]; checks?: Record<string, unknown>[]; constraintNames?: { name: string; kind: string; index: number }[] }
export interface Diagnostic { message: string; stage?: string; line?: number; column?: number }
export interface PlanRow { id: number; parent: number; detail: string; depth?: number; kind?: string }
export interface QueryResult {
  executionStats?: { scope: 'query'; nodeStatisticsAvailable: boolean; actualRows: number; durationMs: number; loops: number; hits: number; misses: number; diskReads: number; diskWrites: number; stagedPageReads: number; stagedPageWrites: number; ioErrors: number };
  transactionState?: string;
  results?: { commitState?: string; kind?: string }[];
  columns: string[];
  columnTypes?: string[];
  integerEncoding?: 'safe-number-or-decimal-string';
  rows: Cell[][];
  affectedRows: number;
  durationMs: number;
  ast?: unknown;
  astError?: string;
  plan: PlanRow[];
  optimizedPlan?: PlanRow[];
  optimizationRules?: unknown[];
  optimizer?: { iterations: number; converged: boolean; diagnostics: { code: string; message: string }[] };
  statements: number;
  tables?: Table[];
  warning?: string;
  diagnostics?: Diagnostic[];
  tokens?: SqlToken[];
  stages?: Record<string, string>;
}
export interface Connection { mode: 'demo' | 'api'; name: string; url: string; sessionId?: string }
export interface QueryTab { id: string; name: string; sql: string; dirty?: boolean }
export interface HistoryItem { id: string; sql: string; at: number; durationMs: number; rows: number; error?: string; mode: Connection['mode']; connection: string; action: 'compile' | 'execute' }
export interface SqlToken { type: string; text: string; line: number; column: number }

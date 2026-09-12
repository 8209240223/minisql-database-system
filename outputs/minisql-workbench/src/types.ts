export type Cell = string | number | boolean | null;
export interface Column { name: string; type: string; primaryKey: boolean; nullable: boolean; defaultValue?: string | null; unique?: boolean; references?: { table: string; column: string } | null }
export interface Table { name: string; columns: Column[]; indexes?: { name: string; columns: string[]; unique: boolean }[]; rowCount: number; keys?: { primary: boolean; columns: string[] }[]; foreignKeys?: { columns: string[]; table: string; referencedColumns: string[] }[]; checks?: Record<string, unknown>[]; constraintNames?: { name: string; kind: string; index: number }[] }
export interface Diagnostic { code?: number; message: string; suggestion?: string; actual?: string; expected?: string[]; stage?: string; line?: number; column?: number; endLine?: number; endColumn?: number; statementIndex?: number }
export interface PlanColumn { name: string; type: string; columnId?: number; identity?: string; nullable?: boolean; primaryKey?: boolean; unique?: boolean; defaultValue?: string | null }
export interface PlanRow {
  id: number;
  nodeId?: number;
  parent: number;
  detail: string;
  depth?: number;
  kind?: string;
  children?: number[];
  output?: PlanColumn[];
  predicate?: unknown;
  projections?: unknown[];
  sortKeys?: unknown[];
  groupKeys?: unknown[];
  aggregates?: unknown[];
  estimatedRows?: number;
  actualRows?: number;
  sourceSpan?: { start: { line: number; column: number }; end: { line: number; column: number } };
  outputSchema?: PlanColumn[];
}
export interface QueryResult {
  protocolVersion?: number;
  requestId?: string;
  executionStats?: { scope: 'query'; nodeStatisticsAvailable: boolean; actualRows: number; durationMs: number; loops: number; hits: number; misses: number; diskReads: number; diskWrites: number; stagedPageReads: number; stagedPageWrites: number; ioErrors: number };
  transactionState?: string;
  results?: { commitState?: string; kind?: string }[];
  columns: string[];
  columnTypes?: string[];
  integerEncoding?: 'safe-number-or-decimal-string';
  rows: Cell[][];
  rowCount?: number;
  truncated?: boolean;
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
export interface Connection {
  mode: 'api';
  kind: 'native' | 'demo';
  name: string;
  url: string;
  user: string;
  password: string;
  sessionId?: string;
}
export interface ConnectionProfile {
  mode: 'api';
  kind: 'native' | 'demo';
  name: string;
  url: string;
  user: string;
}
export interface QueryTab {
  id: string;
  name: string;
  sql: string;
  dirty?: boolean;
  editVersion: number;
  connection?: Connection;
  sessionId?: string;
  transactionState: string;
  running: boolean;
}
export interface HistoryItem { id: string; sql: string; at: number; durationMs: number; rows: number; error?: string; mode: Connection['mode']; connection: string; action: 'compile' | 'execute' }
export interface SqlToken { type: string; text: string; line: number; column: number; endLine?: number; endColumn?: number; byteStart?: number; byteEnd?: number }

// X24 / C2 权限与审计
export interface AccessGrant { object: string; permissions: string[] }
export interface AccessUser { name: string; roles: string[]; grants: AccessGrant[]; passwordProtected: boolean }
export interface AccessRole { name: string; inherits: string[]; grants: AccessGrant[] }
export interface AccessState { version: number; users: AccessUser[]; roles: AccessRole[] }
export interface AuditEntry {
  id: string; at: string; method: string; path: string; user: string; sessionId?: string;
  sql?: string; object?: string; status: number; success: boolean; durationMs: number;
  affectedRows?: number; errorCode?: number; transactionState?: string; quarantined?: boolean;
}
export interface SessionEntry {
  sessionId: string; user: string; transactionState: string; activeRequest: boolean;
  waitingForLock: boolean; ownsTransactionLock: boolean; lastActiveAt: string;
}

export interface Capabilities {
  engine: string;
  persistence: boolean;
  maxSessions: number;
  sessionIdleMs: number;
  transactionLockTimeoutMs: number;
  cancellation: boolean;
  streamingResults: boolean;
  maxResultRows: number;
  externalSort: boolean;
  externalAggregate: boolean;
  backupRestore: boolean;
  backupIncremental: boolean;
  backupMigration: string;
  backupManifestVersions: number[];
  permissions: boolean;
  audit: boolean;
  engineAuthorization?: boolean;
  directBinaryAuth?: boolean;
  accessCatalogHotReload?: boolean;
  indexPageStorage: boolean;
  capabilities: string[];
}

export interface BackupEntry {
  name: string;
  kind: string;
  base?: string;
  bytes: number;
  createdAt?: string;
  manifestVersion?: number;
  pageFormatVersion?: number;
  walBytes?: number;
  migrationState?: 'pending' | 'ready' | 'unknown';
}

export interface BackupValidation {
  name: string;
  kind: string;
  migrated: boolean;
  manifestVersion: number;
  pageFormatVersion: number;
  pages: number;
}

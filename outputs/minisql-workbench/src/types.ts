export type Cell = string | number | boolean | null;
// 结果单元格类型：数据库返回的四类标量值。
export interface Column { name: string; type: string; primaryKey: boolean; nullable: boolean; defaultValue?: string | null; unique?: boolean; references?: { table: string; column: string } | null }
// 列结构：表名、类型、主键/可空/默认值、唯一约束和外键引用。
export interface Table { name: string; columns: Column[]; indexes?: { name: string; columns: string[]; unique: boolean }[]; rowCount: number; keys?: { primary: boolean; columns: string[] }[]; foreignKeys?: { columns: string[]; table: string; referencedColumns: string[] }[]; checks?: Record<string, unknown>[]; constraintNames?: { name: string; kind: string; index: number }[] }
// 表结构：列、索引、行数、键、外键、CHECK 约束和约束名。
export interface Diagnostic { code?: number; message: string; suggestion?: string; actual?: string; expected?: string[]; stage?: string; line?: number; column?: number; endLine?: number; endColumn?: number; statementIndex?: number }
// 诊断信息：错误码、消息、建议、源码位置和语句序号。
export interface PlanColumn { name: string; type: string; columnId?: number; identity?: string; nullable?: boolean; primaryKey?: boolean; unique?: boolean; defaultValue?: string | null }
// 计划输出列定义。
export interface PlanRow {
// 计划树中的一行节点。
  id: number;
  // 节点编号。
  nodeId?: number;
  parent: number;
  // 父节点编号；根节点通常为 0。
  detail: string;
  // 节点说明文字。
  depth?: number;
  // 节点在计划树中的深度。
  kind?: string;
  // 节点类型，例如 SeqScan、Filter、Join。
  children?: number[];
  // 孩子节点编号列表。
  output?: PlanColumn[];
  // 该节点输出的列模式。
  predicate?: unknown;
  // 过滤谓词表达式。
  projections?: unknown[];
  // 投影表达式列表。
  sortKeys?: unknown[];
  // 排序键列表。
  groupKeys?: unknown[];
  // 分组键列表。
  aggregates?: unknown[];
  // 聚合函数列表。
  estimatedRows?: number;
  // 优化器估算的行数。
  actualRows?: number;
  // 实际执行返回的行数。
  sourceSpan?: { start: { line: number; column: number }; end: { line: number; column: number } };
  outputSchema?: PlanColumn[];
}
export interface QueryResult {
// 一次查询/执行返回的完整结果对象。
  protocolVersion?: number;
  // 前后端协议版本号。
  requestId?: string;
  // 本次请求的唯一编号。
  executionStats?: { scope: 'query'; nodeStatisticsAvailable: boolean; actualRows: number; durationMs: number; loops: number; hits: number; misses: number; diskReads: number; diskWrites: number; stagedPageReads: number; stagedPageWrites: number; ioErrors: number };
  // 实际执行统计：行数、耗时、缓冲命中与磁盘 IO。
  transactionState?: string;
  // 当前事务状态。
  results?: { commitState?: string; kind?: string }[];
  // 多语句执行的每条语句摘要。
  columns: string[];
  // 结果列名。
  columnTypes?: string[];
  // 结果列类型。
  integerEncoding?: 'safe-number-or-decimal-string';
  // 整数编码策略说明。
  rows: Cell[][];
  // 结果行数据。
  rowCount?: number;
  truncated?: boolean;
  affectedRows: number;
  // 受影响行数。
  durationMs: number;
  // 本次执行总耗时（毫秒）。
  ast?: unknown;
  // 解析出的抽象语法树。
  astError?: string;
  // 语法树解析错误信息。
  plan: PlanRow[];
  // 原始逻辑计划树。
  optimizedPlan?: PlanRow[];
  // 优化后的逻辑计划树。
  optimizationRules?: unknown[];
  // 优化器应用的改写规则。
  optimizer?: { iterations: number; converged: boolean; diagnostics: { code: string; message: string }[] };
  // 优化器迭代统计与诊断。
  statements: number;
  // 本次执行的语句数量。
  tables?: Table[];
  // 查询涉及的表结构缓存。
  warning?: string;
  // 非致命告警信息。
  diagnostics?: Diagnostic[];
  // 逐条语句的诊断信息。
  tokens?: SqlToken[];
  // 词法分析得到的 Token 列表。
  stages?: Record<string, string>;
  // 各编译阶段的执行情况。
}
export interface Connection {
// 前端连接配置。
  mode: 'api';
  // 固定使用 HTTP API 模式。
  kind: 'native' | 'demo';
  // 连接类型：真实后端或演示模式。
  name: string;
  // 连接显示名称。
  url: string;
  // 后端服务地址。
  user: string;
  // 登录用户名。
  password: string;
  // 登录密码。
  sessionId?: string;
  // 后端分配的会话编号。
}
export interface ConnectionProfile {
// 保存到本地的连接配置（不含密码）。
  mode: 'api';
  // 固定使用 HTTP API 模式。
  kind: 'native' | 'demo';
  // 连接类型：真实后端或演示模式。
  name: string;
  // 连接显示名称。
  url: string;
  // 后端服务地址。
  user: string;
  // 登录用户名（不保存密码）。
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
// 查询历史记录。
export interface SqlToken { type: string; text: string; line: number; column: number; endLine?: number; endColumn?: number; byteStart?: number; byteEnd?: number }

// X24 / C2 权限与审计
export interface AccessGrant { object: string; permissions: string[] }
// 权限授予：对象名与权限列表。
export interface AccessUser { name: string; roles: string[]; grants: AccessGrant[]; passwordProtected: boolean }
// 用户权限模型。
export interface AccessRole { name: string; inherits: string[]; grants: AccessGrant[] }
// 角色权限模型。
export interface AccessState { version: number; users: AccessUser[]; roles: AccessRole[] }
// 权限目录快照。
export interface AuditEntry {
// 审计日志条目。
  id: string; at: string; method: string; path: string; user: string; sessionId?: string;
  sql?: string; object?: string; status: number; success: boolean; durationMs: number;
  affectedRows?: number; errorCode?: number; transactionState?: string; quarantined?: boolean;
}
export interface SessionEntry {
// 会话状态条目。
  sessionId: string; user: string; transactionState: string; activeRequest: boolean;
  waitingForLock: boolean; ownsTransactionLock: boolean; lastActiveAt: string;
}

export interface Capabilities {
// 后端能力声明。
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
// 备份条目元数据。
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
// 备份校验结果。
  name: string;
  kind: string;
  migrated: boolean;
  manifestVersion: number;
  pageFormatVersion: number;
  pages: number;
}

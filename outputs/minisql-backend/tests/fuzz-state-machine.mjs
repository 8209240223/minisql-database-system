// X27 状态机式 Fuzz 生成器。在 JS 中同时建模 schema 与行集合，让生成的语句"构造即合法"：
// 每个动作只在模型允许时发出，并立刻回放到模型上，因此始终知道预期结果的形态。
// 语法严格限制在 src/sql/parser.cpp 已确认支持的子集：没有 <>、BETWEEN、LIKE、CROSS JOIN、
// 序号 ORDER BY、SELECT 不带 FROM、DROP TABLE、ALTER TABLE。DROP TABLE / ALTER TABLE 仅作为
// 「引擎不支持」的负向探针出现，预期 SyntaxError，且不改变模型状态。

// 参考引擎（SQLite）与 MiniSQL 语义一致的列类型才算可差分比较：
// - int/bigint/varchar：整数截断除法、UTF-8 二进制字符串比较、三值逻辑三者一致。
// - decimal：MiniSQL 是精确十进制并以定标字符串输出，SQLite 只有浮点，输出形态不同。
// - bool：MiniSQL 输出 JSON true/false，SQLite 输出 1/0。
// - date：MiniSQL 是独立类型并渲染为 YYYY-MM-DD，SQLite 只保留原文本。
const TYPES = {
  int: { sql: 'INT', comparable: true, numeric: true },
  bigint: { sql: 'BIGINT', comparable: true, numeric: true },
  varchar: { sql: 'VARCHAR', comparable: true, numeric: false },
  varchar32: { sql: 'VARCHAR(32)', comparable: true, numeric: false },
  decimal: { sql: 'DECIMAL(10,2)', comparable: false, numeric: false },
  bool: { sql: 'BOOL', comparable: false, numeric: false },
  date: { sql: 'DATE', comparable: false, numeric: false },
};
// 字符串取值池全部落在 BMP 且低于 U+E000，UTF-8 字节序、码点序与 UTF-16 序一致，
// 两个引擎的字符串排序结果相同；同时覆盖空串、单引号转义与中文。
const TEXTS = ['alpha', 'zeta', '中文', "O'Brien", '', 'Mixed_Case'];
const DECIMALS = ['0.00', '12.34', '-7.50', '99.99'];
const DATES = ['2024-01-05', '2024-06-30', '1999-12-31', '2026-09-09'];
export const ACTION_KINDS = ['createTable', 'dropTable', 'alterTable', 'createIndex', 'dropIndex',
  'insert', 'update', 'delete', 'select', 'begin', 'commit', 'rollback'];
const MAX_TABLES = 4, MAX_ABS = 1000000, MAX_STEP = 9;

// 复用 fuzz-model.mjs 的 LCG，同一种子必须产生逐字节相同的程序。
function createRandom(seed) {
  let state = seed >>> 0;
  const next = () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state >>> 8; };
  return { next, below: bound => next() % bound, chance: percent => next() % 100 < percent,
    pick: values => values[next() % values.length] };
}

export function createState() {
  return { tables: [], nextTable: 1, nextIndex: 1, nextUnique: 1, snapshot: null };
}

const findTable = (state, name) => state.tables.find(table => table.name === name);
const dataColumns = table => table.columns.filter(column => !column.primaryKey);
const numericColumns = table => table.columns.filter(column => TYPES[column.type].numeric);
// UPDATE 只改非主键、非 UNIQUE 的列，任何谓词误差都无法破坏唯一性约束。
const writableColumns = table => table.columns.filter(column => !column.primaryKey && !column.unique);
const allIndexes = state => state.tables.flatMap(table => table.indexes.map(index => ({ ...index, table: table.name })));

function renderLiteral(type, value) {
  if (value === null) return 'NULL';
  if (type === 'varchar' || type === 'varchar32') return `'${String(value).replaceAll("'", "''")}'`;
  if (type === 'bool') return value ? 'TRUE' : 'FALSE';
  if (type === 'date') return `DATE '${value}'`;
  return String(value);
}
// 参考引擎不认识 DATE 字面量关键字，且不支持 VALUES / SET 中的 DEFAULT 关键字，
// 因此镜像语句里统一替换成具体字面量。
function renderReferenceLiteral(type, value) {
  if (type === 'date') return value === null ? 'NULL' : `'${value}'`;
  return renderLiteral(type, value);
}

function sampleValue(random, type) {
  if (type === 'int') return random.below(2001) - 1000;
  if (type === 'bigint') return random.below(2000001) - 1000000;
  if (type === 'varchar' || type === 'varchar32') return random.pick(TEXTS);
  if (type === 'decimal') return random.pick(DECIMALS);
  if (type === 'bool') return random.chance(50);
  return random.pick(DATES);
}

// ---------------------------------------------------------------------------
// 表达式：小型 AST，既能渲染成两种方言，也能在 JS 中按三值逻辑精确求值。
// ---------------------------------------------------------------------------
function renderExpression(node, qualify = null) {
  const name = column => (qualify ? `${qualify}.${column}` : column);
  switch (node.k) {
    case 'col': return name(node.name);
    case 'lit': return renderLiteral(node.type, node.value);
    case 'add': return `(${renderExpression(node.left, qualify)}${node.op}${renderExpression(node.right, qualify)})`;
    case 'cmp': return `${renderExpression(node.left, qualify)}${node.op}${renderExpression(node.right, qualify)}`;
    case 'isNull': return `${renderExpression(node.operand, qualify)} IS ${node.not ? 'NOT ' : ''}NULL`;
    case 'in': return `${renderExpression(node.left, qualify)} ${node.not ? 'NOT ' : ''}IN (${node.values.map(value => renderLiteral(node.type, value)).join(',')})`;
    case 'and': case 'or': return `(${renderExpression(node.left, qualify)} ${node.k.toUpperCase()} ${renderExpression(node.right, qualify)})`;
    case 'not': return `NOT (${renderExpression(node.operand, qualify)})`;
    case 'tautology': return '1=1';
    default: throw new Error(`Unknown expression node: ${node.k}`);
  }
}
function renderReferenceExpression(node, qualify = null) {
  const name = column => (qualify ? `${qualify}.${column}` : column);
  switch (node.k) {
    case 'col': return name(node.name);
    case 'lit': return renderReferenceLiteral(node.type, node.value);
    case 'add': return `(${renderReferenceExpression(node.left, qualify)}${node.op}${renderReferenceExpression(node.right, qualify)})`;
    case 'cmp': return `${renderReferenceExpression(node.left, qualify)}${node.op}${renderReferenceExpression(node.right, qualify)}`;
    case 'isNull': return `${renderReferenceExpression(node.operand, qualify)} IS ${node.not ? 'NOT ' : ''}NULL`;
    case 'in': return `${renderReferenceExpression(node.left, qualify)} ${node.not ? 'NOT ' : ''}IN (${node.values.map(value => renderReferenceLiteral(node.type, value)).join(',')})`;
    case 'and': case 'or': return `(${renderReferenceExpression(node.left, qualify)} ${node.k.toUpperCase()} ${renderReferenceExpression(node.right, qualify)})`;
    case 'not': return `NOT (${renderReferenceExpression(node.operand, qualify)})`;
    case 'tautology': return '1=1';
    default: throw new Error(`Unknown expression node: ${node.k}`);
  }
}

// 三值逻辑求值。仅用于 UPDATE / DELETE 命中判定，因此上游只生成可精确判定的谓词。
export function evaluate(node, row) {
  switch (node.k) {
    case 'col': return row[node.name] ?? null;
    case 'lit': return node.value;
    case 'tautology': return true;
    case 'add': {
      const left = evaluate(node.left, row), right = evaluate(node.right, row);
      if (left === null || right === null) return null;
      if (node.op === '+') return left + right;
      if (node.op === '-') return left - right;
      if (node.op === '*') return left * right;
      return Math.trunc(left / right);
    }
    case 'cmp': {
      const left = evaluate(node.left, row), right = evaluate(node.right, row);
      if (left === null || right === null) return null;
      if (node.op === '=') return left === right;
      if (node.op === '!=') return left !== right;
      if (node.op === '<') return left < right;
      if (node.op === '<=') return left <= right;
      if (node.op === '>') return left > right;
      return left >= right;
    }
    case 'isNull': {
      const value = evaluate(node.operand, row);
      return node.not ? value !== null : value === null;
    }
    case 'in': {
      const left = evaluate(node.left, row);
      if (left === null) return null;
      const hit = node.values.some(value => value === left);
      return node.not ? !hit : hit;
    }
    case 'and': {
      const left = evaluate(node.left, row), right = evaluate(node.right, row);
      if (left === false || right === false) return false;
      if (left === null || right === null) return null;
      return true;
    }
    case 'or': {
      const left = evaluate(node.left, row), right = evaluate(node.right, row);
      if (left === true || right === true) return true;
      if (left === null || right === null) return null;
      return false;
    }
    case 'not': {
      const value = evaluate(node.operand, row);
      return value === null ? null : !value;
    }
    default: throw new Error(`Unknown expression node: ${node.k}`);
  }
}
const matches = (node, row) => (node === null ? true : evaluate(node, row) === true);

// 谓词字面量优先取自模型中已有的行值，否则随机采样几乎总是筛出空结果集，
// 参考引擎就失去判别力。取值依赖当时的模型状态，仍然完全确定。
function predicateValue(random, table, column) {
  const present = table.rows.map(row => row[column.name]).filter(value => value !== null);
  return present.length > 0 && random.chance(65) ? random.pick(present) : sampleValue(random, column.type);
}

// exact=true 时只生成 JS 可精确判定的谓词：数值列做序比较，其他列只做等值和 IS NULL。
function buildPredicate(random, table, exact) {
  const numeric = numericColumns(table), terms = [];
  const count = 1 + random.below(2);
  for (let index = 0; index < count; ++index) {
    const useNumeric = numeric.length > 0 && random.chance(exact ? 70 : 55);
    const column = useNumeric ? random.pick(numeric) : random.pick(table.columns);
    const reference = { k: 'col', name: column.name, type: column.type };
    const shape = random.below(4);
    let term;
    if (shape === 0) term = { k: 'isNull', not: random.chance(50), operand: reference };
    else if (shape === 1 && (useNumeric || !exact)) {
      const operator = useNumeric ? random.pick(['=', '!=', '<', '<=', '>', '>=']) : random.pick(['=', '!=']);
      term = { k: 'cmp', op: operator, left: reference, right: { k: 'lit', type: column.type, value: predicateValue(random, table, column) } };
    } else if (shape === 2 && useNumeric && !exact) {
      // 只在非变更语句里生成列算术，避免 UPDATE 的命中集合依赖溢出边界。
      term = { k: 'cmp', op: random.pick(['<', '>=']),
        left: { k: 'add', op: random.pick(['+', '-', '*']), left: reference, right: { k: 'lit', type: 'int', value: 1 + random.below(MAX_STEP) } },
        right: { k: 'lit', type: column.type, value: predicateValue(random, table, column) } };
    } else if (shape === 3 && (useNumeric || column.type === 'varchar' || column.type === 'varchar32')) {
      const values = Array.from({ length: 1 + random.below(3) }, () => predicateValue(random, table, column));
      term = { k: 'in', not: random.chance(30), left: reference, type: column.type, values };
    } else term = { k: 'cmp', op: random.pick(['=', '!=']), left: reference, right: { k: 'lit', type: column.type, value: predicateValue(random, table, column) } };
    terms.push(random.chance(20) ? { k: 'not', operand: term } : term);
  }
  let predicate = terms[0];
  for (let index = 1; index < terms.length; ++index)
    predicate = { k: random.chance(65) ? 'and' : 'or', left: predicate, right: terms[index] };
  return random.chance(15) ? { k: 'and', left: predicate, right: { k: 'tautology' } } : predicate;
}

// ---------------------------------------------------------------------------
// 动作构造。每个 build* 返回 action 或 null（状态不允许时）。
// ---------------------------------------------------------------------------
function buildCreateTable(state, random) {
  if (state.tables.length >= MAX_TABLES) return null;
  const name = `t${state.nextTable}`;
  const columns = [{ name: 'id', type: 'int', nullable: false, primaryKey: true, unique: false, default: null, hasDefault: false }];
  const count = 1 + random.below(4);
  // 前两张表固定使用可比较类型，保证每个程序都有可差分的 SELECT 与 JOIN 素材。
  const restricted = state.nextTable <= 2;
  const pool = Object.keys(TYPES).filter(type => !restricted || TYPES[type].comparable);
  for (let index = 0; index < count; ++index) {
    const type = random.pick(pool), nullable = random.chance(55);
    const unique = type === 'int' && !nullable && random.chance(20);
    const hasDefault = !unique && random.chance(30);
    columns.push({ name: `c${index + 1}`, type, nullable: nullable && !unique, primaryKey: false, unique,
      default: hasDefault ? sampleValue(random, type) : null, hasDefault });
  }
  return { kind: 'createTable', table: name, columns, comparable: columns.every(column => TYPES[column.type].comparable) };
}

function buildCreateIndex(state, random) {
  if (!state.tables.length) return null;
  const table = random.pick(state.tables);
  // UNIQUE 索引只建在主键列上：主键值由模型独占分配且从不更新，绝不会出现意外重复。
  const unique = random.chance(30);
  const columns = unique ? ['id'] : [random.pick(table.columns).name];
  if (!unique && random.chance(35)) {
    const second = random.pick(table.columns).name;
    if (second !== columns[0]) columns.push(second);
  }
  const name = `x${state.nextIndex}`;
  if (table.indexes.some(index => index.name === name)) return null;
  return { kind: 'createIndex', table: table.name, name, unique, columns, comparable: table.comparable };
}

function buildDropIndex(state, random) {
  const indexes = allIndexes(state);
  if (!indexes.length) return null;
  const index = random.pick(indexes);
  const table = findTable(state, index.table);
  return { kind: 'dropIndex', table: index.table, name: index.name, withOn: random.chance(50), comparable: table.comparable };
}

function buildInsert(state, random) {
  if (!state.tables.length) return null;
  const table = random.pick(state.tables);
  const optional = table.columns.filter(column => !column.primaryKey && (column.nullable || column.hasDefault) && !column.unique);
  const useColumnList = random.chance(60);
  const omitted = useColumnList && optional.length > 0 && random.chance(45) ? [random.pick(optional).name] : [];
  const columns = table.columns.filter(column => !omitted.includes(column.name));
  const rowCount = 1 + random.below(4);
  const rows = [];
  for (let index = 0; index < rowCount; ++index) {
    const row = columns.map(column => {
      if (column.primaryKey) return { value: table.nextId + index, type: column.type, useDefault: false };
      if (column.unique) return { value: state.nextUnique + rows.length * 8 + index, type: column.type, useDefault: false };
      if (column.hasDefault && random.chance(25)) return { value: column.default, type: column.type, useDefault: true };
      if (column.nullable && random.chance(30)) return { value: null, type: column.type, useDefault: false };
      return { value: sampleValue(random, column.type), type: column.type, useDefault: false };
    });
    rows.push(row);
  }
  return { kind: 'insert', table: table.name, columns: columns.map(column => column.name),
    useColumnList: useColumnList || omitted.length > 0, rows, comparable: table.comparable };
}

function buildUpdate(state, random) {
  const candidates = state.tables.filter(table => writableColumns(table).length > 0 && table.rows.length > 0);
  if (!candidates.length) return null;
  const table = random.pick(candidates);
  const targets = writableColumns(table);
  const count = 1 + random.below(Math.min(2, targets.length));
  const chosen = [], assignments = [];
  for (let index = 0; index < count; ++index) {
    const column = targets[(random.below(targets.length) + index) % targets.length];
    if (chosen.includes(column.name)) continue;
    chosen.push(column.name);
    const bounded = TYPES[column.type].numeric &&
      table.rows.every(row => row[column.name] === null || Math.abs(row[column.name]) <= MAX_ABS);
    if (bounded && random.chance(40))
      // c=c±k：源列为 NOT NULL 时结果也非空，取值上界受 MAX_ABS 约束，不会触发溢出。
      assignments.push({ column: column.name, mode: 'shift', op: random.chance(50) ? '+' : '-', amount: 1 + random.below(MAX_STEP), type: column.type });
    else if (column.hasDefault && random.chance(25))
      assignments.push({ column: column.name, mode: 'default', value: column.default, type: column.type });
    else if (column.nullable && random.chance(25))
      assignments.push({ column: column.name, mode: 'literal', value: null, type: column.type });
    else assignments.push({ column: column.name, mode: 'literal', value: sampleValue(random, column.type), type: column.type });
  }
  if (!assignments.length) return null;
  const where = random.chance(75) ? buildPredicate(random, table, true) : null;
  return { kind: 'update', table: table.name, assignments, where, comparable: table.comparable };
}

function buildDelete(state, random) {
  const candidates = state.tables.filter(table => table.rows.length > 0);
  if (!candidates.length) return null;
  const table = random.pick(candidates);
  return { kind: 'delete', table: table.name, where: random.chance(85) ? buildPredicate(random, table, true) : null, comparable: table.comparable };
}

function buildSelect(state, random) {
  if (!state.tables.length) return null;
  // 优先查询有数据的表：否则大部分 SELECT 落在空表上，参考引擎失去判别力。
  // 仍保留 12% 的概率查询空表，覆盖空结果集与聚合空组路径。
  const populated = state.tables.filter(table => table.rows.length > 0);
  const base = populated.length > 0 && !random.chance(12) ? random.pick(populated) : random.pick(state.tables);
  const query = { table: base.name, alias: 'a0', join: null, where: null, distinct: false,
    groupBy: null, having: null, orderBy: [], items: [], limit: null, offset: null };
  const joinCandidates = state.tables.filter(table => numericColumns(table).length > 0 &&
    (table.rows.length > 0 || !populated.some(candidate => numericColumns(candidate).length > 0)));
  if (numericColumns(base).length > 0 && joinCandidates.length > 0 && random.chance(35)) {
    const right = random.pick(joinCandidates);
    // 参考引擎自 3.39 起才支持 RIGHT/FULL JOIN，且 MiniSQL 的外连接空值补齐路径值得单独压测，
    // 因此 RIGHT/FULL 只执行不比对。
    const type = random.pick(['INNER', 'LEFT', 'LEFT OUTER', 'RIGHT', 'FULL OUTER']);
    query.join = { table: right.name, alias: 'a1', type,
      left: random.pick(numericColumns(base)).name, right: random.pick(numericColumns(right)).name };
  }
  const scope = [{ table: base, alias: 'a0' }];
  if (query.join) scope.push({ table: findTable(state, query.join.table), alias: 'a1' });
  const visible = scope.flatMap(entry => entry.table.columns.map(column => ({ ...column, alias: entry.alias })));
  const grouped = random.chance(30) && visible.some(column => TYPES[column.type].numeric);
  if (grouped) {
    const key = random.pick(visible);
    query.groupBy = [{ alias: key.alias, name: key.name }];
    query.items.push({ expression: { k: 'col', name: key.name, type: key.type }, qualify: key.alias, label: 'g1', type: key.type });
    const aggregate = random.pick(['COUNT', 'SUM', 'MIN', 'MAX', 'AVG']);
    const argument = aggregate === 'COUNT' ? null : random.pick(aggregate === 'SUM' || aggregate === 'AVG'
      ? visible.filter(column => TYPES[column.type].numeric) : visible);
    query.items.push({ aggregate, argument: argument ? { alias: argument.alias, name: argument.name, type: argument.type } : null,
      label: 'agg1', type: aggregate === 'COUNT' ? 'bigint' : aggregate === 'AVG' ? 'decimal' : argument.type });
    if (random.chance(40)) query.having = { count: 1 + random.below(3) };
  } else {
    const count = 1 + random.below(Math.min(3, visible.length));
    const used = new Set();
    for (let index = 0; index < count; ++index) {
      const column = visible[(random.below(visible.length) + index) % visible.length];
      const label = `v${index + 1}`;
      if (used.has(`${column.alias}.${column.name}`)) continue;
      used.add(`${column.alias}.${column.name}`);
      query.items.push({ expression: { k: 'col', name: column.name, type: column.type }, qualify: column.alias, label, type: column.type });
    }
    query.distinct = random.chance(25);
  }
  if (random.chance(70)) query.where = buildPredicate(random, base, false);
  // ORDER BY 覆盖全部投影列并显式写出 NULLS 位置：MiniSQL 的 ASC 默认 NULLS LAST，
  // SQLite 默认 NULLS FIRST，不写显式位置就会出现与缺陷无关的差异。
  query.orderBy = query.items.map(item => ({ label: item.label, descending: random.chance(40), nullsFirst: random.chance(50) }));
  // LIMIT 0 与超出行数的 OFFSET 是有意保留的边界，但不能成为多数情况。
  if (random.chance(45)) { query.limit = random.chance(12) ? 0 : 1 + random.below(11); if (random.chance(40)) query.offset = random.below(4); }
  const referencedComparable = scope.every(entry => entry.table.comparable);
  const skipReason = !referencedComparable ? 'nonComparableColumnType'
    : query.join && (query.join.type === 'RIGHT' || query.join.type === 'FULL OUTER') ? 'outerJoinDirection'
    : grouped && query.items.some(item => item.aggregate === 'AVG') ? 'avgResultFormat'
    : query.where && whereTouchesNonComparable(query.where) ? 'nonComparableColumnType' : null;
  return { kind: 'select', query, comparable: skipReason === null, skipReason };
}
function whereTouchesNonComparable(node) {
  if (node.k === 'col') return !TYPES[node.type].comparable;
  if (node.k === 'lit' || node.k === 'tautology') return !TYPES[node.type ?? 'int'].comparable;
  if (node.k === 'in') return !TYPES[node.type].comparable || whereTouchesNonComparable(node.left);
  return ['left', 'right', 'operand'].some(key => node[key] && whereTouchesNonComparable(node[key]));
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------
function columnDefinition(column) {
  const parts = [column.name, TYPES[column.type].sql];
  if (column.primaryKey) parts.push('PRIMARY KEY');
  if (column.unique) parts.push('UNIQUE');
  if (!column.nullable && !column.primaryKey) parts.push('NOT NULL');
  if (column.hasDefault) parts.push(`DEFAULT ${renderLiteral(column.type, column.default)}`);
  return parts.join(' ');
}
function referenceColumnDefinition(column) {
  const parts = [column.name, TYPES[column.type].sql];
  if (column.primaryKey) parts.push('PRIMARY KEY');
  if (column.unique) parts.push('UNIQUE');
  if (!column.nullable && !column.primaryKey) parts.push('NOT NULL');
  if (column.hasDefault) parts.push(`DEFAULT ${renderReferenceLiteral(column.type, column.default)}`);
  return parts.join(' ');
}

function renderSelect(query, reference) {
  const expression = reference ? renderReferenceExpression : renderExpression;
  const items = query.items.map(item => item.aggregate
    ? `${item.aggregate}(${item.argument ? `${item.argument.alias}.${item.argument.name}` : '*'}) AS ${item.label}`
    : `${expression(item.expression, item.qualify)} AS ${item.label}`);
  let sql = `SELECT ${query.distinct ? 'DISTINCT ' : ''}${items.join(',')} FROM ${query.table} AS ${query.alias}`;
  if (query.join) sql += ` ${query.join.type} JOIN ${query.join.table} AS ${query.join.alias} ON ${query.alias}.${query.join.left}=${query.join.alias}.${query.join.right}`;
  if (query.where) sql += ` WHERE ${expression(query.where, query.alias)}`;
  if (query.groupBy) sql += ` GROUP BY ${query.groupBy.map(key => `${key.alias}.${key.name}`).join(',')}`;
  if (query.having) sql += ` HAVING COUNT(*)>=${query.having.count}`;
  sql += ` ORDER BY ${query.orderBy.map(key => `${key.label} ${key.descending ? 'DESC' : 'ASC'} NULLS ${key.nullsFirst ? 'FIRST' : 'LAST'}`).join(',')}`;
  if (query.limit !== null) sql += ` LIMIT ${query.limit}`;
  if (query.offset !== null) sql += ` OFFSET ${query.offset}`;
  return `${sql};`;
}

function renderAction(action, reference = false) {
  const literal = reference ? renderReferenceLiteral : renderLiteral;
  const expression = reference ? renderReferenceExpression : renderExpression;
  const definition = reference ? referenceColumnDefinition : columnDefinition;
  switch (action.kind) {
    case 'createTable': return `CREATE TABLE ${action.table}(${action.columns.map(definition).join(',')});`;
    case 'dropTable': return `DROP TABLE ${action.table};`;
    case 'alterTable': return `ALTER TABLE ${action.table} ADD COLUMN ${action.column} INT;`;
    case 'createIndex': return `CREATE ${action.unique ? 'UNIQUE ' : ''}INDEX ${action.name} ON ${action.table}(${action.columns.join(',')});`;
    // 参考引擎的 DROP INDEX 没有 ON 子句，镜像时去掉。
    case 'dropIndex': return `DROP INDEX ${action.name}${action.withOn && !reference ? ` ON ${action.table}` : ''};`;
    case 'insert': {
      const rows = action.rows.map(row => `(${row.map(cell => (cell.useDefault && !reference ? 'DEFAULT' : literal(cell.type, cell.value))).join(',')})`);
      return `INSERT INTO ${action.table}${action.useColumnList ? `(${action.columns.join(',')})` : ''} VALUES${rows.join(',')};`;
    }
    case 'update': {
      const sets = action.assignments.map(assignment => assignment.mode === 'shift'
        ? `${assignment.column}=${assignment.column}${assignment.op}${assignment.amount}`
        : `${assignment.column}=${assignment.mode === 'default' && !reference ? 'DEFAULT' : literal(assignment.type, assignment.value)}`);
      return `UPDATE ${action.table} SET ${sets.join(',')}${action.where ? ` WHERE ${expression(action.where)}` : ''};`;
    }
    case 'delete': return `DELETE FROM ${action.table}${action.where ? ` WHERE ${expression(action.where)}` : ''};`;
    case 'select': return renderSelect(action.query, reference);
    case 'begin': return 'BEGIN;';
    case 'commit': return 'COMMIT;';
    case 'rollback': return 'ROLLBACK;';
    default: throw new Error(`Unknown action kind: ${action.kind}`);
  }
}

// ---------------------------------------------------------------------------
// 前置条件与模型回放
// ---------------------------------------------------------------------------
export function precondition(state, action) {
  const table = action.table ? findTable(state, action.table) : null;
  switch (action.kind) {
    case 'createTable': return table ? `table ${action.table} already exists` : null;
    case 'dropTable': case 'alterTable': return table ? null : `table ${action.table} does not exist`;
    case 'createIndex':
      if (!table) return `table ${action.table} does not exist`;
      if (action.columns.some(name => !table.columns.some(column => column.name === name))) return `missing index column in ${action.table}`;
      if (allIndexes(state).some(index => index.name === action.name)) return `index ${action.name} already exists`;
      return null;
    case 'dropIndex':
      if (!table) return `table ${action.table} does not exist`;
      return table.indexes.some(index => index.name === action.name) ? null : `index ${action.name} does not exist`;
    case 'insert': {
      if (!table) return `table ${action.table} does not exist`;
      if (action.columns.some(name => !table.columns.some(column => column.name === name))) return `missing insert column in ${action.table}`;
      const required = table.columns.filter(column => !column.nullable && !column.hasDefault && !action.columns.includes(column.name));
      if (required.length) return `missing required column ${required[0].name}`;
      const keys = new Set(table.rows.map(row => row.id));
      const uniques = table.columns.filter(column => column.unique)
        .map(column => ({ name: column.name, seen: new Set(table.rows.map(row => row[column.name])) }));
      for (const row of action.rows) {
        const cells = new Map(action.columns.map((name, index) => [name, row[index]]));
        const key = cells.get('id');
        if (key === undefined || key.value === null) return 'primary key is missing or null';
        if (keys.has(key.value)) return `duplicate primary key ${key.value}`;
        keys.add(key.value);
        for (const unique of uniques) {
          const cell = cells.get(unique.name);
          const value = cell === undefined ? null : cell.value;
          if (unique.seen.has(value)) return `duplicate unique value in ${unique.name}`;
          unique.seen.add(value);
        }
        for (const [name, cell] of cells) {
          const column = table.columns.find(candidate => candidate.name === name);
          if (cell.value === null && !cell.useDefault && !column.nullable) return `NULL into NOT NULL column ${name}`;
          if (cell.useDefault && !column.hasDefault) return `DEFAULT without declared default on ${name}`;
        }
      }
      return null;
    }
    case 'update': {
      if (!table) return `table ${action.table} does not exist`;
      for (const assignment of action.assignments) {
        const column = table.columns.find(candidate => candidate.name === assignment.column);
        if (!column) return `missing update column ${assignment.column}`;
        if (column.primaryKey || column.unique) return `update targets key column ${assignment.column}`;
        if (assignment.mode === 'literal' && assignment.value === null && !column.nullable) return `NULL into NOT NULL column ${assignment.column}`;
        if (assignment.mode === 'default' && !column.hasDefault) return `DEFAULT without declared default on ${assignment.column}`;
        if (assignment.mode === 'shift') {
          if (!TYPES[column.type].numeric) return `arithmetic update on non-numeric ${assignment.column}`;
          const overflow = table.rows.some(row => row[assignment.column] !== null &&
            Math.abs(row[assignment.column] + (assignment.op === '+' ? assignment.amount : -assignment.amount)) > MAX_ABS + MAX_STEP);
          if (overflow) return `arithmetic update would exceed the modelled bound on ${assignment.column}`;
        }
      }
      return referencedColumns(action.where, table);
    }
    case 'delete': return table ? referencedColumns(action.where, table) : `table ${action.table} does not exist`;
    case 'select': {
      const base = findTable(state, action.query.table);
      if (!base) return `table ${action.query.table} does not exist`;
      const scope = [{ table: base, alias: action.query.alias }];
      if (action.query.join) {
        const right = findTable(state, action.query.join.table);
        if (!right) return `table ${action.query.join.table} does not exist`;
        if (right.name === base.name && action.query.join.alias === action.query.alias) return 'duplicate table alias';
        scope.push({ table: right, alias: action.query.join.alias });
        if (!base.columns.some(column => column.name === action.query.join.left)) return `missing join column ${action.query.join.left}`;
        if (!right.columns.some(column => column.name === action.query.join.right)) return `missing join column ${action.query.join.right}`;
      }
      const resolve = (alias, name) => scope.some(entry => entry.alias === alias && entry.table.columns.some(column => column.name === name));
      for (const item of action.query.items) {
        if (item.aggregate) { if (item.argument && !resolve(item.argument.alias, item.argument.name)) return `missing aggregate column ${item.argument.name}`; }
        else if (!resolve(item.qualify, item.expression.name)) return `missing projected column ${item.expression.name}`;
      }
      for (const key of action.query.groupBy ?? []) if (!resolve(key.alias, key.name)) return `missing group column ${key.name}`;
      return referencedColumns(action.query.where, base);
    }
    case 'begin': return state.snapshot ? 'BEGIN inside an active transaction' : null;
    case 'commit': case 'rollback': return state.snapshot ? null : `${action.kind.toUpperCase()} outside a transaction`;
    default: return `unknown action kind ${action.kind}`;
  }
}
function referencedColumns(node, table) {
  if (!node) return null;
  if (node.k === 'col') return table.columns.some(column => column.name === node.name) ? null : `missing predicate column ${node.name}`;
  for (const key of ['left', 'right', 'operand'])
    if (node[key] && typeof node[key] === 'object' && node[key].k) { const reason = referencedColumns(node[key], table); if (reason) return reason; }
  return null;
}

export function applyAction(state, action, expect = 'success') {
  // 预期失败的语句（含引擎不支持的 DDL 探针）不改变数据库状态。
  if (expect === 'error') return;
  const table = action.table ? findTable(state, action.table) : null;
  switch (action.kind) {
    case 'createTable':
      state.tables.push({ name: action.table, columns: structuredClone(action.columns), indexes: [], rows: [], nextId: 1, comparable: action.comparable });
      state.nextTable = Math.max(state.nextTable, Number(action.table.slice(1)) + 1);
      break;
    case 'createIndex':
      table.indexes.push({ name: action.name, unique: action.unique, columns: [...action.columns] });
      state.nextIndex = Math.max(state.nextIndex, Number(action.name.slice(1)) + 1);
      break;
    case 'dropIndex':
      table.indexes = table.indexes.filter(index => index.name !== action.name);
      break;
    case 'insert':
      for (const row of action.rows) {
        const record = {};
        for (const column of table.columns) {
          const position = action.columns.indexOf(column.name);
          const cell = position === -1 ? null : row[position];
          record[column.name] = cell === null || cell.useDefault ? (column.hasDefault ? column.default : null) : cell.value;
        }
        table.rows.push(record);
        table.nextId = Math.max(table.nextId, record.id + 1);
        for (const column of table.columns) if (column.unique) state.nextUnique = Math.max(state.nextUnique, record[column.name] + 1);
      }
      break;
    case 'update':
      for (const row of table.rows) {
        if (!matches(action.where, row)) continue;
        for (const assignment of action.assignments) {
          if (assignment.mode === 'shift') row[assignment.column] = row[assignment.column] === null ? null
            : row[assignment.column] + (assignment.op === '+' ? assignment.amount : -assignment.amount);
          else if (assignment.mode === 'default') row[assignment.column] = assignment.value;
          else row[assignment.column] = assignment.value;
        }
      }
      break;
    case 'delete':
      table.rows = table.rows.filter(row => !matches(action.where, row));
      break;
    case 'begin':
      state.snapshot = structuredClone({ tables: state.tables, nextTable: state.nextTable, nextIndex: state.nextIndex, nextUnique: state.nextUnique });
      break;
    case 'commit':
      state.snapshot = null;
      break;
    case 'rollback': {
      // ROLLBACK 恢复 BEGIN 时的行与 schema 快照，但主键、唯一值和对象名计数器不回退：
      // 回滚后重新分配同一个键，会让真正的隔离缺陷伪装成普通的唯一性冲突。
      const highWater = new Map(state.tables.map(table => [table.name, table.nextId]));
      const { nextUnique, nextTable, nextIndex } = state;
      state.tables = state.snapshot.tables;
      state.nextTable = Math.max(nextTable, state.snapshot.nextTable);
      state.nextIndex = Math.max(nextIndex, state.snapshot.nextIndex);
      state.nextUnique = Math.max(nextUnique, state.snapshot.nextUnique);
      for (const restored of state.tables) restored.nextId = Math.max(restored.nextId, highWater.get(restored.name) ?? 1);
      state.snapshot = null;
      break;
    }
    case 'dropTable': case 'alterTable': case 'select': break;
    default: throw new Error(`Unknown action kind: ${action.kind}`);
  }
}

// ---------------------------------------------------------------------------
// 程序生成
// ---------------------------------------------------------------------------
const WEIGHTS = [['createTable', 8], ['createIndex', 8], ['dropIndex', 4], ['insert', 22], ['update', 14],
  ['delete', 8], ['select', 22], ['begin', 6], ['dropTable', 3], ['alterTable', 3]];

export function generateProgram(seed, steps, options = {}) {
  if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff) throw new Error('seed must be a UINT32');
  if (!Number.isInteger(steps) || steps < 1 || steps > 512) throw new Error('steps must be 1..512');
  const probeUnsupportedDdl = options.probeUnsupportedDdl ?? true;
  const random = createRandom(seed), state = createState(), program = [];
  const pool = WEIGHTS.filter(([kind]) => probeUnsupportedDdl || (kind !== 'dropTable' && kind !== 'alterTable'))
    .flatMap(([kind, weight]) => Array.from({ length: weight }, () => kind));
  let transactionLength = 0;
  const emit = action => {
    const expect = action.kind === 'dropTable' || action.kind === 'alterTable' ? 'error' : 'success';
    if (precondition(state, action)) return false;
    // comparable 只描述「结果集能与参考引擎逐行比对」，因此只有 SELECT 可能为真；
    // 其余语句仍然会检查接受/拒绝，不计入 skipped。
    const step = { index: program.length, kind: action.kind, action, expect, sql: renderAction(action),
      comparable: expect === 'success' && action.kind === 'select' && !action.skipReason,
      skipReason: action.kind === 'select' ? action.skipReason ?? null : null };
    if (expect === 'error') {
      // 引擎不支持这两条 DDL：DROP TABLE 在 expect("INDEX") 处报错，位置是 TABLE 记号（第 1 行第 6 列）；
      // ALTER 在 statement() 分派处报错，位置是语句首记号（第 1 行第 1 列）。
      step.errorType = 'SyntaxError';
      step.errorLine = 1;
      step.errorColumn = action.kind === 'dropTable' ? 6 : 1;
      step.unsupportedByEngine = true;
      step.comparable = false;
      step.skipReason = 'unsupportedByEngine';
    }
    step.referenceSql = referenceSqlFor(step);
    program.push(step);
    applyAction(state, action, expect);
    if (action.kind === 'begin') transactionLength = 0;
    else if (state.snapshot) ++transactionLength;
    return true;
  };
  for (let guard = 0; program.length < steps; ++guard) {
    if (guard > steps * 64) throw new Error('generateProgram made no progress');
    const remaining = steps - program.length;
    if (state.snapshot && (transactionLength >= 4 || remaining <= 1 || random.chance(30))) {
      emit({ kind: random.chance(35) ? 'rollback' : 'commit' });
      continue;
    }
    const builders = { createTable: buildCreateTable, createIndex: buildCreateIndex, dropIndex: buildDropIndex,
      insert: buildInsert, update: buildUpdate, delete: buildDelete, select: buildSelect };
    let kind = random.pick(pool);
    // 事务内不放负向探针：脚本在第一条错误处终止，会掩盖事务尾部的语句。
    if (state.snapshot && (kind === 'dropTable' || kind === 'alterTable')) kind = 'insert';
    if (kind === 'begin') { if (!state.snapshot && remaining > 2) emit({ kind: 'begin' }); continue; }
    if (kind === 'dropTable') { if (state.tables.length) emit({ kind: 'dropTable', table: random.pick(state.tables).name }); continue; }
    if (kind === 'alterTable') { if (state.tables.length) emit({ kind: 'alterTable', table: random.pick(state.tables).name, column: 'c9' }); continue; }
    const action = builders[kind](state, random);
    if (action) emit(action);
    else if (!state.tables.length) emit(buildCreateTable(state, random));
  }
  // 脚本结束时必须没有活跃事务：MiniSQL 会把未提交事务回滚并整体判失败。
  if (state.snapshot) { program.push(closingStep(program.length, 'commit')); applyAction(state, { kind: 'commit' }); }
  return program;
}
function closingStep(index, kind) {
  const step = { index, kind, action: { kind }, expect: 'success', sql: renderAction({ kind }), comparable: false, skipReason: null };
  step.referenceSql = renderAction({ kind }, true);
  return step;
}
function referenceSqlFor(step) {
  if (step.expect === 'error') return null;
  if (step.kind === 'begin' || step.kind === 'commit' || step.kind === 'rollback') return renderAction(step.action, true);
  // 只镜像全部列类型都可比较的表，其余表在参考引擎里根本不存在。
  if (!(step.action.comparable ?? true)) return null;
  if (step.kind === 'select') return step.comparable ? renderAction(step.action, true) : null;
  return renderAction(step.action, true);
}

export function renderProgram(program) {
  return program.map(step => step.sql).join('\n');
}

// 把程序切成执行块：事务整体在一个进程内执行（事务状态不跨进程），
// 其余语句各自独立成块，便于精确归因错误。
export function chunkProgram(program) {
  const chunks = [];
  let open = null;
  for (const step of program) {
    if (step.kind === 'begin') { open = { steps: [step], transaction: true }; chunks.push(open); continue; }
    if (open) {
      open.steps.push(step);
      if (step.kind === 'commit' || step.kind === 'rollback') open = null;
      continue;
    }
    chunks.push({ steps: [step], transaction: false });
  }
  return chunks.map((chunk, index) => ({ index, transaction: chunk.transaction, steps: chunk.steps,
    sql: chunk.steps.map(step => step.sql).join(' '),
    expect: chunk.steps.some(step => step.expect === 'error') ? 'error' : 'success' }));
}

// 用新模型重放整个程序，确认没有悬空表引用、缺列、重复键或不平衡事务。
export function validateProgram(program) {
  const state = createState();
  for (const step of program) {
    const reason = precondition(state, step.action);
    if (reason) return { valid: false, index: step.index, reason };
    if (step.sql !== renderAction(step.action)) return { valid: false, index: step.index, reason: 'rendered SQL does not match the action' };
    applyAction(state, step.action, step.expect);
  }
  if (state.snapshot) return { valid: false, index: program.length, reason: 'program ends inside an active transaction' };
  return { valid: true, index: null, reason: null };
}

// 逐步删除语句做 delta debugging。任何让程序变得非法的候选都直接丢弃，
// 否则真实缺陷会被伪装成「非法 SQL」样本。
export async function minimizeProgram(program, stillFails, budget = 128) {
  let current = structuredClone(program), attempts = 0;
  if (!validateProgram(current).valid) return { program: current, attempts: 0 };
  let changed = true;
  while (changed && attempts < budget) {
    changed = false;
    for (let index = current.length - 1; index >= 0 && attempts < budget; --index) {
      for (const candidate of removalCandidates(current, index)) {
        if (attempts >= budget) break;
        if (!validateProgram(candidate).valid) continue;
        ++attempts;
        if (!await stillFails(candidate)) continue;
        current = candidate;
        changed = true;
        break;
      }
      if (changed) break;
    }
  }
  const check = validateProgram(current);
  if (!check.valid) throw new Error(`minimizeProgram produced an invalid program: ${check.reason}`);
  return { program: current, attempts: Math.min(attempts, budget) };
}
function reindex(steps) {
  return steps.map((step, index) => ({ ...step, index }));
}
// 单条删除会让 BEGIN/COMMIT 失配，所以对事务首步额外提供两种成对候选：
// 整块删除，以及只删除事务边界、保留其中的语句。
function removalCandidates(program, index) {
  const drop = positions => reindex(program.filter((_, position) => !positions.has(position)));
  const candidates = [drop(new Set([index]))];
  if (program[index].kind !== 'begin') return candidates;
  const closer = program.findIndex((step, position) => position > index && (step.kind === 'commit' || step.kind === 'rollback'));
  if (closer === -1) return candidates;
  candidates.unshift(drop(new Set(Array.from({ length: closer - index + 1 }, (_, offset) => index + offset))));
  if (program[closer].kind === 'commit') candidates.push(drop(new Set([index, closer])));
  return candidates;
}

export function describe(program) {
  const kinds = Object.fromEntries(ACTION_KINDS.map(kind => [kind, 0]));
  const skipReasons = {};
  let comparableSelects = 0, skippedSelects = 0, errorProbes = 0, mirrored = 0;
  for (const step of program) {
    ++kinds[step.kind];
    if (step.referenceSql) ++mirrored;
    if (step.expect === 'error') ++errorProbes;
    if (step.kind !== 'select') continue;
    if (step.comparable) ++comparableSelects;
    else { ++skippedSelects; skipReasons[step.skipReason ?? 'unknown'] = (skipReasons[step.skipReason ?? 'unknown'] ?? 0) + 1; }
  }
  const state = createState();
  for (const step of program) applyAction(state, step.action, step.expect);
  return { steps: program.length, kinds, comparableSelects, skippedSelects, skipReasons, errorProbes,
    mirroredStatements: mirrored, transactions: kinds.begin, tables: state.tables.length,
    rows: state.tables.reduce((total, table) => total + table.rows.length, 0),
    indexes: allIndexes(state).length,
    chunks: chunkProgram(program).length };
}

// 与旧 SELECT 差分共用的固定数据集保持不变，程序模式自己建表，不需要 fixture。
export const UNSUPPORTED_STATEMENTS = ['dropTable', 'alterTable'];

import { acceptCompletion, autocompletion, completionStatus, type Completion } from '@codemirror/autocomplete';
import { syntaxTree } from '@codemirror/language';
import { Facet, Prec, RangeSetBuilder, type EditorState } from '@codemirror/state';
import { Decoration, type DecorationSet, EditorView, ViewPlugin, type ViewUpdate, WidgetType, keymap } from '@codemirror/view';
import type { SQLNamespace } from '@codemirror/lang-sql';
import type { Table } from './types';

// ---------- schema ----------

// 把后端目录转成 CodeMirror schema：顶层是表名，表下挂列名。
// 交给 sql({ schema }) 后，Ctrl+Space 的下拉列表就能给出表名/列名/列类型。
export function buildSqlSchema(tables: Table[]): SQLNamespace {
  const schema: Record<string, SQLNamespace> = {};
  for (const table of tables) {
    const columns: Completion[] = table.columns.map(column => ({
      label: column.name,
      type: column.primaryKey ? 'keyword' : 'property',
      detail: column.type,
    }));
    schema[table.name] = columns;
  }
  return schema;
}

export function sqlSchemaFor(tables: Table[]): SQLNamespace | undefined {
  const schema = buildSqlSchema(tables);
  return Object.keys(schema).length > 0 ? schema : undefined;
}

// ---------- 行内补全候选 ----------

// 常用 SQL 关键字：没有连接数据库时也能补全。
const SQL_KEYWORDS = [
  'SELECT','FROM','WHERE','INSERT','INTO','VALUES','UPDATE','SET','DELETE','CREATE','TABLE','DROP','ALTER',
  'INDEX','JOIN','INNER','LEFT','RIGHT','FULL','OUTER','CROSS','ON','USING','GROUP','BY','HAVING','ORDER',
  'ASC','DESC','LIMIT','OFFSET','DISTINCT','AS','AND','OR','NOT','NULL','IS','IN','EXISTS','BETWEEN','LIKE',
  'CASE','WHEN','THEN','ELSE','END','PRIMARY','KEY','FOREIGN','REFERENCES','UNIQUE','CHECK','DEFAULT',
  'CONSTRAINT','BEGIN','COMMIT','ROLLBACK','TRANSACTION','SAVEPOINT','CHECKPOINT','ANALYZE','EXPLAIN',
  'INT','BIGINT','FLOAT','DECIMAL','VARCHAR','BOOL','DATE','COUNT','SUM','AVG','MIN','MAX',
];

// 一条候选：word 是完整词，detail 是提示信息。
interface Candidate { word: string; detail: string }
// 当前生效的行内建议。
interface InlineSuggestion { text: string; prefix: string; pos: number }

// 候选来源：表名 + 全部列名 + 关键字。表名/列名排在关键字前面，优先补全数据库对象。
export function collectCandidates(tables: Table[]): Candidate[] {
  const seen = new Set<string>();
  const candidates: Candidate[] = [];
  const push = (word: string, detail: string) => {
    const lower = word.toLowerCase();
    if (seen.has(lower)) return;
    seen.add(lower);
    candidates.push({ word, detail });
  };
  for (const table of tables) {
    push(table.name, '表');
    for (const column of table.columns) push(column.name, `${table.name}.${column.name} · ${column.type}`);
  }
  for (const keyword of SQL_KEYWORDS) push(keyword, '关键字');
  return candidates;
}

// 候选通过 Facet 暴露给 keymap 与 ViewPlugin，避免在两个地方各存一份。
const candidateFacet = Facet.define<Candidate[], Candidate[]>({
  combine: values => (values.length ? values[values.length - 1] : []),
});

// ---------- 幽灵文本渲染 ----------

class GhostWidget extends WidgetType {
  // 用显式字段而不是构造函数参数属性：Node 的 strip-only 模式不支持后者。
  text: string;
  constructor(text: string) { super(); this.text = text; }
  toDOM() {
    const span = document.createElement('span');
    span.className = 'mini-sql-inline-completion';
    span.textContent = this.text;
    span.contentEditable = 'false';
    return span;
  }
  eq(other: GhostWidget) { return other.text === this.text; }
}

// 纯函数：从候选表里按前缀挑一个词。选最短匹配，让 students 优先于 students_backup；
// 同长时按字母序，保证同一前缀每次都得到相同结果。
export function pickCandidate(candidates: Candidate[], prefix: string): string | null {
  const lower = prefix.toLowerCase();
  const matches = candidates
    .filter(candidate => {
      const word = candidate.word.toLowerCase();
      return word.startsWith(lower) && word !== lower;
    })
    .sort((a, b) => a.word.length - b.word.length || a.word.localeCompare(b.word));
  return matches.length ? matches[0].word : null;
}

// 让补全结果跟随用户已输入部分的大小写风格，避免小写输入配出大写结果。
// 引擎对标识符大小写不敏感，这里只影响显示与插入文本的可读性，不改变语义。
export function applyCaseStyle(word: string, prefix: string): string {
  const hasLower = /[a-z]/.test(prefix);
  const hasUpper = /[A-Z]/.test(prefix);
  // 全大写输入 → 全大写结果，例如 SEL → SELECT
  if (hasUpper && !hasLower) return word.toUpperCase();
  // 全小写输入 → 全小写结果，例如 sel → select
  if (hasLower && !hasUpper) return word.toLowerCase();
  // 混合输入 → 首字母大写，其余小写，例如 Sel → Select
  return word.charAt(0).toUpperCase() + word.slice(1).toLowerCase();
}

// 判断光标位置是否落在注释或字符串里。这两种位置不是标识符，
// 弹补全只会打扰用户，所以直接不给建议。
function inCommentOrString(state: EditorState, pos: number): boolean {
  let node = syntaxTree(state).resolveInner(pos, -1);
  while (node) {
    if (node.name === 'LineComment' || node.name === 'BlockComment' || node.name === 'String') return true;
    if (!node.parent) break;
    node = node.parent;
  }
  return false;
}

// 根据当前光标位置和候选表算出建议；无法确定时返回 null。
// keymap 可以直接重算，不必依赖 ViewPlugin 的内部状态。
function pickSuggestion(state: EditorState): InlineSuggestion | null {
  if (state.selection.ranges.length !== 1) return null;
  const range = state.selection.main;
  if (!range.empty) return null;
  // 注释与字符串内不提示
  if (inCommentOrString(state, range.head)) return null;
  const line = state.doc.lineAt(range.head);
  const before = line.text.slice(0, range.head - line.from);
  const match = /[A-Za-z_][A-Za-z0-9_$]*$/.exec(before);
  if (!match) return null;
  const prefix = match[0];
  if (prefix.length < 1) return null;
  const word = pickCandidate(state.facet(candidateFacet), prefix);
  if (!word) return null;
  // 按输入的大小写风格给出补全结果，避免小写输入配出大写词
  return { text: applyCaseStyle(word, prefix), prefix, pos: range.head };
}

// ViewPlugin 只负责把建议画成灰色文本；下拉面板打开时隐藏，避免两个提示同时出现。
const inlinePlugin = ViewPlugin.fromClass(
  class {
    decorations: DecorationSet = Decoration.none;
    view: EditorView;
    constructor(view: EditorView) { this.view = view; this.compute(view.state); }
    update(update: ViewUpdate) { this.compute(update.state); }
    private compute(state: EditorState) {
      const dropdownActive = completionStatus(state) !== null;
      const suggestion = dropdownActive ? null : pickSuggestion(state);
      const builder = new RangeSetBuilder<Decoration>();
      if (suggestion) {
        const suffix = suggestion.text.slice(suggestion.prefix.length);
        if (suffix) {
          builder.add(suggestion.pos, suggestion.pos, Decoration.widget({ widget: new GhostWidget(suffix), side: 1 }));
        }
      }
      this.decorations = builder.finish();
    }
  },
  { decorations: v => v.decorations },
);

// 接受行内建议：只插入前缀之后的部分，光标停在补全词末尾。
function acceptInline(view: EditorView): boolean {
  const suggestion = pickSuggestion(view.state);
  if (!suggestion) return false;
  const suffix = suggestion.text.slice(suggestion.prefix.length);
  view.dispatch({
    changes: { from: suggestion.pos, insert: suffix },
    selection: { anchor: suggestion.pos + suffix.length },
    userEvent: 'input.complete',
  });
  return true;
}

// ---------- Tab 调度 ----------
// 优先级：下拉面板打开时选下拉项 → 否则接受行内灰色补全 → 都没有则放行给缩进。
function tabCommand(view: EditorView): boolean {
  if (completionStatus(view.state) === 'active') return acceptCompletion(view);
  return acceptInline(view);
}

export const completionTabKeymap = Prec.highest(keymap.of([
  { key: 'Tab', run: tabCommand },
]));

// ---------- 下拉列表 ----------
// activateOnTyping 关闭：输入时只显示行内灰色补全，不自动弹列表。
// 需要浏览全部候选时按 Ctrl+Space（autocompletion 的默认键位）主动打开。
export const completionBaseExtension = autocompletion({
  activateOnTyping: false,
  maxRenderedOptions: 60,
});

// ---------- 主题 ----------

export const inlineCompletionTheme = EditorView.theme({
  '.mini-sql-inline-completion': {
    color: '#9ca3af',
    pointerEvents: 'none',
    userSelect: 'none',
  },
});

// 行内补全 + 样式；App.tsx 里与候选 Facet 一起展开。
export function inlineCompletionExtensions(tables: Table[]) {
  return [candidateFacet.of(collectCandidates(tables)), inlinePlugin, inlineCompletionTheme];
}

// ---------- 诊断 ----------

export function isCompletionOpen(state: EditorState) {
  return completionStatus(state) === 'active';
}

export { acceptCompletion };

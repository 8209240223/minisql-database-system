import { StandardSQL } from '@codemirror/lang-sql';
// 引入 CodeMirror 的标准 SQL 方言解析器。

export function mutationWarning(source: string): string | undefined {
// 解析 SQL 文本，提示可能影响整表的 UPDATE/DELETE。
  const tree = StandardSQL.language.parser.parse(source);
// 用 SQL 语法树分析语句结构。
  let uncertain = false;
// 标记语法树中是否存在错误节点，便于提示无法确定范围。
  tree.iterate({ enter(node) { if (node.type.isError) uncertain = true; } });
// 遍历语法树，发现错误节点就记录不确定状态。
  let fullTable = 0;
// 统计没有顶层 WHERE 的整表更新/删除语句数。
  for (let statement = tree.topNode.firstChild; statement; statement = statement.nextSibling) {
// 逐条顶层语句检查。
    if (statement.name !== 'Statement') continue;
    // 只处理顶层语句节点。
    const tokens: string[] = [];
    // 收集语句顶层的关键字。
    for (let node = statement.firstChild; node; node = node.nextSibling) {
      // 遍历语句的直接子节点。
      if (node.name === 'LineComment' || node.name === 'BlockComment') continue;
      // 注释不参与关键字判断。
      tokens.push(node.name === 'Keyword' ? source.slice(node.from, node.to).toUpperCase() : '');
      // 关键字转大写后放入列表，其他节点用空串占位。
    }
    // 只认语句顶层 WHERE，括号内的子查询条件不能保护外层写入。
    if ((tokens[0] === 'UPDATE' || tokens[0] === 'DELETE') && !tokens.includes('WHERE')) ++fullTable;
// 顶层关键字是 UPDATE/DELETE 且没有 WHERE 时计为整表操作。
  }
  if (fullTable) return `此批次包含 ${fullTable} 条没有 WHERE 条件的 UPDATE/DELETE，可能修改或删除整表数据。确认执行？`;
// 存在整表写操作时返回高风险提示。
  if (uncertain && /\b(?:UPDATE|DELETE)\b/i.test(source)) return '无法完整识别此批次的更新或删除范围，可能影响整表数据。确认执行？';
// 语法不确定且包含写操作时也提示风险。
  return undefined;
// 没有风险时不提示。
}

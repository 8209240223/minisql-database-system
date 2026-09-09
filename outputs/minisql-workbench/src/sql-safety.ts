import { StandardSQL } from '@codemirror/lang-sql';

export function mutationWarning(source: string): string | undefined {
  const tree = StandardSQL.language.parser.parse(source);
  let uncertain = false;
  tree.iterate({ enter(node) { if (node.type.isError) uncertain = true; } });
  let fullTable = 0;
  for (let statement = tree.topNode.firstChild; statement; statement = statement.nextSibling) {
    if (statement.name !== 'Statement') continue;
    const tokens: string[] = [];
    for (let node = statement.firstChild; node; node = node.nextSibling) {
      if (node.name === 'LineComment' || node.name === 'BlockComment') continue;
      tokens.push(node.name === 'Keyword' ? source.slice(node.from, node.to).toUpperCase() : '');
    }
    // 只认语句顶层 WHERE，括号内的子查询条件不能保护外层写入。
    if ((tokens[0] === 'UPDATE' || tokens[0] === 'DELETE') && !tokens.includes('WHERE')) ++fullTable;
  }
  if (fullTable) return `此批次包含 ${fullTable} 条没有 WHERE 条件的 UPDATE/DELETE，可能修改或删除整表数据。确认执行？`;
  if (uncertain && /\b(?:UPDATE|DELETE)\b/i.test(source)) return '无法完整识别此批次的更新或删除范围，可能影响整表数据。确认执行？';
  return undefined;
}

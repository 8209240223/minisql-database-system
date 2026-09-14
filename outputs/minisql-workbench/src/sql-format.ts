export async function formatMiniSql(sql: string): Promise<string> {
// 使用 sql-formatter 把 SQL 统一格式化为大写关键字的两空格缩进文本。
  if (!sql.trim()) return sql;
// 空白输入直接原样返回。
  const { format } = await import('sql-formatter');
// 按需加载格式化库，避免首屏体积过大。
  return format(sql, {
// 调用格式化函数并传入风格选项。
    language: 'sql',
    // 按标准 SQL 语法格式化。
    keywordCase: 'upper',
    // 关键字统一转大写。
    tabWidth: 2,
    // 使用两个空格缩进。
    linesBetweenQueries: 1,
    // 多条语句之间保留一个空行。
  });
// 格式化选项结束。
}

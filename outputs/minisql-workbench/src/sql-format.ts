export async function formatMiniSql(sql: string): Promise<string> {
  if (!sql.trim()) return sql;
  const { format } = await import('sql-formatter');
  return format(sql, {
    language: 'sql',
    keywordCase: 'upper',
    tabWidth: 2,
    linesBetweenQueries: 1,
  });
}

export const MAX_SQL_FILE_BYTES = 8 * 1024 * 1024;
export function decodeSqlFile(bytes: Uint8Array): string {
  if (bytes.byteLength > MAX_SQL_FILE_BYTES) throw new Error('SQL 文件不能超过 8 MiB。');
  try { return new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
  catch { throw new Error('SQL 文件必须使用有效的 UTF-8 编码。'); }
}
export function sqlFilename(name: string): string {
  const clean = name.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').trim().replace(/[. ]+$/, '') || 'query';
  return /\.sql$/i.test(clean) ? clean : `${clean}.sql`;
}

export const MAX_SQL_FILE_BYTES = 8 * 1024 * 1024;
// SQL 文件大小上限：8 MiB。
export function decodeSqlFile(bytes: Uint8Array): string {
// 把上传文件字节解码成 UTF-8 文本，并检查大小。
  if (bytes.byteLength > MAX_SQL_FILE_BYTES) throw new Error('SQL 文件不能超过 8 MiB。');
// 超过大小上限直接拒绝。
  try { return new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
// 严格按 UTF-8 解码，非法字节会抛错。
  catch { throw new Error('SQL 文件必须使用有效的 UTF-8 编码。'); }
// 非法编码转成用户可读错误。
}
export function sqlFilename(name: string): string {
// 把用户提供的名称清理成安全的 .sql 文件名。
  const clean = name.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').trim().replace(/[. ]+$/, '') || 'query';
// 替换非法字符并去掉末尾的点和空格。
  return /\.sql$/i.test(clean) ? clean : `${clean}.sql`;
// 确保文件名以 .sql 结尾。
}

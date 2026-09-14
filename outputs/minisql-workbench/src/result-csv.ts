import { stringify } from 'csv-stringify/browser/esm/sync';
// 引入 CSV 序列化库，用于生成标准 CSV/TSV 文本。
import type { Cell } from './types';
// 引入结果单元格类型。

export interface CsvOptions { formulaProtection: boolean; distinguishNull: boolean }
// 导出选项：是否做公式注入防护，以及是否区分 NULL 与空字符串。

function protect(value: string, options: CsvOptions): string {
// 对可能被表格软件当作公式的文本做前缀保护。
  return options.formulaProtection && (/^[\u0000-\u0020]*[=+@-]/.test(value) || /^[\t\r\n]/.test(value)) ? `'${value}` : value;
// 公式防护开启且文本以危险字符开头时，前面补单引号。
}
function encodeCell(value: Cell, options: CsvOptions): string {
// 把单个结果单元格转换成 CSV 字段文本。
    if (value === null) return options.distinguishNull ? '\\N' : '';
// NULL 按配置编码成 \N 或空字段。
    if (typeof value === 'number') {
// 数值类型需要检查有限性和整数精度。
      if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) throw new Error('结果含非有限数值或已失去精度的整数，无法可靠导出。');
// 非有限数或超出安全整数范围时拒绝导出，避免静默丢精度。
      return String(value);
// 通过检查后转成字符串。
    }
// 数值分支结束。
    if (typeof value === 'boolean') return String(value);
// 布尔值直接转成 true/false 文本。
    return protect(options.distinguishNull ? value.replace(/\\/g, '\\\\') : value, options);
// 字符串按配置处理反斜杠后做公式防护。
}
export function resultCsv(columns: string[], rows: Cell[][], options: CsvOptions): string {
// 把完整结果导出为 CSV 文本。
  if (rows.some(row => row.length !== columns.length)) throw new Error('结果列数不一致，无法导出。');
// 任一行列数与表头不一致时拒绝导出。
  if (!columns.length) return '';
// 没有列时返回空文本。
  return stringify([columns.map(value => protect(value, options)), ...rows.map(row => row.map(value => encodeCell(value, options)))], { record_delimiter: '\r\n', quoted_match: /[\r\n]/, bom: true });
// 先写表头，再逐行编码，并输出带 BOM 的 CSV。
}

export function resultTsv(rows: Cell[][], options: CsvOptions): string {
// 把选区导出为 TSV 文本，便于直接粘贴到表格软件。
  if (!rows.length) return '';
// 没有行时返回空文本。
  if (rows.some(row => row.length !== rows[0].length)) throw new Error('选区列数不一致，无法复制。');
// 选区行宽不一致时拒绝复制。
  return stringify(rows.map(row => row.map(value => encodeCell(value, options))), { delimiter: '\t', record_delimiter: '\r\n', quoted_match: /[\r\n]/, eof: false });
// 用制表符分隔编码后的单元格。
}

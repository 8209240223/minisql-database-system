import { stringify } from 'csv-stringify/browser/esm/sync';
import type { Cell } from './types';

export interface CsvOptions { formulaProtection: boolean; distinguishNull: boolean }

function protect(value: string, options: CsvOptions): string {
  return options.formulaProtection && (/^[\u0000-\u0020]*[=+@-]/.test(value) || /^[\t\r\n]/.test(value)) ? `'${value}` : value;
}
function encodeCell(value: Cell, options: CsvOptions): string {
    if (value === null) return options.distinguishNull ? '\\N' : '';
    if (typeof value === 'number') {
      if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) throw new Error('结果含非有限数值或已失去精度的整数，无法可靠导出。');
      return String(value);
    }
    if (typeof value === 'boolean') return String(value);
    return protect(options.distinguishNull ? value.replace(/\\/g, '\\\\') : value, options);
}
export function resultCsv(columns: string[], rows: Cell[][], options: CsvOptions): string {
  if (rows.some(row => row.length !== columns.length)) throw new Error('结果列数不一致，无法导出。');
  if (!columns.length) return '';
  return stringify([columns.map(value => protect(value, options)), ...rows.map(row => row.map(value => encodeCell(value, options)))], { record_delimiter: '\r\n', quoted_match: /[\r\n]/, bom: true });
}

export function resultTsv(rows: Cell[][], options: CsvOptions): string {
  if (!rows.length) return '';
  if (rows.some(row => row.length !== rows[0].length)) throw new Error('选区列数不一致，无法复制。');
  return stringify(rows.map(row => row.map(value => encodeCell(value, options))), { delimiter: '\t', record_delimiter: '\r\n', quoted_match: /[\r\n]/, eof: false });
}

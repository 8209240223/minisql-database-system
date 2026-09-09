export interface DiagnosticLocation {
  line: number;
  column: number;
  offset: number;
  length: number;
  source: string;
}
export function mapDiagnostic(source: string, from: number, to: number, line: number, column: number): DiagnosticLocation | undefined {
  if (![from, to, line, column].every(Number.isSafeInteger) || from < 0 || to < from || to > source.length || line < 1 || column < 1) return;
  const submitted = source.slice(from, to);
  const breaks = /\r\n|\r|\n/g;
  let start = 0;
  for (let index = 1; index < line; ++index) {
    const next = breaks.exec(submitted);
    if (!next) return;
    start = next.index + next[0].length;
  }
  breaks.lastIndex = start;
  const next = breaks.exec(submitted);
  const text = submitted.slice(start, next?.index ?? submitted.length);
  const characters = Array.from(text);
  if (column > characters.length + 1) return;
  const rawOffset = from + start + characters.slice(0, column - 1).join('').length;
  const prefix = source.slice(0, rawOffset);
  const lines = prefix.split(/\r\n|\r|\n/);
  // 后端列号按码点计数，CodeMirror 的偏移按 UTF-16 且换行规范化后计数。
  return { line: lines.length, column: Array.from(lines.at(-1)!).length + 1,
    offset: prefix.replace(/\r\n|\r/g, '\n').length,
    length: characters[column - 1]?.length ?? 0,
    source: source.replace(/\r\n|\r/g, '\n') };
}

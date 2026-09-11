import { useEffect, useMemo, useRef, useState } from 'react';
import { Copy, Download, Table2 } from 'lucide-react';
import type { QueryResult } from './types';
import { resultCsv, resultTsv } from './result-csv';
import { calculateVirtualRange } from './virtual-window';

type Position = { row: number; column: number };

export function Results({ result, rowLimit }: { result: QueryResult | null; rowLimit?: number }) {
  rowLimit ??= Math.max(100, Math.min(10000, Number(localStorage.getItem('minisql-result-limit') ?? 1000) || 1000));
  const [formulaProtection, setFormulaProtection] = useState(true);
  const [distinguishNull, setDistinguishNull] = useState(true);
  const [exportError, setExportError] = useState('');
  const [selection, setSelection] = useState<{ source: QueryResult; anchor: Position; focus: Position } | null>(null);
  const [copied, setCopied] = useState(false);
  const scrollRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewportHeight, setViewportHeight] = useState(360);
  useEffect(() => { setSelection(null); setExportError(''); setCopied(false); }, [result]);
  useEffect(() => {
    const element = scrollRef.current;
    if (!element) return;
    const measure = () => setViewportHeight(Math.max(0, element.clientHeight - 34));
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [result]);
  const visibleRows = useMemo(() => result?.rows.slice(0, rowLimit) ?? [], [result, rowLimit]);
  const virtualRange = calculateVirtualRange({ rowCount: visibleRows.length, rowHeight: 30, viewportHeight, scrollTop });
  const current = selection?.source === result ? selection : null;
  const bounds = current ? { top: Math.min(current.anchor.row,current.focus.row), bottom: Math.max(current.anchor.row,current.focus.row), left: Math.min(current.anchor.column,current.focus.column), right: Math.max(current.anchor.column,current.focus.column) } : null;
  function select(position: Position, extend: boolean) {
    if (!result) return;
    setSelection({ source: result, anchor: extend && current ? current.anchor : position, focus: position });
    setCopied(false); setExportError('');
  }
  async function copy() {
    if (!result || !bounds) return;
    try {
      if (!navigator.clipboard?.writeText) throw new Error('当前浏览器不允许访问剪贴板。');
      const rows = result.rows.slice(bounds.top, bounds.bottom+1).map(row=>row.slice(bounds.left,bounds.right+1));
      await navigator.clipboard.writeText(resultTsv(rows, { formulaProtection, distinguishNull }));
      setCopied(true); setExportError('');
    } catch (error) { setExportError(error instanceof Error ? error.message : String(error)); }
  }
  function cellKeys(event: React.KeyboardEvent<HTMLTableCellElement>, position: Position) {
    if (!result) return;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'c') { event.preventDefault(); void copy(); return; }
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a') {
      event.preventDefault(); setSelection({source:result,anchor:{row:0,column:0},focus:{row:result.rows.length-1,column:result.columns.length-1}});setCopied(false);return;
    }
    const delta = {ArrowUp:[-1,0],ArrowDown:[1,0],ArrowLeft:[0,-1],ArrowRight:[0,1]}[event.key];
    if (!delta) return;
    event.preventDefault();
    const next = {row:Math.max(0,Math.min(result.rows.length-1,position.row+delta[0])),column:Math.max(0,Math.min(result.columns.length-1,position.column+delta[1]))};
    select(next,event.shiftKey);
    event.currentTarget.closest('table')?.querySelector<HTMLElement>(`[data-row="${next.row}"][data-column="${next.column}"]`)?.focus();
  }
  function download() {
    if (!result?.columns.length) return;
    try {
      const csv = resultCsv(result.columns, result.rows, { formulaProtection, distinguishNull });
      const url = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }));
      const link = document.createElement('a'); link.href = url; link.download = 'minisql-loaded-results.csv'; link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000); setExportError('');
    } catch (error) { setExportError(error instanceof Error ? error.message : String(error)); }
  }
  if (!result) return <div className="empty-results"><Table2 size={28}/><strong>暂无查询结果</strong></div>;
  return <div className="result-content">
    <div className="result-tools" aria-label="结果导出">
      <button className="icon-btn" title="导出已加载结果 CSV" aria-label="导出已加载结果 CSV" disabled={!result.columns.length} onClick={download}><Download size={16}/></button>
      <button className="icon-btn" title="复制选区" aria-label="复制选区" disabled={!bounds} onClick={() => void copy()}><Copy size={16}/></button>
      <span title={`数据库返回 ${result.rows.length} 行，当前最多显示 ${rowLimit} 行`}>已加载 {result.rows.length} 行 · 显示上限 {rowLimit}</span>
      <label><input type="checkbox" checked={formulaProtection} onChange={event => setFormulaProtection(event.target.checked)}/>公式防护</label>
      <label title="NULL 编码为反斜杠 N，字符串中的反斜杠加倍；关闭后 NULL 编码为空字段。"><input type="checkbox" checked={distinguishNull} onChange={event => setDistinguishNull(event.target.checked)}/>区分 NULL</label>
      {bounds && <span>选区 {bounds.bottom-bounds.top+1} × {bounds.right-bounds.left+1}</span>}
      {copied && <span role="status">已复制</span>}
    </div>
    {exportError && <div className="result-warning" role="alert">{exportError}</div>}
    <div className="table-scroll" ref={scrollRef} onScroll={event => setScrollTop(event.currentTarget.scrollTop)}><table role="grid" aria-label="查询结果"><thead><tr><th className="index-col">#</th>{result.columns.map((column,index) => <th key={index}>{column}</th>)}</tr></thead><tbody>{virtualRange.start > 0 && <tr className="virtual-spacer" aria-hidden="true"><td colSpan={result.columns.length + 1} style={{ height: virtualRange.start * 30 }}/></tr>}{visibleRows.slice(virtualRange.start, virtualRange.end).map((row,offset) => { const i = virtualRange.start + offset; return <tr key={i}><td className="index-col">{i+1}</td>{row.map((cell,j) => <td key={j} role="gridcell" data-row={i} data-column={j} tabIndex={current ? current.focus.row===i && current.focus.column===j ? 0 : -1 : i===0 && j===0 ? 0 : -1} aria-selected={!!bounds && i>=bounds.top && i<=bounds.bottom && j>=bounds.left && j<=bounds.right} onMouseDown={event=>{event.preventDefault();select({row:i,column:j},event.shiftKey);}} onClick={event=>event.currentTarget.focus()} onKeyDown={event=>cellKeys(event,{row:i,column:j})} className={cell === null ? 'null-cell' : ''}>{cell === null ? 'NULL' : String(cell)}</td>)}</tr>; })}{virtualRange.end < visibleRows.length && <tr className="virtual-spacer" aria-hidden="true"><td colSpan={result.columns.length + 1} style={{ height: (visibleRows.length - virtualRange.end) * 30 }}/></tr>}</tbody></table>{result.rows.length>rowLimit && <div className="result-warning">仅显示前 {rowLimit} 行，导出仍使用全部已加载结果。</div>}{result.warning && <div className="result-warning">{result.warning}</div>}</div>
  </div>;
}

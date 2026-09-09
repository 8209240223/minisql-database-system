import { useState } from 'react';
import type { QueryResult } from './types';

export function Plan({ result }: { result: QueryResult | null }) {
  const [optimized, setOptimized] = useState(false);
  const [filter, setFilter] = useState('');
  const rows = optimized ? result?.optimizedPlan : result?.plan;
  const visibleRows = rows?.filter(row => `${row.kind ?? ''} ${row.detail ?? ''}`.toLowerCase().includes(filter.toLowerCase()));
  const rawCount = result?.plan?.length ?? 0;
  const optimizedCount = result?.optimizedPlan?.length ?? 0;
  function exportPlan() {
    const blob = new Blob([JSON.stringify({ version: 1, optimized, nodes: visibleRows ?? [], optimizer: result?.optimizer ?? null }, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `minisql-${optimized ? 'optimized' : 'raw'}-plan.json`;
    anchor.click();
    URL.revokeObjectURL(url);
  }
  return <div className="plan-view">
    {result?.executionStats && <section className="diagnostics" aria-label="查询执行统计">
      <strong>查询级实测统计</strong>
      <p>实际行数 {result.executionStats.actualRows} · 执行耗时 {result.executionStats.durationMs.toFixed(3)} ms · 执行次数 {result.executionStats.loops}</p>
      <p>缓存命中 {result.executionStats.hits} · 未命中 {result.executionStats.misses} · 磁盘读页 {result.executionStats.diskReads} · 写页 {result.executionStats.diskWrites}</p>
      <p>事务暂存读页 {result.executionStats.stagedPageReads} · 写页 {result.executionStats.stagedPageWrites} · I/O 错误 {result.executionStats.ioErrors}</p>
      <p>逐节点统计：未提供</p>
    </section>}
    <div role="group" aria-label="计划版本" className="output-tabs">
      <button className={!optimized ? 'selected' : ''} aria-pressed={!optimized} onClick={() => setOptimized(false)}>原始计划</button>
      <button className={optimized ? 'selected' : ''} aria-pressed={optimized} disabled={!result?.optimizedPlan} onClick={() => setOptimized(true)}>优化计划</button>
    </div>
    <div className="plan-toolbar"><input aria-label="过滤计划节点" value={filter} onChange={event => setFilter(event.target.value)} placeholder="过滤节点或详情"/><button className="toolbar-btn" onClick={exportPlan} disabled={!visibleRows?.length}>导出 JSON</button><span className="query-meta" role="status">显示 {visibleRows?.length ?? 0} / {optimized ? optimizedCount : rawCount}</span></div>
    {optimized && result?.optimizer && <div className="result-warning" role="status">
      {result.optimizer.converged ? '已收敛' : '未确认收敛'} · {result.optimizer.iterations} 轮
      {result.optimizer.diagnostics.map((diagnostic, index) => <p key={index}>{diagnostic.code}: {diagnostic.message}</p>)}
    </div>}
    {visibleRows?.length ? visibleRows.map(row => <div className="plan-row" style={{ paddingLeft: 14 + Math.min(32, Math.max(0, row.depth ?? 0)) * 20 }} key={row.id}>
      <span className="plan-node">{row.kind ?? 'STEP'}</span><strong>{row.detail}</strong>
    </div>) : <p className="result-warning">暂无计划</p>}
    {optimized && result?.optimizationRules && <details><summary>优化记录（{result.optimizationRules.length}）</summary><pre className="json-view">{JSON.stringify(result.optimizationRules, null, 2)}</pre></details>}
  </div>;
}

export function Diagnostics({ result }: { result: QueryResult | null }) {
  const labels: Record<string, string> = { passed: '已通过', skipped: '已跳过', notRun: '未运行', notImplemented: '未实现', failed: '失败' };
  return <div className="table-scroll">
    {result?.stages && <div className="diagnostics">{Object.entries(result.stages).map(([name, state]) => <div className="diag-row" key={name}><strong>{name}</strong><span>{labels[state] ?? state}</span></div>)}</div>}
    {result?.tokens ? <table aria-label="Token 序列"><thead><tr><th>#</th><th>类型</th><th>原文</th><th>行</th><th>列</th></tr></thead><tbody>
      {result.tokens.map((token, index) => <tr key={index}><td>{index + 1}</td><td>{token.type}</td><td style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere', maxWidth: 400 }}>{token.text}</td><td>{token.line}</td><td>{token.column}</td></tr>)}
    </tbody></table> : <p className="result-warning">暂无 Token 数据</p>}
  </div>;
}

import { useState } from 'react';
import type { QueryResult, SqlToken } from './types';

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
      <button className={!optimized ? 'selected' : ''} title="原始计划" data-tip="优化器改写前的物理计划，用来和优化计划对比看改写效果。" aria-pressed={!optimized} onClick={() => setOptimized(false)}>原始计划</button>
      <button className={optimized ? 'selected' : ''} title="优化计划" data-tip="经过规则改写与成本模型选择后的计划：去相关、谓词下推、Join 算法选择都在这体现。" aria-pressed={optimized} disabled={!result?.optimizedPlan} onClick={() => setOptimized(true)}>优化计划</button>
    </div>
    <div className="plan-toolbar"><input aria-label="过滤计划节点" title="过滤计划节点" data-tip="按节点名或详情关键字过滤计划行。" value={filter} onChange={event => setFilter(event.target.value)} placeholder="过滤节点或详情"/><button className="toolbar-btn" title="导出 JSON" data-tip="把当前显示的计划节点（含优化器记录）导出为 JSON 文件。" onClick={exportPlan} disabled={!visibleRows?.length}>导出 JSON</button><span className="query-meta" role="status">显示 {visibleRows?.length ?? 0} / {optimized ? optimizedCount : rawCount}</span></div>
    {optimized && result?.optimizer && <div className="result-warning" role="status">
      {result.optimizer.converged ? '已收敛' : '未确认收敛'} · {result.optimizer.iterations} 轮
      {result.optimizer.diagnostics.map((diagnostic, index) => <p key={index}>{diagnostic.code}: {diagnostic.message}</p>)}
    </div>}
    {visibleRows?.length ? visibleRows.map(row => <div className="plan-row" style={{ paddingLeft: 14 + Math.min(32, Math.max(0, row.depth ?? 0)) * 20 }} key={row.id}>
      <span className="plan-node">{row.kind ?? 'STEP'}</span><strong>{row.detail}</strong>
      <details className="plan-node-details">
        <summary title="节点详情" data-tip="展开看输入节点、输出列、条件与估算/实际行数。">节点详情</summary>
        <p>输入节点：{row.children?.length ? row.children.join(', ') : '无'}</p>
        <p>输出列：{row.output?.length ? row.output.map(column => `${column.name}:${column.type}`).join(', ') : '无'}</p>
        {row.predicate != null && <p>条件：<code>{JSON.stringify(row.predicate)}</code></p>}
        {!!row.projections?.length && <p>投影：<code>{JSON.stringify(row.projections)}</code></p>}
        {!!row.sortKeys?.length && <p>排序键：<code>{JSON.stringify(row.sortKeys)}</code></p>}
        {!!row.groupKeys?.length && <p>分组键：<code>{JSON.stringify(row.groupKeys)}</code></p>}
        {!!row.aggregates?.length && <p>聚合：<code>{JSON.stringify(row.aggregates)}</code></p>}
        <p>估算行数：{row.estimatedRows ?? '未提供'} · 实际行数：{row.actualRows ?? '未提供'}</p>
      </details>
    </div>) : <p className="result-warning">暂无计划</p>}
    {optimized && result?.optimizationRules && <details><summary title="优化记录" data-tip="优化器每条改写规则的应用记录：规则名、命中位置与收益。">优化记录（{result.optimizationRules.length}）</summary><pre className="json-view">{JSON.stringify(result.optimizationRules, null, 2)}</pre></details>}
  </div>;
}

export function TokenStream({ result, onSelect }: { result: QueryResult | null; onSelect?: (token: SqlToken) => void }) {
  const tokens = result?.tokens ?? [];
  if (!tokens.length) return <div className="table-scroll"><p className="result-warning">暂无 Token 数据</p></div>;
  return <div className="token-view">
    <div className="token-summary">
      <strong>词法分析结果</strong>
      <span>共 {tokens.length} 个 Token</span>
      <span>范围采用 UTF-8 字节右开区间</span>
    </div>
    <div className="table-scroll">
      <table aria-label="Token 流">
        <thead><tr><th>#</th><th>种别码</th><th>词素值</th><th>起点</th><th>终点</th><th>字节范围</th></tr></thead>
        <tbody>{tokens.map((token, index) => <tr className="token-row" key={`${token.line}-${token.column}-${index}`} tabIndex={0} title="点击定位到 SQL 源码" data-tip="点击或按回车，编辑器会光标跳到这个 token 的行列位置。" onClick={() => onSelect?.(token)} onKeyDown={event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); onSelect?.(token); } }}>
          <td>{index + 1}</td>
          <td><span className={`token-type token-${token.type.toLowerCase().replace(/[^a-z0-9_-]/g, '')}`}>{token.type}</span></td>
          <td className="token-lexeme">{token.text || '<空>'}</td>
          <td>{token.line}:{token.column}</td>
          <td>{token.endLine ?? token.line}:{token.endColumn ?? token.column + token.text.length}</td>
          <td>[{token.byteStart ?? '—'}, {token.byteEnd ?? '—'})</td>
        </tr>)}</tbody>
      </table>
    </div>
  </div>;
}

export function Diagnostics({ result }: { result: QueryResult | null }) {
  const labels: Record<string, string> = { passed: '已通过', skipped: '已跳过', notRun: '未运行', notImplemented: '未实现', failed: '失败' };
  return <div className="table-scroll">
    {result?.stages && <div className="diagnostics">{Object.entries(result.stages).map(([name, state]) => <div className="diag-row" key={name}><strong>{name}</strong><span>{labels[state] ?? state}</span></div>)}</div>}
    {result?.diagnostics?.length ? <div className="diagnostics" aria-label="编译诊断">{result.diagnostics.map((item, index) => <div className="diag-row" key={`${item.statementIndex ?? index}-${item.line ?? 0}-${item.column ?? 0}`}><strong>{item.code ?? 'SQL'}</strong><span>第 {item.line ?? 1} 行，第 {item.column ?? 1} 列：{item.message}</span>{(item.actual || item.expected?.length) && <p>实际：{item.actual || '空'} · 期望：{item.expected?.join(' | ') || '未提供'}</p>}{item.suggestion && <p>{item.suggestion}</p>}</div>)}</div> : null}
    {!result?.stages && !result?.diagnostics?.length && <p className="result-warning">暂无诊断数据</p>}
  </div>;
}

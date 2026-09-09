import type { StorageStatistics } from './client';
import { useState } from 'react';
import { RotateCcw } from 'lucide-react';

export function StorageStatisticsView({ value, disabled, onConfigure }: { value: StorageStatistics; disabled: boolean; onConfigure: (action: 'LRU' | 'FIFO' | 'RESET') => Promise<void> }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  async function configure(action: 'LRU' | 'FIFO' | 'RESET') {
    if (busy || disabled) return;
    setBusy(true); setError('');
    try { await onConfigure(action); }
    catch (failure) { setError(failure instanceof Error ? failure.message : String(failure)); }
    finally { setBusy(false); }
  }
  const buffer = value.buffer;
  return <section className="storage-info" aria-label="存储统计">
    <div>页大小 {value.pageSize} B · 文件大小 {value.fileBytes} B</div>
    <div>已分配页 {value.allocatedPages}</div>
    {buffer?.available ? <>
      <h3>当前会话缓冲池</h3>
      <label>替换策略<select value={buffer.policy} disabled={disabled || busy} onChange={event => void configure(event.target.value as 'LRU' | 'FIFO')}><option value="LRU">LRU</option><option value="FIFO">FIFO</option></select></label>
      <button className="icon-btn" title="清零缓存统计" aria-label="清零缓存统计" disabled={disabled || busy} onClick={() => void configure('RESET')}><RotateCcw size={16}/></button>
      {error && <p role="alert">{error}</p>}
      <div>策略 {buffer.policy} · 驻留页 {buffer.residentPages} / {buffer.capacity}</div>
      <div>命中 {buffer.hits} · 未命中 {buffer.misses} · 命中率 {(buffer.hitRate * 100).toFixed(2)}%</div>
      <div>磁盘读页 {buffer.diskReads} · 磁盘写页 {buffer.diskWrites} · I/O 错误 {buffer.ioErrors}</div>
      <div>事务暂存读页 {buffer.stagedPageReads} · 事务暂存写页 {buffer.stagedPageWrites}</div>
      <details><summary>淘汰记录（{buffer.evictions.length}）</summary>
        <div className="eviction-log"><table aria-label="缓冲池淘汰记录"><thead><tr><th>序号</th><th>策略</th><th>页</th><th>版本</th><th>脏页</th><th>写回结果</th></tr></thead>
          <tbody>{buffer.evictions.map((event, index) => <tr key={index}><td>{event.sequence}</td><td>{event.policy}</td><td>{event.pageId}</td><td>{event.generation}</td><td>{event.dirty ? '是' : '否'}</td><td>{({ 'not-needed': '无需写回', written: '已写入', staged: '已暂存', failed: '失败，保留原页' } as Record<string, string>)[event.writeBack ?? ''] ?? '未提供'}</td></tr>)}</tbody>
        </table></div>
      </details>
    </> : <div>当前后端未提供会话缓存统计。</div>}
  </section>;
}

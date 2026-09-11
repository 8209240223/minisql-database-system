import type { IndexInspect } from './client';

function ref(value: { id: number; generation: number } | undefined): string {
  if (!value) return '—';
  return `${value.id}.${value.generation}`;
}

export function IndexInspectView({ info }: { info: IndexInspect | undefined }) {
  if (!info) return <p className="inspect-empty">选择一个真实数据库会话中的索引执行页级结构检查。本地演示引擎不提供页级索引结构。</p>;
  if (info.storage === 'memory' || info.present === false) {
    return <section className="inspect-info" aria-label="索引检查">
      <header><strong>{info.table}.{info.index}</strong><span className="inspect-mode">{info.storage === 'memory' ? '内存索引' : '页级索引'}</span></header>
      {info.message && <p>{info.message}</p>}
      {!info.present && info.storage !== 'memory' && <p>索引元数据页缺失：当前未构建可检查的页级结构。</p>}
    </section>;
  }
  return <section className="inspect-info" aria-label="索引检查">
    <header><strong>{info.table}.{info.index}</strong><span className="inspect-mode">页级索引</span></header>
    <div className="inspect-summary">
      <dl><dt>树高</dt><dd>{info.height}</dd><dt>节点页</dt><dd>{info.nodeCount}</dd><dt>叶节点</dt><dd>{info.leafCount}</dd><dt>行数</dt><dd>{info.rowCount}</dd><dt>叶链长度</dt><dd>{info.leafChainLength}</dd><dt>根</dt><dd>{ref(info.root)}</dd></dl>
    </div>
    <div className="inspect-checks">
      {[{ ok: info.rootReachable, label: '根可达' }, { ok: info.leafChainLinked, label: '叶链完整' }, { ok: info.parentLinksValid, label: '父指针有效' }].map(check =>
        <span key={check.label} className={check.ok ? 'ok' : 'bad'}>{check.ok ? '✓' : '✗'} {check.label}</span>)}
    </div>
    {info.problems.length > 0 && <div className="inspect-problems"><h3>结构问题（{info.problems.length}）</h3><ul>{info.problems.map((problem, index) => <li key={index}>{problem}</li>)}</ul></div>}
    <details open={info.pages.length <= 64}><summary>页明细（{info.pages.length}）</summary>
      <div className="inspect-pages"><table aria-label="索引页明细"><thead><tr><th>页</th><th>类型</th><th>层高</th><th>键数</th><th>父</th><th>左兄弟</th><th>右兄弟</th></tr></thead>
        <tbody>{info.pages.map((page, index) => <tr key={index}><td>{ref(page.page)}</td><td>{page.leaf ? '叶' : '内部'}</td><td>{page.height}</td><td>{page.keyCount}</td><td>{ref(page.parent)}</td><td>{ref(page.left)}</td><td>{ref(page.right)}</td></tr>)}</tbody>
      </table></div>
    </details>
  </section>;
}
import type { Table } from './types';
import './inspect-table.css';

export function TableInspectView({ table }: { table: Table | undefined }) {
  if (!table) return <p className="inspect-empty">点击左侧表名查看表结构详情。</p>;
  const primaryKeys = table.keys?.filter(key => key.primary) ?? [];
  const uniqueKeys = table.keys?.filter(key => !key.primary) ?? [];
  return <section className="inspect-info" aria-label="表结构详情">
    <header><strong>{table.name}</strong><span className="inspect-mode">表结构</span></header>
    <div className="inspect-summary"><dl>
      <dt>行数</dt><dd>{table.rowCount}</dd>
      <dt>列数</dt><dd>{table.columns.length}</dd>
      <dt>索引</dt><dd>{table.indexes?.length ?? 0}</dd>
      {primaryKeys.length > 0 && <><dt>主键</dt><dd>{primaryKeys.map(key => key.columns.join(', ')).join(' / ')}</dd></>}
    </dl></div>
    <div className="inspect-table-block"><h3>列</h3>
      <table aria-label={`${table.name} 列定义`}>
        <thead><tr><th>列名</th><th>类型</th><th>主键</th><th>可空</th><th>默认值</th><th>唯一</th><th>外键</th></tr></thead>
        <tbody>{table.columns.map(column => <tr key={column.name}>
          <td>{column.name}</td>
          <td>{column.type}</td>
          <td>{column.primaryKey ? '是' : '否'}</td>
          <td>{column.nullable ? '是' : '否'}</td>
          <td>{column.defaultValue ?? ''}</td>
          <td>{column.unique ? '是' : '否'}</td>
          <td>{column.references ? `${column.references.table}(${column.references.column})` : ''}</td>
        </tr>)}</tbody>
      </table>
    </div>
    {uniqueKeys.length > 0 && <div className="inspect-table-block"><h3>唯一键</h3>
      <table aria-label={`${table.name} 唯一键`}>
        <thead><tr><th>列</th></tr></thead>
        <tbody>{uniqueKeys.map((key, index) => <tr key={index}><td>{key.columns.join(', ')}</td></tr>)}</tbody>
      </table>
    </div>}
    {table.foreignKeys?.length ? <div className="inspect-table-block"><h3>外键</h3>
      <table aria-label={`${table.name} 外键`}>
        <thead><tr><th>列</th><th>引用表</th><th>引用列</th></tr></thead>
        <tbody>{table.foreignKeys.map((key, index) => <tr key={index}>
          <td>{key.columns.join(', ')}</td>
          <td>{key.table}</td>
          <td>{key.referencedColumns.join(', ')}</td>
        </tr>)}</tbody>
      </table>
    </div> : null}
    {table.indexes?.length ? <div className="inspect-table-block"><h3>索引</h3>
      <table aria-label={`${table.name} 索引`}>
        <thead><tr><th>索引名</th><th>列</th><th>唯一</th></tr></thead>
        <tbody>{table.indexes.map(index => <tr key={index.name}>
          <td>{index.name}</td>
          <td>{index.columns.join(', ')}</td>
          <td>{index.unique ? '是' : '否'}</td>
        </tr>)}</tbody>
      </table>
    </div> : null}
  </section>;
}

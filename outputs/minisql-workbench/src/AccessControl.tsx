import { useCallback, useEffect, useState } from 'react';
import type { AccessState, AccessUser, AccessRole, AuditEntry, Connection, SessionEntry } from './types';
import { getAccess, getAudit, getSessions, createUser, dropUser, createRole, dropRole,
  grantAccess, revokeAccess, setUserPassword, addUserRole, removeUserRole } from './client';
import { X, Shield, Users, FileSearch, Activity } from 'lucide-react';

const PERMISSIONS = ['*', 'connect', 'read', 'select', 'insert', 'update', 'delete', 'create', 'drop', 'transaction', 'checkpoint', 'compile', 'grant', 'audit'];

type Tab = 'users' | 'roles' | 'audit' | 'sessions';
type Subject = { type: 'user' | 'role'; name: string };

function GrantEditor({ subject, connection, onChanged }: { subject: Subject; connection: Connection; onChanged: () => void }) {
  const [object, setObject] = useState('*');
  const [selected, setSelected] = useState<string[]>(['read']);
  const [message, setMessage] = useState<string>();
  const toggle = (permission: string) => setSelected(previous => previous.includes(permission) ? previous.filter(p => p !== permission) : [...previous, permission]);
  const run = async (revoke = false) => {
    setMessage(undefined);
    try {
      if (revoke) {
        const all = selected.length === 0;
        await revokeAccess(connection, { subject, object, permissions: all ? [] : selected });
      } else {
        await grantAccess(connection, { subject, object, permissions: selected });
      }
      setMessage(revoke ? '已撤销' : '已授权');
      onChanged();
    } catch (error) { setMessage(error instanceof Error ? error.message : String(error)); }
  };
  return <div className="access-grant">
    <div className="access-grant-row"><label>对象 <input value={object} onChange={event => setObject(event.target.value)} placeholder="* 或表名"/></label></div>
    <div className="access-permissions">{PERMISSIONS.map(permission => <label key={permission} className="perm-chip"><input type="checkbox" checked={selected.includes(permission)} onChange={() => toggle(permission)}/>{permission}</label>)}</div>
    <div className="access-grant-actions"><button onClick={() => void run(false)} className="grant">授权</button><button onClick={() => void run(true)} className="revoke">撤销</button>{message && <span className="access-message">{message}</span>}</div>
  </div>;
}

export function AccessControl({ connection, open, onClose }: { connection: Connection; open: boolean; onClose: () => void }) {
  const [access, setAccess] = useState<AccessState>();
  const [audit, setAudit] = useState<AuditEntry[]>([]);
  const [sessions, setSessions] = useState<SessionEntry[]>([]);
  const [tab, setTab] = useState<Tab>('users');
  const [error, setError] = useState<string>();
  const [newUser, setNewUser] = useState({ name: '', password: '', roles: '' });
  const [newRole, setNewRole] = useState({ name: '', inherits: '', grants: '' });

  const refresh = useCallback(async () => {
    if (connection.mode !== 'api') { setError('需要连接真实 C++ 数据库。'); return; }
    setError(undefined);
    try {
      const [a, ae, se] = await Promise.all([getAccess(connection), getAudit(connection, {}), getSessions(connection)]);
      setAccess(a); setAudit(ae.entries); setSessions(se);
    } catch (e) { setError(e instanceof Error ? e.message : String(e)); }
  }, [connection]);

  useEffect(() => { if (open) void refresh(); }, [open, refresh]);

  if (!open) return null;
  const apiMode = connection.mode === 'api';

  const doAction = async (run: () => Promise<unknown>, success?: string) => {
    setError(undefined);
    try { await run(); setNewUser({ name: '', password: '', roles: '' }); setNewRole({ name: '', inherits: '', grants: '' }); await refresh(); }
    catch (e) { setError(e instanceof Error ? e.message : String(e)); }
    void success;
  };

  return <dialog className="settings-dialog access-dialog" open aria-label="权限与审计"><div className="settings-body access-body">
    <button className="icon-btn settings-close" aria-label="关闭" onClick={onClose}><X size={17}/></button>
    <h2 className="access-title"><Shield size={16}/> 权限与审计</h2>
    {!apiMode && <div className="storage-info">连接真实 C++ 数据库后可管理用户、角色、授权与查看审计。</div>}
    {error && <div className="access-error">⚠ {error}</div>}
    <div className="access-tabs">
      <button className={tab === 'users' ? 'selected' : ''} onClick={() => setTab('users')}><Users size={14}/> 用户</button>
      <button className={tab === 'roles' ? 'selected' : ''} onClick={() => setTab('roles')}><Shield size={14}/> 角色</button>
      <button className={tab === 'sessions' ? 'selected' : ''} onClick={() => setTab('sessions')}><Activity size={14}/> 会话</button>
      <button className={tab === 'audit' ? 'selected' : ''} onClick={() => setTab('audit')}><FileSearch size={14}/> 审计</button>
    </div>

    {tab === 'users' && <div className="access-panel">
      <div className="access-create">
        <input aria-label="用户名" placeholder="新用户" value={newUser.name} onChange={e => setNewUser(v => ({ ...v, name: e.target.value }))}/>
        <input aria-label="密码" type="password" placeholder="密码" value={newUser.password} onChange={e => setNewUser(v => ({ ...v, password: e.target.value }))}/>
        <input aria-label="角色" placeholder="角色（逗号分隔）" value={newUser.roles} onChange={e => setNewUser(v => ({ ...v, roles: e.target.value }))}/>
        <button disabled={!newUser.name} onClick={() => void doAction(() => createUser(connection, { name: newUser.name, password: newUser.password, roles: newUser.roles.split(',').map(s => s.trim()).filter(Boolean) }))}>新增用户</button>
      </div>
      {(access?.users ?? []).map(user => <UserRow key={user.name} user={user} connection={connection} onChanged={refresh} onAction={doAction}/>)}
    </div>}

    {tab === 'roles' && <div className="access-panel">
      <div className="access-create">
        <input aria-label="角色名" placeholder="新角色" value={newRole.name} onChange={e => setNewRole(v => ({ ...v, name: e.target.value }))}/>
        <input aria-label="继承" placeholder="继承角色（逗号分隔）" value={newRole.inherits} onChange={e => setNewRole(v => ({ ...v, inherits: e.target.value }))}/>
        <button disabled={!newRole.name} onClick={() => void doAction(() => createRole(connection, { name: newRole.name, inherits: newRole.inherits.split(',').map(s => s.trim()).filter(Boolean) }))}>新增角色</button>
      </div>
      {(access?.roles ?? []).map(role => <RoleRow key={role.name} role={role} connection={connection} onChanged={refresh} onAction={doAction}/>)}
    </div>}

    {tab === 'sessions' && <div className="access-panel">
      {sessions.length === 0 ? <p className="access-empty">暂无活动会话。</p> : <table className="access-table"><thead><tr><th>会话</th><th>用户</th><th>事务</th><th>活动</th><th>锁等待</th><th>最后活动</th></tr></thead><tbody>{sessions.map(s => <tr key={s.sessionId}><td>{s.sessionId.slice(0, 8)}…</td><td>{s.user}</td><td>{s.transactionState}</td><td>{s.activeRequest ? '是' : '否'}</td><td>{s.waitingForLock ? '是' : '否'}</td><td>{new Date(s.lastActiveAt).toLocaleTimeString()}</td></tr>)}</tbody></table>}
    </div>}

    {tab === 'audit' && <AuditPanel entries={audit} connection={connection} onChanged={refresh}/>}
  </div></dialog>;
}

function UserRow({ user, connection, onChanged, onAction }: {
  user: AccessUser; connection: Connection; onChanged: () => Promise<void>; onAction: (run: () => Promise<unknown>, success?: string) => Promise<void>;
}) {
  const [roleName, setRoleName] = useState('');
  const [expanded, setExpanded] = useState(false);
  return <div className="access-item">
    <div className="access-item-head">
      <button className="access-expand" onClick={() => setExpanded(v => !v)}>{expanded ? '▾' : '▸'}</button>
      <strong>{user.name}</strong><span className="access-badge">{user.passwordProtected ? '密码保护' : '无密码'}</span>
      <span className="access-roles">{user.roles.join(', ') || '无角色'}</span>
      <span className="access-spacer"/>
      <input className="access-mini-input" placeholder="绑定角色" value={roleName} onChange={e => setRoleName(e.target.value)}/>
      <button className="grant" disabled={!roleName} onClick={() => void onAction(() => addUserRole(connection, user.name, roleName), '')}>绑定</button>
      {user.roles.map(r => <button key={r} className="revoke" title={`移除角色 ${r}`} onClick={() => void onAction(() => removeUserRole(connection, user.name, r), '')}>{r} ×</button>)}
      <button className="revoke" onClick={() => { const password = window.prompt(`设置 ${user.name} 的新密码`); if (password !== null) void onAction(() => setUserPassword(connection, user.name, password), '已改密'); }}>改密</button>
      <button className="danger" onClick={() => { if (window.confirm(`删除用户 ${user.name}？`)) void onAction(() => dropUser(connection, user.name), '已删除'); }}>删除</button>
    </div>
    {expanded && <div className="access-grants"><GrantEditor subject={{ type: 'user', name: user.name }} connection={connection} onChanged={onChanged}/></div>}
  </div>;
}

function RoleRow({ role, connection, onChanged, onAction }: {
  role: AccessRole; connection: Connection; onChanged: () => Promise<void>; onAction: (run: () => Promise<unknown>, success?: string) => Promise<void>;
}) {
  const [expanded, setExpanded] = useState(false);
  return <div className="access-item">
    <div className="access-item-head">
      <button className="access-expand" onClick={() => setExpanded(v => !v)}>{expanded ? '▾' : '▸'}</button>
      <strong>{role.name}</strong>
      <span className="access-roles">继承: {role.inherits.join(', ') || '无'}</span>
      <span className="access-spacer"/>
      <button className="danger" onClick={() => { if (window.confirm(`删除角色 ${role.name}？`)) void onAction(() => dropRole(connection, role.name), '已删除'); }}>删除</button>
    </div>
    {expanded && <div className="access-grants"><GrantEditor subject={{ type: 'role', name: role.name }} connection={connection} onChanged={onChanged}/></div>}
  </div>;
}

function AuditPanel({ entries, connection, onChanged }: { entries: AuditEntry[]; connection: Connection; onChanged: () => Promise<void> }) {
  const [filter, setFilter] = useState('');
  const filtered = entries.filter(e => !filter || e.user.includes(filter) || e.object?.includes(filter) || e.path.includes(filter));
  return <div className="access-panel">
    <div className="history-search"><FileSearch size={14}/><input aria-label="过滤审计" placeholder="过滤：用户 / 对象 / 路径" value={filter} onChange={e => setFilter(e.target.value)}/></div>
    {filtered.length === 0 ? <p className="access-empty">暂无审计记录。</p> : <table className="access-table audit-table"><thead><tr><th>时间</th><th>用户</th><th>方法</th><th>路径</th><th>对象</th><th>状态</th><th>耗时</th></tr></thead><tbody>{filtered.map(e => <tr key={e.id}><td>{new Date(e.at).toLocaleTimeString()}</td><td>{e.user}</td><td>{e.method}</td><td>{e.path}</td><td>{e.object ?? '—'}</td><td className={e.success ? 'ok-text' : 'err-text'}>{e.status}</td><td>{e.durationMs.toFixed(0)} ms</td></tr>)}</tbody></table>}
  </div>;
}

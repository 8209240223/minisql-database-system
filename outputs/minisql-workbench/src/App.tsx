import { useEffect, useMemo, useRef, useState } from 'react';
import CodeMirror from '@uiw/react-codemirror';
import { EditorView } from '@codemirror/view';
import { lintGutter, setDiagnostics } from '@codemirror/lint';
import { sql } from '@codemirror/lang-sql';
import { Activity, Braces, ChevronDown, ChevronRight, CircleHelp, Database, FileCode2, FolderTree, History, Play, Plus, RefreshCw, Search, Settings2, Table2, Terminal, Trash2, X } from 'lucide-react';
import { getCatalog, getHealth, getStorage, runSql, openApiSession, closeApiSession, releaseApiSession } from './client';
import { StorageStatisticsView } from './StorageStatistics';
import { configureBuffer } from './client';
import { Plug, Unplug, Check, Undo2, CirclePlay, Pencil, Upload, Download } from 'lucide-react';
import { Plan, Diagnostics } from './CompilerViews';
import type { Connection, HistoryItem, QueryTab, Table } from './types';
import { Results } from './Results';
import { DRAFT_KEY, emptyView, restoreTabs } from './query-tabs';
import type { QueryView } from './query-tabs';
import { decodeSqlFile, sqlFilename, MAX_SQL_FILE_BYTES } from './sql-files';
import { mapDiagnostic } from './diagnostic-location';
import { mutationWarning } from './sql-safety';
import { HISTORY_KEY, MAX_HISTORY_SQL, limitHistory, restoreHistory, filterHistory } from './query-history';

const starter = `-- MiniSQL Studio
SELECT s.id, s.name, s.department, e.score
FROM students s
JOIN enrollments e ON e.student_id = s.id
WHERE e.score >= 90
ORDER BY e.score DESC
LIMIT 12;`;
const connection: Connection = { mode: 'api', name: 'MiniSQL C++', url: import.meta.env.VITE_MINISQL_API ?? 'http://127.0.0.1:8081/api' };
const editorExtensions = [sql(), lintGutter()];

function App() {
  const [tables, setTables] = useState<Table[]>([]);
  const [tabs, setTabs] = useState<QueryTab[]>(() => restoreTabs([{ id: 'q1', name: 'query_1.sql', sql: starter }]));
  const [active, setActive] = useState(tabs[0].id);
  const [views, setViews] = useState<Record<string, QueryView>>({});
  const liveTabs = useRef(new Set(tabs.map(tab => tab.id)));
  liveTabs.current = new Set(tabs.map(tab => tab.id));
  const { result, output, notice, source: resultSource, outcome, selection: selectedResult, diagnostic } = views[active] ?? emptyView;
  const editor = useRef<EditorView | undefined>(undefined);
  const [editorInstance, setEditorInstance] = useState<EditorView>();
  const fileInput = useRef<HTMLInputElement>(null);
  const [draftSaved, setDraftSaved] = useState(true);
  const importing = useRef(false);
  const [runningTab, setRunningTab] = useState<string>();
  function updateView(patch: Partial<QueryView>) {
    const id = active;
    setViews(previous => liveTabs.current.has(id) ? { ...previous, [id]: { ...(previous[id] ?? emptyView), ...patch } } : previous);
  }
  function setOutput(value: QueryView['output']) { updateView({ output: value }); }
  function setNotice(value: string) { updateView({ notice: value, diagnostic: undefined }); }
  function settlePending(value: string) {
    setViews(previous => Object.fromEntries(Object.entries(previous).map(([id, view]) => [id, view.outcome === 'pending' ? { ...view, outcome: value } : view])));
  }
  const [running, setRunning] = useState(false);
  const [history, setHistory] = useState<HistoryItem[]>(restoreHistory);
  const [historyQuery, setHistoryQuery] = useState('');
  const [historySaved, setHistorySaved] = useState(true);
  const [historyOmitted, setHistoryOmitted] = useState(false);
  const [historyDialogOpen, setHistoryDialogOpen] = useState(false);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [resultLimit, setResultLimit] = useState(() => Number(localStorage.getItem('minisql-result-limit') ?? 1000));
  const [darkMode, setDarkMode] = useState(() => localStorage.getItem('minisql-theme') === 'dark');
  const [storageInfo, setStorageInfo] = useState<Awaited<ReturnType<typeof getStorage>>>();
  const [health, setHealth] = useState<string>();
  const historyDialog = useRef<HTMLDialogElement>(null);
  const autoConnectStarted = useRef(false);
  const visibleHistory = useMemo(() => filterHistory(history, historyQuery), [history, historyQuery]);
  useEffect(() => {
    if (historyDialogOpen && !historyDialog.current?.open) historyDialog.current?.showModal();
  }, [historyDialogOpen]);
  useEffect(() => {
    const wide = window.matchMedia('(min-width: 1001px)');
    const closeOnWide = () => { if (wide.matches) historyDialog.current?.close(); };
    wide.addEventListener('change', closeOnWide);
    return () => wide.removeEventListener('change', closeOnWide);
  }, []);
  useEffect(() => {
    if (autoConnectStarted.current) return;
    autoConnectStarted.current = true;
    void connect();
  }, []);
  useEffect(() => {
    const sidebar = document.querySelector<HTMLElement>('.sidebar');
    if (!sidebar) return;
    const minimum = 220;
    const maximum = 520;
    const stored = Number(localStorage.getItem('minisql-sidebar-width'));
    const clamp = (value: number) => Math.max(minimum, Math.min(maximum, value));
    sidebar.style.width = `${clamp(Number.isFinite(stored) ? stored : 245)}px`;
    const resizer = document.createElement('div');
    resizer.className = 'sidebar-resizer';
    resizer.setAttribute('role', 'separator');
    resizer.setAttribute('aria-orientation', 'vertical');
    resizer.setAttribute('aria-label', '拖动调整侧栏宽度');
    sidebar.appendChild(resizer);
    let drag: { pointerId: number; startX: number; startWidth: number } | undefined;
    const finish = (event: PointerEvent) => {
      if (!drag || drag.pointerId !== event.pointerId) return;
      try { resizer.releasePointerCapture(event.pointerId); } catch { /* Pointer capture may already be released. */ }
      drag = undefined;
      resizer.classList.remove('dragging');
      document.body.classList.remove('sidebar-resizing');
      localStorage.setItem('minisql-sidebar-width', String(clamp(sidebar.getBoundingClientRect().width)));
    };
    const start = (event: PointerEvent) => {
      if (event.button !== 0) return;
      drag = { pointerId: event.pointerId, startX: event.clientX, startWidth: sidebar.getBoundingClientRect().width };
      resizer.setPointerCapture(event.pointerId);
      resizer.classList.add('dragging');
      document.body.classList.add('sidebar-resizing');
      event.preventDefault();
    };
    const move = (event: PointerEvent) => {
      if (!drag || drag.pointerId !== event.pointerId) return;
      sidebar.style.width = `${clamp(drag.startWidth + event.clientX - drag.startX)}px`;
      event.preventDefault();
    };
    resizer.addEventListener('pointerdown', start);
    resizer.addEventListener('pointermove', move);
    resizer.addEventListener('pointerup', finish);
    resizer.addEventListener('pointercancel', finish);
    return () => {
      resizer.removeEventListener('pointerdown', start);
      resizer.removeEventListener('pointermove', move);
      resizer.removeEventListener('pointerup', finish);
      resizer.removeEventListener('pointercancel', finish);
      resizer.remove();
      document.body.classList.remove('sidebar-resizing');
    };
  }, []);
  function recordHistory(source: string, compile: boolean, durationMs: number, rows: number, error?: string) {
    setHistoryOmitted(source.length > MAX_HISTORY_SQL);
    if (source.length > MAX_HISTORY_SQL) return;
    const item: HistoryItem = { id: crypto.randomUUID(), sql: source, at: Date.now(), durationMs, rows,
      mode: connection.mode, connection: connection.name, action: compile ? 'compile' : 'execute',
      ...(error === undefined ? {} : { error: error.slice(0, 1000) }) };
    setHistory(previous => limitHistory([item, ...previous]));
  }
  const [filter, setFilter] = useState('');
  const [expanded, setExpanded] = useState<Record<string, boolean>>({ students: true, courses: false, enrollments: false });
  const [sessionId, setSessionId] = useState<string>();
  useEffect(() => {
    if (!sessionId) return;
    const timer = window.setInterval(() => { void getHealth(connection).then(value => setHealth(value.status)).catch(() => setHealth('degraded')); }, 15000);
    return () => window.clearInterval(timer);
  }, [connection.url, sessionId]);
  const [transactionState, setTransactionState] = useState('IDLE');
  const busy = useRef(false);
  const effectiveConnection = { ...connection, sessionId };
  const controller = useRef<AbortController | null>(null);
  const current = tabs.find(t => t.id === active) ?? tabs[0];
  const visibleTables = useMemo(() => tables.filter(t => t.name.toLowerCase().includes(filter.toLowerCase())), [tables, filter]);
  useEffect(() => {
    if (!editorInstance?.dom.isConnected) return;
    const valid = diagnostic && notice && editorInstance.state.doc.toString() === diagnostic.source;
    editorInstance.dispatch(setDiagnostics(editorInstance.state, valid ? [{
      from: diagnostic.offset, to: diagnostic.offset + diagnostic.length,
      severity: 'error', message: notice, source: 'MiniSQL',
    }] : []));
  }, [editorInstance, current.sql, diagnostic, notice]);

  function handleFailure(error: unknown) {
    const details = error as { transactionState?: string; status?: number; commitState?: string };
    if (sessionId) {
      if (details.transactionState === 'ABORTED') settlePending('rolledBack');
      if (details.commitState === 'unknown') settlePending('unknown');
      if (details.status === 404) { setSessionId(undefined); setTransactionState('EXPIRED'); setTables([]); setViews({}); }
      else setTransactionState(details.commitState === 'unknown' ? 'UNKNOWN' : details.transactionState ?? 'UNKNOWN');
    }
    setNotice(error instanceof Error ? error.message : String(error));
  }
  async function refresh() { try { setTables(await getCatalog(effectiveConnection)); } catch (e) { handleFailure(e); } }
  async function refreshStorage() { if (sessionId) { try { setStorageInfo(await getStorage(effectiveConnection)); } catch (e) { handleFailure(e); } } }
  useEffect(() => {
    try { localStorage.setItem(DRAFT_KEY, JSON.stringify(tabs)); setDraftSaved(true); }
    catch { setDraftSaved(false); }
  }, [tabs]);
  useEffect(() => {
    try {
      if (history.length) localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
      else localStorage.removeItem(HISTORY_KEY);
      setHistorySaved(true);
    } catch { setHistorySaved(false); }
  }, [history]);
  useEffect(() => {
    const release = () => releaseApiSession(effectiveConnection);
    window.addEventListener('pagehide', release);
    return () => window.removeEventListener('pagehide', release);
  }, [connection.url, sessionId]);
  async function connect() {
    if (busy.current) return;
    busy.current = true; setRunning(true); setNotice('');
    let openedId: string | undefined;
    try {
      const opened = await openApiSession(connection);
      openedId = opened.sessionId;
      setSessionId(opened.sessionId); setTransactionState(opened.transactionState);
      setHealth((await getHealth(connection)).status);
      setTables(await getCatalog({ ...connection, sessionId: opened.sessionId }));
      setStorageInfo(await getStorage({ ...connection, sessionId: opened.sessionId }));
    } catch (e) { handleFailure(e); if (openedId) setTransactionState('UNKNOWN'); }
    finally { busy.current = false; setRunning(false); }
  }
  async function disconnect() {
    if (busy.current) return;
    if (sessionId && transactionState !== 'IDLE' && !window.confirm(transactionState === 'UNKNOWN' ? '提交结果未知，断开不会撤销已提交的数据。确认断开？' : '断开会话将撤销未提交修改。确认断开？')) return;
    busy.current = true; setRunning(true);
    try {
      await closeApiSession(effectiveConnection);
      setSessionId(undefined); setTransactionState('IDLE'); setTables([]); setViews({});
      setHealth(undefined); setStorageInfo(undefined);
    } catch (e) { handleFailure(e); }
    finally { busy.current = false; setRunning(false); }
  }
  function updateSql(sqlText: string) { setTabs(v => v.map(t => t.id === active ? { ...t, sql: sqlText, dirty: t.dirty || t.sql !== sqlText } : t)); }
  async function execute(compile = false, command?: string) {
    const range = editor.current?.state.selection.main;
    const selected = range && !range.empty ? editor.current!.state.sliceDoc(range.from, range.to) : '';
    const hasSelection = !!range && !range.empty;
    const source = command ?? (hasSelection ? selected : current.sql);
    const selection = command === undefined && hasSelection;
    const documentSource = selection ? editor.current!.state.doc.toString() : current.sql;
    const submittedFrom = selection ? range!.from : 0;
    const submittedTo = selection ? range!.to : documentSource.length;
    if (busy.current || !source.trim() || !sessionId || transactionState === 'UNKNOWN') return;
    if (compile && transactionState === 'ABORTED') return;
    const warning = compile ? undefined : mutationWarning(source);
    if (warning && !window.confirm(warning)) return;
    busy.current = true;
    setRunningTab(active);
    controller.current?.abort(); controller.current = new AbortController(); setRunning(true); setNotice('');
    const started = Date.now();
    try {
      const data = await runSql(effectiveConnection, source, compile, controller.current.signal);
      if (data.transactionState) setTransactionState(data.transactionState);
      for (const item of data.results ?? []) {
        if (item.kind === 'Commit') settlePending('committed');
        if (item.kind === 'Rollback') settlePending('rolledBack');
      }
      if (command === undefined) updateView({ result: data, output: compile ? 'plan' : 'results', source: current.sql, selection, outcome: compile ? undefined : data.results?.at(-1)?.commitState });
      recordHistory(source, compile, data.durationMs, data.rows.length);
      if (!compile) { await refresh(); await refreshStorage(); }
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e); handleFailure(e);
      if ((e as { transactionState?: string }).transactionState === 'ABORTED') await refresh();
      if (command === undefined) {
        const position = e as { line?: number; column?: number };
        updateView({ diagnostic: mapDiagnostic(documentSource, submittedFrom, submittedTo, position.line ?? 0, position.column ?? 0) });
      }
      recordHistory(source, compile, Date.now() - started, 0, message);
    } finally { busy.current = false; setRunning(false); setRunningTab(undefined); }
  }
  function addTab() { if (tabs.length >= 50) { setNotice('最多保留 50 个查询标签。'); return; } const id = crypto.randomUUID(); setTabs(v => [...v, { id, name: `query_${v.length + 1}.sql`, sql: '-- New query\n' }]); setActive(id); }
  function insertTable(table: Table) { updateSql(`${current.sql}\nSELECT * FROM ${table.name};`); }
  function closeTab(id: string) {
    if (tabs.length === 1) return;
    if (id === runningTab) { setNotice('该标签正在执行，请等待请求结束。'); return; }
    if (tabs.find(tab => tab.id === id)?.dirty && !window.confirm('关闭标签将删除此 SQL 草稿，确认关闭？')) return;
    const next = tabs.filter(tab => tab.id !== id);
    liveTabs.current.delete(id);setTabs(next);
    setViews(previous => { const copy = { ...previous };delete copy[id];return copy; });
    if (id === active) setActive(next[next.length - 1].id);
  }
  function renameTab() {
    const name = window.prompt('查询标签名称', current.name)?.trim();
    if (!name) return;
    if (name.length > 100) { setNotice('标签名称不能超过 100 个字符。'); return; }
    setTabs(previous => previous.map(tab => tab.id === active ? { ...tab, name } : tab));
  }
  async function importSql(file?: File) {
    if (!file || importing.current) return;
    importing.current = true;
    try {
      if (!/\.sql$/i.test(file.name)) throw new Error('请选择 .sql 文件。');
      if (file.size > MAX_SQL_FILE_BYTES) throw new Error('SQL 文件不能超过 8 MiB。');
      const source = decodeSqlFile(new Uint8Array(await file.arrayBuffer()));
      if (liveTabs.current.size >= 50) throw new Error('最多保留 50 个查询标签。');
      const id = crypto.randomUUID();
      setTabs(previous => [...previous, { id, name: file.name.slice(0, 100), sql: source, dirty: false }]);
      setActive(id);
    } catch (error) { setNotice(error instanceof Error ? error.message : String(error)); }
    finally { importing.current = false; }
  }
  function exportSql() {
    const url = URL.createObjectURL(new Blob([current.sql], { type: 'application/sql;charset=utf-8' }));
    const link = document.createElement('a');link.href = url;link.download = sqlFilename(current.name);link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function locateDiagnostic() {
    const view = editor.current;
    if (!view || !diagnostic || view.state.doc.toString() !== diagnostic.source) return;
    view.dispatch({ selection: { anchor: diagnostic.offset, head: diagnostic.offset + diagnostic.length },
      effects: EditorView.scrollIntoView(diagnostic.offset, { y: 'center' }) });
    view.focus();
  }
  function shortcut(event: React.KeyboardEvent) {
    if (historyDialogOpen) return;
    if ((!event.ctrlKey && !event.metaKey) || event.altKey || event.nativeEvent.isComposing) return;
    if (event.key === 'Enter' && (event.target as HTMLElement).closest('.cm-editor')) {
      event.preventDefault();event.stopPropagation();void execute(event.shiftKey);
    } else if (event.key.toLowerCase() === 's') { event.preventDefault();exportSql(); }
    else if (event.key.toLowerCase() === 'o') { event.preventDefault();fileInput.current?.click(); }
  }

  function historyKeys(event: React.KeyboardEvent<HTMLDialogElement>) {
    if (event.key !== 'Tab') return;
    const controls = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled)'));
    const first = controls[0], last = controls.at(-1);
    if (!first || !last) return;
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
  }

  const historyContent = <><div className="right-head"><span>查询历史</span><button className="icon-btn" title="清空历史" disabled={!history.length} onClick={() => { if (window.confirm('清除全部本地查询历史？')) setHistory([]); }}><Trash2 size={14}/></button></div><div className="history-search"><Search size={14}/><input aria-label="检索查询历史" placeholder="检索 SQL、连接、错误" value={historyQuery} onChange={event => setHistoryQuery(event.target.value)}/></div>{visibleHistory.length === 0 ? <div className="empty-history"><History size={25}/><p>{history.length ? '没有匹配记录' : '暂无查询历史'}</p></div> : <div className="history-list">{visibleHistory.map(item => <button className="history-item" key={item.id} title="载入 SQL（不执行）" onClick={() => { updateSql(item.sql); if (historyDialog.current?.open) { historyDialog.current.close(); editor.current?.focus(); } }}><div><span className={item.error !== undefined ? 'history-dot failed' : 'history-dot'}/><time>{new Date(item.at).toLocaleString([], { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' })}</time><small>{item.durationMs.toFixed(0)} ms</small></div><div className="history-origin">真实 · {item.connection} · {item.action === 'compile' ? '编译' : '执行'} · {item.error !== undefined ? '失败' : '成功'}</div><p>{item.sql.replace(/\s+/g, ' ').slice(0, 80)}</p>{item.error && <em>{item.error}</em>}</button>)}</div>}</>;

  return <div className={darkMode ? 'app-shell dark-mode' : 'app-shell'} onKeyDownCapture={shortcut}>
    <dialog className="settings-dialog" open={settingsOpen} aria-label="设置"><div className="settings-body"><button className="icon-btn settings-close" aria-label="关闭设置" onClick={() => setSettingsOpen(false)}><X size={17}/></button><h2>工作台设置</h2><label>结果显示上限<input type="number" min="100" max="10000" step="100" value={resultLimit} onChange={event => { const value=Math.max(100,Math.min(10000,Number(event.target.value)||100));setResultLimit(value);localStorage.setItem('minisql-result-limit',String(value)); }}/></label><label className="settings-check"><input type="checkbox" checked={darkMode} onChange={event => {setDarkMode(event.target.checked);localStorage.setItem('minisql-theme',String(event.target.checked?'dark':'light'));}}/>深色主题</label><button className="settings-reset" onClick={() => { localStorage.removeItem('minisql-result-limit');localStorage.removeItem('minisql-theme');setResultLimit(1000);setDarkMode(false); }}>恢复默认设置</button><button className="settings-refresh" onClick={() => void refreshStorage()}>刷新存储统计</button>{storageInfo ? <StorageStatisticsView value={storageInfo} disabled={running || transactionState !== 'IDLE'} onConfigure={async action => { const buffer = await configureBuffer(effectiveConnection, action); setStorageInfo(previous => previous ? { ...previous, buffer } : previous); }}/> : <div className="storage-info">连接真实 C++ 数据库后可查看存储统计。</div>}</div></dialog>
    <dialog className="history-dialog" aria-label="查询历史" ref={historyDialog} onKeyDown={historyKeys} onClose={() => setHistoryDialogOpen(false)}><div className="history-dialog-body"><div className="history-dialog-close"><button className="icon-btn" title="关闭查询历史" autoFocus onClick={() => historyDialog.current?.close()}><X size={18}/></button></div>{historyDialogOpen && historyContent}</div></dialog>
    <input ref={fileInput} type="file" accept=".sql" hidden aria-label="导入 SQL 文件" onChange={event => { const file = event.target.files?.[0];event.target.value = '';void importSql(file); }}/>
    {!draftSaved && <div className="draft-alert" role="alert">草稿保存失败：当前修改仅保留在此页面，请导出 SQL 后再关闭。</div>}
    {!historySaved && <div className="draft-alert" role="alert">历史保存失败：本次历史修改尚未写入本地存储。</div>}
    {historyOmitted && <div className="draft-alert" role="status">本次 SQL 超过历史单条容量，未加入历史；执行结果不受影响。</div>}
    <header className="topbar"><div className="brand"><div className="brand-mark"><Database size={17}/></div><span>MiniSQL</span><b>Studio</b></div><div className="top-actions"><button className="connection-select" disabled={running} onClick={() => void connect()}><span className={health === 'ok' ? 'status-dot' : 'status-dot status-warn'}/>{connection.name}<ChevronDown size={14}/></button>{health && <span className="health-label" title="C++ bridge 健康状态">{health === 'ok' ? '服务正常' : '服务降级'}</span>}<button className="history-toggle" title="打开查询历史" aria-haspopup="dialog" onClick={() => setHistoryDialogOpen(true)}><History size={17}/></button><button className="settings-toggle" title="打开设置" aria-label="打开设置" onClick={() => setSettingsOpen(true)}><Settings2 size={17}/></button><button className="avatar">M</button></div></header>
    <div className="transaction-toolbar" aria-label="事务控制">
      <span role="status" data-testid="transaction-state">{!sessionId ? transactionState === 'EXPIRED' ? '会话已过期' : '未连接' : ({ IDLE: '自动提交', ACTIVE: '事务进行中 · 未提交', ABORTED: '事务失败 · 必须回滚', UNKNOWN: '提交状态未知' }[transactionState] ?? transactionState)}</span>
      <button className="icon-btn" title="连接会话" aria-label="连接会话" disabled={running || !!sessionId} onClick={connect}><Plug size={16}/></button>
      <button className="icon-btn" title="断开会话" aria-label="断开会话" disabled={running || !sessionId} onClick={() => disconnect()}><Unplug size={16}/></button>
      <button className="icon-btn" title="开始事务" aria-label="开始事务" disabled={running || !sessionId || transactionState !== 'IDLE'} onClick={() => execute(false, 'BEGIN;')}><CirclePlay size={16}/></button>
      <button className="icon-btn" title="提交事务" aria-label="提交事务" disabled={running || !sessionId || transactionState !== 'ACTIVE'} onClick={() => execute(false, 'COMMIT;')}><Check size={16}/></button>
      <button className="icon-btn" title="回滚事务" aria-label="回滚事务" disabled={running || !sessionId || !['ACTIVE', 'ABORTED'].includes(transactionState)} onClick={() => execute(false, 'ROLLBACK;')}><Undo2 size={16}/></button>
    </div>
    <div className="workspace">
      <aside className="sidebar"><div className="side-head"><span>DATABASE</span><button className="icon-btn" title="刷新目录" onClick={refresh}><RefreshCw size={15}/></button></div><div className="database-node"><Database size={15}/><span>{connection.name}</span><span className="online">●</span></div><div className="tree-content"><div className="tree-section"><ChevronDown size={14}/><FolderTree size={15}/><span>SCHEMAS</span></div><div className="schema"><ChevronDown size={14}/><span className="schema-dot">◈</span><span>main</span></div><div className="tree-section nested"><ChevronDown size={14}/><Table2 size={15}/><span>TABLES <em>{tables.length}</em></span><button className="tree-add" title="新建查询" onClick={addTab}><Plus size={13}/></button></div>{visibleTables.map(table => <div className="table-node" key={table.name}><button className="tree-chevron" onClick={() => setExpanded(v => ({ ...v, [table.name]: !v[table.name] }))}>{expanded[table.name] ? <ChevronDown size={13}/> : <ChevronRight size={13}/>}</button><Table2 size={14}/><button className="tree-label" onClick={() => insertTable(table)}>{table.name}</button><span className="row-count">{table.rowCount}</span>{expanded[table.name] && <div className="columns">{table.columns.map(column => { const isPrimary = column.primaryKey || table.keys?.some(key => key.primary && key.columns.includes(column.name)); return <div className="column-node" key={column.name}><span className={isPrimary ? 'pk' : 'col-dot'}>{isPrimary ? '◆' : '·'}</span><span>{column.name}</span><small>{column.type}</small></div>; })}</div>}</div>)}</div><div className="sidebar-bottom"><button><CircleHelp size={15}/> Documentation</button><span>v0.1.0 · C++ engine</span></div></aside>
      <main className="main"><div className="query-tabs">{tabs.map(tab => <button className={tab.id === active ? 'query-tab active' : 'query-tab'} key={tab.id} onClick={() => setActive(tab.id)}><FileCode2 size={14}/>{tab.name}<X size={13} onClick={e => { e.stopPropagation(); closeTab(tab.id); }}/></button>)}<button className="new-tab" title="新建查询" onClick={addTab}><Plus size={16}/></button><div className="tab-spacer"/><button className="toolbar-btn" disabled={running || (!sessionId || ['UNKNOWN','ABORTED'].includes(transactionState))} onClick={() => execute(true)}><Braces size={15}/> Explain</button><button className="run-btn" onClick={() => execute(false)} disabled={running || (!sessionId || transactionState === 'UNKNOWN')}><Play size={15} fill="currentColor"/>{running ? 'Running...' : 'Run'}</button></div>
        <div className="query-state"><span title={current.name}>{current.name}</span><button className="icon-btn" aria-label="重命名查询" title="重命名查询" onClick={renameTab}><Pencil size={14}/></button>{current.dirty && <small>已修改</small>}{runningTab === active && <small role="status">执行中</small>}{result && resultSource !== current.sql && <small data-testid="stale-result">结果对应旧 SQL</small>}{['pending','rolledBack','unknown'].includes(outcome ?? '') && <small data-testid="result-outcome">{outcome === 'pending' ? '未提交结果' : outcome === 'rolledBack' ? '事务已回滚' : '提交状态未知'}</small>}</div>
        <div className="file-toolbar"><button className="icon-btn" aria-label="导入 SQL" title="导入 SQL" onClick={() => fileInput.current?.click()}><Upload size={15}/></button><button className="icon-btn" aria-label="导出 SQL" title="导出 SQL" onClick={exportSql}><Download size={15}/></button>{selectedResult && <small data-testid="selection-result">选区结果</small>}{diagnostic && <><button className="icon-btn" aria-label="定位错误" title={`第 ${diagnostic.line} 行，第 ${diagnostic.column} 列`} disabled={current.sql.replace(/\r\n|\r/g, '\n') !== diagnostic.source} onClick={locateDiagnostic}><Search size={15}/></button><small data-testid="diagnostic-position">第 {diagnostic.line} 行，第 {diagnostic.column} 列{current.sql.replace(/\r\n|\r/g, '\n') !== diagnostic.source ? ' · 原 SQL 已修改' : ''}</small></>}</div>
        <section className="editor-wrap"><CodeMirror key={active} onCreateEditor={view => { editor.current = view; setEditorInstance(view); }} value={current?.sql ?? ''} height="100%" theme="light" extensions={editorExtensions} onChange={updateSql} basicSetup={{ lineNumbers: true, foldGutter: true, highlightActiveLine: true, autocompletion: true }} /></section>
        <section className="output"><div className="output-tabs"><button className={output === 'results' ? 'selected' : ''} onClick={() => setOutput('results')}><Table2 size={14}/> Result <span>{result?.rows.length ?? 0}</span></button><button className={output === 'plan' ? 'selected' : ''} onClick={() => setOutput('plan')}><Activity size={14}/> Plan <span>{result?.plan.length ?? 0}</span></button><button className={output === 'ast' ? 'selected' : ''} onClick={() => setOutput('ast')}><Braces size={14}/> AST</button><button className={output === 'tokens' ? 'selected' : ''} onClick={() => setOutput('tokens')}><Terminal size={14}/> Diagnostics</button><div className="output-spacer"/><span className="query-meta">{result ? `${result.durationMs.toFixed(1)} ms · ${result.affectedRows} affected` : 'Ready'}</span></div><div className="output-body">{notice ? <div className="error-state"><span>!</span><div><strong>Query failed</strong><p>{notice}</p><button onClick={() => setNotice('')}>Dismiss</button></div></div> : output === 'results' ? <Results result={result}/> : output === 'plan' ? <Plan result={result}/> : output === 'ast' ? <pre className="json-view">{result?.ast ? JSON.stringify(result.ast, null, 2) : 'Compile a query to inspect its AST.'}</pre> : <Diagnostics result={result}/>}</div></section>
      </main>
      <aside className="rightbar">{historyContent}<div className="right-bottom"><Search size={14}/><input placeholder="Search tables" value={filter} onChange={e => setFilter(e.target.value)}/></div></aside>
    </div>
  </div>;
}

export default App;

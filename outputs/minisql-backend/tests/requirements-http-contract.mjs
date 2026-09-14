// 第十七章 / 第十九章 REQ-UI-010 的 HTTP 集成契约测试。
// 覆盖：/api/storage 与 /api/storage/stats 别名、未实现能力返回 501、
// 序列化计划执行路由以及 PLAN_STALE_SCHEMA 的 HTTP 映射。
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import assert from 'node:assert/strict';

let checks = 0;
const ok = (value, name) => { assert.ok(value, name); ++checks; };
const equal = (actual, expected, name) => { assert.equal(actual, expected, name); ++checks; };

const server = spawn(process.execPath, [fileURLToPath(new URL('../scripts/database-bridge.mjs', import.meta.url))], {
  env: { ...process.env, PORT: '0', MINISQL_DB: join(mkdtempSync(join(tmpdir(), 'minisql-req-http-')), 'db.pages') },
  windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(server, 'exit');
let errors = '', url, session;
server.stderr.on('data', chunk => { errors += chunk; });
const timer = setTimeout(() => server.kill(), 60000);

async function request(path, body, method = 'POST') {
  const response = await fetch(url + path, {
    method,
    headers: method === 'POST' ? { 'Content-Type': 'application/json' } : {},
    signal: AbortSignal.timeout(15000),
    ...(method === 'POST' ? { body: JSON.stringify(body ?? {}) } : {}),
  });
  return { status: response.status, data: await response.json() };
}

try {
  url = await new Promise((resolve, reject) => {
    let output = '';
    server.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) resolve(match[0]);
    });
    server.once('error', reject);
    server.once('exit', () => reject(new Error(errors || 'Server exited before readiness')));
  });

  const opened = await request('/sessions');
  equal(opened.status, 201, 'session opens');
  session = `/sessions/${opened.data.sessionId}`;
  equal((await request(session + '/execute', { sql: 'CREATE TABLE t(id INT, note VARCHAR);' })).status, 200, 'table created');
  equal((await request(session + '/execute', { sql: "INSERT INTO t VALUES(1,'a'),(2,'b');" })).status, 200, 'rows inserted');

  // ---- GET /api/storage 与规格书列出的 /api/storage/stats 等价 ----
  const storage = await request('/storage', undefined, 'GET');
  const storageStats = await request('/storage/stats', undefined, 'GET');
  equal(storage.status, 200, '/api/storage is served');
  equal(storageStats.status, 200, '/api/storage/stats alias is served');
  equal(storageStats.data.pageSize, 4096, 'page size reported');
  ok(storageStats.data.allocatedPages > 0, 'allocated pages reported');
  equal(storageStats.data.buffer.available, false, 'unexposed buffer capability is marked unavailable');
  equal(storageStats.data.buffer.reason, 'backend-not-exposed', 'unavailable reason is explicit');
  equal(storageStats.data.policy, 'backend-not-exposed', 'policy is not faked');
  ok(!('hits' in storageStats.data.buffer), 'unavailable capability does not fake zero values');
  equal(storageStats.data.buffer.endpoint, '/api/sessions/:id/buffer', 'alias points at the real buffer endpoint');

  // ---- 真实缓存统计来自后端会话端点 ----
  const buffer = (await request(session + '/catalog', undefined, 'GET')).data.buffer;
  ok(typeof buffer.capacity === 'number' && buffer.capacity > 0, 'session buffer reports a real capacity');
  ok(typeof buffer.hits === 'number' && typeof buffer.misses === 'number', 'session buffer reports hit/miss counters');
  ok(['LRU', 'FIFO'].includes(buffer.policy), 'session buffer reports the active policy');

  // ---- 未实现的能力返回 501，不伪装成功 ----
  {
    // 派生表上的 JOIN 是引擎明确未实现的构造：绑定成功、规划阶段报 NotImplemented。
    const unsupported = await request(session + '/execute', { sql: 'SELECT * FROM (SELECT id FROM t) x JOIN t y ON x.id = y.id;' });
    equal(unsupported.status, 501, 'unimplemented capability maps to 501');
    equal(unsupported.data.success, false, 'unimplemented capability is not reported as success');
    equal(unsupported.data.error.code, 9001, 'unimplemented capability keeps the NotImplemented code');
    equal(unsupported.data.error.type, 'NotImplementedError', 'unimplemented capability keeps its error type');
  }
  {
    // 无法绑定的语句一律不执行（fail-closed），但错误码回报真实原因，不再把
    // 「SQL 检查失败」统一伪装成权限错误 403；否则同一句 SQL 在启用/禁用权限
    // 目录时会得到不同的状态码。身份校验仍在授权之前完成，因此未授权调用方
    // 拿到的依旧是权限错误，这里不构成能力泄露。
    const unbound = await request(session + '/execute', { sql: 'WITH RECURSIVE r(n) AS (SELECT id FROM t) SELECT * FROM r;' });
    equal(unbound.status, 501, 'unbindable unimplemented construct maps to 501');
    equal(unbound.data.error.code, 9001, 'unbindable statement keeps its real NotImplemented code');
    equal(unbound.data.success, false, 'unbindable statement is not reported as success');

    // 对象不存在属于「SQL 检查失败」，按第十九章映射到 422。
    const missing = await request(session + '/execute', { sql: 'SELECT * FROM missing_relation;' });
    equal(missing.status, 422, 'unbound missing relation maps to 422 instead of a permission error');
    equal(missing.data.error.code, 3001, 'missing relation keeps the catalog error code');
    ok(/missing_relation/.test(missing.data.error.message), 'missing relation diagnostic names the relation');
  }

  // ---- 序列化计划执行：指纹有效时可执行 ----
  const compiled = await request(session + '/compile', { sql: 'SELECT id FROM t ORDER BY id;' });
  equal(compiled.status, 200, 'plan compiles over HTTP');
  const plan = compiled.data.plan;
  ok(Array.isArray(plan) && plan.length > 0, 'compile returns plan nodes');
  ok(typeof plan[0].catalogFingerprint === 'string' && plan[0].catalogFingerprint.length === 16, 'plan nodes carry a catalog fingerprint');

  const executed = await request(session + '/execute-plan', { plan });
  equal(executed.status, 200, 'fresh plan executes over HTTP');
  equal(executed.data.success, true, 'fresh plan execution succeeds');
  assert.deepEqual(executed.data.results[0].rows, [[1], [2]]); ++checks;

  // ---- 缺失计划文档是请求格式错误 ----
  equal((await request(session + '/execute-plan', {})).status, 400, 'missing plan document is a 400');
  equal((await request(session + '/execute-plan', { plan: 'not-an-object' })).status, 400, 'non-object plan document is a 400');

  // ---- Catalog 变化后同一份计划被拒绝，并映射为非 2xx ----
  equal((await request(session + '/execute', { sql: 'CREATE TABLE later(x INT);' })).status, 200, 'catalog changes after compilation');
  const stale = await request(session + '/execute-plan', { plan });
  equal(stale.data.success, false, 'stale plan is refused over HTTP');
  equal(stale.data.error.type, 'PLAN_STALE_SCHEMA', 'stale plan reports PLAN_STALE_SCHEMA');
  equal(stale.status, 422, 'stale plan maps to 422');
  ok(/recompile/.test(stale.data.error.message), 'stale diagnostic tells the caller to recompile');

  // ---- 资源上限映射到 413 ----
  const overflow = Array.from({ length: 10001 }, () => 'SELECT * FROM t;').join(' ');
  const budget = await request(session + '/execute', { sql: overflow });
  equal(budget.status, 413, 'statement budget maps to 413');
  ok(/budget exceeded/i.test(budget.data.error.message), 'statement budget message is explicit');

  console.log(`${checks} REQ-UI-010 HTTP contract checks passed`);
} finally {
  clearTimeout(timer);
  server.kill();
  await exited;
}

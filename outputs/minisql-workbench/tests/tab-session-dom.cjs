const path = require('node:path');
let playwright;
try { playwright = require('playwright'); }
catch { playwright = require(path.join(process.env.USERPROFILE, '.codex/playwright-runtime/node_modules/playwright')); }
const { chromium } = playwright;
const assert = require('node:assert/strict');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const context = await browser.newContext({ viewport: { width: 1280, height: 850 } });
  await context.addInitScript(() => localStorage.clear());
  const page = await context.newPage();
  let nextSession = 0;
  const executeSessions = [];
  const maintenance = [];
  const table = { name: 'students', rowCount: 2, columns: [{ name: 'id', type: 'int', primaryKey: true, nullable: false }],
    indexes: [{ name: 'students_pk', columns: ['id'], unique: true }] };
  const inspect = { kind: 'IndexInspect', table: 'students', index: 'students_pk', present: true, root: { id: 3, generation: 1 },
    height: 1, nodeCount: 1, leafCount: 1, rowCount: 2, leafChainLength: 1, rootReachable: true,
    leafChainLinked: true, parentLinksValid: true, storage: 'page-file', problems: [], pages: [] };
  await page.route('http://127.0.0.1:8081/api/**', async route => {
    const request = route.request();
    const pathname = new URL(request.url()).pathname;
    const json = body => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    if (pathname === '/api/sessions' && request.method() === 'POST') return json({ sessionId: `s${++nextSession}`, transactionState: 'IDLE' });
    if (pathname.endsWith('/execute/stream')) {
      const session = pathname.split('/')[3];
      const sql = JSON.parse(request.postData() || '{}').sql || '';
      executeSessions.push([session, sql]);
      const value = sql.includes('2') ? 2 : 1;
      await new Promise(resolve => setTimeout(resolve, value === 2 ? 350 : 40));
      const frames = [
        { type: 'meta', columns: ['value'], columnTypes: ['int'], transactionState: 'IDLE' },
        { type: 'row', values: [value] },
        { type: 'complete', success: true, rowCount: 1, affectedRows: 0, durationMs: value, statements: 1, transactionState: 'IDLE' },
      ];
      return route.fulfill({ status: 200, contentType: 'application/x-ndjson', body: frames.map(frame => JSON.stringify(frame)).join('\n') + '\n' });
    }
    if (pathname.endsWith('/index-inspect')) return json(inspect);
    if (pathname.endsWith('/index-verify')) { maintenance.push('verify'); return json({ kind: 'IndexVerify', valid: true, problems: [] }); }
    if (pathname.endsWith('/index-rebuild')) { maintenance.push('rebuild'); return json({ kind: 'IndexRebuild', valid: true }); }
    if (pathname.endsWith('/catalog')) return json({ tables: [table], buffer: { available: false } });
    if (pathname.endsWith('/close')) return json({ closed: true });
    if (pathname === '/api/health') return json({ status: 'ok', engine: 'mock' });
    if (pathname === '/api/storage') return json({ pageSize: 4096, fileBytes: 0, allocatedPages: 0 });
    return json({});
  });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'networkidle' });
    await page.getByRole('button', { name: '断开会话' }).waitFor();
    await page.locator('.new-tab').click();
    await page.getByRole('button', { name: '连接会话' }).click();
    await page.waitForFunction(() => !document.querySelector('button[aria-label="断开会话"]')?.hasAttribute('disabled'));
    await page.locator('.cm-content').fill('SELECT 2;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    await page.locator('.query-tab').filter({ hasText: 'query_1.sql' }).click();
    await page.locator('.cm-content').fill('SELECT 1;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    const resultGrid = page.getByRole('grid', { name: '查询结果' });
    const resultCell = resultGrid.locator('[data-row="0"][data-column="0"]');
    await resultCell.filter({ hasText: /^1$/ }).waitFor();
    assert.equal(await resultCell.textContent(), '1', 'first tab receives its own response');
    await page.locator('.query-tab').filter({ hasText: 'query_2.sql' }).click();
    await resultCell.filter({ hasText: /^2$/ }).waitFor();
    assert.equal(await resultCell.textContent(), '2', 'second tab receives its delayed response');
    assert.deepEqual(
      executeSessions.map(item => item[0]).sort(),
      ['s1', 's2'],
      `queries use independent backend sessions: ${JSON.stringify(executeSessions)}`,
    );
    const draft = await page.evaluate(() => localStorage.getItem('minisql-studio-tabs-v1') || '');
    assert.equal(/sessionId|password|s1|s2/.test(draft), false, 'live session and credentials are not persisted');

    await page.locator('.index-node').click();
    await page.getByRole('button', { name: '校验索引' }).click();
    await page.getByRole('status').filter({ hasText: '索引校验通过' }).waitFor();
    page.once('dialog', dialog => dialog.accept());
    await page.getByRole('button', { name: '重建索引' }).click();
    await page.getByRole('status').filter({ hasText: '索引重建完成' }).waitFor();
    assert.deepEqual(maintenance, ['verify', 'rebuild']);
    assert.deepEqual(errors, []);
    console.log('tab session browser regression passed: independent sessions, out-of-order responses, secret-free drafts, index verify/rebuild');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });

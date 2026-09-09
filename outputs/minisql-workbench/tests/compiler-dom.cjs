const path = require('node:path');
let playwright;
try {
  playwright = require('playwright');
} catch {
  playwright = require(path.join(process.env.USERPROFILE, '.codex/playwright-runtime/node_modules/playwright'));
}
const { chromium } = playwright;
const assert = require('node:assert/strict');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 960 } });
  try {
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="断开会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 30000 });
    await page.locator('.cm-content').fill('CREATE TABLE inspect_only(id INT NOT NULL); SELECT s.id+1 AS next FROM inspect_only AS s LEFT JOIN inspect_only AS r ON s.id=r.id WHERE 1=1 ORDER BY next DESC NULLS LAST LIMIT 2;');
    const compiled = page.waitForResponse(response => response.url().includes('/compile') && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Explain', exact: true }).click();
    assert.equal((await compiled).status(), 200);
    await page.getByRole('button', { name: 'AST', exact: true }).click();
    await page.locator('.json-view').filter({ hasText: 'inspect_only' }).waitFor();
    assert.match(await page.locator('.json-view').innerText(), /"alias": "next"/);
    assert.match(await page.locator('.json-view').innerText(), /"tableAlias": "s"/);
    await page.getByRole('button', { name: 'Diagnostics', exact: true }).click();
    await page.getByRole('table', { name: 'Token 序列' }).waitFor();
    assert.ok(await page.getByRole('table', { name: 'Token 序列' }).locator('tbody tr').count() > 20);
    assert.match(await page.locator('.diagnostics').innerText(), /未运行/);
    await page.getByRole('button', { name: /^Plan/ }).click();
    assert.ok(await page.locator('.plan-row').count() > 0);
    assert.match(await page.locator('.plan-view').innerText(), /LeftJoin/);
    await page.getByRole('button', { name: '优化计划', exact: true }).click();
    assert.equal(await page.getByRole('button', { name: '优化计划', exact: true }).getAttribute('aria-pressed'), 'true');
    assert.match(await page.locator('.result-warning[role="status"]').innerText(), /已收敛.*2 轮/);
    await page.locator('summary').filter({ hasText: '优化记录' }).click();
    assert.match(await page.locator('details pre').innerText(), /ruleId/);
    await page.getByRole('button', { name: '断开会话' }).click();
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="连接会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 30000 });
    const catalogResponse = await page.request.get('http://127.0.0.1:8081/api/catalog', { headers: { 'X-MiniSQL-User': 'admin' } });
    assert.equal(catalogResponse.status(), 200);
    const catalog = await catalogResponse.json();
    assert.ok(!catalog.tables.some(table => table.name === 'inspect_only'));
    await page.getByRole('button', { name: '连接会话' }).click();
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="断开会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 30000 });
    let writes = 0;
    page.on('request', request => { if (request.url().endsWith('/api/execute')) ++writes; });
    await page.locator('.cm-content').fill('UPDATE inspect_only SET id=1;');
    const dialog = page.waitForEvent('dialog');
    const click = page.getByRole('button', { name: 'Run', exact: true }).click();
    await (await dialog).dismiss();
    await click;
    assert.equal(writes, 0);
    assert.deepEqual(errors, []);
    console.log('13 browser assertions passed: real C++ compile, table alias AST, join plan, tokens, stages, optimized plan, convergence, no catalog mutation, cancelled write confirmation; no images generated');
  } finally {
    await page.getByRole('button', { name: '断开会话' }).click({ timeout: 1000 }).catch(() => {});
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });

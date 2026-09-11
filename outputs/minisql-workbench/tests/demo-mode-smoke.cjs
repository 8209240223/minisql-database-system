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
    const demoRequests = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => { if (request.url().includes('127.0.0.1:8082')) demoRequests.push(request.url()); });
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'networkidle' });
    await page.locator('.connection-mode').filter({ hasText: '自研 MiniSQL' }).waitFor();
    await page.getByRole('button', { name: '断开会话' }).click();
    await page.locator('.connection-select').click();
    const dialog = page.getByRole('dialog', { name: '连接管理' });
    await dialog.locator('.connection-item-select').filter({ hasText: 'MiniSQL 本地演示' }).click();
    await dialog.getByRole('button', { name: '测试连接', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '连接正常' }).waitFor();
    await dialog.getByRole('button', { name: '连接', exact: true }).click();
    await page.locator('.database-node').filter({ hasText: 'MiniSQL 本地演示' }).waitFor();
    await page.locator('.connection-mode').filter({ hasText: '本地演示' }).waitFor();
    await page.locator('.cm-content').fill('SELECT COUNT(*) AS total FROM students;');
    const executed = page.waitForResponse(response => response.url().includes(':8082/api/sessions/') && response.url().endsWith('/execute/stream') && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    assert.equal((await executed).status(), 200);
    await page.locator('.result-content').filter({ hasText: '48' }).waitFor();
    assert.ok(demoRequests.some(url => url.includes(':8082/api/')), '演示模式请求发送到 8082');
    assert.deepEqual(errors, []);
    console.log('Demo mode smoke passed: independent 8082 bridge, mode label and real query result');
  } finally {
    await page.getByRole('button', { name: '断开会话' }).click({ timeout: 1000 }).catch(() => {});
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="连接会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 3000 }).catch(() => {});
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });

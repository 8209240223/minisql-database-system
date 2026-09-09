const { chromium } = require(require('node:path').join(process.env.USERPROFILE, '.codex/playwright-runtime/node_modules/playwright'));
const { spawn } = require('node:child_process');
const { mkdtempSync } = require('node:fs');
const { join, resolve } = require('node:path');
const { once } = require('node:events');
const assert = require('node:assert/strict');

(async () => {
  const backend = resolve(__dirname, '../../minisql-backend');
  const directory = mkdtempSync(join(backend, 'tests/artifacts/bigint-dom-'));
  const server = spawn(process.execPath, [join(backend, 'scripts/database-bridge.mjs')], {
    env: { ...process.env, PORT: '0', MINISQL_DB: join(directory, 'database.pages') },
    windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
  });
  const exited = once(server, 'exit');
  const timer = setTimeout(() => server.kill(), 60000);
  let browser;
  let page;
  try {
    const api = await new Promise((resolveUrl, reject) => {
      let output = '';
      server.stdout.on('data', chunk => {
        output += chunk;
        const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
        if (match) resolveUrl(match[0]);
      });
      server.on('error', reject);
      server.on('exit', () => reject(new Error('测试数据库服务提前退出')));
      server.stderr.resume();
    });
    browser = await chromium.launch({ channel: 'msedge', headless: true });
    page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    // 页面仍走真实 HTTP/C++ 链路，但所有请求转发到独立数据库，避免修改用户数据。
    await page.route('http://127.0.0.1:8081/api/**', async route => {
      const response = await route.fetch({ url: route.request().url().replace('http://127.0.0.1:8081/api', api) });
      await route.fulfill({ response });
    });
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173');
    await page.locator('.cm-content').fill("CREATE TABLE exact_values(n BIGINT); INSERT INTO exact_values(n) VALUES(CAST('9223372036854775807' AS BIGINT)); SELECT n,n-1 AS previous FROM exact_values;");
    const responsePromise = page.waitForResponse(response => response.url().endsWith('/api/execute'));
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    const response = await responsePromise;
    assert.equal(response.status(), 200);
    const data = await response.json();
    assert.deepEqual(data.columnTypes, ['bigint', 'bigint']);
    assert.equal(data.integerEncoding, 'safe-number-or-decimal-string');
    await page.getByRole('cell', { name: '9223372036854775807', exact: true }).waitFor();
    assert.equal(await page.getByRole('cell', { name: '9223372036854775806', exact: true }).count(), 1);
    assert.deepEqual(data.rows, [['9223372036854775807', '9223372036854775806']]);
    assert.deepEqual(errors, []);
    console.log('BIGINT browser verification passed: INSERT CAST through isolated real database, exact result cells, column types, wire encoding, no page errors; no images');
  } finally {
    if (page) await page.unrouteAll({ behavior: 'wait' });
    if (browser) await browser.close();
    clearTimeout(timer);
    server.kill();
    await exited;
  }
})().catch(error => { console.error(error); process.exitCode = 1; });

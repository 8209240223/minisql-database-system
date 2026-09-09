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
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  await context.addInitScript(() => localStorage.clear());
  const page = await context.newPage();
  const requests = [];
  const errors = [];
  page.on('request', request => {
    if (request.url().includes('127.0.0.1:8081')) requests.push({ url: request.url(), user: request.headers()['x-minisql-user'] ?? '' });
  });
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'networkidle' });
    await page.getByRole('button', { name: 'MiniSQL C++', exact: true }).click();
    const connectionDialog = page.getByRole('dialog', { name: '连接管理' });
    await connectionDialog.waitFor();
    assert.equal(await connectionDialog.getByRole('button', { name: '连接', exact: true }).isDisabled(), true, '活动会话禁止切换连接');
    await page.getByRole('button', { name: '断开会话' }).click();
    await page.waitForTimeout(250);
    await connectionDialog.getByRole('button', { name: '新建', exact: true }).click();
    const inputs = connectionDialog.locator('.connection-form input');
    await inputs.nth(0).fill('QA Connection');
    await inputs.nth(1).fill('http://127.0.0.1:8081/api');
    await inputs.nth(2).fill('admin');
    await inputs.nth(3).fill('secret-not-persisted');
    await connectionDialog.getByRole('button', { name: '保存配置', exact: true }).click();
    const stored = await page.evaluate(() => localStorage.getItem('minisql-studio-connections-v1') ?? '');
    assert.match(stored, /QA Connection/);
    assert.equal(stored.includes('secret-not-persisted'), false, '密码不进入连接配置');
    await connectionDialog.getByRole('button', { name: '测试连接', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '连接失败' }).waitFor();
    await inputs.nth(3).fill('');
    await connectionDialog.getByRole('button', { name: '测试连接', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '连接正常' }).waitFor();
    await connectionDialog.getByRole('button', { name: '连接', exact: true }).click();
    await page.locator('.database-node').filter({ hasText: 'QA Connection' }).waitFor();
    await page.getByRole('button', { name: '打开权限与审计' }).click();
    const accessDialog = page.getByRole('dialog', { name: '权限与审计' });
    await accessDialog.waitFor();
    for (const label of ['用户', '角色', '会话', '审计']) assert.equal(await accessDialog.getByRole('button', { name: label, exact: true }).count(), 1);
    const browserUser = 'c2_browser_user';
    const deleteBrowserUser = async () => {
      const row = accessDialog.locator('.access-item').filter({ hasText: browserUser });
      if (!await row.count()) return;
      const confirmation = page.waitForEvent('dialog').then(dialog => dialog.accept());
      await row.getByRole('button', { name: '删除', exact: true }).click();
      await confirmation;
      await row.waitFor({ state: 'detached' });
    };
    await deleteBrowserUser();
    const accessInputs = accessDialog.locator('.access-create input');
    await accessInputs.nth(0).fill(browserUser);
    await accessInputs.nth(1).fill('c2-only-in-memory');
    await accessDialog.getByRole('button', { name: '新增用户', exact: true }).click();
    await accessDialog.getByText(browserUser, { exact: true }).waitFor();
    const userRow = accessDialog.locator('.access-item').filter({ hasText: browserUser });
    await userRow.locator('.access-expand').click();
    await userRow.locator('.access-grant input[placeholder="* 或表名"]').fill('students');
    await userRow.getByRole('button', { name: '授权', exact: true }).click();
    await userRow.getByText('已授权', { exact: true }).waitFor();
    await userRow.getByRole('button', { name: '撤销', exact: true }).click();
    await userRow.getByText('已撤销', { exact: true }).waitFor();
    await deleteBrowserUser();
    await accessDialog.getByRole('button', { name: '会话', exact: true }).click();
    await accessDialog.locator('.access-table').waitFor({ timeout: 10000 });
    assert.equal(await accessDialog.getByRole('columnheader', { name: '锁等待' }).count(), 1);
    await accessDialog.getByRole('button', { name: '关闭' }).click();

    await page.getByRole('button', { name: '打开设置' }).click();
    assert.equal(await page.getByTestId('system-settings').isVisible(), true);
    assert.match(await page.getByTestId('system-settings').innerText(), /资源预算/);
    assert.equal(await page.getByRole('button', { name: '创建全量备份', exact: true }).count(), 1);
    await page.getByRole('button', { name: '关闭设置' }).click();

    const resizer = page.locator('.sidebar-resizer');
    const before = await page.locator('.sidebar').evaluate(element => element.getBoundingClientRect().width);
    const box = await resizer.boundingBox();
    await page.mouse.move(box.x + 4, box.y + 100);
    await page.mouse.down();
    await page.mouse.move(box.x + 70, box.y + 100);
    await page.mouse.up();
    const after = await page.locator('.sidebar').evaluate(element => element.getBoundingClientRect().width);
    assert.ok(after > before, '侧栏边界可拖动调整宽度');

    await page.getByRole('button', { name: '断开会话' }).click();
    await page.locator('.connection-select').click();
    const confirmation = page.waitForEvent('dialog').then(dialog => dialog.accept());
    await page.getByRole('dialog', { name: '连接管理' }).getByRole('button', { name: '删除连接 QA Connection' }).click({ timeout: 5000 });
    await confirmation;
    assert.equal(await page.getByRole('button', { name: '删除连接 QA Connection' }).count(), 0, '连接配置可以删除');

    await page.setViewportSize({ width: 390, height: 844 });
    await page.getByRole('button', { name: '打开更多工具' }).click();
    assert.equal(await page.locator('.mobile-tools-menu').getByRole('button', { name: '设置', exact: true }).count(), 1);
    await page.getByRole('button', { name: '打开更多工具' }).click();
    const mobile = await page.evaluate(() => ({ scrollWidth: document.documentElement.scrollWidth, clientWidth: document.documentElement.clientWidth }));
    const overflowing = await page.locator('body *').evaluateAll(elements => elements.map(element => {
      const box = element.getBoundingClientRect();
      return { tag: element.tagName, className: element.className, left: box.left, right: box.right, width: box.width };
    }).filter(item => item.right > window.innerWidth + 1 || item.left < -1).slice(-12));
    assert.ok(mobile.scrollWidth <= mobile.clientWidth + 1, `移动端不产生横向溢出: ${JSON.stringify({ mobile, overflowing })}`);
    assert.deepEqual(errors, []);
    assert.ok(requests.some(request => request.user === 'admin'), 'HTTP 请求携带当前用户');
    console.log('C2 browser regression passed: connection CRUD/test, password isolation, access/session/settings panels, resizer, mobile menu/overflow and identity header; no images');
  } finally {
    await page.getByRole('button', { name: '断开会话' }).click({ timeout: 1000 }).catch(() => {});
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });

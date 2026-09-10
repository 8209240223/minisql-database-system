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
  const context = await browser.newContext({ viewport: { width: 1100, height: 800 } });
  await context.addInitScript(() => localStorage.clear());
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'networkidle' });
    await page.locator('.table-node').first().waitFor({ timeout: 15000 });
    for (let i = 0; i < 20; i++) await page.locator('.new-tab').click();

    const scroll = page.locator('.tab-scroll');
    const metrics = await scroll.evaluate(node => ({ scrollWidth: node.scrollWidth, clientWidth: node.clientWidth }));
    console.log('tabs=', await page.locator('.query-tab').count(), 'metrics=', JSON.stringify(metrics));
    assert.ok(metrics.scrollWidth > metrics.clientWidth, '标签过多时应产生横向溢出');
    const activeBox = await page.locator('.query-tab.active').boundingBox();
    assert.ok(activeBox && activeBox.x < 1050, '当前标签应滚动到可视区域');
    const actionsBox = await page.locator('.tab-actions').boundingBox();
    assert.ok(actionsBox && actionsBox.x + actionsBox.width <= 1100, 'Run/Explain 工具栏不应被标签挤出');

    const before = await page.locator('.query-tab').count();
    await page.locator('.query-tab.active svg:last-child').click();
    assert.equal(await page.locator('.query-tab').count(), before - 1, '关闭按钮应可点击并关闭标签');
    assert.equal(errors.length, 0, `页面不应出现未捕获错误: ${errors.join(' | ')}`);
    console.log(`tab-scroll-smoke: 20 个标签可横向滚动（scrollWidth=${metrics.scrollWidth}），关闭按钮可点击，工具栏保持可见。`);
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error);
  process.exit(1);
});

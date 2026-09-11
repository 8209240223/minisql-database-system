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
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'networkidle' });
    await page.locator('.table-node').first().waitFor({ timeout: 15000 });
    await page.locator('.column-node').first().waitFor({ timeout: 10000 });
    const tableName = await page.locator('.table-node').first().locator('.tree-label').innerText();
    const columnName = (await page.locator('.column-node').first().innerText()).split('\n')[1];
    await page.locator('.column-node').first().click();

    await page.locator('.query-tab').filter({ hasText: `${tableName}.${columnName}.sql` }).waitFor({ timeout: 10000 });
    const editorText = await page.locator('.cm-content').innerText();
    assert.equal(editorText.includes(`SELECT ${columnName} FROM ${tableName};`), true, '列点击应生成列查询 SQL');
    await page.locator('table[aria-label="查询结果"] tbody tr').first().waitFor({ timeout: 15000 });
    const resultRows = await page.locator('table[aria-label="查询结果"] tbody tr').count();
    assert.ok(resultRows > 0, '列查询应返回真实数据行');

    await page.locator('.query-tab').first().click();
    await page.locator('.table-node').first().locator('.tree-label').click();
    await page.locator('.inspect-table-block').first().waitFor({ timeout: 5000 });
    const inspectTab = page.locator('.output-tabs button').filter({ hasText: 'Inspect' });
    assert.equal(await inspectTab.getAttribute('class'), 'selected', '点击表名应打开 Inspect 表详情');

    await page.locator('.table-node').first().locator('.tree-query').click();
    const afterInsert = await page.locator('.cm-content').innerText();
    assert.equal(afterInsert.includes(`SELECT * FROM ${tableName};`), true, '表行查询按钮应追加 SELECT *');

    assert.equal(await page.getByRole('button', { name: '打开权限与审计' }).count(), 1, '顶部权限入口应保持可见');
    assert.equal(await page.getByRole('button', { name: '打开设置' }).count(), 1, '顶部设置入口应保持可见');
    assert.equal(await page.locator('.history-toggle').count(), 1, '顶部历史入口应保持可见');
    assert.equal(errors.length, 0, `页面不应出现未捕获错误: ${errors.join(' | ')}`);
    console.log(`column-click-smoke: 点击 ${tableName}.${columnName} 自动执行并返回 ${resultRows} 行；Inspect 与 SELECT 按钮回归通过。`);
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error);
  process.exit(1);
});

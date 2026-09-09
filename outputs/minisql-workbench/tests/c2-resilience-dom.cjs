const path = require('node:path');
const { spawn } = require('node:child_process');
const { mkdtempSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { once } = require('node:events');
let playwright;
try {
  playwright = require('playwright');
} catch {
  playwright = require(path.join(process.env.USERPROFILE, '.codex/playwright-runtime/node_modules/playwright'));
}
const { chromium } = playwright;
const assert = require('node:assert/strict');

const backend = path.resolve(__dirname, '../../minisql-backend');
const bridge = path.join(backend, 'scripts/database-bridge.mjs');
const root = mkdtempSync(join(tmpdir(), 'minisql-c2-browser-'));
const uiOrigin = new URL(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173').origin;
const apiServer = spawn(process.execPath, [bridge], {
  cwd: backend,
  env: {
    ...process.env,
    PORT: '0',
    MINISQL_DB: join(root, 'database.pages'),
    MINISQL_BACKUP_DIR: join(root, 'backups'),
    MINISQL_ORIGINS: `http://127.0.0.1:4173,http://localhost:4173,${uiOrigin}`,
    MINISQL_SESSION_IDLE_MS: '30000',
    MINISQL_ENGINE_REQUEST_TIMEOUT_MS: '120000',
  },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});
const apiExited = once(apiServer, 'exit');
let stderr = '';
apiServer.stderr.on('data', chunk => { stderr += chunk; });

async function waitForApi() {
  return new Promise((resolve, reject) => {
    let output = '';
    const timer = setTimeout(() => reject(new Error(stderr || 'bridge readiness timeout')), 30000);
    apiServer.stdout.on('data', chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+\/api/);
      if (match) { clearTimeout(timer); resolve(match[0]); }
    });
    apiServer.once('error', reject);
    apiServer.once('exit', () => reject(new Error(stderr || 'bridge exited before readiness')));
  });
}

async function request(api, route, body, method = 'POST') {
  const response = await fetch(api + route, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-MiniSQL-User': 'admin' },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    signal: AbortSignal.timeout(180000),
  });
  return { status: response.status, data: await response.json() };
}

async function seed(api) {
  assert.equal((await request(api, '/execute', { sql: 'CREATE TABLE c2_big(id INT);' })).status, 200);
  for (let start = 0; start < 30000; start += 5000) {
    const values = Array.from({ length: 5000 }, (_, index) => `(${start + index})`).join(',');
    const result = await request(api, '/execute', { sql: `INSERT INTO c2_big VALUES ${values};` });
    assert.equal(result.status, 200, JSON.stringify(result.data));
  }
}

(async () => {
  const api = await waitForApi();
  await seed(api);
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  await context.addInitScript(() => localStorage.clear());
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(process.env.MINISQL_UI_URL ?? 'http://127.0.0.1:4173', { waitUntil: 'domcontentloaded' });
    const autoSessionDisconnect = page.getByRole('button', { name: '断开会话' });
    await page.waitForFunction(() => {
      const connect = document.querySelector('button[aria-label="连接会话"]');
      const disconnect = document.querySelector('button[aria-label="断开会话"]');
      return (connect && !connect.disabled) || (disconnect && !disconnect.disabled);
    }, undefined, { timeout: 30000 });
    if (!await autoSessionDisconnect.isDisabled()) await autoSessionDisconnect.click();
    await page.waitForFunction(() => {
      const connect = document.querySelector('button[aria-label="连接会话"]');
      return connect && !connect.disabled;
    }, undefined, { timeout: 30000 });
    await page.locator('.connection-select').click();
    const connectionDialog = page.getByRole('dialog', { name: '连接管理' });
    await connectionDialog.getByRole('button', { name: '新建', exact: true }).click();
    const inputs = connectionDialog.locator('.connection-form input');
    await inputs.nth(0).fill('C2 Resilience');
    await inputs.nth(1).fill(api);
    await inputs.nth(2).fill('admin');
    await inputs.nth(3).fill('');
    await connectionDialog.locator('.connection-form .settings-refresh').click();
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="断开会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 30000 });

    await page.locator('.cm-content').fill('SELECT id FROM c2_big WHERE id=0;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    await page.getByRole('grid', { name: '查询结果' }).waitFor({ timeout: 30000 });
    assert.equal(await page.getByRole('grid', { name: '查询结果' }).getByText('0', { exact: true }).count(), 1);

    await page.locator('.cm-content').fill('SELECT missing FROM c2_big;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    await page.locator('.error-state').waitFor({ timeout: 30000 });
    await page.locator('.error-state button').click();

    await page.locator('.cm-content').fill('SELECT id FROM c2_big ORDER BY id DESC;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    await page.getByRole('button', { name: '打开权限与审计' }).click();
    const accessDialog = page.getByRole('dialog', { name: '权限与审计' });
    await accessDialog.getByRole('button', { name: '会话', exact: true }).click();
    const activeSession = accessDialog.locator('.access-table tbody tr').filter({ hasText: '执行中' });
    await activeSession.waitFor({ timeout: 30000 });
    await activeSession.getByRole('button', { name: '取消', exact: true }).click();
    await accessDialog.getByText('已发送取消请求', { exact: true }).waitFor({ timeout: 30000 });
    await page.waitForTimeout(500);
    await accessDialog.getByRole('button', { name: '关闭', exact: true }).click();
    await page.locator('.error-state').waitFor({ timeout: 30000 });
    assert.match(await page.locator('.error-state').innerText(), /Query cancelled|取消/);
    await page.locator('.error-state button').click();

    await page.getByRole('button', { name: '断开会话' }).click();
    await page.getByRole('button', { name: '打开设置' }).click();
    const settings = page.getByRole('dialog', { name: '设置' });
    const createBackup = settings.getByRole('button', { name: '创建全量备份', exact: true });
    await createBackup.waitFor({ state: 'visible' });
    assert.equal(await createBackup.isDisabled(), false);
    await createBackup.click();
    await settings.getByText('全量备份已创建', { exact: true }).waitFor({ timeout: 30000 });
    const backupRow = settings.locator('.backup-row').filter({ hasText: '.pages' }).last();
    await backupRow.waitFor();
    const restorePrompt = page.waitForEvent('dialog').then(dialog => dialog.accept());
    await backupRow.getByRole('button', { name: '恢复', exact: true }).click();
    await restorePrompt;
    await settings.getByText('备份已恢复', { exact: true }).waitFor({ timeout: 30000 });
    await page.getByRole('button', { name: '关闭设置' }).click();

    await page.getByRole('button', { name: '连接会话' }).click();
    await page.waitForFunction(() => {
      const button = document.querySelector('button[aria-label="断开会话"]');
      return button && !button.disabled;
    }, undefined, { timeout: 30000 });
    await page.locator('.cm-content').fill('SELECT id FROM c2_big WHERE id=29999;');
    await page.getByRole('button', { name: 'Run', exact: true }).click();
    await page.getByRole('grid', { name: '查询结果' }).waitFor({ timeout: 30000 });
    assert.equal(await page.getByRole('grid', { name: '查询结果' }).getByText('29999', { exact: true }).count(), 1);
    assert.deepEqual(errors, []);
    console.log('C2 browser resilience passed: success, semantic failure, active-request cancellation, backup/restore and post-recovery query; no images');
  } finally {
    await page.getByRole('button', { name: '断开会话' }).click({ timeout: 1000 }).catch(() => {});
    await browser.close();
  }
})().catch(error => {
  console.error(stderr);
  console.error(error);
  process.exitCode = 1;
}).finally(async () => {
  apiServer.kill();
  await apiExited.catch(() => {});
});

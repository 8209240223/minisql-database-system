#!/usr/bin/env node

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

const HELP = `MiniSQL 权限感知 CLI

用法：
  node scripts/minisql-cli.mjs --execute "SELECT * FROM students;"
  node scripts/minisql-cli.mjs --compile "SELECT * FROM students;"
  node scripts/minisql-cli.mjs --file query.sql --user reader --password reader-secret
  Get-Content query.sql -Raw | node scripts/minisql-cli.mjs --user admin

选项：
  -e, --execute <sql>   执行 SQL
  -c, --compile <sql>   仅编译并输出 AST/计划
  -f, --file <path>     从 UTF-8 SQL 文件读取
  -u, --user <name>     登录用户，默认 admin
  -p, --password <pass> 登录密码；也可用 MINISQL_PASSWORD
      --url <url>       HTTP bridge 地址，默认 http://127.0.0.1:8081/api
      --session <id>    复用已有会话，不自动关闭
      --json            原样输出 JSON
  -h, --help            显示帮助

说明：该 CLI 只访问 HTTP bridge，因此权限、审计、会话绑定和取消语义与工作台完全一致。
直接运行 minisql_database.exe 属于内核诊断入口，不提供面向用户的身份认证。`;

export function parseCliArgs(argv) {
  const { values, positionals } = parseArgs({
    args: argv,
    allowPositionals: true,
    options: {
      execute: { type: 'string', short: 'e' },
      compile: { type: 'string', short: 'c' },
      file: { type: 'string', short: 'f' },
      user: { type: 'string', short: 'u' },
      password: { type: 'string', short: 'p' },
      url: { type: 'string' },
      session: { type: 'string' },
      json: { type: 'boolean' },
      help: { type: 'boolean', short: 'h' },
    },
  });
  if (values.execute !== undefined && values.compile !== undefined) throw new Error('--execute 与 --compile 不能同时使用');
  if (values.execute !== undefined && values.file !== undefined) throw new Error('--execute 与 --file 不能同时使用');
  if (values.compile !== undefined && values.file !== undefined) throw new Error('--compile 与 --file 不能同时使用');
  const inlineSql = values.execute ?? values.compile ?? (positionals.length ? positionals.join(' ') : undefined);
  return {
    ...values,
    user: values.user ?? process.env.MINISQL_USER ?? 'admin',
    password: values.password ?? process.env.MINISQL_PASSWORD ?? '',
    url: (values.url ?? process.env.MINISQL_API_URL ?? 'http://127.0.0.1:8081/api').replace(/\/$/, ''),
    mode: values.compile !== undefined ? 'compile' : 'execute',
    sql: values.file ? readFileSync(resolve(values.file), 'utf8') : inlineSql,
  };
}

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  return Buffer.concat(chunks).toString('utf8');
}

function headers(config, extra = {}) {
  return { ...extra, 'X-MiniSQL-User': config.user, ...(config.password ? { 'X-MiniSQL-Password': config.password } : {}) };
}

async function request(config, path, options = {}) {
  const response = await fetch(config.url + path, { ...options, headers: headers(config, options.headers), signal: AbortSignal.timeout(30000) });
  const data = await response.json().catch(() => { throw new Error(`HTTP ${response.status}: 响应不是 JSON`); });
  if (!response.ok || data.error) throw Object.assign(new Error(data.error?.message ?? `HTTP ${response.status}`), data.error ?? {}, { status: response.status, response: data });
  return data;
}

export async function runCli(config) {
  if (config.help) return { help: true };
  const sql = config.sql ?? await readStdin();
  if (!sql.trim()) throw new Error('没有 SQL 输入');
  let sessionId = config.session;
  let opened = false;
  try {
    if (!sessionId) {
      const session = await request(config, '/sessions', { method: 'POST' });
      sessionId = session.sessionId;
      opened = true;
    }
    if (config.mode === 'compile') return await request(config, `/sessions/${encodeURIComponent(sessionId)}/compile`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }),
    });
    return await request(config, `/sessions/${encodeURIComponent(sessionId)}/execute`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }),
    });
  } finally {
    if (opened && sessionId) await request(config, `/sessions/${encodeURIComponent(sessionId)}/close`, { method: 'POST' }).catch(() => {});
  }
}

function printHuman(data) {
  if (Array.isArray(data.columns) && data.columns.length) {
    console.log(data.columns.join('\t'));
    for (const row of data.rows ?? []) console.log(row.map(value => value === null ? 'NULL' : String(value)).join('\t'));
  }
  if (data.plan?.length) console.log(`计划节点: ${data.plan.length}`);
  if (data.affectedRows) console.log(`影响行数: ${data.affectedRows}`);
  if (data.transactionState) console.log(`事务状态: ${data.transactionState}`);
  if (!data.columns?.length && !data.plan?.length && !data.affectedRows) console.log('OK');
}

async function main() {
  const config = parseCliArgs(process.argv.slice(2));
  if (config.help) { console.log(HELP); return; }
  const data = await runCli(config);
  if (config.json) console.log(JSON.stringify(data, null, 2));
  else printHuman(data);
}

if (process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url) {
  main().catch(error => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  });
}

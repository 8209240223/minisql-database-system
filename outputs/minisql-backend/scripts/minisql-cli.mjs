#!/usr/bin/env node
// shebang：让这个脚本在类 Unix 系统上可以直接当可执行文件运行。

import { readFileSync } from 'node:fs';
// 从文件读 SQL（--file 用）。
import { resolve } from 'node:path';
// 把相对路径解析成绝对路径。
import { pathToFileURL } from 'node:url';
// 用于判断"这个模块是被直接运行还是被 import"，见文件末尾。
import { parseArgs } from 'node:util';
// Node 内置的参数解析器，避免为几个选项引入第三方依赖。

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
// 帮助文本结束。这里特意说明"CLI 只走 HTTP bridge"，
// 目的是让用户知道：权限校验、审计、会话绑定与取消语义都和工作台完全一致；
// 直接跑 minisql_database.exe 是内核诊断入口，不做面向用户的身份认证。

export function parseCliArgs(argv) {
// 解析命令行参数并补上默认值。
  const { values, positionals } = parseArgs({
  // values 是具名选项，positionals 是位置参数（允许直接写 SQL）。
    args: argv,
    // 待解析的参数数组（调用方已去掉 node 与脚本路径）。
    allowPositionals: true,
    // 允许不带选项名的位置参数。
    options: {
    // 选项定义。
      execute: { type: 'string', short: 'e' },
      // -e / --execute：要执行的 SQL。
      compile: { type: 'string', short: 'c' },
      // -c / --compile：只编译不执行的 SQL。
      file: { type: 'string', short: 'f' },
      // -f / --file：从文件读 SQL。
      user: { type: 'string', short: 'u' },
      // -u / --user：登录用户。
      password: { type: 'string', short: 'p' },
      // -p / --password：登录密码。
      url: { type: 'string' },
      // --url：bridge 地址。
      session: { type: 'string' },
      // --session：复用已有会话。
      json: { type: 'boolean' },
      // --json：输出原始 JSON。
      help: { type: 'boolean', short: 'h' },
      // -h / --help：显示帮助。
    },
    // 选项定义结束。
  });
  // 解析结束。
  if (values.execute !== undefined && values.compile !== undefined) throw new Error('--execute 与 --compile 不能同时使用');
  // 执行与编译互斥，避免歧义。
  if (values.execute !== undefined && values.file !== undefined) throw new Error('--execute 与 --file 不能同时使用');
  // 直接给 SQL 与从文件读也互斥。
  if (values.compile !== undefined && values.file !== undefined) throw new Error('--compile 与 --file 不能同时使用');
  // 编译模式同理。
  const inlineSql = values.execute ?? values.compile ?? (positionals.length ? positionals.join(' ') : undefined);
  // 内联 SQL 的来源优先级：--execute、--compile、最后把所有位置参数拼起来（方便直接贴 SQL）。
  return {
  // 返回归一化后的配置对象。
    ...values,
    // 先原样带上用户给的选项。
    user: values.user ?? process.env.MINISQL_USER ?? 'admin',
    // 用户名：命令行 > 环境变量 > 默认 admin。
    password: values.password ?? process.env.MINISQL_PASSWORD ?? '',
    // 密码：命令行 > 环境变量 > 空串（不用命令行传密码更安全，避免进 shell 历史）。
    url: (values.url ?? process.env.MINISQL_API_URL ?? 'http://127.0.0.1:8081/api').replace(/\/$/, ''),
    // bridge 地址：命令行 > 环境变量 > 默认值；末尾斜杠统一去掉，方便拼路径。
    mode: values.compile !== undefined ? 'compile' : 'execute',
    // 模式：给了 --compile 就是编译，否则执行。
    sql: values.file ? readFileSync(resolve(values.file), 'utf8') : inlineSql,
    // SQL 正文：给了文件就按 UTF-8 读文件，否则用内联 SQL。
  };
  // 配置对象组装完毕。
}

async function readStdin() {
// 把标准输入整体读成 UTF-8 文本（支持管道喂 SQL）。
  const chunks = [];
  // 分片缓冲。
  for await (const chunk of process.stdin) chunks.push(chunk);
  // 用异步迭代逐块读取，避免大输入一次性占满内存。
  return Buffer.concat(chunks).toString('utf8');
  // 拼起来再按 UTF-8 解码。
}

function headers(config, extra = {}) {
// 构造请求头：把身份信息放在自定义头里，而不是 URL 或正文里。
  return { ...extra, 'X-MiniSQL-User': config.user, ...(config.password ? { 'X-MiniSQL-Password': config.password } : {}) };
  // 先铺开调用方给的头（例如 Content-Type），再加用户名；
  // 密码为空时干脆不带这个头，避免发送空口令。
}

async function request(config, path, options = {}) {
// 统一的 HTTP 请求工具：带身份头、30 秒超时，并把错误整理成可读异常。
  const response = await fetch(config.url + path, { ...options, headers: headers(config, options.headers), signal: AbortSignal.timeout(30000) });
  // 拼地址、合并头、加超时；超时后 fetch 会以 AbortError 拒绝。
  const data = await response.json().catch(() => { throw new Error(`HTTP ${response.status}: 响应不是 JSON`); });
  // 解析 JSON；如果响应不是 JSON（例如网关错误页），给出明确的提示而不是原始解析错误。
  if (!response.ok || data.error) throw Object.assign(new Error(data.error?.message ?? `HTTP ${response.status}`), data.error ?? {}, { status: response.status, response: data });
  // 失败时把后端的错误对象整体挂到异常上，调用方既能读到 message，
  // 也能读到 error code、status 与完整响应，便于测试断言。
  return data;
  // 成功时返回解析后的数据。
}

export async function runCli(config) {
// 一次 CLI 调用的主流程。
  if (config.help) return { help: true };
  // 只要帮助就直接返回，不发起任何网络请求。
  const sql = config.sql ?? await readStdin();
  // SQL 来源：配置里有就用它，否则从标准输入读。
  if (!sql.trim()) throw new Error('没有 SQL 输入');
  // 空输入直接报错，避免发一条空语句给后端。
  let sessionId = config.session;
  // 会话号：用户指定了就复用。
  let opened = false;
  // 记录会话是不是本次打开的，只有本次打开的才在结束时关闭。
  try {
  // try/finally 保证会话一定被关闭。
    if (!sessionId) {
    // 没有现成会话就新开一个。
      const session = await request(config, '/sessions', { method: 'POST' });
      // 请求开会话。
      sessionId = session.sessionId;
      // 记下服务端返回的会话号。
      opened = true;
      // 标记是本次打开的。
    }
    // 会话准备结束。
    if (config.mode === 'compile') return await request(config, `/sessions/${encodeURIComponent(sessionId)}/compile`, {
    // 编译模式：调用编译接口。
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }),
      // SQL 放在 JSON 正文里；会话号做 URL 编码，避免特殊字符破坏路径。
    });
    // 编译请求结束。
    return await request(config, `/sessions/${encodeURIComponent(sessionId)}/execute`, {
    // 执行模式：调用执行接口。
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }),
      // 同样把 SQL 放进 JSON 正文。
    });
    // 执行请求结束。
  } finally {
  // 收尾。
    if (opened && sessionId) await request(config, `/sessions/${encodeURIComponent(sessionId)}/close`, { method: 'POST' }).catch(() => {});
    // 只关闭"本次自己开的"会话；复用别人的会话就保持开着。
    // 关闭失败也不掩盖原来的错误（catch 吞掉）。
  }
  // 收尾结束。
}

function printHuman(data) {
// 把人可读的结果打印出来（不加 --json 时走这里）。
  if (Array.isArray(data.columns) && data.columns.length) {
  // 有结果列说明是查询。
    console.log(data.columns.join('\t'));
    // 先打印表头，用制表符分隔便于直接看。
    for (const row of data.rows ?? []) console.log(row.map(value => value === null ? 'NULL' : String(value)).join('\t'));
    // 逐行打印；NULL 显式写成 NULL，其它值转成字符串。
  }
  // 查询结果打印结束。
  if (data.plan?.length) console.log(`计划节点: ${data.plan.length}`);
  // 有逻辑计划就报节点数（编译模式的主要输出）。
  if (data.affectedRows) console.log(`影响行数: ${data.affectedRows}`);
  // 有影响行数就报出来（写语句的主要输出）。
  if (data.transactionState) console.log(`事务状态: ${data.transactionState}`);
  // 有事务状态就报出来。
  if (!data.columns?.length && !data.plan?.length && !data.affectedRows) console.log('OK');
  // 三者都没有说明这条语句没有别的可见结果，用 OK 表示成功执行。
}

async function main() {
// 进程入口。
  const config = parseCliArgs(process.argv.slice(2));
  // 解析参数（跳过 node 可执行文件与脚本路径）。
  if (config.help) { console.log(HELP); return; }
  // 帮助模式：打印帮助文本后结束。
  const data = await runCli(config);
  // 跑主流程。
  if (config.json) console.log(JSON.stringify(data, null, 2));
  // --json：原样输出缩进后的 JSON，便于脚本化处理。
  else printHuman(data);
  // 否则输出人可读格式。
}

if (process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url) {
// 只有当"这个文件被当成主模块直接运行"时才执行 main；
// 被别的模块 import 时（例如测试里 import runCli）不会自动跑，方便单元测试。
  main().catch(error => {
  // 兜底捕获所有异常。
    console.error(error instanceof Error ? error.message : String(error));
    // 只打印可读的错误信息，不打印整个堆栈，避免吓到命令行用户。
    process.exitCode = 1;
    // 设置非零退出码让调用方（含 CI 与脚本）能感知失败。
  });
}
// 入口判断结束。

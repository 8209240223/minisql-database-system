import { spawn } from 'node:child_process';
// 用子进程方式启动 minisql_database.exe 的会话模式。

export async function openSession(executable, database, { timeoutMs = 30000, maxOutputBytes = 32 * 1024 * 1024, env = {} } = {}) {
// 打开一个会话：启动子进程、完成握手，并返回一个可以发请求的句柄。
// timeoutMs 是启动超时，maxOutputBytes 是单条响应允许的最大字节数。
  const child = spawn(executable, [database, 'session'], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  // 参数是"数据库文件 + session"，即进入会话模式；
  // 三条标准流都走管道由本模块接管；环境变量叠加调用方传入的覆盖项。
  let terminal, ready = false, pending, buffer = Buffer.alloc(0), sequence = 0, queued = 0, closing = false;
  // terminal：致命错误（一旦置位，后续请求全部失败）；
  // ready：是否已完成握手；pending：当前在等响应的那个请求；
  // buffer：还没拼成完整一行的字节；sequence：请求编号自增；queued：排队中的请求数。
  let queue = Promise.resolve(), resolveReady, rejectReady, resolveClosed;
  // queue 用来串行化请求（保证一条请求一条响应地配对）；后三个是 Promise 的回调。
  const started = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
  // started：握手完成（或失败）时兑现。
  const closed = new Promise(resolve => { resolveClosed = resolve; });
  // closed：子进程退出时兑现。
  const fail = error => {
  // 统一的致命失败处理。
    if (terminal) return;
    // 已经失败过就不再重复处理。
    terminal = error;
    // 记下错误，之后所有请求都会拿到它。
    clearTimeout(startupTimer);
    // 清掉启动超时定时器。
    rejectReady(error);
    // 如果还没握手成功，让 started 失败。
    if (pending) { clearTimeout(pending.timer); pending.reject(error); pending = undefined; }
    // 有在等的请求就让它失败，避免调用方永远挂着。
    child.kill();
    // 杀掉子进程，避免留下孤儿进程。
  };
  const startupTimer = setTimeout(() => fail(new Error('Session startup timed out')), timeoutMs);
  // 启动超时保护：超过限时还没握手就按失败处理。
  child.stderr.resume();
  // 把子进程的标准错误读掉，防止管道写满导致子进程卡死。
  child.stdin.on('error', fail);
  // 往子进程写数据出错（通常是它已经退出）也算致命失败。
  child.on('error', fail);
  // 子进程根本起不来（路径不对、没有执行权限）同样按失败处理。
  child.on('close', (code, signal) => {
  // 子进程退出时。
    clearTimeout(startupTimer);
    // 清掉启动定时器。
    if (!terminal) {
    // 如果不是主动失败导致的退出，那就是意外退出。
      terminal = new Error(`Session closed (${code ?? signal})`);
      // 把退出码或信号记进错误信息，便于排查。
      rejectReady(terminal);
      // 让等待握手的调用方失败。
      if (pending) { clearTimeout(pending.timer); pending.reject(terminal); pending = undefined; }
      // 让在等的请求也失败。
    }
    // 意外退出处理结束。
    resolveClosed({ code, signal });
    // 通知 closed 等待者，把退出码与信号一起交出去。
  });
  child.stdout.on('data', chunk => {
  // 处理子进程标准输出的每一段数据。
    if (terminal) return;
    // 已经失败就不再解析。
    try {
    // 解析过程可能失败，用 try 包住以便统一走 fail。
      if (buffer.length + chunk.length > maxOutputBytes) throw new Error('Session response exceeds output limit');
      // 累积的响应超过上限就报错，防止恶意/失控输出把内存吃光。
      buffer = Buffer.concat([buffer, chunk]);
      // 追加上本次收到的字节。
      let newline;
      // 换行位置。
      while ((newline = buffer.indexOf(10)) >= 0) {
      // 只要缓冲区里还有完整的行就逐行处理（协议是"一行一条 JSON"）。
        const message = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(buffer.subarray(0, newline)));
        // 按严格的 UTF-8 解码并解析 JSON；fatal:true 意味着遇到非法字节直接抛错，
        // 这样编码错误不会被悄悄替换成某个字符而掩盖问题。
        buffer = buffer.subarray(newline + 1);
        // 把这一行从缓冲区里去掉。
        if (!ready) {
        // 还没握手时收到的第一条消息必须是 ready。
          if (message.type !== 'ready' || message.protocolVersion !== 1) throw new Error(message.error?.message ?? 'Invalid session handshake');
          // 类型必须是 ready 且协议版本必须是 1，否则判定握手失败。
          ready = true; clearTimeout(startupTimer); resolveReady(); continue;
          // 标记握手完成、清掉启动定时器、让 started 兑现，然后继续处理后面可能一起到达的消息。
        }
        // 握手分支结束。
        if (!pending || message.id !== pending.id) throw new Error('Unexpected session response');
        // 响应必须能对应上"正在等的那个请求"，否则说明协议错乱。
        if (pending.stream) {
        // 流式请求：一条请求会收到多条消息。
          const current = pending;
          current.delivery = current.delivery.then(() => current.onMessage?.(message)).catch(error => {
            current.deliveryError ??= error;
          });
          if (message.type === 'complete' || message.type === 'error') {
          // 收到 complete 或 error 说明本次流结束。
            pending = undefined;
            clearTimeout(current.timer);
            current.delivery.then(() => {
              if (!current.deliveryError) { current.resolve(message); return; }
              current.reject(current.deliveryError);
              fail(current.deliveryError);
            });
          }
          // 结束判断结束。
          continue;
          // 流式消息处理完继续读下一行。
        }
        // 流式分支结束。
        if (typeof message.success !== 'boolean') throw new Error('Unexpected session response');
        // 普通响应的 success 字段必须是布尔值，作为结构校验。
        const current = pending; pending = undefined; clearTimeout(current.timer); current.resolve(message);
        // 兑现当前请求。
      }
      // 行处理循环结束。
    } catch (error) { fail(error); }
    // 解析或校验失败都按致命错误处理。
  });
  try { await started; } catch (error) { await closed; throw error; }
  // 等握手完成；如果失败就先等子进程真正退出（避免调用方过早清理/重启），再把错误抛出去。
  function request(operation, sql, context = {}) {
  // 发一条请求并等它的响应。
    if (terminal || closing) return Promise.reject(terminal ?? new Error('Session is closing'));
    // 已经出错或正在关闭时，直接拒绝新请求。
    if (queued >= 64) return Promise.reject(new Error('Session request queue full'));
    // 排队请求数超过 64 也拒绝，防止调用方无节制地堆积。
    const id = String(++sequence);
    // 分配自增的请求编号（字符串形式），响应靠它配对。
    const frame = JSON.stringify({
    // 组装这一帧 JSON；下面用展开运算只带上"确实给了"的字段。
      id, operation, ...(sql === undefined ? {} : { sql }),
      // 编号、操作名，以及可选的 SQL。
      ...(context.sessionId === undefined ? {} : { sessionId: context.sessionId }),
      // 会话号。
      ...(context.cancelFile === undefined ? {} : { cancelFile: context.cancelFile }),
      // 取消标记文件路径。
      ...(context.table === undefined ? {} : { table: context.table }),
      // 目标表名。
      ...(context.index === undefined ? {} : { index: context.index }),
      // 目标索引名。
      ...(context.target === undefined ? {} : { target: context.target }),
      // 快照目标路径。
      ...(context.plan === undefined ? {} : { plan: context.plan }),
      ...(context.user === undefined ? {} : { user: context.user }),
      // 用户名。
      ...(context.password === undefined ? {} : { password: context.password }),
      // 口令。
    });
    // 帧组装结束。
    if (Buffer.byteLength(frame) > 8 * 1024 * 1024) return Promise.reject(new Error('Session request exceeds 8 MiB'));
    // 单帧上限 8 MiB，与服务端限制一致，在发出去之前就拦住。
    if (operation === 'close') closing = true;
    // 关闭操作一旦发出就标记"正在关闭"，拒绝后续新请求。
    ++queued;
    // 排队计数加一。
    const result = queue.then(() => {
    // 串到队列尾部：保证同一时刻只有一个在等响应，响应才能与请求一一对应。
      if (terminal) throw terminal;
      // 轮到本条时如果已经出错，直接抛出去。
      return new Promise((resolve, reject) => {
      // 真正发请求。
        const timer = setTimeout(() => fail(new Error('Session request timed out; commit state may be unknown')), timeoutMs);
        // 请求超时保护；错误信息特意说明"提交状态可能未知"，
        // 提醒调用方不要想当然认为事务已经回滚。
        pending = { id, timer, resolve, reject };
        // 登记为"正在等的请求"。
        child.stdin.write(frame + '\n');
        // 写进子进程标准输入，并补一个换行作为一帧的结束。
      });
      // Promise 构造结束。
    });
    // 队列挂接结束。
    queue = result.catch(() => {}).finally(() => { --queued; });
    // 队列推进：吞掉错误（它已经交给调用方的 result），并让排队计数减一。
    return result;
    // 返回给调用方。
  }
  function requestStream(operation, sql, context = {}, onMessage) {
  // 流式请求：一条请求会陆续收到多条消息，交给 onMessage 回调处理。
    if (terminal || closing) return Promise.reject(terminal ?? new Error('Session is closing'));
    // 与普通请求相同的准入检查。
    if (queued >= 64) return Promise.reject(new Error('Session request queue full'));
    // 队列上限检查。
    const id = String(++sequence);
    // 分配请求编号。
    const frame = JSON.stringify({
    // 流式请求只带少量字段。
      id, operation, ...(sql === undefined ? {} : { sql }),
      // 编号、操作名与 SQL。
      ...(context.sessionId === undefined ? {} : { sessionId: context.sessionId }),
      // 会话号。
      ...(context.cancelFile === undefined ? {} : { cancelFile: context.cancelFile }),
      // 取消标记文件。
      ...(context.user === undefined ? {} : { user: context.user }),
      // 用户名。
      ...(context.password === undefined ? {} : { password: context.password }),
      // 口令。
    });
    // 帧组装结束。
    if (Buffer.byteLength(frame) > 8 * 1024 * 1024) return Promise.reject(new Error('Session request exceeds 8 MiB'));
    // 同样做 8 MiB 上限检查。
    ++queued;
    // 排队计数加一。
    const result = queue.then(() => {
    // 串到队列尾部。
      if (terminal) throw terminal;
      // 出错就直接抛。
      return new Promise((resolve, reject) => {
      // 发请求。
        const timer = setTimeout(() => fail(new Error('Session streaming request timed out; commit state may be unknown')), timeoutMs);
        // 超时保护；同样提示提交状态可能未知。
        pending = { id, timer, resolve, reject, stream: true, onMessage, delivery: Promise.resolve() };
        child.stdin.write(frame + '\n');
        // 写出这一帧。
      });
      // Promise 构造结束。
    });
    // 队列挂接结束。
    queue = result.catch(() => {}).finally(() => { --queued; });
    // 推进队列并复位计数。
    return result;
    // 返回给调用方。
  }
  return {
  // 返回会话句柄，调用方拿到的就是这几个成员。
    pid: child.pid,
    // 子进程编号，便于排查是哪个进程。
    request,
    // 普通请求方法。
    requestStream,
    // 流式请求方法。
    closed,
    // 子进程退出时兑现的 Promise。
    async close(context = {}) {
    // 优雅关闭：先发 close 请求，再确保进程退出。
      const timer = setTimeout(() => fail(new Error('Session close timed out')), timeoutMs);
      // 关闭超时保护。
      try { return await request('close', undefined, context); }
      // 发关闭请求并等响应。
      finally { if (!closing) child.kill(); await closed; clearTimeout(timer); }
      // 无论成功失败：若关闭请求没走成（closing 为假）就强杀进程；
      // 再等进程真正退出；最后清掉定时器。
    },
    async terminate() { fail(new Error('Session terminated; commit state may be unknown')); await closed; },
    // 强制终止：直接把会话标记为失败，并等进程退出后再返回。
  };
  // 句柄组装完毕。
}

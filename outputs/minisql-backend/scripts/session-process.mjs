import { spawn } from 'node:child_process';

export async function openSession(executable, database, { timeoutMs = 30000, maxOutputBytes = 32 * 1024 * 1024, env = {} } = {}) {
  const child = spawn(executable, [database, 'session'], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  let terminal, ready = false, pending, buffer = Buffer.alloc(0), sequence = 0, queued = 0, closing = false;
  let queue = Promise.resolve(), resolveReady, rejectReady, resolveClosed;
  const started = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
  const closed = new Promise(resolve => { resolveClosed = resolve; });
  const fail = error => {
    if (terminal) return;
    terminal = error;
    clearTimeout(startupTimer);
    rejectReady(error);
    if (pending) { clearTimeout(pending.timer); pending.reject(error); pending = undefined; }
    child.kill();
  };
  const startupTimer = setTimeout(() => fail(new Error('Session startup timed out')), timeoutMs);
  child.stderr.resume();
  child.stdin.on('error', fail);
  child.on('error', fail);
  child.on('close', (code, signal) => {
    clearTimeout(startupTimer);
    if (!terminal) {
      terminal = new Error(`Session closed (${code ?? signal})`);
      rejectReady(terminal);
      if (pending) { clearTimeout(pending.timer); pending.reject(terminal); pending = undefined; }
    }
    resolveClosed({ code, signal });
  });
  child.stdout.on('data', chunk => {
    if (terminal) return;
    try {
      if (buffer.length + chunk.length > maxOutputBytes) throw new Error('Session response exceeds output limit');
      buffer = Buffer.concat([buffer, chunk]);
      let newline;
      while ((newline = buffer.indexOf(10)) >= 0) {
        const message = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(buffer.subarray(0, newline)));
        buffer = buffer.subarray(newline + 1);
        if (!ready) {
          if (message.type !== 'ready' || message.protocolVersion !== 1) throw new Error(message.error?.message ?? 'Invalid session handshake');
          ready = true; clearTimeout(startupTimer); resolveReady(); continue;
        }
        if (!pending || message.id !== pending.id) throw new Error('Unexpected session response');
        const current = pending;
        if (current.onStream) {
          current.onStreamMessage(message);
          continue;
        }
        if (typeof message.success !== 'boolean') throw new Error('Unexpected session response');
        pending = undefined; clearTimeout(current.timer); current.resolve(message);
      }
    } catch (error) { fail(error); }
  });
  try { await started; } catch (error) { await closed; throw error; }
  function request(operation, sql, context = {}) {
    if (terminal || closing) return Promise.reject(terminal ?? new Error('Session is closing'));
    if (queued >= 64) return Promise.reject(new Error('Session request queue full'));
    const id = String(++sequence);
    const frame = JSON.stringify({
      id, operation, ...(sql === undefined ? {} : { sql }),
      ...(context.sessionId === undefined ? {} : { sessionId: context.sessionId }),
      ...(context.cancelFile === undefined ? {} : { cancelFile: context.cancelFile }),
      ...(context.table === undefined ? {} : { table: context.table }),
      ...(context.index === undefined ? {} : { index: context.index }),
    });
    if (Buffer.byteLength(frame) > 8 * 1024 * 1024) return Promise.reject(new Error('Session request exceeds 8 MiB'));
    if (operation === 'close') closing = true;
    ++queued;
    const result = queue.then(() => {
      if (terminal) throw terminal;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => fail(new Error('Session request timed out; commit state may be unknown')), timeoutMs);
        pending = { id, timer, resolve, reject };
        child.stdin.write(frame + '\n');
      });
    });
    queue = result.catch(() => {}).finally(() => { --queued; });
    return result;
  }
  // 流式执行：C++ 端逐步产出 meta/row/complete 帧，并在每个 row 后等待本端 ack 才继续。
  // onMessage 对除 complete/error 外的每帧异步处理并返回 promise；row 帧的 promise 完成后才写
  // ack，从而把 HTTP 层的 TCP 背压转译为对 C++ 端下一行生产的等待（生产者-消费者背压）。
  async function stream(sql, context = {}, onMessage) {
    if (terminal || closing) throw terminal ?? new Error('Session is closing');
    if (queued >= 64) throw new Error('Session request queue full');
    const id = String(++sequence);
    const frame = JSON.stringify({
      id, operation: 'stream', sql,
      ...(context.sessionId === undefined ? {} : { sessionId: context.sessionId }),
      ...(context.cancelFile === undefined ? {} : { cancelFile: context.cancelFile }),
    });
    if (Buffer.byteLength(frame) > 8 * 1024 * 1024) throw new Error('Session request exceeds 8 MiB');
    ++queued;
    return queue.then(() => new Promise((resolve, reject) => {
      if (terminal) { reject(terminal); return; }
      const timer = setTimeout(() => fail(new Error('Session stream timed out; commit state may be unknown')), timeoutMs);
      const prototype = {
        id, timer, resolve, reject, onStream: false, onStreamMessage: null, streamInbox: [], streamBusy: false,
      };
      // 变更 onStream 分支，失败时统一由 fail() 通过 reject 处理。
      pending = prototype;
      let finished = false;
      const finish = message => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        if (pending === prototype) pending = undefined;
        resolve(message);
      };
      const handle = () => {
        if (prototype.streamBusy) return;
        while (!finished && pending === prototype && prototype.streamInbox.length) {
          const message = prototype.streamInbox.shift();
          if (message.type === 'complete' || message.type === 'error') {
            Promise.resolve(onMessage ? onMessage(message) : undefined)
              .then(() => finish(message))
              .catch(error => finish({ type: 'error', id, success: false, error: { code: 5002, message: String(error?.message ?? error) } }));
            return;
          }
          prototype.streamBusy = true;
          Promise.resolve(onMessage ? onMessage(message) : undefined)
            .then(() => {
              if (pending === prototype && !terminal && message.type === 'row') child.stdin.write(JSON.stringify({ ack: id }) + '\n');
              prototype.streamBusy = false;
              handle();
            })
            .catch(error => {
              prototype.streamBusy = false;
              finish({ type: 'error', id, success: false, error: { code: 5002, message: String(error?.message ?? error) } });
            });
          return;
        }
      };
      prototype.onStream = true;
      prototype.onStreamMessage = message => {
        if (finished) return;
        prototype.streamInbox.push(message);
        handle();
      };
      child.stdin.write(frame + '\n');
    })).finally(() => { --queued; });
  }
  return {
    request,
    stream,
    closed,
    async close() {
      const timer = setTimeout(() => fail(new Error('Session close timed out')), timeoutMs);
      try { return await request('close'); }
      finally { if (!closing) child.kill(); await closed; clearTimeout(timer); }
    },
    async terminate() { fail(new Error('Session terminated; commit state may be unknown')); await closed; },
  };
}

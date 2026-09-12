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
        if (pending.stream) {
          const current = pending;
          current.delivery = current.delivery.then(() => current.onMessage?.(message)).catch(error => {
            current.deliveryError ??= error;
          });
          if (message.type === 'complete' || message.type === 'error') {
            pending = undefined;
            clearTimeout(current.timer);
            current.delivery.then(() => {
              if (!current.deliveryError) { current.resolve(message); return; }
              current.reject(current.deliveryError);
              fail(current.deliveryError);
            });
          }
          continue;
        }
        if (typeof message.success !== 'boolean') throw new Error('Unexpected session response');
        const current = pending; pending = undefined; clearTimeout(current.timer); current.resolve(message);
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
      ...(context.target === undefined ? {} : { target: context.target }),
      ...(context.plan === undefined ? {} : { plan: context.plan }),
      ...(context.user === undefined ? {} : { user: context.user }),
      ...(context.password === undefined ? {} : { password: context.password }),
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
  function requestStream(operation, sql, context = {}, onMessage) {
    if (terminal || closing) return Promise.reject(terminal ?? new Error('Session is closing'));
    if (queued >= 64) return Promise.reject(new Error('Session request queue full'));
    const id = String(++sequence);
    const frame = JSON.stringify({
      id, operation, ...(sql === undefined ? {} : { sql }),
      ...(context.sessionId === undefined ? {} : { sessionId: context.sessionId }),
      ...(context.cancelFile === undefined ? {} : { cancelFile: context.cancelFile }),
      ...(context.user === undefined ? {} : { user: context.user }),
      ...(context.password === undefined ? {} : { password: context.password }),
    });
    if (Buffer.byteLength(frame) > 8 * 1024 * 1024) return Promise.reject(new Error('Session request exceeds 8 MiB'));
    ++queued;
    const result = queue.then(() => {
      if (terminal) throw terminal;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => fail(new Error('Session streaming request timed out; commit state may be unknown')), timeoutMs);
        pending = { id, timer, resolve, reject, stream: true, onMessage, delivery: Promise.resolve() };
        child.stdin.write(frame + '\n');
      });
    });
    queue = result.catch(() => {}).finally(() => { --queued; });
    return result;
  }
  return {
    pid: child.pid,
    request,
    requestStream,
    closed,
    async close(context = {}) {
      const timer = setTimeout(() => fail(new Error('Session close timed out')), timeoutMs);
      try { return await request('close', undefined, context); }
      finally { if (!closing) child.kill(); await closed; clearTimeout(timer); }
    },
    async terminate() { fail(new Error('Session terminated; commit state may be unknown')); await closed; },
  };
}

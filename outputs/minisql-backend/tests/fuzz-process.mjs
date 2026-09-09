import { spawnSync } from 'node:child_process';

export function invoke(executable, args, input, { timeoutMs = 5000, outputBytes = 8388608 } = {}) {
  const child = spawnSync(executable, args, { input, encoding: 'utf8', windowsHide: true, timeout: timeoutMs, maxBuffer: outputBytes });
  if (child.error) return { category: child.error.code === 'ETIMEDOUT' ? 'timeout' : child.error.code === 'ENOBUFS' ? 'resourceLimit' : 'crash', detail: child.error.message };
  if (child.status !== 0 && child.status !== 1) return { category: 'crash', detail: { status: child.status, signal: child.signal, stderr: child.stderr } };
  try {
    const data = JSON.parse(child.stdout);
    if (typeof data?.success !== 'boolean' || data.success !== (child.status === 0))
      return { category: 'crash', detail: 'JSON status and process exit disagree' };
    return { data };
  } catch { return { category: 'crash', detail: 'Non-JSON database output' }; }
}

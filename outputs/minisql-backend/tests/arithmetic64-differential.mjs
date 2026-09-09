import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const low = -(1n << 63n), high = (1n << 63n) - 1n;
const boundary = [low, low + 1n, -3037000500n, -3037000499n, -2n, -1n, 0n, 1n, 2n, 3037000499n, 3037000500n, high - 1n, high];
const cases = [];
const operators = ['+', '-', '*', '/'];
for (const a of boundary) for (const b of boundary) for (const op of operators) cases.push({ a, b, op });
let state = 20260908n;
const random = () => {
  state = BigInt.asUintN(64, state * 6364136223846793005n + 1442695040888963407n);
  return BigInt.asIntN(64, state);
};
for (let i = 0; i < 5000; ++i) {
  const a = random(), b = random();
  for (const op of operators) cases.push({ a, b, op });
}
cases.push({ a: 1n, b: 2n, op: '%' });
const expected = ({ a, b, op }) => {
  if (op === '%') return 'ERR 9999 Unknown arithmetic operator';
  if (op === '/' && b === 0n) return 'ERR 5001 Division by zero';
  const value = op === '+' ? a + b : op === '-' ? a - b : op === '*' ? a * b : a / b;
  return value < low || value > high ? 'ERR 5001 BIGINT arithmetic overflow' : `OK ${value}`;
};
const child = spawnSync(fileURLToPath(new URL('../bin/arithmetic64_probe.exe', import.meta.url)), [], {
  input: cases.map(({ a, b, op }) => `${op} ${a} ${b}`).join('\n') + '\n', encoding: 'utf8', windowsHide: true,
  timeout: 15000, maxBuffer: 8 * 1024 * 1024,
});
assert.ifError(child.error);
assert.equal(child.status, 0, child.stderr);
const output = child.stdout.trim().split(/\r?\n/);
assert.equal(output.length, cases.length);
for (let i = 0; i < cases.length; ++i) assert.equal(output[i], expected(cases[i]), `Case ${i}: ${cases[i].a} ${cases[i].op} ${cases[i].b}`);
console.log(`${cases.length} checked INT64 operations match independent JavaScript BigInt results; error locations verified`);

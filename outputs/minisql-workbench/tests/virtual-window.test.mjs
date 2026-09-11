import assert from 'node:assert/strict';
import { calculateVirtualRange } from '../src/virtual-window.ts';

assert.deepEqual(calculateVirtualRange({ rowCount: 1000, rowHeight: 30, viewportHeight: 300, scrollTop: 0, overscan: 2 }), { start: 0, end: 14 });
assert.deepEqual(calculateVirtualRange({ rowCount: 1000, rowHeight: 30, viewportHeight: 300, scrollTop: 3000, overscan: 2 }), { start: 98, end: 112 });
assert.deepEqual(calculateVirtualRange({ rowCount: 5, rowHeight: 30, viewportHeight: 300, scrollTop: 0, overscan: 2 }), { start: 0, end: 5 });
console.log('3 virtual window checks passed');

import assert from 'node:assert/strict';
import test from 'node:test';
import { newQueryTab, serializeTabs } from '../src/query-tabs.ts';

test('tab drafts persist a connection snapshot without credentials or live session state', () => {
  const tab = newQueryTab({ id: 'q1', name: 'one.sql', sql: 'SELECT 1;', editVersion: 3,
    connection: { mode: 'api', kind: 'native', name: 'local', url: 'http://127.0.0.1/api', user: 'alice', password: 'secret', sessionId: 'nested-secret' },
    sessionId: 'live-secret', transactionState: 'ACTIVE', running: true });
  const saved = JSON.parse(serializeTabs([tab]));
  assert.equal(saved[0].editVersion, 3);
  assert.deepEqual(saved[0].connection, { mode: 'api', kind: 'native', name: 'local', url: 'http://127.0.0.1/api', user: 'alice' });
  assert.equal(JSON.stringify(saved).includes('secret'), false);
  assert.equal('sessionId' in saved[0], false);
  assert.equal('transactionState' in saved[0], false);
  assert.equal('running' in saved[0], false);
});

test('new tabs have independent execution state and edit versions', () => {
  const first = newQueryTab({ id: 'q1', name: 'one.sql', sql: 'SELECT 1;' });
  const second = newQueryTab({ id: 'q2', name: 'two.sql', sql: 'SELECT 2;' });
  const updated = [first, second].map(tab => tab.id === 'q1' ? { ...tab, running: true, transactionState: 'ACTIVE' } : tab);
  assert.deepEqual(updated.map(tab => [tab.id, tab.running, tab.transactionState, tab.editVersion]), [
    ['q1', true, 'ACTIVE', 0], ['q2', false, 'IDLE', 0],
  ]);
});

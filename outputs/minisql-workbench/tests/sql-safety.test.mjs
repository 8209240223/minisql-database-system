import assert from 'node:assert/strict';
import test from 'node:test';
import { mutationWarning } from '../src/sql-safety.ts';

for (const source of [
  'DELETE FROM t;', 'UPDATE t SET n=1;',
  'SELECT 1; DELETE FROM t;', 'BEGIN; UPDATE t SET n=1; COMMIT;',
  "UPDATE t SET name='WHERE';", 'DELETE FROM t /* WHERE id=1 */;',
  'UPDATE t SET n=(SELECT n FROM s WHERE id=1);',
  'DELETE FROM t; SELECT * FROM t WHERE id=1;',
  '-- comment\n/* before */ delete /* middle */ FROM t;',
]) {
  test(`confirm whole-table mutation: ${source}`, () => assert.match(mutationWarning(source), /没有 WHERE/));
}
for (const source of [
  '', 'SELECT 1;', "SELECT 'DELETE UPDATE WHERE';", '-- DELETE FROM t;\nSELECT 1;',
  '/* UPDATE t SET n=1; */ SELECT 1;',
  "SELECT 'it''s DELETE; UPDATE';", 'SELECT "UPDATE" FROM t;',
  'UPDATE t SET n=1 WHERE id=2;', 'DELETE FROM t WHERE id=1;',
  'UPDATE t SET n=1 WHERE id=2; DELETE FROM t WHERE id=3;',
  'UPDATE t SET n=1 WHERE TRUE;',
]) {
  test(`no whole-table warning: ${source}`, () => assert.equal(mutationWarning(source), undefined));
}
test('counts mutations separately across statement boundaries', () => {
  assert.match(mutationWarning('UPDATE t SET n=1; DELETE FROM t; DELETE FROM t WHERE id=1;'), /2 条/);
});
test('malformed mutation is conservatively confirmed', () => {
  assert.ok(mutationWarning('UPDATE t SET n=(1 WHERE id=1;'));
});

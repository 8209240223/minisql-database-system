import assert from 'node:assert/strict';
import { formatMiniSql } from '../src/sql-format.ts';

const formatted = await formatMiniSql("select id,name from students where status='a;b' and id=1; -- keep");
assert.match(formatted, /^SELECT/m);
assert.match(formatted, /'a;b'/);
assert.match(formatted, /-- keep/);
assert.match(formatted, /\nFROM/m);
assert.equal(await formatMiniSql(''), '');
console.log('2 SQL formatter checks passed');

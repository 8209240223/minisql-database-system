import assert from 'node:assert/strict';
import { firstKeyword, tableReferences } from '../scripts/sql-object-references.mjs';

let checks = 0;
const equal = (actual, expected, message) => {
  assert.deepEqual(actual, expected, message);
  checks += 1;
};

equal(firstKeyword(' /* leading */ SELECT * FROM public_records;'), 'SELECT', 'comments are ignored before the first keyword');
equal(tableReferences("SELECT * FROM public_records WHERE 'FROM secret_records' = 'x' -- FROM ignored\n;", 'SELECT'), ['public_records'], 'strings and comments do not become objects');
equal(tableReferences('SELECT * FROM (SELECT * FROM secret_records) AS hidden;', 'SELECT'), ['secret_records'], 'derived-table aliases are excluded while nested tables remain visible');
equal(tableReferences('SELECT * FROM public_records WHERE id IN (SELECT id FROM secret_records);', 'SELECT'), ['public_records', 'secret_records'], 'nested subquery tables are collected');
equal(tableReferences('WITH visible AS (SELECT * FROM secret_records) SELECT * FROM visible;', 'SELECT'), ['secret_records'], 'CTE aliases are not treated as persisted tables');
equal(tableReferences('CREATE TABLE child(id INT REFERENCES parent(id));', 'CREATE'), ['child', 'parent'], 'CREATE TABLE and foreign-key target are both checked');
equal(tableReferences('DROP INDEX child_id_idx ON child;', 'DROP'), ['child'], 'DROP INDEX ON table is checked against the owning table');
equal(tableReferences('EXPLAIN SELECT * FROM public_records;', 'EXPLAIN'), ['public_records'], 'EXPLAIN checks the target query object');

console.log(`${checks} SQL object reference contract checks passed`);

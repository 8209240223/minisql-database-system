import assert from 'node:assert/strict';
import test from 'node:test';
import { buildSqlSchema, sqlSchemaFor, collectCandidates, pickCandidate, applyCaseStyle } from '../src/sql-completion.ts';

const tables = [
  { name: 'students', rowCount: 3, columns: [
    { name: 'id', type: 'int', primaryKey: true, nullable: false },
    { name: 'name', type: 'varchar(32)', primaryKey: false, nullable: true },
  ] },
  { name: 'student_backup', rowCount: 1, columns: [
    { name: 'id', type: 'int', primaryKey: true, nullable: false },
  ] },
  { name: 'scores', rowCount: 5, columns: [
    { name: 'score', type: 'decimal(10,2)', primaryKey: false, nullable: true },
  ] },
];

test('schema 顶层列出全部表名', () => {
  const schema = buildSqlSchema(tables);
  assert.deepEqual(Object.keys(schema).sort(), ['scores', 'student_backup', 'students']);
});

test('表下挂该表的列，主键列有独立标记', () => {
  const schema = buildSqlSchema(tables);
  const students = schema.students;
  assert.equal(Array.isArray(students), true);
  assert.deepEqual(students.map(c => c.label), ['id', 'name']);
  assert.equal(students[0].type, 'keyword');
  assert.equal(students[0].detail, 'int');
  assert.equal(students[1].type, 'property');
  assert.equal(students[1].detail, 'varchar(32)');
});

test('目录为空时返回 undefined，让补全退回关键字模式', () => {
  assert.equal(sqlSchemaFor([]), undefined);
});

test('候选里表名和列名排在关键字之前', () => {
  const candidates = collectCandidates(tables);
  const words = candidates.map(c => c.word);
  assert.equal(words.indexOf('students') < words.indexOf('SELECT'), true, '表名应排在关键字前');
  assert.equal(words.indexOf('score') < words.indexOf('SUM'), true, '列名应排在关键字前');
});

test('候选去重且保留大小写', () => {
  const candidates = collectCandidates(tables);
  const words = candidates.map(c => c.word);
  assert.equal(words.filter(w => w.toLowerCase() === 'id').length, 1, 'id 只出现一次');
  assert.equal(words.includes('students'), true);
});

test('前缀匹配选中最短的候选：stud -> students 而非 student_backup', () => {
  const candidates = collectCandidates(tables);
  assert.equal(pickCandidate(candidates, 'stud'), 'students');
});

test('前缀匹配不区分大小写，且不返回与输入完全相同的词', () => {
  const candidates = collectCandidates(tables);
  assert.equal(pickCandidate(candidates, 'SEL'), 'SELECT');
  assert.equal(pickCandidate(candidates, 'select'), null, '已输入完整词时不再提示');
});

test('前缀匹配列名与关键字', () => {
  const candidates = collectCandidates(tables);
  assert.equal(pickCandidate(candidates, 'sc'), 'score');
  assert.equal(pickCandidate(candidates, 'FR'), 'FROM');
});

test('无匹配前缀返回 null，交回缩进行为', () => {
  const candidates = collectCandidates(tables);
  assert.equal(pickCandidate(candidates, 'zzz'), null);
});

test('小写输入得到小写补全：sel -> select', () => {
  assert.equal(applyCaseStyle('SELECT', 'sel'), 'select');
});

test('大写输入得到大写补全：SEL -> SELECT', () => {
  assert.equal(applyCaseStyle('select', 'SEL'), 'SELECT');
});

test('首字母大写输入得到首字母大写补全：Sel -> Select', () => {
  assert.equal(applyCaseStyle('SELECT', 'Sel'), 'Select');
});

test('表名同样跟随输入大小写：stud -> students / STUD -> STUDENTS', () => {
  assert.equal(applyCaseStyle('students', 'stud'), 'students');
  assert.equal(applyCaseStyle('students', 'STUD'), 'STUDENTS');
  assert.equal(applyCaseStyle('students', 'Stud'), 'Students');
});

console.log('sql completion checks passed');
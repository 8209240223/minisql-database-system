import assert from 'node:assert/strict';
import test from 'node:test';
import { resultCsv, resultTsv } from '../src/result-csv.ts';
const raw = { formulaProtection:false, distinguishNull:false };
const safe = { formulaProtection:true, distinguishNull:true };
test('duplicate headers and exact scalar values', () => {
  assert.equal(resultCsv(['x','x','b','d'],[['9223372036854775807','12.30',false,'2024-02-29']],raw), '\ufeffx,x,b,d\r\n9223372036854775807,12.30,false,2024-02-29\r\n');
});
test('quotes, separators, CRLF, Unicode and multiline values', () => {
  assert.equal(resultCsv(['a,b'],[['中文"x\r\ny']],raw), '\ufeff"a,b"\r\n"中文""x\r\ny"\r\n');
});
test('lone CR and LF are always quoted with CRLF records', () => {
  assert.equal(resultCsv(['a\nb'],[['x\ry']],raw),'\ufeff"a\nb"\r\n"x\ry"\r\n');
});
test('NULL, empty string, string NULL and literal escape marker are distinct', () => {
  assert.equal(resultCsv(['n','empty','text','marker','path'],[[null,'','NULL','\\N','C:\\file']],safe), '\ufeffn,empty,text,marker,path\r\n\\N,,NULL,\\\\N,C:\\\\file\r\n');
});
test('NULL encoding can be disabled without altering raw string values', () => {
  assert.equal(resultCsv(['n','text'],[[null,'\\N']],raw), '\ufeffn,text\r\n,\\N\r\n');
});
for (const value of ['=1+1','+1+1','-1+1','@SUM(A1)','  =1','\t1','\r1','\n1']) {
  test(`formula protection ${JSON.stringify(value)}`, () => {
    const encoded = resultCsv(['value'],[[value]],safe);
    assert.ok(encoded.includes("'"+value));
    assert.ok(!resultCsv(['value'],[[value]],raw).includes("'"+value));
  });
}
test('headers receive formula protection', () => assert.equal(resultCsv(['=1'],[],safe),'\ufeff\'=1\r\n'));
test('negative numeric cells remain numeric text', () => assert.equal(resultCsv(['v'],[[-12]],safe),'\ufeffv\r\n-12\r\n'));
test('empty result exports headers, no result exports nothing', () => {
  assert.equal(resultCsv(['a','b'],[],safe),'\ufeffa,b\r\n');
  assert.equal(resultCsv([],[],safe),'');
});
test('malformed rows and unsafe numbers fail explicitly', () => {
  assert.throws(()=>resultCsv(['a'],[[1,2]],safe),/列数/);
  for (const value of [NaN,Infinity,9007199254740992]) assert.throws(()=>resultCsv(['a'],[[value]],safe),/数值|整数/);
});
test('export does not mutate query data', () => {
  const columns=Object.freeze(['=formula','n']);
  const rows=Object.freeze([Object.freeze(['=1',null])]);
  resultCsv(columns,rows,safe);
  assert.deepEqual(rows,[['=1',null]]);assert.deepEqual(columns,['=formula','n']);
});
test('TSV selection excludes headers, BOM and final record delimiter', () => {
  assert.equal(resultTsv([['9223372036854775807','12.30'],['a','b']],safe),'9223372036854775807\t12.30\r\na\tb');
});
test('TSV quotes embedded separators and preserves null encoding', () => {
  assert.equal(resultTsv([['a\tb','x\ny',null,'\\N']],safe),'"a\tb"\t"x\ny"\t\\N\t\\\\N');
});
test('TSV formula toggle and malformed selection', () => {
  assert.equal(resultTsv([['=1']],safe),"'=1");
  assert.equal(resultTsv([['=1']],raw),'=1');
  assert.equal(resultTsv([],safe),'');
  assert.throws(()=>resultTsv([[1],[2,3]],safe),/列数/);
});

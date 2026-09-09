import assert from 'node:assert/strict';
import { mapDiagnostic } from '../src/diagnostic-location.ts';
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
const sameLine = 'abc SELECT missing;';
equal(mapDiagnostic(sameLine, 4, sameLine.length, 1, 8), {line:1,column:12,offset:11,length:1,source:sameLine});
const unicode = '\u{1f600}\u4e2d\tX';
equal(mapDiagnostic(unicode, 0, unicode.length, 1, 4)?.offset, 4);
equal(mapDiagnostic(unicode, 0, unicode.length, 1, 1)?.length, 2);
equal(mapDiagnostic(unicode, 0, unicode.length, 1, 5)?.length, 0);
for (const newline of ['\n','\r','\r\n']) {
  const source = `--\u{1f600}${newline}  SELECT missing;`;
  const from = source.indexOf('SELECT');
  const mapped = mapDiagnostic(source, from, source.length, 1, 8);
  equal(mapped?.line, 2);equal(mapped?.column, 10);
  equal(mapped?.offset, source.replace(/\r\n|\r/g,'\n').indexOf('missing'));
  const full = mapDiagnostic(source, 0, source.length, 2, 10);
  equal(full, mapped);
}
const multiline='prefix SELECT 1;\nSELECT wrong; suffix';
const mapped=mapDiagnostic(multiline,7,multiline.indexOf(' suffix'),2,8);
equal(mapped?.offset,multiline.indexOf('wrong'));
equal(mapped?.line,2);equal(mapped?.column,8);
for (const [from,to,line,column] of [[0,2,0,1],[0,2,1,0],[-1,2,1,1],[0,99,1,1],[2,1,1,1],[0,2,2,1],[0,2,1,4],[0,2,1,1.5],[0,2,NaN,1]])
  equal(mapDiagnostic('ab',from,to,line,column),undefined);
equal(mapDiagnostic('',0,0,1,1),{line:1,column:1,offset:0,length:0,source:''});
console.log(`${checks} diagnostic mapping checks passed: selection offsets, Unicode, tabs, line endings, EOF and invalid positions`);

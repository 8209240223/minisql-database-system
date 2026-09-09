import { mkdtempSync, writeFileSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';
import initSqlJs from '../../minisql-workbench/node_modules/sql.js/dist/sql-wasm.js';
import { generate, minimize, render, fixture } from './fuzz-model.mjs';
import { invoke } from './fuzz-process.mjs';

const seed = Number(process.env.FUZZ_SEED ?? 20260908);
const count = Number(process.env.FUZZ_CASES ?? 100);
if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff || !Number.isInteger(count) || count < 1 || count > 1000)
  throw new Error('FUZZ_SEED must be UINT32 and FUZZ_CASES must be 1..1000');
const directory = mkdtempSync(fileURLToPath(new URL('./artifacts/fuzz-', import.meta.url)));
const database = join(directory, 'database.pages');
const executable = fileURLToPath(new URL('../bin/minisql_database.exe', import.meta.url));
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const SQL = await initSqlJs({ locateFile: name => fileURLToPath(new URL(`../../minisql-workbench/node_modules/sql.js/dist/${name}`, import.meta.url)) });
const reference = new SQL.Database();
const stats = { passed: 0, wrongResult: 0, wrongAccept: 0, wrongReject: 0, errorLocation: 0, crash: 0, timeout: 0, resourceLimit: 0, harnessError: 0 };
const cases = generate(seed, count);
const report = { seed, count, timeoutMs: 5000, outputBytes: 8388608, executableSha256: digest(readFileSync(executable)),
  corpusSha256: digest(cases.map(render).join('\n')), generatorSha256: digest(readFileSync(new URL('./fuzz-model.mjs', import.meta.url))),
  referenceVersion: reference.exec('SELECT sqlite_version();')[0].values[0][0],
  coverage: { predicateCounts: [...new Set(cases.map(model => model.predicates.length))].sort(),
    distinctModes: [...new Set(cases.map(model => model.distinct))], sortDirections: [...new Set(cases.map(model => model.descending))],
    expressionVariants: new Set(cases.map(model => model.expression)).size }, fixture, stats, failures: [] };
writeFileSync(join(directory, 'fixture.sql'), fixture);
writeFileSync(join(directory, 'corpus.sql'), cases.map(render).join('\n'));
function run(sql) {
  return invoke(executable, [database, 'execute'], sql, report);
}
function compare(model) {
  const sql = render(model);
  let expected;
  try { expected = reference.exec(sql)[0]; }
  catch (error) { return { category: 'harnessError', detail: error.message }; }
  const actual = run(sql);
  if (actual.category) return actual;
  if (!actual.data.success) return { category: 'wrongReject', detail: actual.data };
  const last = actual.data.results.at(-1);
  const wanted = { columns: ['id', 'value', 'name'], rows: expected?.values ?? [] };
  const obtained = { columns: last.columns, rows: last.rows };
  return isDeepStrictEqual(wanted, obtained) ? { category: 'passed' } : { category: 'wrongResult', detail: { wanted, obtained } };
}
function saveFailure(index, model, outcome) {
  const failure = { index, sql: render(model), ...outcome };
  if (outcome.category === 'wrongResult' || outcome.category === 'wrongReject') {
    const reduced = minimize(model, candidate => compare(candidate).category === outcome.category);
    const confirmed = compare(reduced.model);
    failure.minimization = { attempts: reduced.attempts, confirmed: confirmed.category === outcome.category, sql: render(reduced.model), outcome: confirmed };
  }
  report.failures.push(failure);
  writeFileSync(join(directory, `failure-${index}.json`), JSON.stringify({ seed, fixture, ...failure }, null, 2));
}
try {
  reference.run(fixture);
  const setup = run(fixture);
  if (!setup.data?.success) throw new Error(`Fixture failed: ${JSON.stringify(setup)}`);
  for (let i = 0; i < cases.length; ++i) {
    const outcome = compare(cases[i]);
    ++stats[outcome.category];
    if (outcome.category !== 'passed') { saveFailure(i, cases[i], outcome); break; }
    const valid = render(cases[i]);
    const mutations = [
      { sql: `-- mutation\n@${valid}`, type: 'LexicalError', line: 2, column: 1 },
      { sql: valid.slice(0, -1), type: 'SyntaxError' },
      { sql: 'SELECT missing_fuzz FROM t;', type: 'SemanticError' },
      { sql: `/* valid mutation */ ${valid.replace(/^SELECT/, 'select')}`, valid: true },
    ];
    for (const mutation of mutations) {
      const actual = run(mutation.sql);
      let category = actual.category;
      if (!category) {
        if (mutation.valid) category = actual.data.success ? 'passed' : 'wrongReject';
        else if (actual.data.success) category = 'wrongAccept';
        else if (actual.data.error?.type !== mutation.type) category = 'wrongReject';
        else if (!Number.isInteger(actual.data.error.line) || actual.data.error.line < 1 || !Number.isInteger(actual.data.error.column) || actual.data.error.column < 1 ||
          (mutation.line && (actual.data.error.line !== mutation.line || actual.data.error.column !== mutation.column))) category = 'errorLocation';
        else category = 'passed';
      }
      ++stats[category];
      if (category !== 'passed') report.failures.push({ index: i, mutation, category, actual });
    }
    if (report.failures.length) break;
  }
} catch (error) {
  ++stats.harnessError;
  report.failures.push({ category: 'harnessError', detail: error.message });
} finally {
  reference.close();
  writeFileSync(join(directory, 'report.json'), JSON.stringify(report, null, 2));
}
console.log(JSON.stringify({ directory, seed, count, corpusSha256: report.corpusSha256, stats }, null, 2));
if (report.failures.length) process.exitCode = 1;

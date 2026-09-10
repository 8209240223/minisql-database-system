import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const db = join(mkdtempSync(join(tmpdir(), 'minisql-analyze-')), 'db.pages');
function run(sql, mode = 'execute') {
  const child = spawnSync('./build/windows/Release/minisql_database.exe', [db, mode], { input: sql, encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.ifError(child.error);
  return JSON.parse(child.stdout);
}
let checks = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); ++checks; }
function ok(condition, message) { assert.ok(condition, message); ++checks; }

equal(run("CREATE TABLE t(id INT,name VARCHAR,v FLOAT); INSERT INTO t VALUES(1,'a',1.5),(2,'a',NULL),(3,NULL,2.5);").success, true);

const fresh = run('', 'statistics');
equal(fresh.source, 'on-demand-scan');
equal(fresh.lastAnalyzeAtMs, 0);
equal(fresh.tables[0].rowCount, 3);

const analyzed = run('ANALYZE t;');
equal(analyzed.success, true);
equal(analyzed.results.length, 1);
const statement = analyzed.results[0];
equal(statement.kind, 'Analyze');
equal(statement.table, 't');
equal(statement.source, 'analyze');
equal(statement.statsVersion, 'stats-v1-histogram');
ok(statement.analyzedAtMs > 0, 'ANALYZE records a refresh timestamp');
equal(statement.rows.length, 1);
equal(statement.rows[0][0], 't');
equal(statement.rows[0][1], 3);
equal(statement.rows[0][2], 3);
equal(statement.rows[0][4], 'stats-v1-histogram');

const afterAnalyze = run('', 'statistics');
equal(afterAnalyze.source, 'analyze');
equal(afterAnalyze.lastAnalyzeAtMs, statement.analyzedAtMs);
equal(afterAnalyze.generatedAtMs, statement.analyzedAtMs);
equal(afterAnalyze.version, 'stats-v1-histogram');
equal(afterAnalyze.tables[0].rowCount, 3);
equal(afterAnalyze.tables[0].columns.length, 3);

const tableKeyword = run('ANALYZE TABLE t;');
equal(tableKeyword.success, true);
equal(tableKeyword.results[0].kind, 'Analyze');

const lowerKeyword = run('analyze t;');
equal(lowerKeyword.success, true);
equal(lowerKeyword.results[0].table, 't');

const invalidated = run("INSERT INTO t VALUES(4,'b',3.5);");
equal(invalidated.success, true);
const afterWrite = run('', 'statistics');
equal(afterWrite.source, 'on-demand-scan');
equal(afterWrite.lastAnalyzeAtMs, 0);
equal(afterWrite.tables[0].rowCount, 4);

const unknown = run('ANALYZE missing;');
equal(unknown.success, false);
const malformed = run('ANALYZE;');
equal(malformed.success, false);
const trailing = run('ANALYZE t extra;');
equal(trailing.success, false);

const explain = run('EXPLAIN SELECT * FROM t;');
equal(explain.success, true);
equal(explain.results[0].kind, 'Explain');

console.log(`${checks} ANALYZE/statistics checks passed`);

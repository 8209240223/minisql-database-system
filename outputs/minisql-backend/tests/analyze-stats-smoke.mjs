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
equal(statement.statsVersion, 'stats-v1');
ok(statement.analyzedAtMs > 0, 'ANALYZE records a refresh timestamp');
equal(statement.rows.length, 1);
equal(statement.rows[0][0], 't');
equal(statement.rows[0][1], 3);
equal(statement.rows[0][2], 3);
equal(statement.rows[0][4], 'stats-v1');

const afterAnalyze = run('', 'statistics');
equal(afterAnalyze.source, 'analyze');
equal(afterAnalyze.lastAnalyzeAtMs, statement.analyzedAtMs);
equal(afterAnalyze.generatedAtMs, statement.analyzedAtMs);
equal(afterAnalyze.statisticsVersion, 1);
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

// 失败写（执行期唯一键冲突）不得误删 ANALYZE 快照：语句在分派后才失败、数据未变，
// 快照仍然有效。任务书 §6.21-c 要求"写语句**成功后**删除"，故删除点须在成功回收处。
equal(run('CREATE TABLE u(id INT PRIMARY KEY,v INT); INSERT INTO u VALUES(1,1);').success, true);
equal(run('ANALYZE u;').success, true);
equal(run('', 'statistics').source, 'analyze');
const rejectedWrite = run('INSERT INTO u VALUES(1,2);');
equal(rejectedWrite.success, false);
equal(rejectedWrite.error.code, 5001);
equal(run('', 'statistics').source, 'analyze', 'failed write must not invalidate the snapshot');
equal(run('INSERT INTO u VALUES(2,2);').success, true);
equal(run('', 'statistics').source, 'on-demand-scan', 'successful write still invalidates the snapshot');

console.log(`${checks} ANALYZE/statistics checks passed`);

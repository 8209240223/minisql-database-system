import initSqlJs, { type Database } from 'sql.js';
import wasmUrl from 'sql.js/dist/sql-wasm.wasm?url';
import { Parser } from 'node-sql-parser/build/sqlite';
import type { Cell, QueryResult, Table } from './types';

let db: Database;
const scope = self as unknown as { postMessage: (v: unknown) => void; onmessage: ((e: MessageEvent) => void) | null };

function catalog(): Table[] {
  const names = db.exec("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
  return (names[0]?.values ?? []).map(([name]) => {
    const safe = `"${String(name).replaceAll('"', '""')}"`;
    const columns = db.exec(`PRAGMA table_info(${safe})`)[0]?.values ?? [];
    return {
      name: String(name), rowCount: Number(db.exec(`SELECT COUNT(*) FROM ${safe}`)[0].values[0][0]),
      columns: columns.map(c => ({ name: String(c[1]), type: String(c[2]), primaryKey: Boolean(c[5]), nullable: !c[3] && !c[5] })),
    };
  });
}

function seed() {
  db.run(`CREATE TABLE students (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, department VARCHAR, age INT, email VARCHAR, status VARCHAR);
    CREATE TABLE courses (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, credits INT, teacher VARCHAR);
    CREATE TABLE enrollments (id INTEGER PRIMARY KEY, student_id INT, course_id INT, score INT);`);
  const names = ['陈思远', '林沐辰', '王可欣', '李明轩', '张若曦', '刘宇航', '赵子涵', '周语桐', '吴昊然', '徐嘉宁', '孙雨泽', '郑书瑶'];
  const departments = ['计算机科学与技术', '软件工程', '数据科学与大数据技术'];
  const insert = db.prepare('INSERT INTO students VALUES (?,?,?,?,?,?)');
  for (let i = 0; i < 48; i++) insert.run([1001 + i, names[i % names.length] + (i >= 12 ? ` ${Math.floor(i / 12) + 1}` : ''), departments[i % 3], 18 + i % 6, `student${1001 + i}@csu.edu.cn`, i % 9 === 0 ? '休学' : '在读']);
  insert.free();
  db.run("INSERT INTO courses VALUES (1,'编译原理',4,'姚老师'),(2,'数据库系统',4,'邓老师'),(3,'操作系统',3,'桂老师'),(4,'软件工程',3,'陈老师')");
  const enroll = db.prepare('INSERT INTO enrollments VALUES (?,?,?,?)');
  for (let i = 0; i < 96; i++) enroll.run([i + 1, 1001 + i % 48, 1 + i % 4, 65 + i % 36]);
  enroll.free();
}

scope.onmessage = async ({ data }) => {
  const { id, action, sql, snapshot } = data;
  try {
    if (action === 'init') {
      const SQL = await initSqlJs({ locateFile: () => wasmUrl });
      db = snapshot?.length ? new SQL.Database(new Uint8Array(snapshot)) : new SQL.Database();
      if (!snapshot?.length) seed();
      scope.postMessage({ id, data: { tables: catalog(), snapshot: db.export() } });
      return;
    }
    if (action === 'catalog') { scope.postMessage({ id, data: { tables: catalog() } }); return; }
    const start = performance.now();
    let ast: unknown, astError: string | undefined;
    try { ast = new Parser().astify(sql); } catch { astError = '此 SQLite 语法未被演示 AST 解析器支持。'; }
    const result: QueryResult = { columns: [], rows: [], affectedRows: 0, durationMs: 0, ast, astError, plan: [], statements: 0 };
    // A savepoint makes demo batches atomic and makes cancellation restore a committed snapshot.
    db.run('SAVEPOINT studio_batch');
    try {
      for (const statement of db.iterateStatements(sql)) {
        const statementSql = statement.getSQL();
        if (/^\s*(BEGIN|COMMIT|ROLLBACK|SAVEPOINT|RELEASE|END|ATTACH|DETACH|VACUUM)\b/i.test(statementSql)) throw new Error('本地演示暂不支持事务控制、ATTACH 或 VACUUM；可连接 MiniSQL API 执行。');
        if (/^\s*(SELECT|WITH)\b/i.test(statementSql)) {
          try {
            result.plan = (db.exec(`EXPLAIN QUERY PLAN ${statementSql}`)[0]?.values ?? []).map(p => ({ id: Number(p[0]), parent: Number(p[1]), detail: String(p[3]) }));
          } catch { result.plan = []; }
        }
        if (action === 'compile') {
          result.statements++;
          continue;
        }
        result.columns = statement.getColumnNames();
        result.rows = [];
        while (statement.step()) {
          if (result.rows.length >= 10000) { result.warning = '结果已截取前 10,000 行。'; break; }
          result.rows.push(statement.get().map(v => v instanceof Uint8Array ? `[BLOB ${v.length}]` : v) as Cell[]);
        }
        if (!result.columns.length && /^\s*(INSERT|UPDATE|DELETE|REPLACE)\b/i.test(statementSql)) result.affectedRows += db.getRowsModified();
        result.statements++;
      }
      db.run('RELEASE studio_batch');
    } catch (error) {
      db.run('ROLLBACK TO studio_batch');
      db.run('RELEASE studio_batch');
      throw error;
    }
    result.durationMs = performance.now() - start;
    result.tables = catalog();
    scope.postMessage({ id, data: result, snapshot: action === 'execute' ? db.export() : undefined });
  } catch (error) {
    scope.postMessage({ id, error: { message: error instanceof Error ? error.message : String(error), stage: 'SQLite' } });
  }
};

const api = (process.env.MINISQL_API ?? 'http://127.0.0.1:8081/api').replace(/\/$/, '');
const headers = { 'Content-Type': 'application/json', 'X-MiniSQL-User': 'admin' };

async function request(path, options = {}) {
  const response = await fetch(api + path, { headers, ...options });
  const data = await response.json();
  if (!response.ok || data.success === false || data.error) {
    throw new Error(data.error?.message ?? `HTTP ${response.status}`);
  }
  return data;
}

const quote = value => `'${String(value).replaceAll("'", "''")}'`;
const departments = ['计算机科学与技术', '软件工程', '数据科学与大数据技术'];
const names = ['陈思远', '林沐辰', '王可欣', '李明轩', '张若曦', '刘宇航', '赵子涵', '周语桐', '吴昊然', '徐嘉宁', '孙雨泽', '郑书瑶'];

const students = Array.from({ length: 48 }, (_, i) => {
  const name = names[i % names.length] + (i >= 12 ? ` ${Math.floor(i / 12) + 1}` : '');
  const id = 1001 + i;
  return `(${id},${quote(name)},${quote(departments[i % departments.length])},${18 + i % 6},${quote(`student${id}@csu.edu.cn`)},${quote(i % 9 === 0 ? '休学' : '在读')})`;
}).join(',');

const courses = `(1,'编译原理',4,'姚老师'),(2,'数据库系统',4,'邓老师'),(3,'操作系统',3,'桂老师'),(4,'软件工程',3,'陈老师')`;
const enrollments = Array.from({ length: 96 }, (_, i) =>
  `(${i + 1},${1001 + i % 48},${1 + i % 4},${65 + i % 36})`
).join(',');

const sql = `BEGIN;
CREATE TABLE students (id INT PRIMARY KEY, name VARCHAR NOT NULL, department VARCHAR, age INT, email VARCHAR, status VARCHAR);
CREATE TABLE courses (id INT PRIMARY KEY, name VARCHAR NOT NULL, credits INT, teacher VARCHAR);
CREATE TABLE enrollments (id INT PRIMARY KEY, student_id INT, course_id INT, score INT);
INSERT INTO students VALUES ${students};
INSERT INTO courses VALUES ${courses};
INSERT INTO enrollments VALUES ${enrollments};
COMMIT;`;

let sessionId;
try {
  const opened = await request('/sessions', { method: 'POST' });
  sessionId = opened.sessionId;
  const catalog = await request(`/sessions/${sessionId}/catalog`);
  if (catalog.tables.length) {
    console.log(`Catalog already contains ${catalog.tables.length} tables; seed skipped.`);
  } else {
    const result = await request(`/sessions/${sessionId}/execute`, {
      method: 'POST', body: JSON.stringify({ sql }),
    });
    if (result.success === false) throw new Error(result.error?.message ?? 'Seed failed');
    const refreshed = await request(`/sessions/${sessionId}/catalog`);
    console.log(`Seeded ${refreshed.tables.length} real C++ tables.`);
  }
} finally {
  if (sessionId) await request(`/sessions/${sessionId}/close`, { method: 'POST' }).catch(() => {});
}

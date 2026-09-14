const api = (process.env.MINISQL_API ?? 'http://127.0.0.1:8081/api').replace(/\/$/, '');
// 后端 API 地址：可用环境变量覆盖，默认本地 8081 端口；末尾斜杠统一去掉，方便后面拼路径。
const headers = { 'Content-Type': 'application/json', 'X-MiniSQL-User': 'admin' };
// 所有请求共用的头：声明 JSON 内容类型，并以 admin 身份调用（权限目录默认放开）。

async function request(path, options = {}) {
// 统一的请求工具：负责拼地址、解析响应、并在失败时抛出可读错误。
  const response = await fetch(api + path, { headers, ...options });
  // 发请求；调用方传入的 options 可以覆盖默认头（例如带 body 的 POST）。
  const data = await response.json();
  // 后端一律返回 JSON。
  if (!response.ok || data.success === false || data.error) {
  // 三种失败情形：HTTP 状态不对、业务层标了 success:false、或者带 error 字段。
    throw new Error(data.error?.message ?? `HTTP ${response.status}`);
    // 优先用后端给的中文错误信息，没有就退化成状态码。
  }
  // 失败判断结束。
  return data;
  // 成功时返回解析后的数据。
}

const quote = value => `'${String(value).replaceAll("'", "''")}'`;
// 把值包成 SQL 字符串字面量，并把内部的单引号翻倍（SQL 标准的转义方式）。
const departments = ['计算机科学与技术', '软件工程', '数据科学与大数据技术'];
// 造数据用的院系列表。
const names = ['陈思远', '林沐辰', '王可欣', '李明轩', '张若曦', '刘宇航', '赵子涵', '周语桐', '吴昊然', '徐嘉宁', '孙雨泽', '郑书瑶'];
// 造数据用的姓名池。

const students = Array.from({ length: 48 }, (_, i) => {
// 生成 48 条学生记录的 VALUES 片段。
  const name = names[i % names.length] + (i >= 12 ? ` ${Math.floor(i / 12) + 1}` : '');
  // 姓名按池子取模；超过一轮后补一个编号，保证不重名。
  const id = 1001 + i;
  // 学号从 1001 起递增。
  return `(${id},${quote(name)},${quote(departments[i % departments.length])},${18 + i % 6},${quote(`student${id}@csu.edu.cn`)},${quote(i % 9 === 0 ? '休学' : '在读')})`;
  // 每条记录包含学号、姓名、院系、年龄（18-23 轮转）、邮箱，以及"每 9 人一个休学"的状态。
}).join(',');
// 用逗号把所有记录拼成一条 INSERT 的多值列表。

const courses = `(1,'编译原理',4,'姚老师'),(2,'数据库系统',4,'邓老师'),(3,'操作系统',3,'桂老师'),(4,'软件工程',3,'陈老师')`;
// 四门课程的固定数据：课程号、课名、学分、任课老师。
const enrollments = Array.from({ length: 96 }, (_, i) =>
// 生成 96 条选课记录。
  `(${i + 1},${1001 + i % 48},${1 + i % 4},${65 + i % 36})`
  // 字段依次是选课号、学号（与学生表对得上）、课程号（与课程表对得上）、成绩。
).join(',');
// 同样拼成一条多值列表。

const sql = `BEGIN;
// 整段装载脚本：先用 BEGIN 显式开启事务，保证要么全成功要么全回滚。
CREATE TABLE students (id INT PRIMARY KEY, name VARCHAR NOT NULL, department VARCHAR, age INT, email VARCHAR, status VARCHAR);
// 建学生表。
CREATE TABLE courses (id INT PRIMARY KEY, name VARCHAR NOT NULL, credits INT, teacher VARCHAR);
// 建课程表。
CREATE TABLE enrollments (id INT PRIMARY KEY, student_id INT, course_id INT, score INT);
// 建选课表。
INSERT INTO students VALUES ${students};
// 灌入学生数据。
INSERT INTO courses VALUES ${courses};
// 灌入课程数据。
INSERT INTO enrollments VALUES ${enrollments};
// 灌入选课数据。
COMMIT;`;
// 提交事务；到这里三条表都建好、数据也灌好了。

let sessionId;
// 会话编号，最后要在 finally 里用它关闭会话。
try {
// 用 try/finally 保证异常时也把会话关掉。
  const opened = await request('/sessions', { method: 'POST' });
  // 开一个新会话。
  sessionId = opened.sessionId;
  // 记住会话号。
  const catalog = await request(`/sessions/${sessionId}/catalog`);
  // 查一下当前目录。
  if (catalog.tables.length) {
  // 已经有表说明之前灌过。
    console.log(`Catalog already contains ${catalog.tables.length} tables; seed skipped.`);
    // 打印跳过信息，避免重复灌数据导致主键冲突。
  } else {
  // 空库才真正执行装载脚本。
    const result = await request(`/sessions/${sessionId}/execute`, {
    // 提交要执行的 SQL。
      method: 'POST', body: JSON.stringify({ sql }),
      // 用 POST 并把 SQL 放进 JSON 请求体。
    });
    // 请求结束。
    if (result.success === false) throw new Error(result.error?.message ?? 'Seed failed');
    // 业务层报失败时抛出，交外层 finally 收尾。
    const refreshed = await request(`/sessions/${sessionId}/catalog`);
    // 重新取一次目录。
    console.log(`Seeded ${refreshed.tables.length} real C++ tables.`);
    // 打印实际建出来的表数量，作为成功的证据。
  }
  // 分支结束。
} finally {
// 收尾。
  if (sessionId) await request(`/sessions/${sessionId}/close`, { method: 'POST' }).catch(() => {});
  // 关闭会话；即使关闭失败也不掩盖原来的错误（catch 吞掉异常）。
}
// 脚本结束。

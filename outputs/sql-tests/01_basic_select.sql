-- 基础查询：查看学生表前 20 行
SELECT id, name, department, age, email, status
FROM students
ORDER BY id
LIMIT 20;

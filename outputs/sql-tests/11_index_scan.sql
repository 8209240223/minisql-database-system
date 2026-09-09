-- 索引测试：创建索引后执行等值查询，随后回滚
BEGIN;
CREATE INDEX idx_students_department ON students(department);
SELECT id, name, department
FROM students
WHERE department = '软件工程'
ORDER BY id
LIMIT 10;
ROLLBACK;

-- 索引事务测试：创建索引、查看索引计划，最后回滚索引变更
BEGIN;
CREATE INDEX students_status_idx ON students(status);
EXPLAIN
SELECT id, name
FROM students
WHERE status = '在读';
SELECT COUNT(*) AS active_students
FROM students
WHERE status = '在读';
ROLLBACK;

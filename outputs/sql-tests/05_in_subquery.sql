-- IN 子查询：查询有 95 分以上记录的学生
SELECT id, name
FROM students
WHERE id IN (SELECT student_id FROM enrollments WHERE score >= 95)
ORDER BY id;

-- EXISTS 相关子查询：查询存在满分记录的学生
SELECT s.id, s.name
FROM students s
WHERE EXISTS (
    SELECT e.student_id
    FROM enrollments e
    WHERE e.student_id = s.id AND e.score = 100
)
ORDER BY s.id;

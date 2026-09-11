-- 反连接测试：查询没有高分段课程记录的学生
SELECT s.id, s.name
FROM students s
WHERE NOT EXISTS (
    SELECT e.id
    FROM enrollments e
    WHERE e.student_id = s.id AND e.score >= 90
)
ORDER BY s.id
LIMIT 10;

-- 排序、过滤和分页：查询高分选课记录
SELECT s.id, s.name, e.score
FROM students s
JOIN enrollments e ON e.student_id = s.id
WHERE e.score >= 90
ORDER BY e.score DESC, s.id
LIMIT 10;

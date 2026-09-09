-- 三表连接：学生、选课、课程
SELECT s.id, s.name, c.name AS course_name, e.score
FROM students s
JOIN enrollments e ON e.student_id = s.id
JOIN courses c ON c.id = e.course_id
ORDER BY e.score DESC, s.id
LIMIT 20;

-- 综合报表：学生、选课和课程三表连接后的分组汇总
SELECT s.department,
       c.name AS course_name,
       COUNT(*) AS enrollment_count,
       AVG(e.score) AS avg_score
FROM students s
JOIN enrollments e ON e.student_id = s.id
JOIN courses c ON c.id = e.course_id
GROUP BY s.department, c.name
HAVING COUNT(*) >= 20
ORDER BY enrollment_count DESC, s.department, c.name;

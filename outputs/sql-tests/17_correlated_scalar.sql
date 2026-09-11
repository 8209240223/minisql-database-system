-- 相关标量子查询：统计每个学生的选课数量
SELECT s.id, s.name,
       (SELECT COUNT(*)
        FROM enrollments e
        WHERE e.student_id = s.id) AS course_count
FROM students s
ORDER BY course_count DESC, s.id
LIMIT 10;

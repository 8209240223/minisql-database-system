-- JOIN + GROUP BY + HAVING：统计每门课程的选课人数和成绩指标
SELECT c.name,
       COUNT(*) AS total,
       AVG(e.score) AS avg_score,
       MIN(e.score) AS min_score,
       MAX(e.score) AS max_score
FROM enrollments e
JOIN courses c ON e.course_id = c.id
GROUP BY c.name
HAVING COUNT(*) >= 10
ORDER BY avg_score DESC, c.name;

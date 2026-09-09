-- 分组聚合：统计各学院人数和平均年龄
SELECT department, COUNT(*) AS student_count, AVG(age) AS avg_age
FROM students
GROUP BY department
ORDER BY department;

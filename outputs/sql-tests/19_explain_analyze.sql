-- EXPLAIN 与 EXPLAIN ANALYZE：查看估计计划和实际执行统计
EXPLAIN
SELECT s.department, COUNT(*) AS total
FROM students s
WHERE s.status = '在读'
GROUP BY s.department
ORDER BY total DESC;

EXPLAIN ANALYZE
SELECT s.department, COUNT(*) AS total
FROM students s
WHERE s.status = '在读'
GROUP BY s.department
ORDER BY total DESC;

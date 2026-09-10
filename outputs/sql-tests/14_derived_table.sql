-- 派生表测试：先筛选学生，再从派生结果中读取字段
SELECT d.id, d.department
FROM (
    SELECT id, department
    FROM students
    WHERE status = '在读'
) AS d
ORDER BY d.id
LIMIT 10;

-- 用途：演示主键重复导致的唯一性约束错误。
-- 预期结果：id = 1001 已存在，INSERT 失败并返回 UNIQUE constraint failed on key 1。
INSERT INTO students (id, name, department, age, email, status)
VALUES (1001, '重复主键演示', '测试部', 20, 'duplicate_demo@example.com', '在读');

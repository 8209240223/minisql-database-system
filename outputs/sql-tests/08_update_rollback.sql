-- UPDATE 回滚：临时修改学生年龄后撤销
BEGIN;
UPDATE students SET age = age + 1 WHERE id = 1001;
SELECT id, name, age FROM students WHERE id = 1001;
ROLLBACK;

-- DELETE 回滚：临时删除低分选课记录后撤销
BEGIN;
DELETE FROM enrollments WHERE score < 70;
SELECT COUNT(*) AS remaining_enrollments FROM enrollments;
ROLLBACK;

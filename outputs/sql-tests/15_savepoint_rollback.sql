-- 保存点测试：回滚到保存点后只保留保存点之前的数据，最后整体回滚
BEGIN;
CREATE TABLE savepoint_test(id INT, note VARCHAR);
INSERT INTO savepoint_test VALUES(1, 'kept');
SAVEPOINT s1;
INSERT INTO savepoint_test VALUES(2, 'rolled-back');
ROLLBACK TO SAVEPOINT s1;
SELECT id, note FROM savepoint_test ORDER BY id;
RELEASE SAVEPOINT s1;
ROLLBACK;

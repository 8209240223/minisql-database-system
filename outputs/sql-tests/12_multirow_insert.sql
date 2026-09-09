-- 多行插入：临时插入三行并统计，随后回滚
BEGIN;
CREATE TABLE multirow_test(id INT PRIMARY KEY, value VARCHAR);
INSERT INTO multirow_test VALUES
    (1, 'alpha'),
    (2, 'beta'),
    (3, 'gamma');
SELECT COUNT(*) AS inserted_rows FROM multirow_test;
ROLLBACK;

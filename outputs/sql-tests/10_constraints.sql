-- 约束测试：主键、NOT NULL、CHECK、UNIQUE 和多行 INSERT
BEGIN;
CREATE TABLE constraint_test(
    id INT PRIMARY KEY,
    name VARCHAR NOT NULL,
    score INT CHECK(score >= 0),
    code VARCHAR UNIQUE
);
INSERT INTO constraint_test VALUES
    (1, 'first', 10, 'A'),
    (2, 'second', 20, 'B');
SELECT id, name, score, code
FROM constraint_test
ORDER BY id;
ROLLBACK;

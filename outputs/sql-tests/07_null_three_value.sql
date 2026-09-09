-- NULL 三值逻辑：临时表中 NULL 与 0 必须区分
BEGIN;
CREATE TABLE null_test(id INT, value INT);
INSERT INTO null_test VALUES(1, NULL), (2, 0), (3, 10);
SELECT id, value, value IS NULL AS is_null
FROM null_test
ORDER BY id;
ROLLBACK;

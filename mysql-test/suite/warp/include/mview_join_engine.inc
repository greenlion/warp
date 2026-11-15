# Join materialized views in one storage engine.
#   $mv_engine  the engine of the base tables and of the views

--echo #
--echo # Join materialized views, ENGINE=$mv_engine
--echo #

--replace_result $mv_engine ENGINE
eval CREATE TABLE cust (
  id INT NOT NULL PRIMARY KEY,
  name VARCHAR(20),
  region CHAR(2)
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
--replace_result $mv_engine ENGINE
eval CREATE TABLE ord (
  id BIGINT NOT NULL PRIMARY KEY,
  cust_id INT,
  amount DECIMAL(10,2),
  placed DATE
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
--replace_result $mv_engine ENGINE
eval CREATE TABLE item (
  ord_id BIGINT NOT NULL,
  line INT NOT NULL,
  sku VARCHAR(10),
  qty INT,
  PRIMARY KEY (ord_id, line)
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

INSERT INTO cust VALUES (1, 'ann', 'eu'), (2, 'bob', 'eu'), (3, 'cy', 'us'),
                        (4, NULL, 'us');
INSERT INTO ord VALUES (10, 1, 15.50, '2026-01-01'), (11, 1, 4.25, '2026-01-02'),
                       (12, 2, 100.00, '2026-02-01'), (13, NULL, 7.00, NULL),
                       (14, 99, 1.00, '2026-03-01');
INSERT INTO item VALUES (10, 1, 'nut', 3), (10, 2, 'bolt', 5), (12, 1, 'nut', 1);

--echo # Inner join
--let $sel_inner = SELECT c.id AS cid, c.name, o.id AS oid, o.amount FROM cust c JOIN ord o ON o.cust_id = c.id
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_inner ENGINE=$mv_engine REFRESH MANUAL AS $sel_inner;
SELECT * FROM mv_inner ORDER BY cid, oid;
--let $mv_view = mv_inner
--let $mv_select = $sel_inner
--source suite/warp/include/mview_check.inc

--echo # LEFT joins, an expression, WHERE; NULL-extended rows
--let $sel_left = SELECT c.id AS cid, UPPER(c.name) AS uname, o.id AS oid, o.amount * 2 AS dbl, i.sku, COALESCE(i.qty, 0) AS qty FROM cust c LEFT JOIN ord o ON o.cust_id = c.id LEFT JOIN item i ON i.ord_id = o.id WHERE c.region IN ('eu', 'us')
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_left ENGINE=$mv_engine REFRESH MANUAL AS $sel_left;
SELECT * FROM mv_left ORDER BY cid, oid, sku;
--let $mv_view = mv_left
--let $mv_select = $sel_left
--source suite/warp/include/mview_check.inc
--echo # The invisible key columns: NULL for the NULL-extended instances
# (the keys of c and o are the columns cid and oid of the user)
SELECT cid, oid, `mv$pk2_0`, `mv$pk2_1` FROM mv_left
ORDER BY cid, oid, `mv$pk2_1`;

--echo # RIGHT join: the root is the outer table
--let $sel_right = SELECT o.id AS oid, o.placed, c.id AS cid, c.region FROM ord o RIGHT JOIN cust c ON o.cust_id = c.id
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_right ENGINE=$mv_engine REFRESH MANUAL AS $sel_right;
SELECT * FROM mv_right ORDER BY cid, oid;
--let $mv_view = mv_right
--let $mv_select = $sel_right
--source suite/warp/include/mview_check.inc

--echo # Self join, with a column list
--let $sel_self = SELECT a.id, b.id, a.region FROM cust a JOIN cust b ON a.region = b.region AND a.id < b.id
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_self (first_id, second_id, region) ENGINE=$mv_engine REFRESH MANUAL AS $sel_self;
SELECT * FROM mv_self ORDER BY first_id, second_id;
--let $mv_view = mv_self
--let $mv_select = $sel_self
--source suite/warp/include/mview_check.inc

--echo # The layout: user columns, invisible key columns, their indexes
--replace_regex / AUTO_INCREMENT=[0-9]+//
--replace_result $mv_engine ENGINE
SHOW CREATE TABLE mv_left;
SELECT TABLE_NAME, COLUMN_NAME, EXTRA FROM INFORMATION_SCHEMA.COLUMNS
WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME LIKE 'mv\_%' AND EXTRA <> ''
ORDER BY TABLE_NAME, ORDINAL_POSITION;
--replace_result $mv_engine ENGINE
SELECT TABLE_NAME, ENGINE, TABLE_TYPE FROM INFORMATION_SCHEMA.TABLES
WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME LIKE 'mv\_%' ORDER BY TABLE_NAME;

--echo # Changes of the base tables are seen after REFRESH
INSERT INTO cust VALUES (5, 'eve', 'eu');
INSERT INTO ord VALUES (15, 5, 9.99, '2026-04-01'), (16, 3, 2.00, '2026-04-02');
INSERT INTO item VALUES (15, 1, 'gear', 2), (11, 1, 'bolt', 7);
UPDATE cust SET name = 'bobby', region = 'us' WHERE id = 2;
UPDATE ord SET cust_id = 4 WHERE id = 13;
DELETE FROM item WHERE ord_id = 10 AND line = 2;
DELETE FROM ord WHERE id = 14;

REFRESH MATERIALIZED VIEW mv_inner;
REFRESH MATERIALIZED VIEW mv_left;
REFRESH MATERIALIZED VIEW mv_right;
REFRESH MATERIALIZED VIEW mv_self;

SELECT * FROM mv_inner ORDER BY cid, oid;
--let $mv_view = mv_inner
--let $mv_select = $sel_inner
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_left ORDER BY cid, oid, sku;
--let $mv_view = mv_left
--let $mv_select = $sel_left
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_right ORDER BY cid, oid;
--let $mv_view = mv_right
--let $mv_select = $sel_right
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_self ORDER BY first_id, second_id;
--let $mv_view = mv_self
--let $mv_select = $sel_self
--source suite/warp/include/mview_check.inc

--echo # REFRESH of an emptied join
DELETE FROM ord;
REFRESH MATERIALIZED VIEW mv_inner;
REFRESH MATERIALIZED VIEW mv_left;
SELECT COUNT(*) FROM mv_inner;
SELECT * FROM mv_left ORDER BY cid;

--echo # SHOW CREATE VIEW gives a statement that makes the same view
--replace_result $mv_engine ENGINE
SHOW CREATE VIEW mv_self;
--let $mv_create = query_get_value(SHOW CREATE VIEW mv_left, Create View, 1)
DROP MATERIALIZED VIEW mv_left;
--disable_query_log
eval $mv_create;
--enable_query_log
--let $mv_view = mv_left
--let $mv_select = $sel_left
--source suite/warp/include/mview_check.inc
--replace_result $mv_engine ENGINE
SHOW CREATE VIEW mv_left;

DROP MATERIALIZED VIEW mv_inner, mv_left, mv_right, mv_self;
SHOW TABLES;
DROP TABLE cust, ord, item;

# Aggregate materialized views in one storage engine.
#   $mv_engine  the engine of the base tables and of the views

--echo #
--echo # Aggregate materialized views, ENGINE=$mv_engine
--echo #

--replace_result $mv_engine ENGINE
eval CREATE TABLE prod (
  id INT NOT NULL PRIMARY KEY,
  name VARCHAR(20),
  kind CHAR(1)
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
--replace_result $mv_engine ENGINE
eval CREATE TABLE sales (
  id INT NOT NULL PRIMARY KEY,
  prod_id INT,
  region CHAR(2),
  qty INT,
  price DECIMAL(10,2),
  day DATE
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
--replace_result $mv_engine ENGINE
eval CREATE TABLE nothing (
  id INT NOT NULL PRIMARY KEY,
  v INT
) ENGINE=$mv_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

INSERT INTO prod VALUES (1, 'nut', 'a'), (2, 'bolt', 'a'), (3, 'gear', 'b'),
                        (4, 'unsold', 'b');
INSERT INTO sales VALUES
  (1, 1, 'eu', 3, 1.50, '2026-01-01'), (2, 1, 'eu', 5, 1.25, '2026-01-02'),
  (3, 2, 'us', NULL, 9.99, '2026-01-03'), (4, 3, NULL, 2, NULL, '2026-01-04'),
  (5, 3, NULL, 7, 20.00, NULL), (6, NULL, 'us', 1, 0.10, '2026-01-06'),
  (7, 2, 'ap', NULL, NULL, '2026-01-07');

--echo # COUNT(*) of a table: one row, and no table with a row per base row
--let $sel_cnt = SELECT COUNT(*) AS c FROM sales
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_cnt ENGINE=$mv_engine REFRESH MANUAL AS $sel_cnt;
SELECT * FROM mv_cnt;
SELECT COUNT(*) FROM mv_cnt;
SHOW TABLES;
SELECT COUNT(*) FROM `mv_cnt$mv_state`;
--replace_result $mv_engine ENGINE
SHOW CREATE TABLE mv_cnt;

--echo # GROUP BY with every kind of state, NULL arguments and a NULL group
--let $sel_grp = SELECT region, COUNT(*) AS cnt, COUNT(qty) AS cnt_qty, SUM(qty) AS sum_qty, AVG(price) AS avg_price, MIN(price) AS min_price, MAX(day) AS max_day, AVG(qty) AS avg_qty FROM sales GROUP BY region
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_grp ENGINE=$mv_engine REFRESH MANUAL AS $sel_grp;
SELECT * FROM mv_grp ORDER BY region;
--let $mv_view = mv_grp
--let $mv_select = $sel_grp
--source suite/warp/include/mview_check.inc
--replace_result $mv_engine ENGINE
SHOW CREATE TABLE mv_grp;
--echo # The invisible columns: group, row count, states
SELECT `mv$g0`, `mv$count`, `mv$s0`, `mv$s1`, `mv$s2`, `mv$s3`, `mv$s4`, `mv$s5`
FROM mv_grp ORDER BY `mv$g0`;

--echo # Expressions over aggregates and over GROUP BY expressions
--let $sel_expr = SELECT qty % 2 AS parity, SUM(qty) / COUNT(*) AS per_row, SUM(price) * 2 + 1 AS twice, (qty % 2) + 10 AS shifted, CONCAT(MIN(region), '-', MAX(region)) AS regions, 'tag' AS tag FROM sales WHERE id < 100 GROUP BY qty % 2
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_expr ENGINE=$mv_engine REFRESH MANUAL AS $sel_expr;
SELECT * FROM mv_expr ORDER BY parity;
--let $mv_view = mv_expr
--let $mv_select = $sel_expr
--source suite/warp/include/mview_check.inc

--echo # SELECT DISTINCT
--let $sel_dist = SELECT DISTINCT region, qty IS NULL AS no_qty FROM sales
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_dist ENGINE=$mv_engine REFRESH MANUAL AS $sel_dist;
SELECT * FROM mv_dist ORDER BY region, no_qty;
--let $mv_view = mv_dist
--let $mv_select = $sel_dist
--source suite/warp/include/mview_check.inc

--echo # Aggregates that are stored as they are: COUNT(DISTINCT), GROUP_CONCAT
--let $sel_rec = SELECT region, COUNT(DISTINCT prod_id) AS prods, GROUP_CONCAT(id ORDER BY id) AS ids, SUM(DISTINCT qty) AS dsum, COUNT(*) AS cnt FROM sales GROUP BY region
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_rec ENGINE=$mv_engine REFRESH MANUAL AS $sel_rec;
SELECT * FROM mv_rec ORDER BY region;
--let $mv_view = mv_rec
--let $mv_select = $sel_rec
--source suite/warp/include/mview_check.inc

--echo # Aggregate over an inner join
--let $sel_join = SELECT p.kind, p.name, COUNT(*) AS cnt, SUM(s.qty * s.price) AS turnover, MAX(s.qty) AS max_qty FROM prod p JOIN sales s ON s.prod_id = p.id GROUP BY p.kind, p.name
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_join ENGINE=$mv_engine REFRESH MANUAL AS $sel_join;
SELECT * FROM mv_join ORDER BY kind, name;
--let $mv_view = mv_join
--let $mv_select = $sel_join
--source suite/warp/include/mview_check.inc

--echo # Aggregate over a LEFT join
--let $sel_left = SELECT p.name, COUNT(s.id) AS cnt, COUNT(*) AS cnt_rows, SUM(s.qty) AS qty, MIN(s.day) AS first_day FROM prod p LEFT JOIN sales s ON s.prod_id = p.id GROUP BY p.name
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_left ENGINE=$mv_engine REFRESH MANUAL AS $sel_left;
SELECT * FROM mv_left ORDER BY name;
--let $mv_view = mv_left
--let $mv_select = $sel_left
--source suite/warp/include/mview_check.inc

--echo # No GROUP BY over an empty table: one row
--let $sel_empty = SELECT COUNT(*) AS c, COUNT(v) AS cv, SUM(v) AS s, AVG(v) AS a, MIN(v) AS mn, MAX(v) AS mx FROM nothing
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_empty ENGINE=$mv_engine REFRESH MANUAL AS $sel_empty;
SELECT * FROM mv_empty;
--let $mv_view = mv_empty
--let $mv_select = $sel_empty
--source suite/warp/include/mview_check.inc
--echo # GROUP BY over an empty table: no row
--replace_result $mv_engine ENGINE
eval CREATE MATERIALIZED VIEW mv_empty_grp ENGINE=$mv_engine REFRESH MANUAL AS
  SELECT v, COUNT(*) AS c FROM nothing GROUP BY v;
SELECT COUNT(*) FROM mv_empty_grp;

--echo # The state table: one row with the binary log position of the contents
SELECT COUNT(*), MIN(binlog_file LIKE 'binlog.0%'), MIN(binlog_pos > 4)
FROM `mv_grp$mv_state`;
SELECT binlog_pos INTO @mv_pos_before FROM `mv_grp$mv_state`;
--error ER_MVIEW_IS_READ_ONLY
UPDATE `mv_grp$mv_state` SET binlog_pos = 4;
--error ER_MVIEW_IS_READ_ONLY
DROP TABLE `mv_grp$mv_state`;
--error ER_MVIEW_NOT_A_MVIEW
DROP MATERIALIZED VIEW `mv_grp$mv_state`;
--error ER_MVIEW_IS_READ_ONLY
INSERT INTO mv_grp (`mv$g0`, `mv$count`) VALUES ('zz', 1);

--echo # What a table cannot hold
--error ER_MVIEW_UNSUPPORTED_DEFINITION
CREATE MATERIALIZED VIEW mv_bad AS
  SELECT region, COUNT(*) FROM sales GROUP BY region HAVING COUNT(*) > 1;
--error ER_MVIEW_UNSUPPORTED_DEFINITION
CREATE MATERIALIZED VIEW mv_bad AS
  SELECT region, COUNT(*) FROM sales GROUP BY region WITH ROLLUP;
--error ER_MVIEW_UNSUPPORTED_DEFINITION
CREATE MATERIALIZED VIEW mv_bad AS
  SELECT id, region, COUNT(*) FROM sales GROUP BY id;
--error ER_MVIEW_UNSUPPORTED_DEFINITION
CREATE MATERIALIZED VIEW mv_bad AS
  SELECT DISTINCT region, COUNT(*) FROM sales GROUP BY region, qty;
--error ER_MVIEW_UNSUPPORTED_DEFINITION
CREATE MATERIALIZED VIEW mv_bad AS
  SELECT region, SUM(qty) OVER (PARTITION BY region) FROM sales;

--echo # Changes of the base tables are seen after REFRESH
INSERT INTO sales VALUES (8, 4, 'eu', 10, 2.00, '2026-02-01'),
                         (9, 1, 'za', NULL, NULL, NULL);
UPDATE sales SET qty = 4, region = 'us' WHERE id = 1;
UPDATE sales SET price = NULL WHERE id = 6;
DELETE FROM sales WHERE id IN (4, 5);
UPDATE prod SET kind = 'c' WHERE id = 2;
INSERT INTO nothing VALUES (1, NULL), (2, 5);

REFRESH MATERIALIZED VIEW mv_cnt;
REFRESH MATERIALIZED VIEW mv_grp;
REFRESH MATERIALIZED VIEW mv_expr;
REFRESH MATERIALIZED VIEW mv_dist;
REFRESH MATERIALIZED VIEW mv_rec;
REFRESH MATERIALIZED VIEW mv_join;
REFRESH MATERIALIZED VIEW mv_left;
REFRESH MATERIALIZED VIEW mv_empty;
REFRESH MATERIALIZED VIEW mv_empty_grp;

SELECT * FROM mv_cnt;
SELECT * FROM mv_grp ORDER BY region;
--let $mv_view = mv_grp
--let $mv_select = $sel_grp
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_expr ORDER BY parity;
--let $mv_view = mv_expr
--let $mv_select = $sel_expr
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_dist ORDER BY region, no_qty;
--let $mv_view = mv_dist
--let $mv_select = $sel_dist
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_rec ORDER BY region;
--let $mv_view = mv_rec
--let $mv_select = $sel_rec
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_join ORDER BY kind, name;
--let $mv_view = mv_join
--let $mv_select = $sel_join
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_left ORDER BY name;
--let $mv_view = mv_left
--let $mv_select = $sel_left
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_empty;
--let $mv_view = mv_empty
--let $mv_select = $sel_empty
--source suite/warp/include/mview_check.inc
SELECT * FROM mv_empty_grp ORDER BY v;

--echo # REFRESH records the new position
SELECT COUNT(*), MIN(binlog_pos > @mv_pos_before) AS position_moved
FROM `mv_grp$mv_state`;

--echo # The one row of an aggregate without GROUP BY stays when the table empties
DELETE FROM nothing;
DELETE FROM sales;
REFRESH MATERIALIZED VIEW mv_empty;
REFRESH MATERIALIZED VIEW mv_cnt;
REFRESH MATERIALIZED VIEW mv_grp;
SELECT * FROM mv_empty;
SELECT * FROM mv_cnt;
SELECT COUNT(*) FROM mv_grp;

--echo # SHOW CREATE VIEW gives a statement that makes the same view
INSERT INTO sales VALUES (1, 1, 'eu', 3, 1.50, '2026-01-01'),
                         (2, 2, 'eu', NULL, 2.50, '2026-01-02');
--replace_result $mv_engine ENGINE
SHOW CREATE VIEW mv_grp;
--let $mv_create = query_get_value(SHOW CREATE VIEW mv_grp, Create View, 1)
DROP MATERIALIZED VIEW mv_grp;
--disable_query_log
eval $mv_create;
--enable_query_log
--let $mv_view = mv_grp
--let $mv_select = $sel_grp
--source suite/warp/include/mview_check.inc

--echo # The state tables are dropped with the views
DROP MATERIALIZED VIEW mv_cnt, mv_grp, mv_expr, mv_dist, mv_rec, mv_join,
                       mv_left, mv_empty, mv_empty_grp;
SHOW TABLES;
DROP TABLE prod, sales, nothing;

# Aggregate views maintained by the WARP_MVIEW daemon from the binary log.
#
#   $view_engine   engine of the materialized views (InnoDB or WARP)
#   $base_engine   engine of the base tables (InnoDB or WARP)
#
# After every change the views are compared with their defining queries
# (mview_sync_check.inc waits for the daemon).

call mtr.add_suppression("WARP_MVIEW");
--let $mview_check_calls = 0

--let $topt = ENGINE=$base_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin

--echo # views: $view_engine, base tables: $base_engine
# The views of WARP tables, and the views over WARP tables, are updated
# while their base tables are locked (WARP has no snapshots).  The daemon
# limits the share of the time it holds such locks; this test changes one
# row at a time and waits for the views, so it lifts the limit.  (The limit
# itself is the subject of mview_sync_agg_concurrent.)
--disable_query_log
SET GLOBAL warp_mview_pin_duty_percent = 100;
--enable_query_log
--disable_query_log
eval CREATE TABLE t (id INT NOT NULL, g VARCHAR(10), x INT, d DECIMAL(10,2), PRIMARY KEY (id)) $topt;
eval CREATE TABLE c (id INT NOT NULL, name VARCHAR(20), region VARCHAR(10), PRIMARY KEY (id)) $topt;
eval CREATE TABLE o (id INT NOT NULL, c_id INT, amount DECIMAL(10,2), PRIMARY KEY (id)) $topt;
--enable_query_log

--let $sel_cnt = SELECT COUNT(*) AS n FROM t
--let $sel_nog = SELECT COUNT(*) AS n, COUNT(x) AS cx, SUM(x) AS sx, MIN(x) AS mn, MAX(x) AS mx, SUM(d) AS sd FROM t
--let $sel_grp = SELECT g, COUNT(*) AS n, COUNT(x) AS cx, SUM(x) AS sx, AVG(d) AS ad, MIN(x) AS mn, MAX(d) AS mxd FROM t GROUP BY g
--let $sel_where = SELECT g, SUM(d) AS sd, COUNT(*) AS n FROM t WHERE x > 10 GROUP BY g
--let $sel_cd = SELECT g, COUNT(DISTINCT x) AS dx, COUNT(*) AS n FROM t GROUP BY g
--let $sel_dist = SELECT DISTINCT g, x FROM t
--let $sel_join = SELECT c.region, COUNT(*) AS n, SUM(o.amount) AS total, MAX(o.amount) AS biggest FROM c JOIN o ON o.c_id = c.id GROUP BY c.region
--let $sel_left = SELECT c.id, c.name, COUNT(o.id) AS orders, SUM(o.amount) AS total FROM c LEFT JOIN o ON o.c_id = c.id GROUP BY c.id, c.name

--echo # Create the views over empty tables
--disable_query_log
eval CREATE MATERIALIZED VIEW a_cnt ENGINE=$view_engine AS $sel_cnt;
eval CREATE MATERIALIZED VIEW a_nog ENGINE=$view_engine AS $sel_nog;
eval CREATE MATERIALIZED VIEW a_grp ENGINE=$view_engine AS $sel_grp;
eval CREATE MATERIALIZED VIEW a_where ENGINE=$view_engine AS $sel_where;
eval CREATE MATERIALIZED VIEW a_cd ENGINE=$view_engine AS $sel_cd;
eval CREATE MATERIALIZED VIEW a_dist ENGINE=$view_engine AS $sel_dist;
eval CREATE MATERIALIZED VIEW a_join ENGINE=$view_engine AS $sel_join;
eval CREATE MATERIALIZED VIEW a_left ENGINE=$view_engine AS $sel_left;
--enable_query_log
--source suite/warp/include/mview_sync_agg_check.inc
SELECT * FROM a_cnt;
SELECT * FROM a_nog;

--echo # From empty: the first rows, NULL arguments and a NULL group key
INSERT INTO t VALUES (1, 'a', 5, 1.50);
--source suite/warp/include/mview_sync_agg_check.inc
INSERT INTO t VALUES (2, 'a', 20, 2.50), (3, 'b', NULL, NULL), (4, NULL, 30, 10.00), (5, NULL, NULL, 0.25), (6, 'b', 11, NULL);
--source suite/warp/include/mview_sync_agg_check.inc
SELECT * FROM a_cnt;
SELECT * FROM a_grp ORDER BY g;

--echo # Updates of the arguments: to and from NULL
UPDATE t SET x = x + 1, d = d * 2 WHERE id IN (1, 2);
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET x = NULL WHERE id = 2;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET x = 7, d = 7.77 WHERE id = 3;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # MIN and MAX: a new extreme, then the extreme is lost
INSERT INTO t VALUES (7, 'a', -100, 999.99);
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM t WHERE id = 7;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET x = 1000 WHERE id = 1;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET x = 6 WHERE id = 1;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # A row moves between groups, to the NULL group and into a new group
UPDATE t SET g = 'b' WHERE id = 1;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET g = NULL WHERE id = 6;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET g = 'new' WHERE id = 4;
--source suite/warp/include/mview_sync_agg_check.inc
--echo # ... and the WHERE of a_where: a row starts and stops to qualify
UPDATE t SET x = 50 WHERE id = 3;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE t SET x = 1 WHERE id = 3;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # Groups disappear and appear; a primary key changes
DELETE FROM t WHERE g = 'new';
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM t WHERE g IS NULL;
--source suite/warp/include/mview_sync_agg_check.inc
INSERT INTO t VALUES (8, NULL, 8, 8.00), (9, 'z', 9, 9.00);
UPDATE t SET id = 90 WHERE id = 9;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # Duplicates for COUNT(DISTINCT) and SELECT DISTINCT
INSERT INTO t VALUES (10, 'z', 9, 1.00), (11, 'z', 9, 2.00), (12, 'z', 10, 3.00);
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM t WHERE id = 10;
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM t WHERE id IN (11, 90);
--source suite/warp/include/mview_sync_agg_check.inc

--echo # Several statements and a rollback in the same batch
BEGIN;
INSERT INTO t VALUES (20, 'q', 1, 1.00), (21, 'q', 2, 2.00);
UPDATE t SET x = x * 10 WHERE g = 'q';
DELETE FROM t WHERE id = 20;
INSERT INTO t VALUES (22, 'q', 3, 3.00);
COMMIT;
BEGIN;
INSERT INTO t VALUES (30, 'rolled', 1, 1.00);
UPDATE t SET x = 0;
ROLLBACK;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # To empty and back
DELETE FROM t;
--source suite/warp/include/mview_sync_agg_check.inc
SELECT * FROM a_cnt;
SELECT * FROM a_nog;
SELECT COUNT(*) FROM a_grp;
INSERT INTO t VALUES (1, 'a', 1, 1.00), (2, 'a', 2, 2.00), (3, 'b', 30, 3.00), (4, NULL, 40, NULL);
--source suite/warp/include/mview_sync_agg_check.inc

--echo # Join views: one side, the other side, both sides in one transaction
INSERT INTO c VALUES (1, 'ann', 'north'), (2, 'bob', 'south'), (3, 'cy', 'north'), (4, 'dee', NULL);
--source suite/warp/include/mview_sync_agg_check.inc
INSERT INTO o VALUES (10, 1, 15.50), (11, 1, 250.00), (12, 2, 99.99), (13, NULL, 5.00), (14, 4, 1.00);
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE o SET amount = amount + 1 WHERE c_id = 1;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE c SET region = 'south' WHERE id = 1;
--source suite/warp/include/mview_sync_agg_check.inc
BEGIN;
INSERT INTO c VALUES (5, 'eve', 'east');
INSERT INTO o VALUES (15, 5, 500.00), (16, 3, 30.00);
UPDATE c SET region = 'east' WHERE id = 2;
UPDATE o SET c_id = 5 WHERE id = 13;
DELETE FROM o WHERE id = 11;
UPDATE c SET name = 'ANN' WHERE id = 1;
COMMIT;
--source suite/warp/include/mview_sync_agg_check.inc
--echo # LEFT JOIN: a customer loses its last order and gets one again
DELETE FROM o WHERE c_id = 4;
--source suite/warp/include/mview_sync_agg_check.inc
INSERT INTO o VALUES (17, 4, 4.00);
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM c WHERE id = 5;
--source suite/warp/include/mview_sync_agg_check.inc
UPDATE c SET id = 40 WHERE id = 4;
UPDATE o SET c_id = 40 WHERE c_id = 4;
--source suite/warp/include/mview_sync_agg_check.inc
--echo # the biggest order of a region goes away
DELETE FROM o WHERE id = 15;
UPDATE o SET amount = 0.01 WHERE id = 12;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # Many rows
--disable_query_log
SET SESSION cte_max_recursion_depth = 100000;
INSERT INTO t WITH RECURSIVE n(i) AS (SELECT 1000 UNION ALL SELECT i + 1 FROM n WHERE i < 1599) SELECT i, CONCAT('g', i MOD 50), i MOD 97, i / 7 FROM n;
INSERT INTO c WITH RECURSIVE n(i) AS (SELECT 1000 UNION ALL SELECT i + 1 FROM n WHERE i < 1099) SELECT i, CONCAT('c', i), CONCAT('r', i MOD 7) FROM n;
BEGIN;
INSERT INTO o WITH RECURSIVE n(i) AS (SELECT 1000 UNION ALL SELECT i + 1 FROM n WHERE i < 1399) SELECT i, 1000 + (i MOD 150), i / 10 FROM n;
UPDATE t SET x = x + 1 WHERE id BETWEEN 1200 AND 1299;
COMMIT;
--enable_query_log
--source suite/warp/include/mview_sync_agg_check.inc
DELETE FROM t WHERE id >= 1000 AND id MOD 3 = 0;
UPDATE c SET region = 'moved' WHERE id BETWEEN 1050 AND 1069;
DELETE FROM o WHERE id >= 1000 AND id MOD 5 = 0;
--source suite/warp/include/mview_sync_agg_check.inc

--echo # binlog_row_image=MINIMAL: the views cannot be updated from the row
--echo # images and are refreshed instead
--let $refreshes = `SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_full_refreshes'`
SET SESSION binlog_row_image = MINIMAL;
UPDATE t SET x = x + 5 WHERE id IN (1, 2);
DELETE FROM o WHERE id = 16;
SET SESSION binlog_row_image = FULL;
--let $mview_check_saved = $mview_check_every
--let $mview_check_every = 0
--source suite/warp/include/mview_sync_agg_check.inc
--let $mview_check_every = $mview_check_saved
--disable_query_log
eval SELECT VARIABLE_VALUE - $refreshes > 0 AS views_were_refreshed FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_full_refreshes';
--enable_query_log
--echo # and are updated from the row images again afterwards
INSERT INTO t VALUES (5000, 'after', 1, 1.00);
INSERT INTO o VALUES (5000, 1, 1.00);
--source suite/warp/include/mview_sync_agg_check.inc

--let $mview_check_every = 0
--source suite/warp/include/mview_sync_agg_check.inc
--disable_query_log
SET GLOBAL warp_mview_pin_duty_percent = DEFAULT;
--enable_query_log

--echo # The contents, for the record
SELECT * FROM a_cnt;
SELECT * FROM a_nog;
SELECT * FROM a_join ORDER BY region;
SELECT * FROM a_left WHERE id < 100 ORDER BY id;
SELECT VARIABLE_VALUE AS stale_views FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_stale_views';

DROP MATERIALIZED VIEW a_cnt;
DROP MATERIALIZED VIEW a_nog;
DROP MATERIALIZED VIEW a_grp;
DROP MATERIALIZED VIEW a_where;
DROP MATERIALIZED VIEW a_cd;
DROP MATERIALIZED VIEW a_dist;
DROP MATERIALIZED VIEW a_join;
DROP MATERIALIZED VIEW a_left;
DROP TABLE t, c, o;

# Join views filled by several sessions (mview_populate_threads).
#   $engine   engine of the base tables and of the views
--let $topt = ENGINE=$engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
--echo # base tables and views: $engine
--disable_query_log
eval CREATE TABLE f (id BIGINT NOT NULL, d_id INT, c_id INT, amt INT, PRIMARY KEY (id)) $topt;
eval CREATE TABLE d (id INT NOT NULL, y INT, PRIMARY KEY (id)) $topt;
eval CREATE TABLE c (id INT NOT NULL, name VARCHAR(10), PRIMARY KEY (id)) $topt;
# no integer column: one session
eval CREATE TABLE s (k VARCHAR(10) NOT NULL, v VARCHAR(10), PRIMARY KEY (k)) $topt;
# no PRIMARY KEY, a nullable integer column with NULLs
eval CREATE TABLE n (x INT, amt INT) $topt;
INSERT INTO d WITH RECURSIVE r (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 50) SELECT i, 1990 + i % 9 FROM r;
INSERT INTO c VALUES (1, 'ann'), (2, 'bob'), (3, 'cy');
SET SESSION cte_max_recursion_depth = 100000;
INSERT INTO f WITH RECURSIVE r (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 20000) SELECT i * 7 - 3000, 1 + i % 60, IF(i % 5 = 0, NULL, 1 + i % 4), i % 100 FROM r;
INSERT INTO s VALUES ('a', 'x'), ('b', 'y'), ('c', NULL);
INSERT INTO n VALUES (NULL, 1), (NULL, 2), (5, 3), (5, 3), (-7, 4), (100, 5);
--enable_query_log

--let $sel_fd = SELECT f.*, d.y FROM f JOIN d ON f.d_id = d.id
--let $sel_where = SELECT f.id, f.amt, d.y FROM f JOIN d ON f.d_id = d.id WHERE d.y >= 1995 AND f.amt < 50
--let $sel_left = SELECT f.id, f.amt, c.name FROM f LEFT JOIN c ON c.id = f.c_id
--let $sel_s = SELECT s.k, s.v FROM s
--let $sel_n = SELECT n.x, n.amt FROM n

SET SESSION mview_populate_threads = 4;
--let $par0 = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_populate_parallel', Value, 1)
--let $ses0 = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_populate_sessions', Value, 1)
--disable_query_log
eval CREATE MATERIALIZED VIEW v_fd ENGINE=$engine AS $sel_fd;
eval CREATE MATERIALIZED VIEW v_where ENGINE=$engine AS $sel_where;
eval CREATE MATERIALIZED VIEW v_left ENGINE=$engine AS $sel_left;
eval CREATE MATERIALIZED VIEW v_s ENGINE=$engine AS $sel_s;
eval CREATE MATERIALIZED VIEW v_n ENGINE=$engine AS $sel_n;
--enable_query_log
--let $mv_view = v_fd
--let $mv_select = ($sel_fd)
--source suite/warp/include/mview_check.inc
--echo # The layout: an INVISIBLE AUTO_INCREMENT PRIMARY KEY, a key per table
--replace_regex / AUTO_INCREMENT=[0-9]+//
SHOW CREATE TABLE v_fd;
--replace_regex / AUTO_INCREMENT=[0-9]+//
SHOW CREATE TABLE v_left;
--let $mv_view = v_where
--let $mv_select = ($sel_where)
--source suite/warp/include/mview_check.inc
--let $mv_view = v_left
--let $mv_select = ($sel_left)
--source suite/warp/include/mview_check.inc
--let $mv_view = v_s
--let $mv_select = ($sel_s)
--source suite/warp/include/mview_check.inc
--let $mv_view = v_n
--let $mv_select = ($sel_n)
--source suite/warp/include/mview_check.inc

--echo # Filled by several sessions: v_fd, v_where, v_left and v_n (4 each);
--echo # v_s alone (no integer column)
--let $par1 = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_populate_parallel', Value, 1)
--let $ses1 = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_populate_sessions', Value, 1)
--expr $par = $par1 - $par0
--expr $ses = $ses1 - $ses0
--echo parallel: $par, sessions: $ses

--echo # REFRESH
INSERT INTO f VALUES (999999, 3, 1, 1);
REFRESH MATERIALIZED VIEW v_fd;
--let $mv_view = v_fd
--let $mv_select = ($sel_fd)
--source suite/warp/include/mview_check.inc

--echo # One session
SET SESSION mview_populate_threads = 1;
DELETE FROM f WHERE id = 999999;
REFRESH MATERIALIZED VIEW v_fd;
--let $mv_view = v_fd
--let $mv_select = ($sel_fd)
--source suite/warp/include/mview_check.inc
DROP MATERIALIZED VIEW v_left;
eval CREATE MATERIALIZED VIEW v_left ENGINE=$engine AS $sel_left;
--let $mv_view = v_left
--let $mv_select = ($sel_left)
--source suite/warp/include/mview_check.inc

--echo # An error in a session: nothing is left behind
SET SESSION mview_populate_threads = 4;
--error ER_INVALID_JSON_TEXT_IN_PARAM
eval CREATE MATERIALIZED VIEW v_err ENGINE=$engine AS
  SELECT f.id, IF(f.id = 7 * 15000 - 3000, JSON_EXTRACT('not json', '\$.a'), NULL) AS q FROM f;
SHOW TABLES LIKE 'v\_err';

--echo # KILL QUERY stops the sessions, rolls them back, drops the table
--connect (con_kill, localhost, root,,)
SET SESSION mview_populate_threads = 4;
--let $kill_id = `SELECT CONNECTION_ID()`
--disable_query_log
--send_eval CREATE MATERIALIZED VIEW v_kill ENGINE=$engine AS SELECT f.id, f.amt, d.y, d.id AS did FROM f JOIN d ON d.id > 0 JOIN c ON c.id > 0
--enable_query_log
--connection default
--let $wait_condition = SELECT VARIABLE_VALUE > $ses1 FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Mview_populate_sessions'
--source include/wait_condition.inc
--sleep 1
--disable_query_log
eval KILL QUERY $kill_id;
--enable_query_log
--connection con_kill
--error ER_QUERY_INTERRUPTED
--reap
--disconnect con_kill
--connection default
SHOW TABLES LIKE 'v\_kill';
--echo # no session is left
SELECT COUNT(*) FROM performance_schema.threads
 WHERE PROCESSLIST_INFO LIKE 'INSERT INTO `test`.`v\_kill`%';

SET SESSION mview_populate_threads = DEFAULT;
DROP MATERIALIZED VIEW v_fd, v_where, v_left, v_s, v_n;
DROP TABLE f, d, c, s, n;

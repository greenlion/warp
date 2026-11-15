# Materialized views of base tables without a PRIMARY KEY, maintained by
# the WARP_MVIEW daemon from the binary log (Mview_key_kind in sql/mview.h):
# a UNIQUE key of NOT NULL columns, every exact column (with NULLs and
# duplicate rows), no usable column at all, and aggregate views.
#
#   $view_engine   engine of the materialized views
#   $base_engine   engine of the base tables
#
# After every change the views are compared with their defining queries.

call mtr.add_suppression("WARP_MVIEW");

--let $topt = ENGINE=$base_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
--echo # views: $view_engine, base tables: $base_engine
--disable_query_log
# every column nullable, duplicate rows allowed
eval CREATE TABLE c (id INT, name VARCHAR(20), region VARCHAR(10)) $topt;
# FLOAT is not part of the row key
eval CREATE TABLE o (id INT, c_id INT, amount INT, f FLOAT) $topt;
# a UNIQUE key of NOT NULL columns
eval CREATE TABLE u (id INT NOT NULL, x INT, UNIQUE KEY (id)) $topt;
# a PRIMARY KEY, joined to a table without one
eval CREATE TABLE pkt (id INT NOT NULL, v INT, PRIMARY KEY (id)) $topt;
# no column that can be a row key
eval CREATE TABLE w (f FLOAT, g FLOAT) $topt;
--enable_query_log

INSERT INTO c VALUES (1, 'ann', 'north'), (1, 'ann', 'north'), (2, 'bob', 'south'),
                     (NULL, 'nul', NULL), (3, NULL, 'north');
INSERT INTO o VALUES (10, 1, 15, 1.5), (11, 1, 250, 2.5), (12, 2, 99, NULL),
                     (13, NULL, 5, 3), (14, 3, NULL, 4), (14, 3, NULL, 4);
INSERT INTO u VALUES (1, 1), (2, NULL), (3, 2);
INSERT INTO pkt VALUES (1, 1), (2, 2), (3, NULL);
INSERT INTO w VALUES (10, 0.5), (11, NULL);

--let $sel_inner = SELECT c.id AS cid, c.name, o.id AS oid, o.amount, o.f FROM c JOIN o ON o.c_id = c.id
--let $sel_left = SELECT c.id AS cid, c.name, c.region, o.id AS oid, o.amount FROM c LEFT JOIN o ON o.c_id = c.id
--let $sel_where = SELECT c.id AS cid, c.name, o.id AS oid, o.amount FROM c LEFT JOIN o ON o.c_id = c.id WHERE o.amount IS NULL OR o.amount < 100
--let $sel_uniq = SELECT u.id, u.x, c.name FROM u LEFT JOIN c ON c.id = u.x
--let $sel_mixed = SELECT pkt.id, pkt.v, c.name FROM pkt JOIN c ON c.id = pkt.v
--let $sel_nokey = SELECT o.id AS oid, w.g FROM o LEFT JOIN w ON w.f = o.id
--let $sel_rootnokey = SELECT w.f, w.g FROM w
--let $sel_agg = SELECT c.region, COUNT(*) AS n, SUM(o.amount) AS s, MIN(o.id) AS mn FROM c JOIN o ON o.c_id = c.id GROUP BY c.region

--echo # Create the views
--disable_query_log
eval CREATE MATERIALIZED VIEW v_inner ENGINE=$view_engine AS $sel_inner;
eval CREATE MATERIALIZED VIEW v_left ENGINE=$view_engine AS $sel_left;
eval CREATE MATERIALIZED VIEW v_where ENGINE=$view_engine AS $sel_where;
eval CREATE MATERIALIZED VIEW v_uniq ENGINE=$view_engine AS $sel_uniq;
eval CREATE MATERIALIZED VIEW v_mixed ENGINE=$view_engine AS $sel_mixed;
eval CREATE MATERIALIZED VIEW v_nokey ENGINE=$view_engine AS $sel_nokey;
eval CREATE MATERIALIZED VIEW v_rootnokey ENGINE=$view_engine AS $sel_rootnokey;
eval CREATE MATERIALIZED VIEW v_agg ENGINE=$view_engine AS $sel_agg;
--enable_query_log

--echo # The row keys: user columns that are key columns are not stored twice
SHOW CREATE TABLE v_inner;
SHOW CREATE TABLE v_uniq;

--let $mview_sync_nopk_check = suite/warp/include/mview_sync_nopk_check.inc
--source $mview_sync_nopk_check

--echo # Duplicate rows: one of two goes
DELETE FROM c WHERE id = 1 LIMIT 1;
--source $mview_sync_nopk_check
DELETE FROM o WHERE id = 14 LIMIT 1;
--source $mview_sync_nopk_check

--echo # Duplicate rows: one of two changes, then both
INSERT INTO c VALUES (1, 'ann', 'north');
--source $mview_sync_nopk_check
UPDATE c SET region = 'east' WHERE id = 1 LIMIT 1;
--source $mview_sync_nopk_check
UPDATE c SET name = 'anna' WHERE id = 1;
--source $mview_sync_nopk_check

--echo # NULLs in row keys
UPDATE c SET name = 'null id' WHERE id IS NULL;
--source $mview_sync_nopk_check
UPDATE c SET id = 2 WHERE id IS NULL;
--source $mview_sync_nopk_check
UPDATE c SET name = NULL, region = NULL WHERE name = 'bob';
--source $mview_sync_nopk_check
UPDATE o SET c_id = NULL WHERE id = 12;
--source $mview_sync_nopk_check
UPDATE o SET amount = NULL WHERE id = 10;
--source $mview_sync_nopk_check
UPDATE o SET amount = 20, c_id = 2 WHERE id = 13;
--source $mview_sync_nopk_check
INSERT INTO c VALUES (NULL, NULL, NULL), (NULL, NULL, NULL);
--source $mview_sync_nopk_check
DELETE FROM c WHERE id IS NULL AND name IS NULL LIMIT 1;
--source $mview_sync_nopk_check

--echo # A column that is not in the row key (FLOAT)
UPDATE o SET f = f + 10;
--source $mview_sync_nopk_check

--echo # The WHERE of v_where: rows that go in and out of it
UPDATE o SET amount = 500 WHERE id = 13;
--source $mview_sync_nopk_check
UPDATE o SET amount = 1 WHERE amount = 500;
--source $mview_sync_nopk_check

--echo # UNIQUE key and PRIMARY KEY tables
UPDATE u SET x = 3 WHERE id = 2;
--source $mview_sync_nopk_check
UPDATE u SET id = 7 WHERE id = 1;
--source $mview_sync_nopk_check
UPDATE pkt SET v = 3 WHERE id = 3;
--source $mview_sync_nopk_check
DELETE FROM pkt WHERE id = 1;
--source $mview_sync_nopk_check

--echo # No usable row key: the views are refreshed
UPDATE w SET g = 7 WHERE f = 11;
--source $mview_sync_nopk_check
INSERT INTO w VALUES (12, 1.25), (12, 1.25);
--source $mview_sync_nopk_check
DELETE FROM w WHERE f = 10;
--source $mview_sync_nopk_check

--echo # One transaction changes several tables
BEGIN;
INSERT INTO c VALUES (4, 'dee', 'west'), (4, 'dee', 'west');
INSERT INTO o VALUES (15, 4, 7, NULL), (16, 4, NULL, NULL);
UPDATE c SET region = 'south' WHERE id = 3;
DELETE FROM o WHERE id = 11;
UPDATE u SET x = 4 WHERE id = 3;
COMMIT;
--source $mview_sync_nopk_check

--echo # binlog_row_image=MINIMAL: the before image of a table without a
--echo # PRIMARY KEY has every column, the after image the changed ones
SET SESSION binlog_row_image = MINIMAL;
UPDATE c SET name = 'min' WHERE id = 4 LIMIT 1;
UPDATE o SET amount = amount + 1 WHERE c_id = 4;
DELETE FROM c WHERE id = 3;
INSERT INTO c VALUES (5, 'eve', NULL);
SET SESSION binlog_row_image = FULL;
--source $mview_sync_nopk_check

--echo # Everything goes
DELETE FROM o;
DELETE FROM c;
--source $mview_sync_nopk_check

DROP MATERIALIZED VIEW v_inner, v_left, v_where, v_uniq, v_mixed, v_nokey,
  v_rootnokey, v_agg;
DROP TABLE c, o, u, pkt, w;

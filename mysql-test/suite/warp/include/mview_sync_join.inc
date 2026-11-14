# Join views maintained by the WARP_MVIEW daemon from the binary log.
#
#   $view_engine   engine of the materialized views (InnoDB or WARP)
#   $base_engine   engine of the base tables (InnoDB or WARP)
#
# After every change the views are compared with their defining queries
# (mview_sync_check.inc waits for the daemon).

call mtr.add_suppression("WARP_MVIEW");
--let $mview_check_calls = 0

--let $topt = ENGINE=$base_engine DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
# WARP has no keys on DECIMAL columns
--let $k_amt = DECIMAL(6,2)
if ($base_engine == WARP)
{
  --let $k_amt = INT
}

--echo # views: $view_engine, base tables: $base_engine
--disable_query_log
eval CREATE TABLE c (id INT NOT NULL, name VARCHAR(20), region VARCHAR(10), PRIMARY KEY (id)) $topt;
eval CREATE TABLE o (id INT NOT NULL, c_id INT, amount DECIMAL(10,2), d DATE, PRIMARY KEY (id), KEY (c_id), KEY (d)) $topt;
eval CREATE TABLE i (o_id INT NOT NULL, line INT NOT NULL, sku VARCHAR(20), qty INT, PRIMARY KEY (o_id, line), KEY (sku)) $topt;
eval CREATE TABLE p (sku VARCHAR(20) NOT NULL, descr VARCHAR(40), price DECIMAL(8,2), PRIMARY KEY (sku)) $topt;
eval CREATE TABLE e (id BIGINT UNSIGNED NOT NULL, boss BIGINT UNSIGNED, name VARCHAR(20), PRIMARY KEY (id)) $topt;
eval CREATE TABLE k (d DATE NOT NULL, amt $k_amt NOT NULL, tag VARCHAR(10) NOT NULL, note VARCHAR(20), PRIMARY KEY (d, amt, tag)) $topt;
--enable_query_log

INSERT INTO c VALUES (1, 'ann', 'north'), (2, 'bob', 'south'), (3, 'cy', 'north'), (4, 'dee', NULL);
INSERT INTO o VALUES (10, 1, 15.50, '2026-01-01'), (11, 1, 250.00, '2026-01-02'), (12, 2, 99.99, '2026-01-02'), (13, NULL, 5.00, NULL);
INSERT INTO i VALUES (10, 1, 'apple', 3), (10, 2, 'pear', 1), (11, 1, 'apple', 7), (12, 1, 'it''s', 2);
INSERT INTO p VALUES ('apple', 'An apple', 0.50), ('pear', 'A pear', 0.75), ('it''s', 'quote \\ and backslash', 1.00), ('', 'empty key', 0.00);
INSERT INTO e VALUES (1, NULL, 'root'), (2, 1, 'left'), (3, 1, 'right'), (18446744073709551615, 2, 'max');
INSERT INTO k VALUES ('2026-01-01', 1.50, 'a', 'first'), ('2026-01-02', -2.25, 'b', 'second'), ('2026-01-02', 2.25, 'b', 'third');

--let $sel_inner = SELECT c.id AS cid, c.name, o.id AS oid, o.amount FROM c JOIN o ON o.c_id = c.id
--let $sel_left = SELECT c.id AS cid, c.name, o.id AS oid, o.amount FROM c LEFT JOIN o ON o.c_id = c.id
--let $sel_right = SELECT o.id AS oid, o.amount, c.name FROM c RIGHT JOIN o ON o.c_id = c.id
--let $sel_chain = SELECT c.id AS cid, o.id AS oid, i.line, i.sku, i.qty, p.descr FROM c LEFT JOIN (o LEFT JOIN i ON i.o_id = o.id) ON o.c_id = c.id LEFT JOIN p ON p.sku = i.sku
--let $sel_self = SELECT e.id, e.name, b.name AS boss_name, b.id AS boss_id FROM e LEFT JOIN e AS b ON b.id = e.boss
--let $sel_keys = SELECT k.d, k.amt, k.tag, k.note, o.id AS oid FROM k LEFT JOIN o ON o.d = k.d
--let $sel_where = SELECT c.id AS cid, o.id AS oid, o.amount FROM c LEFT JOIN o ON o.c_id = c.id WHERE o.amount IS NULL OR o.amount < 100
--let $sel_wchain = SELECT c.id AS cid, o.id AS oid, i.line, i.qty FROM c LEFT JOIN (o JOIN i ON i.o_id = o.id) ON o.c_id = c.id WHERE i.qty IS NULL OR i.qty < 5
--let $sel_wchain2 = SELECT c.id AS cid, o.id AS oid, i.line, i.qty FROM c LEFT JOIN o ON o.c_id = c.id LEFT JOIN i ON i.o_id = o.id WHERE i.qty IS NULL OR i.qty < 5
--let $sel_wright = SELECT o.id AS oid, c.id AS cid, c.region FROM c RIGHT JOIN o ON o.c_id = c.id WHERE c.region IS NULL OR c.region <> 'north'

--echo # Create the views
--disable_query_log
eval CREATE MATERIALIZED VIEW v_inner ENGINE=$view_engine AS $sel_inner;
eval CREATE MATERIALIZED VIEW v_left ENGINE=$view_engine REFRESH SYNCHRONOUS AS $sel_left;
eval CREATE MATERIALIZED VIEW v_right ENGINE=$view_engine AS $sel_right;
eval CREATE MATERIALIZED VIEW v_chain ENGINE=$view_engine AS $sel_chain;
eval CREATE MATERIALIZED VIEW v_self ENGINE=$view_engine AS $sel_self;
eval CREATE MATERIALIZED VIEW v_keys ENGINE=$view_engine AS $sel_keys;
eval CREATE MATERIALIZED VIEW v_where ENGINE=$view_engine AS $sel_where;
eval CREATE MATERIALIZED VIEW v_wchain ENGINE=$view_engine AS $sel_wchain;
eval CREATE MATERIALIZED VIEW v_wchain2 ENGINE=$view_engine AS $sel_wchain2;
eval CREATE MATERIALIZED VIEW v_wright ENGINE=$view_engine AS $sel_wright;
--enable_query_log
--source suite/warp/include/mview_sync_join_check.inc

--echo # Inner join: insert, update and delete on both sides
INSERT INTO o VALUES (14, 3, 42.00, '2026-01-03');
--source suite/warp/include/mview_sync_join_check.inc
UPDATE c SET name = 'ANN' WHERE id = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET amount = amount + 1 WHERE c_id = 1;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 14;
--source suite/warp/include/mview_sync_join_check.inc

--echo # LEFT JOIN: customer 4 gains its first order, the order is updated,
--echo # moved to another customer, and customer 4 loses its last order
INSERT INTO o VALUES (20, 4, 1.00, '2026-02-01');
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET amount = 2.00 WHERE id = 20;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET c_id = 3 WHERE id = 20;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET c_id = 4 WHERE id = 20;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 20;
--source suite/warp/include/mview_sync_join_check.inc

--echo # The WHERE of v_where: the only order of customer 3 fails the WHERE,
--echo # then passes it, then is removed
INSERT INTO o VALUES (21, 3, 500.00, '2026-02-02');
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET amount = 50.00 WHERE id = 21;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET amount = 500.00 WHERE id = 21;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 21;
--source suite/warp/include/mview_sync_join_check.inc

--echo # RIGHT JOIN: an order without customer gets one, the customer goes away
UPDATE o SET c_id = 2 WHERE id = 13;
--source suite/warp/include/mview_sync_join_check.inc
INSERT INTO c VALUES (5, 'eve', 'east');
INSERT INTO o VALUES (22, 5, 7.00, '2026-02-03');
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM c WHERE id = 5;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 22;
--source suite/warp/include/mview_sync_join_check.inc

--echo # Three tables and a product with a string key: changes at every level
INSERT INTO i VALUES (12, 2, 'kiwi', 9);
--source suite/warp/include/mview_sync_join_check.inc
INSERT INTO p VALUES ('kiwi', 'A kiwi', 2.00);
--source suite/warp/include/mview_sync_join_check.inc
UPDATE p SET descr = 'Green kiwi' WHERE sku = 'kiwi';
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET sku = 'pear' WHERE o_id = 12 AND line = 2;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET sku = '' WHERE o_id = 10 AND line = 2;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM p WHERE sku = 'it''s';
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM i WHERE o_id = 10;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 12;
--source suite/warp/include/mview_sync_join_check.inc

--echo # Self join: the boss changes its name, an employee changes its boss
UPDATE e SET name = 'ROOT' WHERE id = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE e SET boss = 3 WHERE id = 18446744073709551615;
--source suite/warp/include/mview_sync_join_check.inc
INSERT INTO e VALUES (4, 18446744073709551615, 'below max');
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM e WHERE id = 3;
--source suite/warp/include/mview_sync_join_check.inc

--echo # Primary key updates: root tables and joined tables
UPDATE c SET id = 100 WHERE id = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET id = id + 1000 WHERE c_id = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET c_id = 100 WHERE c_id = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET line = line + 10 WHERE o_id = 11;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE p SET sku = 'APPLE' WHERE sku = 'apple';
--source suite/warp/include/mview_sync_join_check.inc
UPDATE e SET id = 7 WHERE id = 2;
--source suite/warp/include/mview_sync_join_check.inc

--echo # Composite key of DATE, DECIMAL and VARCHAR
INSERT INTO k VALUES ('2026-01-01', 0.00, '', 'zero'), ('1000-01-01', -9999.99, 'min', 'low'), ('9999-12-31', 9999.99, 'max', 'high');
--source suite/warp/include/mview_sync_join_check.inc
UPDATE k SET note = 'changed' WHERE d = '2026-01-02' AND amt = -2.25;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE k SET amt = amt + 1, tag = 'moved' WHERE d = '2026-01-02';
--source suite/warp/include/mview_sync_join_check.inc
UPDATE k SET d = '2026-01-02' WHERE tag = 'min';
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET d = '9999-12-31' WHERE id = 13;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM k WHERE amt < 0;
--source suite/warp/include/mview_sync_join_check.inc

--echo # A transaction of several statements on several tables
BEGIN;
INSERT INTO c VALUES (6, 'fay', 'west'), (7, 'gus', 'west');
INSERT INTO o VALUES (30, 6, 10.00, '2026-03-01'), (31, 6, 20.00, '2026-03-01'), (32, 7, 30.00, '2026-03-02');
INSERT INTO i VALUES (30, 1, 'pear', 1), (31, 1, 'APPLE', 2), (32, 1, 'none', 3);
UPDATE c SET name = 'FAY' WHERE id = 6;
DELETE FROM o WHERE id = 31;
INSERT INTO k VALUES ('2026-03-01', 1.00, 'x', 'march');
COMMIT;
--source suite/warp/include/mview_sync_join_check.inc

--echo # A transaction that is rolled back changes nothing; the next one does
BEGIN;
INSERT INTO c VALUES (8, 'hal', 'west');
UPDATE o SET amount = 0 WHERE id = 30;
DELETE FROM i WHERE o_id = 32;
ROLLBACK;
INSERT INTO c VALUES (9, 'ivy', 'west');
--source suite/warp/include/mview_sync_join_check.inc
SELECT COUNT(*) FROM v_left WHERE cid = 8;
SELECT amount FROM v_inner WHERE oid = 30;

--echo # Views with a WHERE on the last table of a chain: rows that the WHERE
--echo # filters out are not in the view, and still change it when they go
INSERT INTO c VALUES (50, 'whe', 'north');
INSERT INTO o VALUES (40, 50, 1.00, '2026-05-01');
--source suite/warp/include/mview_sync_join_check.inc
INSERT INTO i VALUES (40, 1, 'pear', 9);
--source suite/warp/include/mview_sync_join_check.inc
--echo # the row moves into the WHERE and out of it again
UPDATE i SET qty = 2 WHERE o_id = 40;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET qty = 50 WHERE o_id = 40;
--source suite/warp/include/mview_sync_join_check.inc
--echo # the row that is filtered out is deleted
DELETE FROM i WHERE o_id = 40 AND line = 1;
--source suite/warp/include/mview_sync_join_check.inc
--echo # two filtered rows; one, then the other leaves by an update of the key
INSERT INTO i VALUES (40, 1, 'pear', 9), (40, 2, 'apple', 8);
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET o_id = 41 WHERE o_id = 40 AND line = 1;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET o_id = 41 WHERE o_id = 40 AND line = 2;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE i SET o_id = 40 WHERE o_id = 41;
--source suite/warp/include/mview_sync_join_check.inc
--echo # the order in the middle of the chain moves to another customer
UPDATE o SET c_id = 3 WHERE id = 40;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE o SET c_id = 50 WHERE id = 40;
--source suite/warp/include/mview_sync_join_check.inc
--echo # the order and its filtered items go in one transaction
BEGIN;
DELETE FROM i WHERE o_id = 40;
DELETE FROM o WHERE id = 40;
COMMIT;
--source suite/warp/include/mview_sync_join_check.inc
--echo # RIGHT JOIN with a WHERE on the NULL side
INSERT INTO o VALUES (42, 50, 2.00, '2026-05-02');
--source suite/warp/include/mview_sync_join_check.inc
UPDATE c SET region = 'west' WHERE id = 50;
--source suite/warp/include/mview_sync_join_check.inc
UPDATE c SET region = 'north' WHERE id = 50;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM c WHERE id = 50;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id = 42;
--source suite/warp/include/mview_sync_join_check.inc

--echo # Many rows in one statement and in one transaction
--disable_query_log
SET GLOBAL warp_mview_in_list_chunk_size = 40;
INSERT INTO c WITH RECURSIVE n(x) AS (SELECT 1000 UNION ALL SELECT x + 1 FROM n WHERE x < 1299) SELECT x, CONCAT('c', x), 'bulk' FROM n;
BEGIN;
INSERT INTO o WITH RECURSIVE n(x) AS (SELECT 100000 UNION ALL SELECT x + 1 FROM n WHERE x < 100499) SELECT x, 1000 + (x MOD 250), x / 100, '2026-04-01' FROM n;
UPDATE c SET region = 'BULK' WHERE id BETWEEN 1100 AND 1199;
COMMIT;
--enable_query_log
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM o WHERE id >= 100000 AND id MOD 3 = 0;
--source suite/warp/include/mview_sync_join_check.inc
DELETE FROM c WHERE id >= 1000;
DELETE FROM o WHERE id >= 100000;
--let $mview_check_every = 0
--source suite/warp/include/mview_sync_join_check.inc
SET GLOBAL warp_mview_in_list_chunk_size = DEFAULT;

--echo # The contents, for the record
SELECT * FROM v_left ORDER BY cid, oid;
SELECT * FROM v_chain ORDER BY cid, oid, line;
SELECT * FROM v_self ORDER BY id;
SELECT * FROM v_keys ORDER BY d, amt, tag, oid;

--echo # No view became stale and no statement of the daemon failed
SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_stale_views';

DROP MATERIALIZED VIEW v_inner;
DROP MATERIALIZED VIEW v_left;
DROP MATERIALIZED VIEW v_right;
DROP MATERIALIZED VIEW v_chain;
DROP MATERIALIZED VIEW v_self;
DROP MATERIALIZED VIEW v_keys;
DROP MATERIALIZED VIEW v_where;
DROP MATERIALIZED VIEW v_wchain;
DROP MATERIALIZED VIEW v_wchain2;
DROP MATERIALIZED VIEW v_wright;
DROP TABLE c, o, i, p, e, k;

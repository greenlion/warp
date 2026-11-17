# Compares the result of a query on the WARP tables ($q_w) with the same query
# on the InnoDB twins of the tables ($q_i).  $label names the check.  The WARP
# query is run twice: a block of rows at a time (warp_star_aggregation_blocks,
# the default) and row by row.
--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS r_w, r_i, r_r;
eval CREATE TABLE r_w ENGINE=InnoDB $q_w;
let $star_blocks = `SELECT @@session.warp_star_aggregation_blocks`;
SET SESSION warp_star_aggregation_blocks = OFF;
eval CREATE TABLE r_r ENGINE=InnoDB $q_w;
eval SET SESSION warp_star_aggregation_blocks = $star_blocks;
eval CREATE TABLE r_i ENGINE=InnoDB $q_i;
--enable_warnings
let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT * FROM r_w EXCEPT ALL SELECT * FROM r_i) a) + (SELECT COUNT(*) FROM (SELECT * FROM r_i EXCEPT ALL SELECT * FROM r_w) b)`;
let $diff_rows = `SELECT (SELECT COUNT(*) FROM (SELECT * FROM r_r EXCEPT ALL SELECT * FROM r_i) a) + (SELECT COUNT(*) FROM (SELECT * FROM r_i EXCEPT ALL SELECT * FROM r_r) b)`;
let $rows = `SELECT COUNT(*) FROM r_i`;
DROP TABLE r_w, r_i, r_r;
--enable_query_log
if ($diff)
{
  --echo MISMATCH of $label: $diff rows differ
}
if ($diff_rows)
{
  --echo MISMATCH of $label, row by row: $diff_rows rows differ
}
--echo $label: $rows rows

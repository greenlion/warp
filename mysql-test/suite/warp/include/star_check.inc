# Compares the result of a query on the WARP tables ($q_w) with the same query
# on the InnoDB twins of the tables ($q_i).  $label names the check.
--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS r_w, r_i;
--enable_warnings
eval CREATE TABLE r_w ENGINE=InnoDB $q_w;
eval CREATE TABLE r_i ENGINE=InnoDB $q_i;
let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT * FROM r_w EXCEPT ALL SELECT * FROM r_i) a) + (SELECT COUNT(*) FROM (SELECT * FROM r_i EXCEPT ALL SELECT * FROM r_w) b)`;
let $rows = `SELECT COUNT(*) FROM r_i`;
DROP TABLE r_w, r_i;
--enable_query_log
if ($diff)
{
  --echo MISMATCH of $label: $diff rows differ
}
--echo $label: $rows rows

# Waits until the materialized view $mview has exactly the rows of its
# defining query $mview_select (EXCEPT ALL in both directions, so that
# duplicate rows count), then prints the number of rows and of differences.
# The expected output is "<view>: N rows, 0 missing, 0 extra".
#
# Usage:
#   --let $mview = v1
#   --let $mview_select = SELECT ... FROM ...
#   [--let $mview_no_rows = 1]     do not print the number of rows
#   --source suite/warp/include/mview_sync_check.inc
#
# The daemon is polled every 10 ms for at most 60 seconds (it normally
# takes a few milliseconds; wait_condition.inc would sleep 100 ms).
--disable_query_log
--let $_mvs_diff = SELECT (SELECT COUNT(*) FROM (($mview_select) EXCEPT ALL (SELECT * FROM $mview)) mvs_a) + (SELECT COUNT(*) FROM ((SELECT * FROM $mview) EXCEPT ALL ($mview_select)) mvs_b)
--let $_mvs_tries = 500
--let $_mvs_left = `$_mvs_diff`
while ($_mvs_left)
{
  --sleep 0.01
  --dec $_mvs_tries
  --let $_mvs_left = `$_mvs_diff`
  if (!$_mvs_tries)
  {
    --let $_mvs_left = 0
  }
}
# The view was equal to the query when the loop ended.  The numbers are
# only counted again after a timeout: the daemon may be replacing rows of
# the view at this moment, and a WARP table does not show the delete and
# the insert of one transaction to other sessions at the same instant.
--let $_mvs_missing = 0
--let $_mvs_extra = 0
if (!$_mvs_tries)
{
  --let $_mvs_missing = `SELECT COUNT(*) FROM (($mview_select) EXCEPT ALL (SELECT * FROM $mview)) mvs_a`
  --let $_mvs_extra = `SELECT COUNT(*) FROM ((SELECT * FROM $mview) EXCEPT ALL ($mview_select)) mvs_b`
}
if (!$mview_no_rows)
{
  --let $_mvs_rows = `SELECT COUNT(*) FROM $mview`
  --echo $mview: $_mvs_rows rows, $_mvs_missing missing, $_mvs_extra extra
}
if ($mview_no_rows)
{
  --echo $mview: $_mvs_missing missing, $_mvs_extra extra
}
if (!$_mvs_tries)
{
  --echo # mview_sync_check.inc: $mview did not become current
  --echo # rows of the query that are missing in the view:
  eval ($mview_select) EXCEPT ALL (SELECT * FROM $mview);
  --echo # rows of the view that are not in the query:
  eval (SELECT * FROM $mview) EXCEPT ALL ($mview_select);
  SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME LIKE 'Warp_mview%' ORDER BY 1;
  --die materialized view did not become current
}
--enable_query_log

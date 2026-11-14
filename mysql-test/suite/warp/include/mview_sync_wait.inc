# Waits until the materialized view daemon (plugin WARP_MVIEW) has applied
# the binary log up to its current end: the saved position of the daemon
# (status Warp_mview_applied_binlog_file/_position) is at or after the
# position SHOW BINARY LOG STATUS reports now.
#
# Use it when a view is expected NOT to change (REFRESH MANUAL, stale
# views); to wait for a view to become current use mview_sync_check.inc,
# which waits for the contents.
#
# Usage:
#   --source suite/warp/include/mview_sync_wait.inc
--disable_query_log
--let $_mvs_file = query_get_value(SHOW BINARY LOG STATUS, File, 1)
--let $_mvs_pos = query_get_value(SHOW BINARY LOG STATUS, Position, 1)
--let $_mvs_applied_file = (SELECT SUBSTRING_INDEX(VARIABLE_VALUE, '/', -1) FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_applied_binlog_file')
--let $_mvs_applied_pos = (SELECT VARIABLE_VALUE + 0 FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_applied_binlog_position')
--let $wait_condition = SELECT $_mvs_applied_file > '$_mvs_file' OR ($_mvs_applied_file = '$_mvs_file' AND $_mvs_applied_pos >= $_mvs_pos)
--let $wait_timeout = 120
--source include/wait_condition.inc
if (!$success)
{
  --echo # mview_sync_wait.inc: the daemon did not reach $_mvs_file $_mvs_pos
  SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME LIKE 'Warp_mview%' ORDER BY 1;
}
--enable_query_log

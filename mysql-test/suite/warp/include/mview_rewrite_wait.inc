# Wait until a statement is rewritten (a SYNCHRONOUS view is used once the
# WARP_MVIEW daemon keeps it current).
#   $mvr_query  the statement
--disable_query_log
--disable_result_log
--let $_mvrw_tries = 600
while ($_mvrw_tries)
{
  --let $_mvrw_before = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_rewrite_rewritten', Value, 1)
  --eval $mvr_query
  --let $_mvrw_after = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_rewrite_rewritten', Value, 1)
  if ($_mvrw_after != $_mvrw_before)
  {
    --let $_mvrw_tries = 0
  }
  if ($_mvrw_after == $_mvrw_before)
  {
    --sleep 0.1
    --dec $_mvrw_tries
    if (!$_mvrw_tries)
    {
      --echo # mview_rewrite_wait.inc: the statement is not rewritten
      SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME LIKE 'Warp_mview%' ORDER BY 1;
    }
  }
}
--enable_result_log
--enable_query_log

# Run a statement with the MVIEW_REWRITE plugin and without it, check that
# both give the same rows, and show the rewritten statement.
#   $mvr_query  the statement
# The session must have mview_rewrite_enabled = ON.  The statement is also
# run with every view tested (no filter tree): the rewrite must be the
# same, as the filter tree only leaves out views that cannot match.

--let $mvr_tmp = $MYSQLTEST_VARDIR/tmp/mview_rewrite
--let $mvr_before = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_rewrite_rewritten', Value, 1)
--echo $mvr_query;
--disable_query_log
--disable_warnings
--sorted_result
--output $mvr_tmp.on
--eval $mvr_query
--output $mvr_tmp.on_warnings
SHOW WARNINGS;
--let $mvr_after = query_get_value(SHOW GLOBAL STATUS LIKE 'Mview_rewrite_rewritten', Value, 1)
if ($mvr_after == $mvr_before)
{
  --echo # not rewritten
}
if ($mvr_after != $mvr_before)
{
  --disable_result_log
  --eval $mvr_query
  --enable_result_log
  # the server shows the rewritten statement as the text of the statement
  SELECT SQL_TEXT AS `# rewritten` FROM performance_schema.events_statements_history
  WHERE THREAD_ID = PS_CURRENT_THREAD_ID() ORDER BY EVENT_ID DESC LIMIT 1;
}
# every view tested
SET SESSION mview_rewrite_use_filter_tree = OFF;
--sorted_result
--output $mvr_tmp.all
--eval $mvr_query
--output $mvr_tmp.all_warnings
SHOW WARNINGS;
SET SESSION mview_rewrite_use_filter_tree = ON;
# no rewrite
SET SESSION mview_rewrite_enabled = OFF;
--sorted_result
--output $mvr_tmp.off
--eval $mvr_query
SET SESSION mview_rewrite_enabled = ON;
--enable_warnings
--enable_query_log
--diff_files $mvr_tmp.on $mvr_tmp.off
--diff_files $mvr_tmp.on $mvr_tmp.all
--diff_files $mvr_tmp.on_warnings $mvr_tmp.all_warnings
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp mview_rewrite.*

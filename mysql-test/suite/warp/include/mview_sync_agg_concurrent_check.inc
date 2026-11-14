# Checks the views of mview_sync_agg_concurrent.test.  The number of rows
# depends on the interleaving of the writers, so only the differences are
# printed.
--let $mview_no_rows = 1
--let $mview = m_fast
--let $mview_select = $sel_fast
--source suite/warp/include/mview_sync_check.inc
--let $mview = m_one
--let $mview_select = $sel_one
--source suite/warp/include/mview_sync_check.inc
--let $mview = m_minmax
--let $mview_select = $sel_minmax
--source suite/warp/include/mview_sync_check.inc
--let $mview = m_join
--let $mview_select = $sel_join
--source suite/warp/include/mview_sync_check.inc
--let $mview = m_left
--let $mview_select = $sel_left
--source suite/warp/include/mview_sync_check.inc
--let $mview_no_rows = 0

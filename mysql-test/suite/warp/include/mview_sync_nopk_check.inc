# Checks all views of mview_sync_nopk.inc against their defining queries.
# The daemon has applied everything first: a change that leaves a view as
# it was may still be applied (WARP shows its delete before its insert).
--source suite/warp/include/mview_sync_wait.inc
--let $mview = v_inner
--let $mview_select = $sel_inner
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_left
--let $mview_select = $sel_left
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_where
--let $mview_select = $sel_where
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_uniq
--let $mview_select = $sel_uniq
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_mixed
--let $mview_select = $sel_mixed
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_nokey
--let $mview_select = $sel_nokey
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_rootnokey
--let $mview_select = $sel_rootnokey
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_agg
--let $mview_select = $sel_agg
--source suite/warp/include/mview_sync_check.inc

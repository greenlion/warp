# the views of mview_sync_count.test against their queries
--let $mview = mv_t
--let $mview_select = $sel_t
--source suite/warp/include/mview_sync_check.inc
--let $mview = mv_w
--let $mview_select = $sel_w
--source suite/warp/include/mview_sync_check.inc
--let $mview = mv_tw
--let $mview_select = $sel_tw
--source suite/warp/include/mview_sync_check.inc

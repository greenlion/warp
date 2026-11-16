# The views of mview_sync_timestamp.test against their queries, compared in
# UTC: a date and time of UTC is one point in time.
--disable_query_log
--let $_saved_tz = `SELECT @@session.time_zone`
SET SESSION time_zone = '+00:00';
--enable_query_log
--let $mview = v_e
--let $mview_select = $sel_e
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_k
--let $mview_select = $sel_k
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_nk
--let $mview_select = $sel_nk
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_agg
--let $mview_select = $sel_agg
--source suite/warp/include/mview_sync_check.inc
--let $mview = v_e_warp
--let $mview_select = $sel_e
--source suite/warp/include/mview_sync_check.inc
--disable_query_log
eval SET SESSION time_zone = '$_saved_tz';
--enable_query_log

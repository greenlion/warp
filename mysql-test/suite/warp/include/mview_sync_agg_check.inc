# Checks all views of mview_sync_agg.inc against their defining queries.
#
# With $mview_check_every = N only every Nth call checks (the views of WARP
# tables cost tens of milliseconds per statement of the daemon; the changes
# of the calls in between are applied together).
--inc $mview_check_calls
--let $_mvs_do = 1
if ($mview_check_every)
{
  --let $_mvs_do = `SELECT $mview_check_calls MOD $mview_check_every = 0`
}
if ($_mvs_do)
{
  --let $mview = a_cnt
  --let $mview_select = $sel_cnt
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_nog
  --let $mview_select = $sel_nog
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_grp
  --let $mview_select = $sel_grp
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_where
  --let $mview_select = $sel_where
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_cd
  --let $mview_select = $sel_cd
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_dist
  --let $mview_select = $sel_dist
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_join
  --let $mview_select = $sel_join
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = a_left
  --let $mview_select = $sel_left
  --source suite/warp/include/mview_sync_check.inc
}

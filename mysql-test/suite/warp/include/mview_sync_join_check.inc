# Checks all views of mview_sync_join.inc against their defining queries.
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
  --let $mview = v_inner
  --let $mview_select = $sel_inner
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_left
  --let $mview_select = $sel_left
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_right
  --let $mview_select = $sel_right
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_chain
  --let $mview_select = $sel_chain
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_self
  --let $mview_select = $sel_self
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_keys
  --let $mview_select = $sel_keys
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_where
  --let $mview_select = $sel_where
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_wchain
  --let $mview_select = $sel_wchain
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_wchain2
  --let $mview_select = $sel_wchain2
  --source suite/warp/include/mview_sync_check.inc
  --let $mview = v_wright
  --let $mview_select = $sel_wright
  --source suite/warp/include/mview_sync_check.inc
}

# Waits until the status variable Warp_mview_state is $mview_state.
--disable_query_log
--let $wait_condition = SELECT VARIABLE_VALUE = '$mview_state' FROM performance_schema.global_status WHERE VARIABLE_NAME = 'Warp_mview_state'
--let $wait_timeout = 60
--source include/wait_condition.inc
--enable_query_log

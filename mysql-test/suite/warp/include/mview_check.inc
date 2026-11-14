# Compare the contents of a materialized view with its defining SELECT.
#   $mv_view    the materialized view
#   $mv_select  the SELECT that defines it
--disable_query_log
# only_full_group_by does not see GROUP BY expressions inside expressions
# of a query block of a set operation
SET @mv_saved_sql_mode = @@sql_mode;
SET sql_mode = '';
--let $mv_diff = `SELECT COUNT(*) FROM ((SELECT * FROM $mv_view EXCEPT ALL $mv_select) UNION ALL ($mv_select EXCEPT ALL SELECT * FROM $mv_view)) AS mv_difference`
--let $mv_rows = `SELECT COUNT(*) FROM $mv_view`
SET sql_mode = @mv_saved_sql_mode;
--enable_query_log
--echo # $mv_view: $mv_rows rows, $mv_diff differ from the defining SELECT

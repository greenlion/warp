# WarpSQL changelog

## WARP 2.0 beta 5 (unreleased)

Changes since `v26.10.0-warp-2.0-beta4` (2026-11-14): 34 commits on `trunk`,
through `a99afb09a9b` (2026-11-17). Every change is also on the `9.7` branch.

### Highlights

- **MVIEW_REWRITE**, a new plugin that answers queries from materialized
  views, including the common table expressions, subqueries and derived
  tables of a statement.
- **RAPID**, a secondary engine that runs queries in DuckDB 2.0.0 and keeps
  its tables current from the binary log. WARP and InnoDB tables can use it.
- **Star aggregation a block of rows at a time**: the 13 queries of the Star
  Schema Benchmark at scale factor 100 take 4.04 s instead of 9.51 s.  
- On the SSB at scale factor 100 WARP is 19% faster than ClickHouse.
- **Faster loads**: 600 million rows of SSB `lineorder` load in 346 s instead
  of about 1000 s.
- **Fixes for lost rows and wrong results** (LOCK TABLES, triggers,
  REPLACE ... SELECT, pushed conditions, subqueries of engine joins).

### Upgrade notes

- **The WARP_MVIEW daemon moved to its own library.** A server that has the
  plugin installed has it from `ha_warp.so`, which no longer contains it:

  ```sql
  UNINSTALL PLUGIN warp_mview;
  INSTALL PLUGIN warp_mview SONAME 'mview_refresh.so';
  ```

  Where it is loaded by an option, use
  `--plugin-load-add=WARP_MVIEW=mview_refresh.so`. The plugin name, its
  variables (`warp_mview_*`) and status counters (`Warp_mview_*`) are
  unchanged.
- **`warp_adjust_table_stats_for_joins` is now OFF by default.** Turn it on
  per session for star schema workloads that need it.  Place PRIMARY KEYs on
  dimension tables to achieve the same result as this variable.
- **Materialized views with TIMESTAMP columns should be created again.**
  Views are now computed in UTC and marked as time zone dependent; views
  created before have no mark.
- **Join views have a new column**, `mv$row_id` (INVISIBLE AUTO_INCREMENT
  PRIMARY KEY).

### New features

#### MVIEW_REWRITE: answer queries from materialized views

- **Add MVIEW_REWRITE** (`e3133fc3578`). A post-parse query rewrite plugin,
  `mview_rewrite.so`, rewrites a SELECT to read a materialized view when the
  view can compute it. The view matching follows Goldstein and Larson,
  "Optimizing Queries Using Materialized Views" (SIGMOD 2001): equijoin, range
  and residual subsumption tests, compensating predicates, extra tables of the
  view removed through foreign keys, aggregate views read directly or rolled
  up, and a view replacing part of the tables of a query block. A filter tree
  limits the views that are tested. SYNCHRONOUS views are used while the
  daemon maintains them, and a session that has committed a change waits for
  the daemon to apply it (read your writes); MANUAL views only with
  `mview_rewrite_use_manual_views`. Variables: `mview_rewrite_enabled`,
  `_use_manual_views`, `_use_foreign_keys`, `_partial`, `_use_filter_tree`,
  `_trace`. Status: `Mview_rewrite_statements`, `_rewritten`, `_candidates`,
  `_registry_loads`, `_views`.
- **Common table expressions are rewritten** (`fd2f0c86a58`). Each common
  table expression that is one query block is matched to the views as if it
  were a statement of its own and replaced in the text of the statement; the
  rest of the statement may be anything. Recursive ones, ones with set
  operations or their own WITH clause are left alone. TPC-DS scale factor 1
  with views for the common table expressions: query 81 6.9 s to 0.4 s,
  query 30 5.3 s to 2.1 s, query 31 0.19 s to 0.02 s.
- **Subqueries and derived tables are rewritten** (`54f303e9c39`). The same
  is done for IN, EXISTS and scalar subqueries in any clause and for derived
  tables, the outer ones first. Correlated subqueries and LATERAL derived
  tables are left as they are. A view that is nearly as large as the table it
  replaces can make a query slower (TPC-DS query 65); the plugin does not
  check the size.

#### Materialized views

- **Base tables without a PRIMARY KEY** (`12d443257b5`). CREATE MATERIALIZED
  VIEW no longer requires a PRIMARY KEY on every base table. A table instance
  is maintained by its PRIMARY KEY, a UNIQUE key of NOT NULL columns, or all
  of its columns that compare exactly; key columns may be nullable. A table
  with no usable column gets a full refresh of its views on every change.
- **Join views are filled by several sessions** (`116cd29dc04`). A join view
  is filled by `mview_populate_threads` sessions (default: one per CPU, at
  most 32) over ranges of an integer column of the root table; they commit
  together or all roll back. Aggregate views are still filled by one session.
  Status: `Mview_populate_parallel`, `Mview_populate_sessions`.
- **INVISIBLE AUTO_INCREMENT PRIMARY KEY on join views** (`435e0b7cd1e`).
  Every join view gets the column `mv$row_id`; the key columns of the table
  instances keep their secondary indexes.
- **Views are computed in UTC** (`b5ac998e668`). The statements that fill a
  view and the sessions of the daemon run in UTC, so that a TIMESTAMP is one
  point in time also where daylight saving time repeats an hour. A view that
  reads a TIMESTAMP is marked, and MVIEW_REWRITE only uses it for sessions in
  UTC.
- **The daemon is a library of its own, `mview_refresh.so`** (`306caf97977`).
  It does not need the WARP storage engine: a server can have the daemon
  without WARP. See the upgrade notes.

#### RAPID secondary engine (DuckDB)

- **Add the RAPID secondary engine** (`b5e059ad476`). `ALTER TABLE ...
  SECONDARY_ENGINE=RAPID` and `SECONDARY_LOAD` run queries in DuckDB and keep
  the loaded tables current from the binary log. DuckDB 2.0.0 is downloaded
  and built with the plugin (`-DRAPID_DUCKDB_VERSION`, `-DRAPID_DUCKDB_URL`);
  its sources are not in the tree. See `storage/rapid/README.duckdb`.
- **The statement for DuckDB is written from the resolved query**
  (`061c80db88e`). Instead of the text MySQL prints for the optimized query,
  RAPID walks the resolved query and writes semijoins as EXISTS, and derived
  tables, common table expressions, set operations, ROLLUP and window
  functions as they were written; the differences between MySQL and DuckDB
  (NULL ordering, DECIMAL types of division and AVG, and others) are written
  out. A killed statement interrupts the query in DuckDB. `rapid_trace_sql`
  shows the statement. All 99 queries of TPC-DS run (20 before fixing query 
  generation); 86 have the reference result.
- **WARP tables can have a secondary engine** (`5f88b1dff35`). `ALTER TABLE t
  SECONDARY_ENGINE = RAPID` works for a WARP table and does not copy it. In
  the server, SECONDARY_LOAD no longer requires an engine with atomic DDL, and
  `SECONDARY_ENGINE = NULL` is no longer refused.
- **A hook for secondary engines after a statement is resolved**
  (`8c8463697d5`). `handlerton::secondary_engine_resolved` is called when a
  statement has been resolved and before it is optimized.

### Performance

- **Star aggregation a block of rows at a time** (`a99afb09a9b`). Fact rows
  are processed 2048 at a time: the join keys of a dimension are looked up for
  the block, the most selective dimension first, and the SUM arguments are
  computed column-wise without overflow checks when the column types rule an
  overflow out. Dimension lookups use a table indexed by the key (with a
  bitmap when few keys match) or an open addressing hash table. Partitions
  whose rows are all visible to the transaction are not checked row by row.
  New session variable `warp_star_aggregation_blocks` (default ON). SSB scale
  factor 100, 13 queries: 9.51 s to 4.04 s.
- **Index lookups of a statement are answered from memory** (`19d39512de4`).
  When a statement has made enough lookups of whole keys in an index to pay
  for a scan, the rows are read once and kept by key. New session variable
  `warp_lookup_cache_size` (256M, 0 disables); status `Warp_lookup_caches`.
- **Row ids are reserved in the state file in blocks** (`eff7139ff94`). The
  state file was written with fsync for every batch of 4096 row ids; it now
  reserves 16 million at a time. Loading SSB scale factor 100 `lineorder`
  with 16 connections takes 346 s instead of about 1000 s. Also fixes a race
  in which two connections could get the same batch.
- **Row locks are released without scanning all locks** (`92768085cad`). A
  commit walked the row locks of every open transaction under the engine
  mutex; it now releases only its own. With 10.5 million locks held, 20
  single-row commits take 0.64 s instead of 9.68 s.
- **The log of a transaction that deleted nothing is not read at commit**
  (`6b1c009ff27`). Group commit read the whole log of every transaction with a
  system call per inserted row.
- **Strings, NULL markers and row ids of a scan are read from the column
  arrays** (`692161460ad`). A scan of 2.88 million rows of TPC-DS
  `store_sales` takes 1.0 s instead of 2.4 s.
- **A division by a constant is aggregated in the engine when it is exact**
  (`910c48e5f20`). `SUM(x / 2)`, `/ 4`, `/ 100`, `/ 0.5` no longer leave the
  aggregation to MySQL. TPC-DS query 4: 16 s to 1.6 s.
- **A join of the engine that is read for distinct values returns a row once**
  (`cf9821e3079`). For an IN subquery or a materialization that removes
  duplicates, the parallel hash join returns each row of the build table
  once. TPC-DS query 14 with `semijoin=off`: 250 s to 11 s.
- **Scans are limited to the keys of derived tables** (part of
  `5a526271033`). With `warp_semi_key_pushdown`, the operands of a UNION
  derived table read by a key, and a table inner joined with a materialized
  derived table or CTE, are limited to the keys. TPC-DS query 54: 11 s to
  0.7 s.

### Optimizer statistics

- **`warp_adjust_table_stats_for_joins` is OFF by default** (`30e2327458c`).
  The option gives wrong join estimates for queries with subqueries and
  derived tables (TPC-DS query 95: more than 900 s with it, 2.4 s without).
- **The small row count is not given to a fact table of a subquery**
  (`e3fbfdd44f3`). With the option ON, an instance of the fact table in a
  semijoin or antijoin nest keeps its real row count when another instance is
  in the same query block outside of one.
- **A statement with a materialized view is left alone** (`659394ed08c`).
  With the option ON, no table is adjusted when a table of the statement is a
  materialized view. TPC-DS query 59 with a view: 29 s to 0.05 s.

### Bug fixes

- **Rows written under LOCK TABLES, by triggers and by REPLACE ... SELECT
  were lost** (`e0d37c7426f`). Rows inserted while tables were locked were
  never visible again, an INSERT after `ALTER TABLE ... DISABLE KEYS` (what
  mysqldump writes) crashed the server, REPLACE ... SELECT lost its rows, and
  rows of tables with triggers and of statements in triggers and stored
  functions were dropped when the table was closed.
- **Conditions that FastBit cannot parse are not pushed** (`5a526271033`).
  A condition whose values are columns (`c IN (d, 5)`, `c BETWEEN x.lo AND
  x.hi`) made the scan return every row: wrong results. New status
  `Warp_invalid_pushed_conditions`.
- **The subquery of a join of the engine uses the engine for its own plans**
  (`bdb4821c592`). A parallel aggregation inside such a subquery returned no
  keys, so the IN of the outer query found nothing (wrong result), and a
  parallel hash join inside it ran as a nested loop.
- **Parallel aggregation returns the columns that are not grouped**
  (`8305ab93741`). A column read above the aggregation that MySQL had removed
  from the GROUP BY (constant through a condition) was returned as NULL.
- **WARP_MVIEW: a view that reads no column of a table is maintained**
  (`282d803911b`). A `SELECT COUNT(*) FROM t` view was marked STALE and never
  used.
- **WARP_MVIEW: a statement on a table of another schema is not one on a
  base table** (`31a782f3bb4`). DDL on a table of the same name in another
  schema refreshed every view of the base table completely.
- **RAPID: a TIMESTAMP is the same point in time when loaded and through the
  binary log** (`262ce0389ee`). Rows that came through the binary log differed
  from loaded rows by the offset of the session time zone.
- **Compiler warnings of the WARP engine** (`ad13e7f353b`). Three were bugs:
  a short read at commit could set a wrong bit in the delete bitmap, the
  recovery of a sparse bitmap read the commit marker without checking the
  read, and FastBit never took the string "1" as true.

### Tests

- **Test of a WARP table with the secondary engine RAPID** (`0927eaa828f`):
  `rapid.warp_primary`.
- **`warp.mview_rewrite_cte` without the notes of EXPLAIN FORMAT=TREE**
  (`70fb7fcedf1`): the notes differ between server versions.
- **`warp.mview_sync_warp` counts its own full refreshes** (`a79c74949a9`):
  the counter is server-wide and an earlier test could change it.

### New variables and status counters

| Name | Kind | Default |
|---|---|---|
| `mview_rewrite_enabled`, `mview_rewrite_use_manual_views`, `mview_rewrite_use_foreign_keys`, `mview_rewrite_partial`, `mview_rewrite_use_filter_tree`, `mview_rewrite_trace` | variables | see `plugin/mview_rewrite/README` |
| `mview_populate_threads` | variable | 0 (one per CPU, at most 32) |
| `warp_lookup_cache_size` | session variable | 256M |
| `warp_star_aggregation_blocks` | session variable | ON |
| `rapid_trace_sql` | session variable | OFF |
| `Mview_rewrite_statements`, `_rewritten`, `_candidates`, `_registry_loads`, `_views` | status | |
| `Mview_populate_parallel`, `Mview_populate_sessions` | status | |
| `Warp_lookup_caches`, `Warp_invalid_pushed_conditions` | status | |

/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/mview.cc

  Metadata of table-backed materialized views: the data dictionary
  representation, the registry the maintenance daemon loads, and the guard
  against direct changes of the tables.  The statements are in
  sql/sql_mview.cc.
*/

#include "sql/mview.h"

#include <my_rapidjson_size_t.h>  // NOLINT(misc-include-cleaner)

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "my_sys.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_error.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/properties.h"
#include "sql/dd/string_type.h"
#include "sql/dd/types/abstract_table.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/index_element.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/table.h"
#include "sql/mdl.h"
#include "sql/sql_class.h"
#include "sql/sql_mview.h"
#include "sql/transaction.h"

const char *const MVIEW_DD_KIND_KEY = "mview";
const char *const MVIEW_DD_DEF_KEY = "mview_def";
const char *const MVIEW_INTERNAL_PREFIX = "mv$";
const char *const MVIEW_STATE_SUFFIX = "$mv_state";

std::atomic<uint64_t> mview_registry_version{1};

void mview_registry_changed() {
  mview_registry_version.fetch_add(1, std::memory_order_release);
}

namespace {
std::mutex maintained_mutex;
/// "schema\0name" -> epoch of the maintained definition.
std::map<std::string, uint64_t> maintained_views;

bool applied_known = false;
std::string applied_file;
uint64_t applied_pos = 0;

std::string maintained_key(const std::string &db, const std::string &name) {
  std::string key = db;
  key.push_back('\0');
  key += name;
  return key;
}
}  // namespace

void mview_set_maintained(const std::vector<Mview_maintained> &views) {
  std::map<std::string, uint64_t> map;
  for (const Mview_maintained &view : views)
    map[maintained_key(view.db, view.name)] = view.epoch;
  const std::lock_guard<std::mutex> guard(maintained_mutex);
  maintained_views.swap(map);
}

void mview_clear_maintained() {
  const std::lock_guard<std::mutex> guard(maintained_mutex);
  maintained_views.clear();
  applied_known = false;
}

void mview_set_applied_position(const std::string &file, uint64_t pos) {
  const std::lock_guard<std::mutex> guard(maintained_mutex);
  applied_known = true;
  applied_file = file;
  applied_pos = pos;
}

bool mview_get_applied_position(std::string *file, uint64_t *pos) {
  const std::lock_guard<std::mutex> guard(maintained_mutex);
  if (!applied_known) return false;
  *file = applied_file;
  *pos = applied_pos;
  return true;
}

bool mview_is_maintained(const std::string &db, const std::string &name,
                         uint64_t epoch) {
  const std::lock_guard<std::mutex> guard(maintained_mutex);
  const auto it = maintained_views.find(maintained_key(db, name));
  return it != maintained_views.end() && it->second == epoch;
}

void mview_set_maintenance_mode(THD *thd, bool on) {
  thd->mview_maintenance_mode = on;
}

bool mview_in_maintenance_mode(const THD *thd) {
  return thd->mview_maintenance_mode;
}

std::string mview_quote_identifier(const std::string &name) {
  std::string res;
  res.reserve(name.size() + 2);
  res.push_back('`');
  for (const char c : name) {
    if (c == '`') res.push_back('`');
    res.push_back(c);
  }
  res.push_back('`');
  return res;
}

std::string mview_refresh_statement(const Mview_def &def) {
  return "REFRESH MATERIALIZED VIEW " + mview_quote_identifier(def.db) + "." +
         mview_quote_identifier(def.name);
}

Mview_table_kind mview_kind_from_dd(const dd::Abstract_table &table) {
  if (table.type() != dd::enum_table_type::BASE_TABLE) return MVIEW_NOT_A_MVIEW;
  const dd::Properties &options = table.options();
  if (!options.exists(MVIEW_DD_KIND_KEY)) return MVIEW_NOT_A_MVIEW;
  uint32 kind = 0;
  if (options.get(MVIEW_DD_KIND_KEY, &kind)) return MVIEW_NOT_A_MVIEW;
  if (kind == MVIEW_VIEW_TABLE) return MVIEW_VIEW_TABLE;
  if (kind == MVIEW_STATE_TABLE) return MVIEW_STATE_TABLE;
  return MVIEW_NOT_A_MVIEW;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

namespace {

using Json_writer = rapidjson::Writer<rapidjson::StringBuffer>;

void put(Json_writer *w, const char *key, const std::string &value) {
  w->Key(key);
  w->String(value.data(), value.size());
}

void put(Json_writer *w, const char *key,
         const std::vector<std::string> &values) {
  w->Key(key);
  w->StartArray();
  for (const std::string &v : values) w->String(v.data(), v.size());
  w->EndArray();
}

bool get(const rapidjson::Value &obj, const char *key, std::string *value) {
  const auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsString()) return true;
  value->assign(it->value.GetString(), it->value.GetStringLength());
  return false;
}

const char *const state_kind_names[] = {"COUNT_STAR", "COUNT", "SUM",
                                        "MIN",        "MAX",   "RECOMPUTE"};

bool get(const rapidjson::Value &obj, const char *key, bool *value) {
  const auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsBool()) return true;
  *value = it->value.GetBool();
  return false;
}

bool get(const rapidjson::Value &obj, const char *key,
         std::vector<std::string> *values) {
  const auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsArray()) return true;
  values->clear();
  for (const auto &v : it->value.GetArray()) {
    if (!v.IsString()) return true;
    values->emplace_back(v.GetString(), v.GetStringLength());
  }
  return false;
}

}  // namespace

std::string mview_def_to_json(const Mview_def &def,
                              const std::vector<std::string> &user_columns) {
  rapidjson::StringBuffer buffer;
  Json_writer w(buffer);
  w.StartObject();
  w.Key("version");
  w.Uint(1);
  put(&w, "engine", def.engine);
  put(&w, "refresh",
      def.refresh_mode == Mview_refresh_mode::SYNCHRONOUS ? "SYNCHRONOUS"
                                                          : "MANUAL");
  w.Key("epoch");
  w.Uint64(def.epoch);
  put(&w, "definition", def.definition);
  put(&w, "columns", user_columns);
  w.Key("tables");
  w.StartArray();
  for (const Mview_table &t : def.tables) {
    w.StartObject();
    put(&w, "db", t.db);
    put(&w, "table", t.table);
    put(&w, "alias", t.alias);
    put(&w, "pk", t.pk_columns);
    put(&w, "key_columns", t.key_columns);
    put(&w, "ref_columns", t.ref_columns);
    w.Key("null_supplying");
    w.Bool(t.null_supplying);
    w.EndObject();
  }
  w.EndArray();
  w.Key("root");
  w.Uint(def.root);
  put(&w, "from", def.from_sql);
  put(&w, "from_template", def.from_template);
  put(&w, "where", def.where_sql);
  w.Key("aggregate");
  w.Bool(def.is_aggregate);
  if (!def.is_aggregate) {
    put(&w, "detail_columns", def.detail_columns);
    put(&w, "detail_select", def.detail_select);
  } else {
    w.Key("delta_capable");
    w.Bool(def.delta_capable);
    put(&w, "group_exprs", def.group_exprs);
    put(&w, "group_columns", def.view_group_columns);
    put(&w, "count_column", def.count_column);
    w.Key("states");
    w.StartArray();
    for (const Mview_agg_state &state : def.states) {
      w.StartObject();
      put(&w, "kind", state_kind_names[state.kind]);
      put(&w, "column", state.column);
      put(&w, "arg", state.arg_sql);
      put(&w, "full", state.full_sql);
      w.Key("count_state");
      w.Int(state.count_state);
      w.EndObject();
    }
    w.EndArray();
    put(&w, "state_table", def.state_name);
  }
  w.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

bool mview_def_from_dd(const char *db, const dd::Abstract_table &table,
                       Mview_def *def, std::vector<std::string> *user_columns,
                       std::string *reason) {
  if (mview_kind_from_dd(table) != MVIEW_VIEW_TABLE) {
    *reason = "the table is not a materialized view";
    return true;
  }
  dd::String_type json;
  if (!table.options().exists(MVIEW_DD_DEF_KEY) ||
      table.options().get(MVIEW_DD_DEF_KEY, &json)) {
    *reason = "the definition is missing";
    return true;
  }
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    *reason = "the definition is not a JSON object";
    return true;
  }

  *def = Mview_def();
  def->db = db;
  def->name = table.name().c_str();

  std::string refresh;
  std::vector<std::string> columns;
  const auto version = doc.FindMember("version");
  const auto epoch = doc.FindMember("epoch");
  const auto root = doc.FindMember("root");
  const auto tables = doc.FindMember("tables");
  if (version == doc.MemberEnd() || !version->value.IsUint() ||
      version->value.GetUint() != 1) {
    *reason = "unknown version of the definition";
    return true;
  }
  if (epoch == doc.MemberEnd() || !epoch->value.IsUint64() ||
      root == doc.MemberEnd() || !root->value.IsUint() ||
      tables == doc.MemberEnd() || !tables->value.IsArray() ||
      get(doc, "engine", &def->engine) || get(doc, "refresh", &refresh) ||
      get(doc, "definition", &def->definition) ||
      get(doc, "columns", &columns) ||
      get(doc, "from", &def->from_sql) ||
      get(doc, "from_template", &def->from_template) ||
      get(doc, "where", &def->where_sql) ||
      get(doc, "aggregate", &def->is_aggregate)) {
    *reason = "the definition is incomplete";
    return true;
  }
  def->epoch = epoch->value.GetUint64();
  def->root = root->value.GetUint();
  def->refresh_mode = refresh == "SYNCHRONOUS" ? Mview_refresh_mode::SYNCHRONOUS
                                               : Mview_refresh_mode::MANUAL;
  for (const auto &t : tables->value.GetArray()) {
    Mview_table mt;
    if (!t.IsObject() || get(t, "db", &mt.db) || get(t, "table", &mt.table) ||
        get(t, "alias", &mt.alias) || get(t, "pk", &mt.pk_columns) ||
        get(t, "key_columns", &mt.key_columns) ||
        get(t, "ref_columns", &mt.ref_columns) ||
        get(t, "null_supplying", &mt.null_supplying) ||
        mt.pk_columns.empty()) {
      *reason = "the list of tables of the definition is broken";
      return true;
    }
    def->tables.push_back(std::move(mt));
  }
  if (def->tables.empty() || def->root >= def->tables.size()) {
    *reason = "the list of tables of the definition is broken";
    return true;
  }
  if (!def->is_aggregate) {
    bool broken = get(doc, "detail_columns", &def->detail_columns) ||
                  get(doc, "detail_select", &def->detail_select);
    for (const Mview_table &mt : def->tables)
      broken = broken || mt.pk_columns.size() != mt.key_columns.size();
    if (broken) {
      *reason = "the join of the definition is incomplete";
      return true;
    }
  } else {
    const auto states = doc.FindMember("states");
    if (states == doc.MemberEnd() || !states->value.IsArray() ||
        get(doc, "delta_capable", &def->delta_capable) ||
        get(doc, "group_exprs", &def->group_exprs) ||
        get(doc, "group_columns", &def->view_group_columns) ||
        get(doc, "count_column", &def->count_column) ||
        get(doc, "state_table", &def->state_name) ||
        def->group_exprs.size() != def->view_group_columns.size()) {
      *reason = "the aggregation of the definition is incomplete";
      return true;
    }
    def->state_db = db;
    for (const auto &s : states->value.GetArray()) {
      Mview_agg_state state;
      std::string kind;
      if (!s.IsObject() || get(s, "kind", &kind) ||
          get(s, "column", &state.column) || get(s, "arg", &state.arg_sql) ||
          get(s, "full", &state.full_sql) || !s.HasMember("count_state") ||
          !s["count_state"].IsInt()) {
        *reason = "the aggregate states of the definition are broken";
        return true;
      }
      state.count_state = s["count_state"].GetInt();
      bool known = false;
      for (int k = 0; k <= Mview_agg_state::RECOMPUTE; k++) {
        if (kind == state_kind_names[k]) {
          state.kind = static_cast<Mview_agg_state::Kind>(k);
          known = true;
        }
      }
      if (!known) {
        *reason = "the aggregate states of the definition are broken";
        return true;
      }
      def->states.push_back(std::move(state));
    }
    for (const Mview_agg_state &state : def->states) {
      if (state.kind == Mview_agg_state::SUM &&
          (state.count_state < 0 ||
           static_cast<size_t>(state.count_state) >= def->states.size())) {
        *reason = "the aggregate states of the definition are broken";
        return true;
      }
    }
  }
  if (user_columns != nullptr) *user_columns = std::move(columns);
  return false;
}

/* ------------------------------------------------------------------ */
/* The write guard                                                     */
/* ------------------------------------------------------------------ */

bool mview_check_table_change(THD *thd, const char *db, const char *table_name,
                              Mview_table_kind kind) {
  if (kind == MVIEW_NOT_A_MVIEW || thd->mview_maintenance_mode) return false;
  my_error(ER_MVIEW_IS_READ_ONLY, MYF(0), db, table_name);
  return true;
}

bool mview_check_table_change(THD *thd, const char *db, const char *table_name,
                              const dd::Abstract_table *table_def) {
  if (table_def == nullptr || thd->mview_maintenance_mode) return false;
  return mview_check_table_change(thd, db, table_name,
                                  mview_kind_from_dd(*table_def));
}

bool mview_check_drop_view(THD *thd, const char *db, const char *table_name,
                           const dd::Abstract_table *table_def) {
  if (table_def == nullptr ||
      mview_kind_from_dd(*table_def) != MVIEW_VIEW_TABLE)
    return false;
  my_error(ER_MVIEW_USE_DROP_MVIEW, MYF(0), db, table_name);
  return true;
}

/* ------------------------------------------------------------------ */
/* The registry                                                        */
/* ------------------------------------------------------------------ */

namespace {

void skip_warning(const Mview_def &def, const std::string &reason) {
  const std::string msg = "Materialized view '" + def.db + "." + def.name +
                          "' is skipped: " + reason;
  LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
}

/**
  Fill in Mview_table::pk_ordinals from the current definition of the base
  table.  The ordinals are positions among the columns of a binlog
  Table_map event: all the columns of the table but those hidden by the
  storage engine and the hidden generated columns of functional indexes
  (see Table_map_log_event and ReplicatedColumnsView).

  @retval false success
  @retval true  the table is not usable any more, see reason
*/
bool fill_pk_ordinals(const dd::Table &base, Mview_table *mt,
                      std::string *reason) {
  const dd::Index *pk = nullptr;
  for (const dd::Index *index : base.indexes()) {
    if (index->type() == dd::Index::IT_PRIMARY && !index->is_hidden()) {
      pk = index;
      break;
    }
  }
  const std::string name = "'" + mt->db + "." + mt->table + "'";
  if (pk == nullptr) {
    *reason = "table " + name + " has no PRIMARY KEY any more";
    return true;
  }
  std::vector<std::string> pk_columns;
  for (const dd::Index_element *element : pk->elements()) {
    if (element->is_hidden()) continue;
    pk_columns.emplace_back(element->column().name().c_str());
  }
  bool same = pk_columns.size() == mt->pk_columns.size();
  for (size_t i = 0; same && i < pk_columns.size(); i++) {
    same = my_strcasecmp(system_charset_info, pk_columns[i].c_str(),
                         mt->pk_columns[i].c_str()) == 0;
  }
  if (!same) {
    *reason = "the PRIMARY KEY of table " + name + " has changed";
    return true;
  }
  mt->pk_ordinals.clear();
  mt->ref_ordinals.clear();
  for (int pass = 0; pass < 2; pass++) {
    const std::vector<std::string> &names =
        pass == 0 ? mt->pk_columns : mt->ref_columns;
    std::vector<unsigned> &ordinals =
        pass == 0 ? mt->pk_ordinals : mt->ref_ordinals;
    for (const std::string &wanted : names) {
      unsigned ordinal = 0;
      bool found = false;
      for (const dd::Column *column : base.columns()) {
        if (column->is_se_hidden()) continue;
        if (column->hidden() == dd::Column::enum_hidden_type::HT_HIDDEN_SQL &&
            !column->is_generation_expression_null())
          continue;
        if (my_strcasecmp(system_charset_info, column->name().c_str(),
                          wanted.c_str()) == 0) {
          found = true;
          break;
        }
        ordinal++;
      }
      if (!found) {
        *reason = "column '" + wanted + "' of table " + name + " is gone";
        return true;
      }
      ordinals.push_back(ordinal);
    }
  }
  return false;
}

/** Releases the metadata locks taken in its scope, unless they are kept. */
class Table_lock_guard {
 public:
  explicit Table_lock_guard(THD *thd)
      : m_thd(thd), m_savepoint(thd->mdl_context.mdl_savepoint()) {}
  ~Table_lock_guard() {
    if (!m_keep) m_thd->mdl_context.rollback_to_savepoint(m_savepoint);
  }
  void keep() { m_keep = true; }

 private:
  THD *m_thd;
  MDL_savepoint m_savepoint;
  bool m_keep{false};
};

}  // namespace

bool mview_fill_pk_ordinals(THD *thd, Mview_def *def, std::string *reason,
                            bool *error) {
  *error = false;
  for (Mview_table &mt : def->tables) {
    MDL_request request;
    MDL_REQUEST_INIT(&request, MDL_key::TABLE, mt.db.c_str(), mt.table.c_str(),
                     MDL_SHARED, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&request,
                                      thd->variables.lock_wait_timeout)) {
      *error = true;
      return true;
    }
    const dd::Table *base = nullptr;
    if (thd->dd_client()->acquire(mt.db.c_str(), mt.table.c_str(), &base)) {
      *error = true;
      return true;
    }
    if (base == nullptr) {
      *reason = "table '" + mt.db + "." + mt.table + "' does not exist";
      return true;
    }
    if (fill_pk_ordinals(*base, &mt, reason)) return true;
  }
  return false;
}

bool mview_load_all(THD *thd, std::vector<Mview_def> *defs) {
  defs->clear();
  bool res = true;
  {
    const dd::cache::Dictionary_client::Auto_releaser releaser(
        thd->dd_client());

    std::vector<const dd::Schema *> schemas;
    if (thd->dd_client()->fetch_global_components(&schemas)) goto end;

    for (const dd::Schema *schema : schemas) {
      /*
        The names first, then every table under a metadata lock: the
        dictionary objects of all the tables of a schema are not read at
        once without one.
      */
      std::vector<dd::String_type> names;
      if (thd->dd_client()->fetch_schema_table_names_not_hidden_by_se(schema,
                                                                      &names))
        goto end;
      for (const dd::String_type &name : names) {
        /*
          The lock on a table that is not a materialized view is released
          when its dictionary object has been (not a lock on every table of
          the server at the end).
        */
        Table_lock_guard lock_guard(thd);
        const dd::cache::Dictionary_client::Auto_releaser table_releaser(
            thd->dd_client());
        MDL_request request;
        MDL_REQUEST_INIT(&request, MDL_key::TABLE, schema->name().c_str(),
                         name.c_str(), MDL_SHARED, MDL_TRANSACTION);
        if (thd->mdl_context.acquire_lock(&request,
                                          thd->variables.lock_wait_timeout))
          goto end;
        const dd::Abstract_table *table = nullptr;
        if (thd->dd_client()->acquire(schema->name(), name, &table)) goto end;
        if (table == nullptr ||
            mview_kind_from_dd(*table) != MVIEW_VIEW_TABLE) {
          continue;
        }
        lock_guard.keep();

        Mview_def def;
        std::string reason;
        if (mview_def_from_dd(schema->name().c_str(), *table, &def, nullptr,
                              &reason)) {
          def.db = schema->name().c_str();
          def.name = name.c_str();
          skip_warning(def, reason);
          continue;
        }
        bool error = false;
        if (mview_fill_pk_ordinals(thd, &def, &reason, &error)) {
          if (error) goto end;
          skip_warning(def, reason);
          continue;
        }
        defs->push_back(std::move(def));
      }
    }
    res = false;
  }
end:
  /* Only dictionary reads were made, there is nothing to commit. */
  trans_rollback_stmt(thd);
  trans_rollback(thd);
  thd->mdl_context.release_transactional_locks();
  if (res) defs->clear();
  return res;
}

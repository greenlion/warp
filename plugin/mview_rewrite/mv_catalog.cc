/* Copyright (c) 2026, Justin Swanhart and contributors.

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
  @file plugin/mview_rewrite/mv_catalog.cc

  Table definitions for the matching, read from the data dictionary.
*/

#include "plugin/mview_rewrite/mview_rewrite.h"

#include "my_sys.h"
#include "mysql/strings/m_ctype.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/abstract_table.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/foreign_key_element.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/index_element.h"
#include "sql/dd/types/table.h"
#include "sql/mdl.h"
#include "sql/sql_class.h"
#include "sql/sql_mview.h"

namespace mv_rewrite {

int Table_info::find(const std::string &column) const {
  for (size_t i = 0; i < columns.size(); i++) {
    if (my_strcasecmp(system_charset_info, columns[i].name.c_str(),
                      column.c_str()) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

namespace {

Type_class type_class(dd::enum_column_types type) {
  switch (type) {
    case dd::enum_column_types::TINY:
    case dd::enum_column_types::SHORT:
    case dd::enum_column_types::INT24:
    case dd::enum_column_types::LONG:
    case dd::enum_column_types::LONGLONG:
      return Type_class::INT;
    case dd::enum_column_types::DECIMAL:
    case dd::enum_column_types::NEWDECIMAL:
      return Type_class::DECIMAL;
    case dd::enum_column_types::FLOAT:
    case dd::enum_column_types::DOUBLE:
      return Type_class::REAL;
    case dd::enum_column_types::VARCHAR:
    case dd::enum_column_types::VAR_STRING:
    case dd::enum_column_types::STRING:
    case dd::enum_column_types::TINY_BLOB:
    case dd::enum_column_types::MEDIUM_BLOB:
    case dd::enum_column_types::LONG_BLOB:
    case dd::enum_column_types::BLOB:
      return Type_class::STRING;
    case dd::enum_column_types::DATE:
    case dd::enum_column_types::NEWDATE:
      return Type_class::DATE;
    case dd::enum_column_types::DATETIME:
    case dd::enum_column_types::DATETIME2:
      return Type_class::DATETIME;
    case dd::enum_column_types::TIMESTAMP:
    case dd::enum_column_types::TIMESTAMP2:
      return Type_class::TIMESTAMP;
    default:
      /* TIME, YEAR, ENUM, SET, BIT, JSON, GEOMETRY, VECTOR: compared in
         ways the ranges do not model. */
      return Type_class::NONE;
  }
}

bool is_system_schema(const std::string &db) {
  return my_strcasecmp(system_charset_info, db.c_str(), "mysql") == 0 ||
         my_strcasecmp(system_charset_info, db.c_str(),
                       "information_schema") == 0 ||
         my_strcasecmp(system_charset_info, db.c_str(),
                       "performance_schema") == 0 ||
         my_strcasecmp(system_charset_info, db.c_str(), "sys") == 0;
}

void read_table(const dd::Abstract_table &abstract, Table_info *info) {
  if (abstract.type() != dd::enum_table_type::BASE_TABLE) return;
  const dd::Table &table = dynamic_cast<const dd::Table &>(abstract);
  info->engine = table.engine().c_str();
  info->mview_table = mview_kind_from_dd(abstract) != MVIEW_NOT_A_MVIEW;
  info->base_table = !is_system_schema(info->db) &&
                     table.hidden() == dd::Abstract_table::HT_VISIBLE;

  for (const dd::Column *column : table.columns()) {
    const auto hidden = column->hidden();
    if (hidden != dd::Column::enum_hidden_type::HT_VISIBLE &&
        hidden != dd::Column::enum_hidden_type::HT_HIDDEN_USER)
      continue;
    Column_info c;
    c.name = column->name().c_str();
    c.tclass = type_class(column->type());
    c.nullable = column->is_nullable();
    c.invisible = hidden == dd::Column::enum_hidden_type::HT_HIDDEN_USER;
    if (c.tclass == Type_class::STRING) {
      c.charset = get_charset(static_cast<uint>(column->collation_id()),
                              MYF(0));
      if (c.charset == nullptr) c.tclass = Type_class::NONE;
    }
    info->columns.push_back(std::move(c));
  }

  for (const dd::Index *index : table.indexes()) {
    if (index->is_hidden()) continue;
    if (index->type() != dd::Index::IT_PRIMARY &&
        index->type() != dd::Index::IT_UNIQUE)
      continue;
    std::vector<std::string> key;
    bool usable = true;
    for (const dd::Index_element *element : index->elements()) {
      if (element->is_hidden()) continue;
      if (element->is_prefix()) usable = false;
      key.emplace_back(element->column().name().c_str());
    }
    if (usable && !key.empty()) info->unique_keys.push_back(std::move(key));
  }

  for (const dd::Foreign_key *fk : table.foreign_keys()) {
    Table_info::Foreign_key key;
    key.ref_db = fk->referenced_table_schema_name().c_str();
    key.ref_table = fk->referenced_table_name().c_str();
    for (const dd::Foreign_key_element *element : fk->elements()) {
      key.columns.emplace_back(element->column().name().c_str());
      key.ref_columns.emplace_back(element->referenced_column_name().c_str());
    }
    if (!key.columns.empty()) info->foreign_keys.push_back(std::move(key));
  }
}

}  // namespace

const Table_info *Catalog::get(const std::string &db,
                               const std::string &table) {
  const auto key = std::make_pair(db, table);
  auto it = m_tables.find(key);
  if (it != m_tables.end()) return it->second.get();

  std::unique_ptr<Table_info> info;
  MDL_request request;
  MDL_REQUEST_INIT(&request, MDL_key::TABLE, db.c_str(), table.c_str(),
                   MDL_SHARED, MDL_EXPLICIT);
  bool locked;
  if (m_wait)
    locked = !m_thd->mdl_context.acquire_lock(
        &request, m_thd->variables.lock_wait_timeout);
  else
    locked = !m_thd->mdl_context.try_acquire_lock(&request) &&
             request.ticket != nullptr;
  if (locked) {
    {
      const dd::cache::Dictionary_client::Auto_releaser releaser(
          m_thd->dd_client());
      const dd::Abstract_table *abstract = nullptr;
      if (!m_thd->dd_client()->acquire(db.c_str(), table.c_str(),
                                       &abstract) &&
          abstract != nullptr) {
        info = std::make_unique<Table_info>();
        info->db = db;
        info->name = table;
        read_table(*abstract, info.get());
      }
    }
    m_thd->mdl_context.release_lock(request.ticket);
  }
  /* A failed read is not an error of the statement. */
  if (m_thd->is_error()) m_thd->clear_error();
  const Table_info *result = info.get();
  m_tables.emplace(key, std::move(info));
  return result;
}

}  // namespace mv_rewrite

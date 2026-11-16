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
  @file storage/warp/warp_mview_rows.cc

  Decodes the rows of binary log row events into SQL literals, for the
  materialized view daemon.

  The caller names the columns it wants; they are rendered for the before
  and the after image of every row, all other fields are skipped with the
  server's own knowledge of the row format (table_def::calc_field_size).
  Join views ask for the primary key columns only.  A column that is not
  in an image (binlog_row_image=MINIMAL or NOBLOB) is reported as ABSENT,
  a type without a literal as UNSUPPORTED; neither fails the decoding.

  The literals:

    integers, YEAR, BIT, ENUM, SET   decimal digits (the sign comes from the
                                     SIGNEDNESS metadata of the table map)
    DECIMAL                          decimal digits
    DOUBLE                           18 significant digits
    FLOAT                            9 significant digits; not "exact": it
                                     must not be used to find a row
    DATE, TIME, DATETIME             quoted strings
    TIMESTAMP                        FROM_UNIXTIME(seconds.microseconds)
    CHAR, VARCHAR, TEXT              _charset X'hex' (the character set
                                     comes from the table map metadata)
    BINARY, VARBINARY, BLOB,
    GEOMETRY, VECTOR                 X'hex'

  JSON (whole documents and the diffs of partial updates) and columns
  whose signedness or character set is not in the table map are
  UNSUPPORTED.
*/

#define MYSQL_SERVER 1
#define WARP_MVIEW_INTERNAL 1

#include "warp_mview.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "decimal.h"
#include "field_types.h"
#include "my_bitmap.h"
#include "my_byteorder.h"
#include "my_temporal.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysql_time.h"
#include "sql/field_common_properties.h"  // has_signedess_information_type
#include "sql/log_event.h"
#include "sql/rpl_utility.h"  // table_def

namespace warp_mview {

Table_map_info::Table_map_info() = default;

Table_map_info::~Table_map_info() { delete def; }

/* The columns that have an entry in the charset metadata of a table map:
   see is_character_type() in sql/log_event.cc.  The type is the real type,
   i.e. ENUM and SET are not strings here. */
static bool has_charset_metadata(enum_field_types real_type) {
  switch (real_type) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VECTOR:
    case MYSQL_TYPE_BLOB:
      return true;
    default:
      return false;
  }
}

bool Table_map_info::init(Table_map_log_event *ev) {
  db = ev->get_db_name();
  table = ev->get_table_name();
  column_count = ev->m_colcnt;
  if (column_count == 0 || ev->m_coltype == nullptr) return true;

  raw_type.assign(ev->m_coltype, ev->m_coltype + column_count);
  for (unsigned char t : raw_type) {
    switch (static_cast<enum_field_types>(t)) {
      case MYSQL_TYPE_TYPED_ARRAY: /* multi-valued index, layout not walked */
      case MYSQL_TYPE_INVALID:
      case MYSQL_TYPE_BOOL:
        return true;
      default:
        break;
    }
  }

  typedef mysql::binlog::event::Table_map_event::Optional_metadata_fields
      Optional_metadata;
  Optional_metadata fields(ev->m_optional_metadata,
                           ev->m_optional_metadata_len);
  const bool have_optional =
      ev->m_optional_metadata_len > 0 && fields.is_valid;

  std::vector<unsigned int> vector_dimensionality;
  if (table_def::vector_column_count(ev->m_coltype, column_count) > 0) {
    if (!have_optional) return true;
    vector_dimensionality = fields.m_vector_dimensionality;
  }
  def = new table_def(ev->m_coltype, column_count, ev->m_field_metadata,
                      static_cast<int>(ev->m_field_metadata_size),
                      ev->m_null_bits, ev->m_flags, vector_dimensionality);
  if (!def->is_valid() || def->size() != column_count) return true;

  is_unsigned.assign(column_count, -1);
  collation.assign(column_count, 0);
  json_columns = static_cast<unsigned>(def->json_column_count());

  /* SIGNEDNESS: one bit per numeric column, in column order */
  size_t sign_pos = 0;
  /* DEFAULT_CHARSET (default and exceptions by string column number) or
     COLUMN_CHARSET (one entry per string column) */
  const bool cs_default = !fields.m_default_charset.empty();
  size_t cs_pair = 0;
  size_t cs_pos = 0;
  unsigned string_column = 0;

  for (unsigned long i = 0; i < column_count; i++) {
    const enum_field_types binlog_type =
        static_cast<enum_field_types>(raw_type[i]);
    if (has_signedess_information_type(binlog_type)) {
      if (have_optional && sign_pos < fields.m_signedness.size())
        is_unsigned[i] = fields.m_signedness[sign_pos] ? 1 : 0;
      sign_pos++;
    }
    if (has_charset_metadata(def->type(i))) {
      if (have_optional) {
        if (cs_default) {
          const auto &pairs = fields.m_default_charset.charset_pairs;
          if (cs_pair < pairs.size() && pairs[cs_pair].first == string_column)
            collation[i] = pairs[cs_pair++].second;
          else
            collation[i] = fields.m_default_charset.default_charset;
        } else if (cs_pos < fields.m_column_charset.size()) {
          collation[i] = fields.m_column_charset[cs_pos];
        }
      }
      cs_pos++;
      string_column++;
    }
  }
  return false;
}

/* Rows_log_event keeps the rows in protected members.  A pointer to a
   member that is named through a derived class may be used on any object
   of the base class, so no object of this class is ever made. */
struct Rows_event_peek : public Rows_log_event {
  static const uchar *rows_begin(const Rows_log_event &e) {
    return e.*(&Rows_event_peek::m_rows_buf);
  }
  static const uchar *rows_end(const Rows_log_event &e) {
    return e.*(&Rows_event_peek::m_rows_end);
  }
};

static void append_hex(std::string *out, const uchar *data, size_t length) {
  static const char digits[] = "0123456789ABCDEF";
  out->reserve(out->size() + 2 * length);
  for (size_t i = 0; i < length; i++) {
    out->push_back(digits[data[i] >> 4]);
    out->push_back(digits[data[i] & 15]);
  }
}

/* The length in bytes of a CHAR or BINARY column, from its metadata (see
   max_display_length_for_field() in the binlog event library) */
static unsigned string_field_length(unsigned meta) {
  return (((meta >> 4) & 0x300) ^ 0x300) + (meta & 0x00ff);
}

/**
  The literal of a string value.

  @param pad_to  for BINARY(n), whose trailing 0x00 bytes are not in the
                 row image: the length of the column; 0 otherwise
*/
static bool string_literal(const uchar *data, size_t length, unsigned coll,
                           unsigned pad_to, std::string *out,
                           std::string *why) {
  if (coll == 0) {
    *why = "the character set of the column is not in the table map";
    return true;
  }
  if (coll == my_charset_bin.number) {
    out->append("X'");
    append_hex(out, data, length);
    for (size_t i = length; i < pad_to; i++) out->append("00");
    out->push_back('\'');
    return false;
  }
  const CHARSET_INFO *cs = get_charset(coll, MYF(0));
  if (cs == nullptr || cs->csname == nullptr) {
    *why = "unknown collation " + std::to_string(coll) + " of the column";
    return true;
  }
  out->push_back('_');
  out->append(cs->csname);
  out->append(" X'");
  append_hex(out, data, length);
  out->push_back('\'');
  return false;
}

static void append_fraction(std::string *out, unsigned long usec,
                            unsigned dec) {
  if (dec == 0) return;
  if (dec > 6) dec = 6;
  char buf[16];
  snprintf(buf, sizeof(buf), ".%06lu", usec % 1000000UL);
  out->append(buf, 1 + dec);
}

/**
  Append the literal of one non-NULL key field.

  @param ptr   the field in the row image
  @param size  its size, from table_def::calc_field_size_checked()
*/
static bool field_literal(const Table_map_info &map, unsigned col,
                          const uchar *ptr, uint32 size, std::string *out,
                          bool *exact, std::string *why) {
  *exact = true;
  const enum_field_types type = map.def->type(col);
  const unsigned meta = map.def->field_metadata(col);
  char buf[160];

  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      if (map.is_unsigned[col] < 0) {
        *why = "the signedness of the column is not in the table map";
        return true;
      }
      const bool uns = map.is_unsigned[col] == 1;
      if (uns) {
        ulonglong v = 0;
        switch (size) {
          case 1:
            v = ptr[0];
            break;
          case 2:
            v = uint2korr(ptr);
            break;
          case 3:
            v = uint3korr(ptr);
            break;
          case 4:
            v = uint4korr(ptr);
            break;
          case 8:
            v = uint8korr(ptr);
            break;
          default:
            *why = "unexpected integer size";
            return true;
        }
        snprintf(buf, sizeof(buf), "%llu", v);
      } else {
        longlong v = 0;
        switch (size) {
          case 1:
            v = static_cast<signed char>(ptr[0]);
            break;
          case 2:
            v = sint2korr(ptr);
            break;
          case 3:
            v = sint3korr(ptr);
            break;
          case 4:
            v = sint4korr(ptr);
            break;
          case 8:
            v = sint8korr(ptr);
            break;
          default:
            *why = "unexpected integer size";
            return true;
        }
        snprintf(buf, sizeof(buf), "%lld", v);
      }
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_YEAR: {
      const unsigned y = ptr[0];
      snprintf(buf, sizeof(buf), "%u", y == 0 ? 0U : 1900U + y);
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_NEWDECIMAL: {
      /* as in calc_field_size(): precision in the high byte */
      const int precision = static_cast<int>(meta >> 8);
      const int scale = static_cast<int>(meta & 0xff);
      if (precision <= 0 || precision > 65 || scale < 0 || scale > 30 ||
          scale > precision ||
          decimal_bin_size(precision, scale) != static_cast<int>(size)) {
        *why = "malformed DECIMAL metadata";
        return true;
      }
      decimal_digit_t digits[9];
      decimal_t dec;
      dec.len = 9;
      dec.buf = digits;
      if (bin2decimal(ptr, &dec, precision, scale) != E_DEC_OK) {
        *why = "a DECIMAL key cannot be decoded";
        return true;
      }
      int len = sizeof(buf) - 1;
      if (decimal2string(&dec, buf, &len) != E_DEC_OK) {
        *why = "a DECIMAL key cannot be printed";
        return true;
      }
      out->append(buf, len);
      return false;
    }

    case MYSQL_TYPE_DOUBLE: {
      if (size != 8) {
        *why = "unexpected DOUBLE size";
        return true;
      }
      double v = float8get(ptr);
      if (!std::isfinite(v)) {
        *why = "a DOUBLE key is not finite";
        return true;
      }
      /* 17 significant digits identify a double; the exponent form is a
         valid approximate-value literal */
      snprintf(buf, sizeof(buf), "%.17e", v);
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_FLOAT: {
      if (size != 4) {
        *why = "unexpected FLOAT size";
        return true;
      }
      const float v = float4get(ptr);
      if (!std::isfinite(v)) {
        *why = "a FLOAT value is not finite";
        return true;
      }
      snprintf(buf, sizeof(buf), "%.8e", static_cast<double>(v));
      out->append(buf);
      *exact = false; /* a FLOAT column does not compare equal to it */
      return false;
    }

    case MYSQL_TYPE_NEWDATE: {
      const uint32 v = uint3korr(ptr);
      snprintf(buf, sizeof(buf), "'%04u-%02u-%02u'", v >> 9, (v >> 5) & 15,
               v & 31);
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_TIME2: {
      if (meta > 6) {
        *why = "malformed TIME metadata";
        return true;
      }
      Time_val t;
      Time_val::load_time(ptr, meta, &t);
      snprintf(buf, sizeof(buf), "'%s%02u:%02u:%02u", t.is_negative() ? "-" : "",
               t.hour(), t.minute(), t.second());
      out->append(buf);
      append_fraction(out, t.microsecond(), meta);
      out->push_back('\'');
      return false;
    }

    case MYSQL_TYPE_DATETIME2: {
      if (meta > 6) {
        *why = "malformed DATETIME metadata";
        return true;
      }
      MYSQL_TIME t;
      TIME_from_longlong_datetime_packed(
          &t, my_datetime_packed_from_binary(ptr, meta));
      snprintf(buf, sizeof(buf), "'%04u-%02u-%02u %02u:%02u:%02u", t.year,
               t.month, t.day, t.hour, t.minute, t.second);
      out->append(buf);
      append_fraction(out, t.second_part, meta);
      out->push_back('\'');
      return false;
    }

    case MYSQL_TYPE_TIMESTAMP2: {
      if (meta > 6) {
        *why = "malformed TIMESTAMP metadata";
        return true;
      }
      my_timeval tv;
      my_timestamp_from_binary(&tv, ptr, meta);
      if (tv.m_tv_sec == 0 && tv.m_tv_usec == 0) {
        out->append("'0000-00-00 00:00:00'");
        return false;
      }
      /* The date and time of the time zone of the session, which is UTC
         in the sessions of the daemon: every date and time is one point
         in time there.  A decimal argument keeps the microseconds. */
      snprintf(buf, sizeof(buf), "FROM_UNIXTIME(%lld.%06lld)",
               static_cast<long long>(tv.m_tv_sec),
               static_cast<long long>(tv.m_tv_usec));
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_BIT: {
      if (size == 0 || size > 8) {
        *why = "unexpected BIT size";
        return true;
      }
      ulonglong v = 0;
      for (uint32 i = 0; i < size; i++) v = (v << 8) | ptr[i]; /* big endian */
      snprintf(buf, sizeof(buf), "%llu", v);
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      if (size == 0 || size > 8) {
        *why = "unexpected ENUM or SET size";
        return true;
      }
      ulonglong v = 0;
      for (uint32 i = 0; i < size; i++)
        v |= static_cast<ulonglong>(ptr[i]) << (8 * i); /* little endian */
      snprintf(buf, sizeof(buf), "%llu", v);
      out->append(buf);
      return false;
    }

    case MYSQL_TYPE_STRING: {
      const unsigned field_length = string_field_length(meta);
      const unsigned prefix = field_length > 255 ? 2 : 1;
      if (size < prefix) {
        *why = "truncated string";
        return true;
      }
      const size_t length = prefix == 1 ? ptr[0] : uint2korr(ptr);
      if (length != size - prefix) {
        *why = "inconsistent string length";
        return true;
      }
      return string_literal(ptr + prefix, length, map.collation[col],
                            field_length, out, why);
    }

    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      const unsigned prefix = meta > 255 ? 2 : 1;
      if (size < prefix) {
        *why = "truncated string";
        return true;
      }
      const size_t length = prefix == 1 ? ptr[0] : uint2korr(ptr);
      if (length != size - prefix) {
        *why = "inconsistent string length";
        return true;
      }
      return string_literal(ptr + prefix, length, map.collation[col], 0, out,
                            why);
    }

    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB: {
      /* the metadata is the number of length bytes, 1 to 4 */
      if (meta < 1 || meta > 4 || size < meta) {
        *why = "malformed BLOB metadata";
        return true;
      }
      return string_literal(ptr + meta, size - meta, map.collation[col], 0,
                            out, why);
    }

    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_VECTOR: {
      if (meta < 1 || meta > 4 || size < meta) {
        *why = "malformed GEOMETRY or VECTOR metadata";
        return true;
      }
      out->append("X'");
      append_hex(out, ptr + meta, size - meta);
      out->push_back('\'');
      return false;
    }

    default:
      *why = "a column of type " + std::to_string(static_cast<int>(type)) +
             " has no exact literal";
      return true;
  }
}

/* A length-encoded integer (net_store_length) */
static bool read_packed_int(const uchar **ptr, const uchar *end,
                            ulonglong *value) {
  if (*ptr >= end) return true;
  const uchar first = **ptr;
  size_t bytes;
  if (first < 251) {
    *value = first;
    (*ptr)++;
    return false;
  } else if (first == 252) {
    bytes = 2;
  } else if (first == 253) {
    bytes = 3;
  } else if (first == 254) {
    bytes = 8;
  } else {
    return true;
  }
  if (static_cast<size_t>(end - *ptr) < 1 + bytes) return true;
  ulonglong v = 0;
  for (size_t i = 0; i < bytes; i++)
    v |= static_cast<ulonglong>((*ptr)[1 + i]) << (8 * i);
  *value = v;
  *ptr += 1 + bytes;
  return false;
}

/**
  Walk one row image (see pack_row() in sql/rpl_record.cc):

    [value_options [partial_bits]]   after image of a PARTIAL_UPDATE only
    null_bits                        one bit per column of the image
    field ...                        the columns that are not NULL

  @param columns     the wanted columns
  @param[out] cells  one cell per wanted column
  @return the end of the image, nullptr when it cannot be walked
*/
static const uchar *walk_image(const Table_map_info &map, const uchar *ptr,
                               const uchar *end, const MY_BITMAP *cols,
                               bool has_value_options,
                               const std::vector<unsigned> &columns,
                               std::vector<Cell> *cells, std::string *why) {
  cells->assign(columns.size(), Cell());

  if (has_value_options) {
    ulonglong value_options = 0;
    if (read_packed_int(&ptr, end, &value_options) || value_options > 1) {
      *why = "unknown value options in a partial update";
      return nullptr;
    }
    if (value_options & 1) { /* PARTIAL_JSON_UPDATES: one bit per JSON column */
      const size_t bytes = (map.json_columns + 7) / 8;
      if (static_cast<size_t>(end - ptr) < bytes) {
        *why = "truncated partial bits";
        return nullptr;
      }
      ptr += bytes;
    }
  }

  unsigned image_columns = 0;
  for (unsigned long i = 0; i < map.column_count; i++)
    if (bitmap_is_set(cols, i)) image_columns++;
  const size_t null_bytes = (image_columns + 7) / 8;
  if (static_cast<size_t>(end - ptr) < null_bytes) {
    *why = "truncated null bits";
    return nullptr;
  }
  const uchar *null_bits = ptr;
  ptr += null_bytes;

  unsigned image_pos = 0;
  for (unsigned long i = 0; i < map.column_count; i++) {
    if (!bitmap_is_set(cols, i)) continue;
    const bool is_null = (null_bits[image_pos / 8] >> (image_pos % 8)) & 1;
    image_pos++;

    uint32 size = 0;
    if (!is_null) {
      /* A JSON column of a partial update holds a diff, behind the same
         length bytes as a whole document, so its size is found the same
         way. */
      if (map.def->calc_field_size_checked(static_cast<uint>(i), ptr,
                                           static_cast<size_t>(end - ptr),
                                           &size) != CALC_FIELD_SIZE_OK ||
          static_cast<size_t>(end - ptr) < size) {
        *why = "a field does not fit in the row event";
        return nullptr;
      }
    }
    for (size_t k = 0; k < columns.size(); k++) {
      if (columns[k] != i) continue;
      Cell &cell = (*cells)[k];
      if (is_null) {
        cell.state = Cell_state::SQL_NULL;
        cell.literal = "NULL";
      } else {
        std::string reason;
        if (field_literal(map, static_cast<unsigned>(i), ptr, size,
                          &cell.literal, &cell.exact, &reason)) {
          cell.state = Cell_state::UNSUPPORTED;
          cell.literal = reason;
        } else {
          cell.state = Cell_state::VALUE;
        }
      }
    }
    ptr += size;
  }
  return ptr;
}

bool decode_rows(const Table_map_info &map, Rows_log_event *ev,
                 const std::vector<unsigned> &columns,
                 std::vector<Row_change> *rows, std::string *why) {
  using namespace mysql::binlog::event;

  if (map.def == nullptr) {
    *why = "the table map cannot be decoded";
    return true;
  }
  for (unsigned ord : columns) {
    if (ord >= map.column_count) {
      *why = "the table map has fewer columns than the table definition";
      return true;
    }
  }
  if (ev->get_width() != map.column_count) {
    *why = "the row event and the table map disagree on the column count";
    return true;
  }

  const Log_event_type type = ev->get_type_code();
  const bool is_update =
      type == UPDATE_ROWS_EVENT || type == PARTIAL_UPDATE_ROWS_EVENT;
  const bool is_partial = type == PARTIAL_UPDATE_ROWS_EVENT;
  const bool is_write = type == WRITE_ROWS_EVENT;
  if (!is_update && !is_write && type != DELETE_ROWS_EVENT) {
    *why = "unknown row event type";
    return true;
  }

  const MY_BITMAP *cols = ev->get_cols();
  const MY_BITMAP *cols_ai = ev->get_cols_ai();
  if (cols->n_bits != map.column_count ||
      (is_update && cols_ai->n_bits != map.column_count)) {
    *why = "the column bitmaps of the row event have the wrong width";
    return true;
  }

  const uchar *ptr = Rows_event_peek::rows_begin(*ev);
  const uchar *end = Rows_event_peek::rows_end(*ev);
  if (ptr == nullptr || end == nullptr || end < ptr) return false; /* no rows */

  while (ptr < end) {
    Row_change row;
    if (is_write) {
      row.kind = Row_change::INSERT;
      ptr = walk_image(map, ptr, end, cols, false, columns, &row.after, why);
    } else {
      row.kind = is_update ? Row_change::UPDATE : Row_change::DELETE;
      ptr = walk_image(map, ptr, end, cols, false, columns, &row.before, why);
      if (ptr != nullptr && is_update)
        ptr = walk_image(map, ptr, end, cols_ai, is_partial, columns,
                         &row.after, why);
    }
    if (ptr == nullptr) return true;
    rows->push_back(std::move(row));
  }
  return false;
}

}  // namespace warp_mview

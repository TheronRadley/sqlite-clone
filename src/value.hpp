// value.hpp — the runtime value type: NULL, INTEGER (i64), TEXT.
//
// NULL semantics (documented in ARCHITECTURE.md):
//   * Comparisons involving NULL yield NULL (three-valued logic, see expr.cpp).
//   * Ordering across types follows SQLite's default: NULL < INTEGER < TEXT.
//   * NULL is *not* equal to NULL at the SQL level; Value::compare() is only
//     used for index/order semantics where a total order is required.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "error.hpp"

namespace sc {

enum class ColType { Integer, Text };

inline const char* col_type_name(ColType t) {
    return t == ColType::Integer ? "INTEGER" : "TEXT";
}

struct Value {
    // monostate = NULL, int64_t = INTEGER, std::string = TEXT
    std::variant<std::monostate, int64_t, std::string> v;

    Value() = default;
    static Value null() { return Value{}; }
    static Value integer(int64_t i) { Value x; x.v = i; return x; }
    static Value text(std::string s) { Value x; x.v = std::move(s); return x; }

    bool is_null() const { return std::holds_alternative<std::monostate>(v); }
    bool is_int() const { return std::holds_alternative<int64_t>(v); }
    bool is_text() const { return std::holds_alternative<std::string>(v); }

    int64_t as_int() const { return std::get<int64_t>(v); }
    const std::string& as_text() const { return std::get<std::string>(v); }

    const char* type_name() const {
        if (is_null()) return "NULL";
        if (is_int()) return "INTEGER";
        return "TEXT";
    }

    // Total order used by indexes and ORDER BY: NULL < INTEGER < TEXT,
    // integers numerically, text by bytes (shorter prefix sorts first).
    // Returns -1 / 0 / +1.
    int compare(const Value& o) const {
        auto rank = [](const Value& x) { return x.is_null() ? 0 : (x.is_int() ? 1 : 2); };
        int ra = rank(*this), rb = rank(o);
        if (ra != rb) return ra < rb ? -1 : 1;
        if (ra == 0) return 0;
        if (ra == 1) {
            int64_t a = as_int(), b = o.as_int();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        const std::string &a = as_text(), &b = o.as_text();
        int c = a.compare(b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    // Structural equality (NULL == NULL here). SQL-level NULL handling lives
    // in the expression evaluator, which never calls this for SQL `=`.
    bool same_as(const Value& o) const { return compare(o) == 0; }

    // Human display (CLI): NULL renders as empty string.
    std::string display() const {
        if (is_null()) return "";
        if (is_int()) return std::to_string(as_int());
        return as_text();
    }

    // SQL literal rendering (used by .dump and index error messages).
    std::string sql_literal() const {
        if (is_null()) return "NULL";
        if (is_int()) return std::to_string(as_int());
        std::string out = "'";
        for (char c : as_text()) {
            out.push_back(c);
            if (c == '\'') out.push_back('\'');
        }
        out.push_back('\'');
        return out;
    }

    // Type affinity coercion (SQLite-like, documented in ARCHITECTURE.md):
    //   INTEGER column: TEXT that is a well-formed integer is accepted as INTEGER.
    //   TEXT column:    INTEGER is rendered as decimal TEXT.
    //   NULL stays NULL. Anything else is a TypeError.
    Value apply_affinity(ColType col, const std::string& column_name) const;
};

// Parse a strict decimal integer (optionally signed), as accepted by affinity.
std::optional<int64_t> parse_i64(const std::string& s);

} // namespace sc

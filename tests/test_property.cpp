// test_property.cpp — randomized differential testing of the SQL layer
// against an independent in-memory reference model (std::map).
//
// The model implements the same semantics from scratch: SQL three-valued
// logic for predicates (comparisons with NULL never match), structural
// ordering for ORDER BY (NULL < INTEGER < TEXT, ties broken by rowid via
// stable sort), and auto-assignment of rowids. Every SELECT is executed
// by both the engine and the model and the results compared; the store is
// validated periodically and the final state is checked to survive a
// close/reopen cycle.
#include "test_util.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>

#include "database.hpp"
#include "pager.hpp"

using namespace sc;

namespace {

// ---- the reference model ----

struct Row {
    Value a;   // INTEGER or NULL
    Value b;   // TEXT or NULL
};

struct Model {
    std::map<int64_t, Row> rows;   // ordered by rowid

    int64_t next_auto() const { return rows.empty() ? 1 : rows.rbegin()->first + 1; }
};

// ---- predicates ----

struct Pred {
    enum class Kind { True, Id, A, B, ANull, BNull } kind = Kind::True;
    BinOp op = BinOp::Eq;
    bool is_not = false;   // for the *Null kinds: IS NULL vs IS NOT NULL
    Value lit;
};

// SQL comparison: NULL operands never match
bool matches(const Value& v, BinOp op, const Value& lit) {
    if (v.is_null() || lit.is_null()) return false;
    int c = v.compare(lit);
    switch (op) {
        case BinOp::Eq: return c == 0;
        case BinOp::Ne: return c != 0;
        case BinOp::Lt: return c < 0;
        case BinOp::Le: return c <= 0;
        case BinOp::Gt: return c > 0;
        case BinOp::Ge: return c >= 0;
        default: return false;
    }
}

bool eval_term(const Pred& p, int64_t id, const Row& r) {
    switch (p.kind) {
        case Pred::Kind::True: return true;
        case Pred::Kind::Id: return matches(Value::integer(id), p.op, p.lit);
        case Pred::Kind::A: return matches(r.a, p.op, p.lit);
        case Pred::Kind::B: return matches(r.b, p.op, p.lit);
        case Pred::Kind::ANull: return p.is_not ? !r.a.is_null() : r.a.is_null();
        case Pred::Kind::BNull: return p.is_not ? !r.b.is_null() : r.b.is_null();
    }
    return false;
}

struct Where {
    Pred lhs, rhs;
    bool is_or = false;
    bool has_rhs = false;
};

bool eval_where(const Where& w, int64_t id, const Row& r) {
    bool l = eval_term(w.lhs, id, r);
    if (!w.has_rhs) return l;
    bool rr = eval_term(w.rhs, id, r);
    return w.is_or ? (l || rr) : (l && rr);
}

// render one predicate as SQL; "1 = 1" for the always-true term (kept
// explicit so compound predicates stay faithful: "x OR TRUE" must not
// silently simplify to "x")
std::string term_sql(const Pred& p) {
    if (p.kind == Pred::Kind::True) return "1 = 1";
    if (p.kind == Pred::Kind::ANull)
        return std::string("a IS") + (p.is_not ? " NOT" : "") + " NULL";
    if (p.kind == Pred::Kind::BNull)
        return std::string("b IS") + (p.is_not ? " NOT" : "") + " NULL";
    const char* col = p.kind == Pred::Kind::Id ? "id" : p.kind == Pred::Kind::A ? "a" : "b";
    const char* o = "=";
    switch (p.op) {
        case BinOp::Eq: o = "="; break;
        case BinOp::Ne: o = "!="; break;
        case BinOp::Lt: o = "<"; break;
        case BinOp::Le: o = "<="; break;
        case BinOp::Gt: o = ">"; break;
        case BinOp::Ge: o = ">="; break;
        default: break;
    }
    return std::string(col) + " " + o + " " + p.lit.sql_literal();
}

// full WHERE clause; always non-empty
std::string where_sql(const Where& w) {
    std::string l = term_sql(w.lhs);
    if (!w.has_rhs) return l;
    return l + (w.is_or ? " OR " : " AND ") + term_sql(w.rhs);
}

Pred random_term(uint64_t& s) {
    Pred p;
    switch (test::rand_below(s, 8)) {
        case 0: p.kind = Pred::Kind::Id; break;
        case 1: case 2: p.kind = Pred::Kind::A; break;
        case 3: p.kind = Pred::Kind::B; break;
        case 4: p.kind = Pred::Kind::ANull; break;
        case 5: p.kind = Pred::Kind::BNull; break;
        default: p.kind = Pred::Kind::True; break;
    }
    if (p.kind == Pred::Kind::ANull || p.kind == Pred::Kind::BNull) {
        p.is_not = test::rand_below(s, 2) == 1;
        return p;
    }
    if (p.kind == Pred::Kind::True) return p;
    switch (" <=>!"[test::rand_below(s, 4)]) {
        case '=': p.op = BinOp::Eq; break;
        case '<': p.op = BinOp::Lt; break;
        case '>': p.op = BinOp::Gt; break;
        case '!': p.op = BinOp::Ne; break;
    }
    if (p.kind == Pred::Kind::Id)
        p.lit = Value::integer(int64_t(test::rand_below(s, 40)));
    else if (p.kind == Pred::Kind::A)
        p.lit = Value::integer(int64_t(test::rand_below(s, 30)) - 10);
    else
        p.lit = Value::text("t" + std::to_string(test::rand_below(s, 8)));
    return p;
}

Where random_where(uint64_t& s) {
    Where w;
    w.lhs = random_term(s);
    if (test::rand_below(s, 3) == 0) {
        w.has_rhs = true;
        w.is_or = test::rand_below(s, 2) == 1;
        w.rhs = random_term(s);
    }
    return w;
}

// ---- result comparison ----

std::string render_rows(const std::vector<std::pair<int64_t, Row>>& rows) {
    std::string out;
    for (const auto& [id, r] : rows)
        out += std::to_string(id) + "|" + r.a.display() + "|" + r.b.display() + "\n";
    return out;
}

std::string engine_rows(Database& db, const std::string& sql) {
    auto rs = db.execute(sql);
    if (rs.size() != 1 || !rs[0].is_rows)
        throw ::test::TestFailure(sc::str("bad result shape for: ", sql));
    std::string got;
    for (const auto& row : rs[0].rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) got += "|";
            got += row[i].display();
        }
        got += "\n";
    }
    return got;
}

void compare_select(Database& db, const Model& model, const Where& w,
                    const std::string& order_col, bool desc, int limit) {
    // model side
    std::vector<std::pair<int64_t, Row>> hit;
    for (const auto& [id, r] : model.rows)
        if (eval_where(w, id, r)) hit.emplace_back(id, r);
    if (!order_col.empty()) {
        std::stable_sort(hit.begin(), hit.end(), [&](const auto& x, const auto& y) {
            Value vx = order_col == "id" ? Value::integer(x.first)
                       : order_col == "a" ? x.second.a
                                          : x.second.b;
            Value vy = order_col == "id" ? Value::integer(y.first)
                       : order_col == "a" ? y.second.a
                                          : y.second.b;
            int c = vx.compare(vy);
            return desc ? c > 0 : c < 0;
        });
    }
    if (limit >= 0 && hit.size() > size_t(limit)) hit.resize(size_t(limit));

    std::string sql = "SELECT id, a, b FROM p";
    std::string ws = where_sql(w);
    if (!ws.empty()) sql += " WHERE " + ws;
    if (!order_col.empty()) sql += " ORDER BY " + order_col + (desc ? " DESC" : "");
    if (limit >= 0) sql += " LIMIT " + std::to_string(limit);

    std::string got = engine_rows(*&db, sql);
    std::string want = render_rows(hit);
    if (got != want)
        throw ::test::TestFailure(sc::str(__FILE__, ":", __LINE__,
                                          "\n  sql:    ", sql,
                                          "\n  engine: ", got.size(), " row(s)\n", got,
                                          "  model:  ", want.size(), " row(s)\n", want));
}

} // namespace

TEST(property_differential_vs_reference_model) {
    const int seeds = 8;
    const int ops_per_seed = 120;
    for (int seed = 1; seed <= seeds; ++seed) {
        uint64_t s = uint64_t(seed) * 0x9e3779b97f4a7c15ull;
        std::string path = test::temp_db_path();
        Model model;
        {
            auto db = Database::open(path);
            (void)db->execute("CREATE TABLE p (id INTEGER PRIMARY KEY, a INTEGER, "
                              "b TEXT)");
            bool index_a = false, index_b = false;

            for (int op = 0; op < ops_per_seed; ++op) {
                uint64_t r = test::rand_below(s, 100);
                if (r < 40) {
                    // ---- INSERT ----
                    bool explicit_id = test::rand_below(s, 4) != 0;
                    int64_t id = explicit_id ? int64_t(test::rand_below(s, 60)) + 1
                                             : model.next_auto();
                    Row row;
                    row.a = test::rand_below(s, 4)
                                ? Value::null()
                                : Value::integer(int64_t(test::rand_below(s, 30)) - 10);
                    row.b = test::rand_below(s, 4)
                                ? Value::null()
                                : Value::text("t" + std::to_string(test::rand_below(s, 8)));
                    std::string sql =
                        "INSERT INTO p VALUES (" +
                        (explicit_id ? std::to_string(id) : std::string("NULL")) + ", " +
                        row.a.sql_literal() + ", " + row.b.sql_literal() + ")";
                    if (model.rows.count(id)) {
                        CHECK_DB_ERROR(db->execute(sql), int(Err::Constraint),
                                       "already contains value");
                    } else {
                        (void)db->execute(sql);
                        model.rows[id] = row;
                    }
                } else if (r < 60) {
                    // ---- DELETE ----
                    Where w = random_where(s);
                    std::string sql = "DELETE FROM p";
                    std::string ws = where_sql(w);
                    if (!ws.empty()) sql += " WHERE " + ws;
                    (void)db->execute(sql);
                    for (auto it = model.rows.begin(); it != model.rows.end();)
                        if (eval_where(w, it->first, it->second))
                            it = model.rows.erase(it);
                        else
                            ++it;
                } else if (r < 78) {
                    // ---- UPDATE (one column, literal or NULL) ----
                    Where w = random_where(s);
                    bool set_a = test::rand_below(s, 2) == 0;
                    Value nv = test::rand_below(s, 4) == 0
                                   ? Value::null()
                                   : (set_a
                                          ? Value::integer(int64_t(test::rand_below(s, 30)) - 10)
                                          : Value::text("t" + std::to_string(test::rand_below(s, 8))));
                    std::string sql = "UPDATE p SET " + std::string(set_a ? "a" : "b") +
                                      " = " + nv.sql_literal();
                    std::string ws = where_sql(w);
                    if (!ws.empty()) sql += " WHERE " + ws;
                    (void)db->execute(sql);
                    for (auto& [id, row] : model.rows)
                        if (eval_where(w, id, row)) {
                            if (set_a) row.a = nv;
                            else row.b = nv;
                        }
                } else if (r < 82) {
                    // ---- indexes appear mid-run ----
                    if (!index_a && test::rand_below(s, 2)) {
                        (void)db->execute("CREATE INDEX p_a ON p(a)");
                        index_a = true;
                    } else if (!index_b && test::rand_below(s, 2)) {
                        (void)db->execute("CREATE INDEX p_b ON p(b)");
                        index_b = true;
                    }
                } else {
                    // ---- SELECT (differential) ----
                    Where w = random_where(s);
                    std::string order_col;
                    switch (test::rand_below(s, 4)) {
                        case 0: order_col = "id"; break;
                        case 1: order_col = "a"; break;
                        case 2: order_col = "b"; break;
                        default: break;
                    }
                    bool desc = test::rand_below(s, 2) == 1;
                    int limit = -1;
                    if (test::rand_below(s, 3) == 0) limit = int(test::rand_below(s, 10));
                    compare_select(*db, model, w, order_col, desc, limit);
                }

                if (op % 15 == 14) CHECK_NO_ERROR(db->validate(false));
            }

            // final full comparison + deep validation
            compare_select(*db, model, Where{}, "", false, -1);
            CHECK_NO_ERROR(db->validate(true));
        }
        // reopen: committed state survives
        {
            auto db = Database::open(path);
            compare_select(*db, model, Where{}, "", false, -1);
            CHECK_NO_ERROR(db->validate(true));
        }
        std::remove(path.c_str());
    }
}

TEST(property_count_agreement) {
    // COUNT(*) and COUNT(col) must agree with the model under every
    // predicate shape
    for (int seed = 20; seed < 24; ++seed) {
        uint64_t s = uint64_t(seed) * 0x9e3779b97f4a7c15ull;
        auto db = Database::open_memory();
        Model model;
        (void)db->execute("CREATE TABLE p (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");
        for (int i = 0; i < 60; ++i) {
            Row row;
            row.a = test::rand_below(s, 3)
                        ? Value::integer(int64_t(test::rand_below(s, 20)))
                        : Value::null();
            row.b = test::rand_below(s, 3)
                        ? Value::text("t" + std::to_string(test::rand_below(s, 5)))
                        : Value::null();
            (void)db->execute("INSERT INTO p VALUES (" + std::to_string(i + 1) + ", " +
                              row.a.sql_literal() + ", " + row.b.sql_literal() + ")");
            model.rows[int64_t(i + 1)] = row;
        }
        for (int q = 0; q < 25; ++q) {
            Where w = random_where(s);
            size_t total = 0, non_null_a = 0;
            for (const auto& [id, r] : model.rows)
                if (eval_where(w, id, r)) {
                    ++total;
                    if (!r.a.is_null()) ++non_null_a;
                }
            std::string ws = where_sql(w);
            std::string sql = "SELECT COUNT(*), COUNT(a) FROM p";
            if (!ws.empty()) sql += " WHERE " + ws;
            std::string got = engine_rows(*db, sql);
            std::string want =
                std::to_string(total) + "|" + std::to_string(non_null_a) + "\n";
            CHECK_EQ(got, want);
        }
    }
}

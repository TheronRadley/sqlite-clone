#include "parser.hpp"

namespace sc {

const char* binop_name(BinOp op) {
    switch (op) {
        case BinOp::And: return "AND";  case BinOp::Or: return "OR";
        case BinOp::Eq: return "=";     case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";     case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";     case BinOp::Ge: return ">=";
        case BinOp::Add: return "+";    case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";    case BinOp::Div: return "/";
        case BinOp::Mod: return "%";
    }
    return "?";
}

std::string Expr::describe() const {
    switch (kind) {
        case Kind::Literal: return lit.sql_literal();
        case Kind::Column: return column;
        case Kind::Binary:
            return "(" + lhs->describe() + " " + binop_name(bop) + " " + rhs->describe() + ")";
        case Kind::Unary:
            switch (uop) {
                case UnOp::Not: return "(NOT " + child->describe() + ")";
                case UnOp::Neg: return "(-" + child->describe() + ")";
                case UnOp::IsNull: return "(" + child->describe() + " IS NULL)";
                case UnOp::IsNotNull: return "(" + child->describe() + " IS NOT NULL)";
            }
            return "?";
        case Kind::Agg: {
            const char* fn = agg == AggFn::Count ? "COUNT" : agg == AggFn::Min ? "MIN"
                           : agg == AggFn::Max ? "MAX" : "SUM";
            return star ? std::string(fn) + "(*)"
                        : std::string(fn) + "(" + child->describe() + ")";
        }
    }
    return "?";
}

namespace {

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::vector<std::unique_ptr<Statement>> parse_program() {
        std::vector<std::unique_ptr<Statement>> out;
        for (;;) {
            while (peek().is(Tok::Semicolon)) next();
            if (peek().is(Tok::Eof)) break;
            out.push_back(parse_statement());
            // statements end at ';' or EOF
            if (peek().is(Tok::Semicolon)) {
                while (peek().is(Tok::Semicolon)) next();
            } else if (!peek().is(Tok::Eof)) {
                throw parse_error("end of statement (';')", describe(peek()));
            }
        }
        return out;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token& peek() const { return toks_[pos_]; }
    const Token& next() { return toks_[std::min(pos_++, toks_.size() - 1)]; }

    static std::string describe(const Token& t) {
        switch (t.kind) {
            case Tok::Eof: return std::string("end of input");
            case Tok::Ident: return "identifier '" + t.text + "'";
            case Tok::IntLit: return "integer literal '" + t.text + "'";
            case Tok::StrLit: return "string literal '" + t.text + "'";
            default: return std::string(tok_name(t.kind));
        }
    }

    DbError parse_error(const std::string& expected, const std::string& found) {
        const Token& t = peek();
        return DbError::parse(t.line, t.col, expected, found);
    }

    const Token& expect(Tok k, const char* what) {
        if (!peek().is(k))
            throw parse_error(what, describe(peek()));
        return next();
    }

    std::string expect_ident(const char* what) {
        if (!peek().is(Tok::Ident))
            throw parse_error(what, describe(peek()));
        const Token& t = next();
        return t.text;
    }

    // ---------------- statements ----------------

    std::unique_ptr<Statement> parse_statement() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::KwSelect: return parse_select();
            case Tok::KwInsert: return parse_insert();
            case Tok::KwUpdate: return parse_update();
            case Tok::KwDelete: return parse_delete();
            case Tok::KwCreate: return parse_create();
            case Tok::KwBegin: return parse_begin();
            case Tok::KwCommit: case Tok::KwRollback: return parse_txn_end();
            case Tok::KwExplain: return parse_explain();
            default:
                throw parse_error("a SQL statement (SELECT, INSERT, UPDATE, DELETE, CREATE, "
                                  "BEGIN, COMMIT, ROLLBACK, EXPLAIN)", describe(t));
        }
    }

    std::unique_ptr<Statement> parse_explain() {
        expect(Tok::KwExplain, "'EXPLAIN'");
        auto s = std::make_unique<Statement>(Statement::Kind::Explain);
        s->inner = parse_statement();
        return s;
    }

    std::unique_ptr<Statement> parse_begin() {
        expect(Tok::KwBegin, "'BEGIN'");
        if (peek().is(Tok::KwTransaction)) next();
        return std::make_unique<Statement>(Statement::Kind::Begin);
    }

    std::unique_ptr<Statement> parse_txn_end() {
        bool commit = peek().is(Tok::KwCommit);
        next();
        if (peek().is(Tok::KwTransaction)) next();
        return std::make_unique<Statement>(
            commit ? Statement::Kind::Commit : Statement::Kind::Rollback);
    }

    std::unique_ptr<Statement> parse_create() {
        expect(Tok::KwCreate, "'CREATE'");
        if (peek().is(Tok::KwTable)) return parse_create_table();
        if (peek().is(Tok::KwIndex)) return parse_create_index();
        throw parse_error("'TABLE' or 'INDEX' after CREATE", describe(peek()));
    }

    std::unique_ptr<Statement> parse_create_table() {
        expect(Tok::KwTable, "'TABLE'");
        auto s = std::make_unique<Statement>(Statement::Kind::CreateTable);
        s->create_table.table = expect_ident("table name");
        expect(Tok::LParen, "'('");
        for (;;) {
            ColumnDefAst col;
            col.name = expect_ident("column name");
            if (peek().is(Tok::KwInteger)) {
                next();
                col.type = ColType::Integer;
            } else if (peek().is(Tok::KwText)) {
                next();
                col.type = ColType::Text;
            } else {
                throw parse_error("column type (INTEGER or TEXT)", describe(peek()));
            }
            if (peek().is(Tok::KwPrimary)) {
                next();
                expect(Tok::KwKey, "'KEY' after PRIMARY");
                col.primary_key = true;
            }
            s->create_table.columns.push_back(std::move(col));
            if (peek().is(Tok::Comma)) { next(); continue; }
            break;
        }
        expect(Tok::RParen, "')' or ',' in column list");
        return s;
    }

    std::unique_ptr<Statement> parse_create_index() {
        expect(Tok::KwIndex, "'INDEX'");
        auto s = std::make_unique<Statement>(Statement::Kind::CreateIndex);
        s->create_index.index = expect_ident("index name");
        expect(Tok::KwOn, "'ON'");
        s->create_index.table = expect_ident("table name");
        expect(Tok::LParen, "'('");
        s->create_index.column = expect_ident("indexed column name");
        expect(Tok::RParen, "')'");
        return s;
    }

    std::unique_ptr<Statement> parse_insert() {
        expect(Tok::KwInsert, "'INSERT'");
        expect(Tok::KwInto, "'INTO'");
        auto s = std::make_unique<Statement>(Statement::Kind::Insert);
        s->insert.table = expect_ident("table name");
        if (peek().is(Tok::LParen)) {   // optional column list
            next();
            s->insert.has_columns = true;
            for (;;) {
                s->insert.columns.push_back(expect_ident("column name"));
                if (peek().is(Tok::Comma)) { next(); continue; }
                break;
            }
            expect(Tok::RParen, "')' or ',' in column list");
        }
        expect(Tok::KwValues, "'VALUES'");
        for (;;) {
            expect(Tok::LParen, "'(' starting a value row");
            std::vector<ExprPtr> row;
            for (;;) {
                row.push_back(parse_expr());
                if (peek().is(Tok::Comma)) { next(); continue; }
                break;
            }
            expect(Tok::RParen, "')' or ',' in value row");
            s->insert.rows.push_back(std::move(row));
            if (peek().is(Tok::Comma)) { next(); continue; }
            break;
        }
        return s;
    }

    std::unique_ptr<Statement> parse_update() {
        expect(Tok::KwUpdate, "'UPDATE'");
        auto s = std::make_unique<Statement>(Statement::Kind::Update);
        s->update.table = expect_ident("table name");
        expect(Tok::KwSet, "'SET'");
        for (;;) {
            std::string col = expect_ident("column name");
            expect(Tok::OpEq, "'=' in SET clause");
            s->update.sets.emplace_back(std::move(col), parse_expr());
            if (peek().is(Tok::Comma)) { next(); continue; }
            break;
        }
        if (peek().is(Tok::KwWhere)) {
            next();
            s->update.where = parse_expr();
        }
        return s;
    }

    std::unique_ptr<Statement> parse_delete() {
        expect(Tok::KwDelete, "'DELETE'");
        expect(Tok::KwFrom, "'FROM'");
        auto s = std::make_unique<Statement>(Statement::Kind::Delete);
        s->del.table = expect_ident("table name");
        if (peek().is(Tok::KwWhere)) {
            next();
            s->del.where = parse_expr();
        }
        return s;
    }

    std::unique_ptr<Statement> parse_select() {
        expect(Tok::KwSelect, "'SELECT'");
        auto s = std::make_unique<Statement>(Statement::Kind::Select);
        auto& sel = s->select;

        if (peek().is(Tok::OpStar)) {
            next();
            sel.star = true;
        } else {
            for (;;) {
                sel.columns.push_back(parse_expr());
                if (peek().is(Tok::Comma)) { next(); continue; }
                break;
            }
        }

        if (peek().is(Tok::KwFrom)) {
            next();
            sel.has_table = true;
            sel.table = expect_ident("table name");
        }
        if (peek().is(Tok::KwWhere)) {
            next();
            sel.where = parse_expr();
        }
        if (peek().is(Tok::KwOrder)) {
            next();
            expect(Tok::KwBy, "'BY' after ORDER");
            for (;;) {
                OrderTerm term;
                term.expr = parse_expr();
                if (peek().is(Tok::KwAsc)) next();
                else if (peek().is(Tok::KwDesc)) { next(); term.desc = true; }
                sel.order_by.push_back(std::move(term));
                if (peek().is(Tok::Comma)) { next(); continue; }
                break;
            }
        }
        if (peek().is(Tok::KwLimit)) {
            next();
            sel.limit = parse_expr();
            if (peek().is(Tok::KwOffset)) {
                next();
                sel.offset = parse_expr();
            }
        } else if (peek().is(Tok::KwOffset)) {
            // SQLite-style: OFFSET is only allowed together with LIMIT.
            throw parse_error("'LIMIT' before OFFSET (OFFSET requires LIMIT)", describe(peek()));
        }
        return s;
    }

    // ---------------- expressions ----------------

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr e = parse_and();
        while (peek().is(Tok::KwOr)) {
            next();
            e = Expr::binary(BinOp::Or, std::move(e), parse_and());
        }
        return e;
    }

    ExprPtr parse_and() {
        ExprPtr e = parse_not();
        while (peek().is(Tok::KwAnd)) {
            next();
            e = Expr::binary(BinOp::And, std::move(e), parse_not());
        }
        return e;
    }

    ExprPtr parse_not() {
        if (peek().is(Tok::KwNot)) {
            next();
            return Expr::unary(UnOp::Not, parse_not());
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        ExprPtr e = parse_additive();
        for (;;) {
            BinOp op;
            switch (peek().kind) {
                case Tok::OpEq: op = BinOp::Eq; break;
                case Tok::OpNe: op = BinOp::Ne; break;
                case Tok::OpLt: op = BinOp::Lt; break;
                case Tok::OpLe: op = BinOp::Le; break;
                case Tok::OpGt: op = BinOp::Gt; break;
                case Tok::OpGe: op = BinOp::Ge; break;
                case Tok::KwIs: {
                    // IS NULL / IS NOT NULL
                    const Token& is_tok = peek();
                    next();
                    bool negated = false;
                    if (peek().is(Tok::KwNot)) { next(); negated = true; }
                    expect(Tok::KwNull, "'NULL' after IS");
                    (void)is_tok;
                    e = Expr::unary(negated ? UnOp::IsNotNull : UnOp::IsNull, std::move(e));
                    continue;
                }
                default: return e;
            }
            next();
            e = Expr::binary(op, std::move(e), parse_additive());
        }
    }

    ExprPtr parse_additive() {
        ExprPtr e = parse_multiplicative();
        for (;;) {
            if (peek().is(Tok::OpPlus)) {
                next();
                e = Expr::binary(BinOp::Add, std::move(e), parse_multiplicative());
            } else if (peek().is(Tok::OpMinus)) {
                next();
                e = Expr::binary(BinOp::Sub, std::move(e), parse_multiplicative());
            } else {
                return e;
            }
        }
    }

    ExprPtr parse_multiplicative() {
        ExprPtr e = parse_unary();
        for (;;) {
            if (peek().is(Tok::OpStar)) {
                next();
                e = Expr::binary(BinOp::Mul, std::move(e), parse_unary());
            } else if (peek().is(Tok::OpSlash)) {
                next();
                e = Expr::binary(BinOp::Div, std::move(e), parse_unary());
            } else if (peek().is(Tok::OpPercent)) {
                next();
                e = Expr::binary(BinOp::Mod, std::move(e), parse_unary());
            } else {
                return e;
            }
        }
    }

    ExprPtr parse_unary() {
        if (peek().is(Tok::OpMinus)) {
            next();
            return Expr::unary(UnOp::Neg, parse_unary());
        }
        if (peek().is(Tok::OpPlus)) {   // unary plus is a no-op
            next();
            return parse_unary();
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::IntLit: {
                next();
                auto n = parse_i64(t.text);
                if (!n)
                    throw DbError::lex(t.line, t.col,
                                       "integer literal '" + t.text + "' out of range");
                return Expr::literal(Value::integer(*n));
            }
            case Tok::StrLit: {
                next();
                return Expr::literal(Value::text(t.text));
            }
            case Tok::KwNull: {
                next();
                return Expr::literal(Value::null());
            }
            case Tok::Ident: {
                next();
                return Expr::col_ref(t.text, t.line, t.col);
            }
            case Tok::KwCount: case Tok::KwMin: case Tok::KwMax: case Tok::KwSum: {
                AggFn fn = t.kind == Tok::KwCount ? AggFn::Count
                         : t.kind == Tok::KwMin ? AggFn::Min
                         : t.kind == Tok::KwMax ? AggFn::Max : AggFn::Sum;
                next();
                expect(Tok::LParen, "'(' after aggregate function name");
                if (peek().is(Tok::OpStar)) {
                    next();
                    expect(Tok::RParen, "')' after '*'");
                    if (fn != AggFn::Count)
                        throw parse_error("'*' argument only allowed in COUNT(*)", "'*'");
                    return Expr::agg_expr(fn, true, nullptr);
                }
                ExprPtr arg = parse_expr();
                expect(Tok::RParen, "')'");
                return Expr::agg_expr(fn, false, std::move(arg));
            }
            case Tok::LParen: {
                next();
                ExprPtr e = parse_expr();
                expect(Tok::RParen, "')'");
                return e;
            }
            default:
                throw parse_error("an expression (literal, column, aggregate, or '(expr)')",
                                  describe(t));
        }
    }
};

} // namespace

std::vector<std::unique_ptr<Statement>> parse(const std::string& sql) {
    return Parser(lex(sql)).parse_program();
}

} // namespace sc

#include "lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace sc {

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::KwSelect: return "SELECT";   case Tok::KwInsert: return "INSERT";
        case Tok::KwUpdate: return "UPDATE";   case Tok::KwDelete: return "DELETE";
        case Tok::KwCreate: return "CREATE";   case Tok::KwTable: return "TABLE";
        case Tok::KwIndex: return "INDEX";     case Tok::KwFrom: return "FROM";
        case Tok::KwWhere: return "WHERE";     case Tok::KwAnd: return "AND";
        case Tok::KwOr: return "OR";           case Tok::KwNot: return "NOT";
        case Tok::KwOrder: return "ORDER";     case Tok::KwBy: return "BY";
        case Tok::KwLimit: return "LIMIT";     case Tok::KwOffset: return "OFFSET";
        case Tok::KwPrimary: return "PRIMARY"; case Tok::KwKey: return "KEY";
        case Tok::KwValues: return "VALUES";   case Tok::KwInto: return "INTO";
        case Tok::KwSet: return "SET";         case Tok::KwAsc: return "ASC";
        case Tok::KwDesc: return "DESC";       case Tok::KwNull: return "NULL";
        case Tok::KwInteger: return "INTEGER"; case Tok::KwText: return "TEXT";
        case Tok::KwIs: return "IS";           case Tok::KwBegin: return "BEGIN";
        case Tok::KwCommit: return "COMMIT";   case Tok::KwRollback: return "ROLLBACK";
        case Tok::KwTransaction: return "TRANSACTION";
        case Tok::KwExplain: return "EXPLAIN";
        case Tok::KwOn: return "ON";
        case Tok::KwCount: return "COUNT";     case Tok::KwMin: return "MIN";
        case Tok::KwMax: return "MAX";         case Tok::KwSum: return "SUM";
        case Tok::Ident: return "identifier";  case Tok::IntLit: return "integer literal";
        case Tok::StrLit: return "string literal";
        case Tok::OpEq: return "'='";          case Tok::OpNe: return "'!='";
        case Tok::OpLt: return "'<'";          case Tok::OpLe: return "'<='";
        case Tok::OpGt: return "'>'";          case Tok::OpGe: return "'>='";
        case Tok::OpPlus: return "'+'";        case Tok::OpMinus: return "'-'";
        case Tok::OpStar: return "'*'";        case Tok::OpSlash: return "'/'";
        case Tok::OpPercent: return "'%'";
        case Tok::LParen: return "'('";        case Tok::RParen: return "')'";
        case Tok::Comma: return "','";         case Tok::Semicolon: return "';'";
        case Tok::Dot: return "'.'";
        case Tok::Eof: return "end of input";
    }
    return "?";
}

bool Token::is_kw(const char* word) const {
    return lower(text) == word;
}

namespace {

const std::unordered_map<std::string, Tok>& keyword_table() {
    static const std::unordered_map<std::string, Tok> table = {
        {"select", Tok::KwSelect}, {"insert", Tok::KwInsert}, {"update", Tok::KwUpdate},
        {"delete", Tok::KwDelete}, {"create", Tok::KwCreate}, {"table", Tok::KwTable},
        {"index", Tok::KwIndex},   {"from", Tok::KwFrom},     {"where", Tok::KwWhere},
        {"and", Tok::KwAnd},       {"or", Tok::KwOr},         {"not", Tok::KwNot},
        {"order", Tok::KwOrder},   {"by", Tok::KwBy},         {"limit", Tok::KwLimit},
        {"offset", Tok::KwOffset}, {"primary", Tok::KwPrimary}, {"key", Tok::KwKey},
        {"values", Tok::KwValues}, {"into", Tok::KwInto},     {"set", Tok::KwSet},
        {"asc", Tok::KwAsc},       {"desc", Tok::KwDesc},     {"null", Tok::KwNull},
        {"integer", Tok::KwInteger}, {"text", Tok::KwText},   {"is", Tok::KwIs},
        {"begin", Tok::KwBegin},   {"commit", Tok::KwCommit}, {"rollback", Tok::KwRollback},
        {"transaction", Tok::KwTransaction}, {"explain", Tok::KwExplain},
        {"count", Tok::KwCount},   {"min", Tok::KwMin},       {"max", Tok::KwMax},
        {"sum", Tok::KwSum},       {"on", Tok::KwOn},
    };
    return table;
}

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

} // namespace

std::vector<Token> lex(const std::string& sql) {
    std::vector<Token> out;
    size_t i = 0;
    int line = 1, col = 1;

    auto advance = [&](size_t n) {
        for (size_t k = 0; k < n && i < sql.size(); ++k, ++i) {
            if (sql[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
    };

    while (i < sql.size()) {
        char c = sql[i];

        // whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(1);
            continue;
        }

        // -- line comment
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') advance(1);
            continue;
        }
        // /* block comment */
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int sl = line, sc = col;
            advance(2);
            bool closed = false;
            while (i < sql.size()) {
                if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    advance(2);
                    closed = true;
                    break;
                }
                advance(1);
            }
            if (!closed)
                throw DbError::lex(sl, sc, "unterminated block comment");
            continue;
        }

        int tl = line, tc = col;

        // identifiers / keywords
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < sql.size() && is_ident_char(sql[i])) advance(1);
            std::string word = sql.substr(start, i - start);
            Tok kind = Tok::Ident;
            auto it = keyword_table().find(lower(word));
            if (it != keyword_table().end()) kind = it->second;
            out.push_back(Token{kind, std::move(word), tl, tc});
            continue;
        }

        // integer literal
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) advance(1);
            std::string digits = sql.substr(start, i - start);
            out.push_back(Token{Tok::IntLit, std::move(digits), tl, tc});
            continue;
        }

        // string literal: single quotes, '' escape
        if (c == '\'') {
            advance(1);
            std::string s;
            for (;;) {
                if (i >= sql.size())
                    throw DbError::lex(tl, tc, "unterminated string literal");
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        s.push_back('\'');
                        advance(2);
                        continue;
                    }
                    advance(1);
                    break;
                }
                s.push_back(sql[i]);
                advance(1);
            }
            out.push_back(Token{Tok::StrLit, std::move(s), tl, tc});
            continue;
        }

        // double-quoted identifier
        if (c == '"') {
            advance(1);
            std::string s;
            for (;;) {
                if (i >= sql.size())
                    throw DbError::lex(tl, tc, "unterminated quoted identifier");
                if (sql[i] == '"') {
                    if (i + 1 < sql.size() && sql[i + 1] == '"') {
                        s.push_back('"');
                        advance(2);
                        continue;
                    }
                    advance(1);
                    break;
                }
                s.push_back(sql[i]);
                advance(1);
            }
            if (s.empty())
                throw DbError::lex(tl, tc, "empty quoted identifier");
            out.push_back(Token{Tok::Ident, std::move(s), tl, tc});
            continue;
        }

        // operators & punctuation
        auto two = [&](char a, char b) {
            return c == a && i + 1 < sql.size() && sql[i + 1] == b;
        };
        Tok kind;
        size_t len;
        if (two('!', '=') || two('<', '>')) { kind = Tok::OpNe; len = 2; }
        else if (two('<', '=')) { kind = Tok::OpLe; len = 2; }
        else if (two('>', '=')) { kind = Tok::OpGe; len = 2; }
        else {
            switch (c) {
                case '=': kind = Tok::OpEq; len = 1; break;
                case '<': kind = Tok::OpLt; len = 1; break;
                case '>': kind = Tok::OpGt; len = 1; break;
                case '+': kind = Tok::OpPlus; len = 1; break;
                case '-': kind = Tok::OpMinus; len = 1; break;
                case '*': kind = Tok::OpStar; len = 1; break;
                case '/': kind = Tok::OpSlash; len = 1; break;
                case '%': kind = Tok::OpPercent; len = 1; break;
                case '(': kind = Tok::LParen; len = 1; break;
                case ')': kind = Tok::RParen; len = 1; break;
                case ',': kind = Tok::Comma; len = 1; break;
                case ';': kind = Tok::Semicolon; len = 1; break;
                case '.': kind = Tok::Dot; len = 1; break;
                default:
                    throw DbError::lex(tl, tc, std::string("unexpected character '") + c + "'");
            }
        }
        std::string text = sql.substr(i, len);
        advance(len);
        out.push_back(Token{kind, std::move(text), tl, tc});
    }

    out.push_back(Token{Tok::Eof, "", line, col});
    return out;
}

} // namespace sc

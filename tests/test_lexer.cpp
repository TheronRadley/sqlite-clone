// test_lexer.cpp — tokenizer unit tests.
#include "lexer.hpp"
#include "test_util.hpp"

using namespace sc;

namespace {

std::vector<Tok> kinds(const std::string& sql) {
    std::vector<Tok> out;
    for (const Token& t : lex(sql)) out.push_back(t.kind);
    return out;
}

std::string text_of(const std::string& sql, size_t i) { return lex(sql)[i].text; }

} // namespace

TEST(lexer_keywords_case_insensitive) {
    auto ks = kinds("select INSERT From WHERE");
    CHECK_EQ(ks.size(), size_t(5));
    CHECK(ks[0] == Tok::KwSelect);
    CHECK(ks[1] == Tok::KwInsert);
    CHECK(ks[2] == Tok::KwFrom);
    CHECK(ks[3] == Tok::KwWhere);
    CHECK(ks[4] == Tok::Eof);
}

TEST(lexer_all_required_keywords) {
    // every keyword the engine knows, in one stream (37 keywords + Eof)
    auto ks = kinds("SELECT INSERT UPDATE DELETE CREATE TABLE INDEX FROM WHERE AND OR "
                    "NOT ORDER BY LIMIT OFFSET PRIMARY KEY VALUES INTO SET ASC DESC "
                    "NULL INTEGER TEXT IS BEGIN COMMIT ROLLBACK TRANSACTION EXPLAIN "
                    "ON COUNT MIN MAX SUM");
    CHECK_EQ(ks.size(), size_t(38));
    for (size_t i = 0; i < 37; ++i) {
        if (ks[i] == Tok::Ident)
            throw ::test::TestFailure(sc::str(__FILE__, ":", __LINE__,
                                              " keyword #", i, " lexed as identifier"));
    }
    CHECK(ks[37] == Tok::Eof);
}

TEST(lexer_identifiers) {
    auto ks = kinds("my_table _x a1 SELECTY");
    CHECK_EQ(ks.size(), size_t(5));
    CHECK(ks[0] == Tok::Ident);
    CHECK(ks[1] == Tok::Ident);
    CHECK(ks[2] == Tok::Ident);
    CHECK(ks[3] == Tok::Ident);  // SELECTY is not the SELECT keyword
    CHECK_EQ(text_of("my_table _x", 0), std::string("my_table"));
}

TEST(lexer_integer_literals) {
    auto ks = kinds("0 42 007 999999999999999999999");
    CHECK(ks[0] == Tok::IntLit);
    CHECK(ks[1] == Tok::IntLit);
    CHECK(ks[2] == Tok::IntLit);
    // the out-of-range literal is still tokenized; the parser reports it
    CHECK(ks[3] == Tok::IntLit);
    CHECK_EQ(text_of("42", 0), std::string("42"));
}

TEST(lexer_string_literal_and_escape) {
    auto ts = lex("'hello' 'it''s' 'a\\nb'");
    CHECK_EQ(ts[0].text, std::string("hello"));
    CHECK_EQ(ts[1].text, std::string("it's"));
    CHECK_EQ(ts[2].text, std::string("a\\nb"));
    CHECK(ts[0].kind == Tok::StrLit);
}

TEST(lexer_double_quoted_identifier) {
    auto ts = lex("\"my col\" \"q\"\"q\"");
    CHECK(ts[0].kind == Tok::Ident);
    CHECK_EQ(ts[0].text, std::string("my col"));
    CHECK_EQ(ts[1].text, std::string("q\"q"));
}

TEST(lexer_operators) {
    auto ks = kinds("= != <> < <= > >= + - * / %");
    CHECK_EQ(ks.size(), size_t(13));   // 12 operators + Eof
    CHECK(ks[0] == Tok::OpEq);
    CHECK(ks[1] == Tok::OpNe);
    CHECK(ks[2] == Tok::OpNe);
    CHECK(ks[3] == Tok::OpLt);
    CHECK(ks[4] == Tok::OpLe);
    CHECK(ks[5] == Tok::OpGt);
    CHECK(ks[6] == Tok::OpGe);
    CHECK(ks[7] == Tok::OpPlus);
    CHECK(ks[8] == Tok::OpMinus);
    CHECK(ks[9] == Tok::OpStar);
    CHECK(ks[10] == Tok::OpSlash);
    CHECK(ks[11] == Tok::OpPercent);
}

TEST(lexer_punctuation) {
    auto ks = kinds("( ) , ; .");
    CHECK(ks[0] == Tok::LParen);
    CHECK(ks[1] == Tok::RParen);
    CHECK(ks[2] == Tok::Comma);
    CHECK(ks[3] == Tok::Semicolon);
    CHECK(ks[4] == Tok::Dot);
}

TEST(lexer_positions) {
    auto ts = lex("SELECT\n  x");
    CHECK_EQ(ts[0].line, 1);
    CHECK_EQ(ts[0].col, 1);
    CHECK_EQ(ts[1].line, 2);
    CHECK_EQ(ts[1].col, 3);
    CHECK_EQ(ts[2].line, 2);
    CHECK_EQ(ts[2].col, 4);   // Eof sits right after the last token
}

TEST(lexer_error_unexpected_character) {
    // the exact error shape required by the brief
    CHECK_DB_ERROR(lex("SELECT @ FROM t"), int(Err::Lex),
                   "unexpected character '@' at line 1, column 8");
    CHECK_DB_ERROR(lex("SELECT x FROM t WHERE a ~ b"), int(Err::Lex), "unexpected character '~'");
}

TEST(lexer_error_unterminated_string) {
    CHECK_DB_ERROR(lex("SELECT 'abc"), int(Err::Lex), "unterminated string literal");
    CHECK_DB_ERROR(lex("SELECT 'it''s"), int(Err::Lex), "unterminated string literal");
}

TEST(lexer_comments) {
    auto ks = kinds("SELECT -- comment to end of line\n x /* block\n comment */ FROM t");
    CHECK_EQ(ks.size(), size_t(5));   // 4 tokens + Eof
    CHECK(ks[0] == Tok::KwSelect);
    CHECK(ks[1] == Tok::Ident);
    CHECK(ks[2] == Tok::KwFrom);
    CHECK(ks[3] == Tok::Ident);
    CHECK_DB_ERROR(lex("/* never closed"), int(Err::Lex), "unterminated block comment");
}

TEST(lexer_empty_input) {
    auto ks = kinds("");
    CHECK_EQ(ks.size(), size_t(1));
    CHECK(ks[0] == Tok::Eof);
}

// test_util.hpp — a zero-dependency test harness.
//
//   TEST(name) { CHECK(...); CHECK_EQ(a, b); }
//
// Tests self-register via static initializers and run in registration
// order. CHECK failures throw TestFailure with file/line context.
// The whole suite is built with ASan/UBSan (see Makefile).
#pragma once

#include <unistd.h>

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "error.hpp"
#include "util.hpp"

namespace test {

struct TestFailure : std::exception {
    std::string msg;
    explicit TestFailure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline bool register_test(const char* name, void (*fn)()) {
    registry().push_back(TestCase{name, fn});
    return true;
}

// Runs all tests whose name contains `filter`. Returns the failure count.
inline int run_all(const std::string& filter) {
    int run = 0, failed = 0;
    for (const TestCase& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos)
            continue;
        ++run;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
            std::printf("[  OK  ] %s\n", t.name);
        } catch (const TestFailure& f) {
            ++failed;
            std::printf("[ FAIL ] %s\n        %s\n", t.name, f.msg.c_str());
        } catch (const sc::DbError& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n        unexpected DbError(%s): %s\n", t.name,
                        sc::err_name(e.code), e.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n        unexpected exception: %s\n", t.name, e.what());
        }
    }
    std::printf("==== %d test(s) run, %d failed ====\n", run, failed);
    return failed;
}

// deterministic xorshift64* PRNG (test/bench use only)
inline uint64_t next_rand(uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ull;
}
inline uint64_t rand_below(uint64_t& s, uint64_t n) { return next_rand(s) % n; }

// unique temp file paths
inline std::string temp_db_path() {
    static int counter = 0;
    return sc::str("/tmp/sc-test-", getpid(), "-", counter++, ".db");
}

} // namespace test

#define TEST(name)                                                                 \
    static void test_fn_##name();                                                  \
    static const bool reg_##name = ::test::register_test(#name, test_fn_##name);   \
    static void test_fn_##name()

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond))                                                               \
            throw ::test::TestFailure(sc::str(__FILE__, ":", __LINE__,             \
                                              " CHECK failed: ", #cond));          \
    } while (0)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        const auto va_ = (a);   /* copy: `a`/`b` may access a temporary */         \
        const auto vb_ = (b);                                                      \
        if (!(va_ == vb_)) {                                                       \
            std::ostringstream os_;                                                \
            os_ << va_ << " != " << vb_;                                           \
            throw ::test::TestFailure(                                             \
                sc::str(__FILE__, ":", __LINE__, " CHECK_EQ failed: ", #a, " == ", \
                        #b, "  (", os_.str(), ")"));                               \
        }                                                                          \
    } while (0)

// The statement must raise a DbError with the given code (or any code when
// -1) whose message contains `needle`.
#define CHECK_DB_ERROR(stmt, want_code, needle)                                    \
    do {                                                                           \
        bool threw_ = false;                                                       \
        try {                                                                      \
            stmt;                                                                  \
        } catch (const sc::DbError& e_) {                                          \
            threw_ = true;                                                         \
            if (int(want_code) >= 0 && e_.code != sc::Err(want_code))              \
                throw ::test::TestFailure(sc::str(                                 \
                    __FILE__, ":", __LINE__, " wrong error code: got ",            \
                    sc::err_name(e_.code), ", wanted ", sc::err_name(sc::Err(want_code)), \
                    " (message: ", e_.message, ")"));                              \
            if (std::string(e_.message).find(needle) == std::string::npos)         \
                throw ::test::TestFailure(sc::str(                                 \
                    __FILE__, ":", __LINE__, " error message does not contain '",  \
                    needle, "': got '", e_.message, "'"));                          \
        }                                                                          \
        if (!threw_)                                                               \
            throw ::test::TestFailure(sc::str(                                     \
                __FILE__, ":", __LINE__, " expected a DbError containing '",       \
                needle, "' but nothing was thrown"));                              \
    } while (0)

#define CHECK_NO_ERROR(stmt)                                                       \
    do {                                                                           \
        try {                                                                      \
            stmt;                                                                  \
        } catch (const std::exception& e_) {                                       \
            throw ::test::TestFailure(sc::str(                                     \
                __FILE__, ":", __LINE__, " unexpected exception: ", e_.what()));   \
        }                                                                          \
    } while (0)

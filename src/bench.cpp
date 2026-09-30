// bench.cpp — the benchmark harness (spec §26).
//
// Measures the whole engine end-to-end through the SQL layer (parse →
// plan → execute → B-Tree → pager), because that is what a user of the
// embedded library actually experiences. Workloads at 10k / 50k / 100k
// rows:
//
//   load sequential    N rowids inserted in ascending order
//   load random        N rowids inserted in shuffled order (split-heavy)
//   scan               full-table COUNT(*)
//   point (pk)         N lookups: WHERE id = <random>
//   point (index)      N lookups: WHERE a = <random> on a secondary index
//   update             N/10 single-row UPDATEs in one transaction
//   delete             N/10 single-row DELETEs in one transaction
//   restart + scan     close, reopen from disk, full COUNT(*)
//   validate           full structural validation of both trees
//   cache cold/warm    scan hit rate with a 64-page then unbounded cache
//   tree height        levels of the table B-Tree at N rows
//
// Build: make && ./build/bench [sizes...]   (default 10000 50000 100000)
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "btree.hpp"
#include "database.hpp"
#include "keys.hpp"

using namespace sc;
using Clock = std::chrono::steady_clock;

namespace {

struct Sample {
    double seconds;
    uint64_t ops;
    const char* unit;
};

double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

void print_rate(const char* name, const Sample& s, const char* note = "") {
    std::printf("  %-18s %9.3f s   %10" PRIu64 " %s/s   %s\n", name, s.seconds,
                s.ops > 0 ? uint64_t(s.ops / s.seconds) : 0, s.unit, note);
}

std::string temp_path() { return "/tmp/sc-bench-" + std::to_string(getpid()) + ".db"; }

uint64_t count_of(Database& db, const std::string& where = "") {
    auto rs = db.execute("SELECT COUNT(*) FROM t" + where);
    return rs[0].rows.at(0).at(0).as_int();
}

// deterministic shuffle of 1..n (xorshift, same PRNG as the tests)
std::vector<int64_t> shuffled_1_to_n(uint64_t seed, int64_t n) {
    std::vector<int64_t> v(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) v[size_t(i)] = i + 1;
    for (int64_t i = n - 1; i > 0; --i) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        uint64_t r = (seed * 2685821657736338717ull) % uint64_t(i + 1);
        std::swap(v[size_t(i)], v[size_t(r)]);
    }
    return v;
}

struct Run {
    int64_t n;
    std::string path;
};

void run_size(const Run& R) {
    const int64_t n = R.n;
    std::printf("N = %" PRId64 "\n", n);

    // ---- load: sequential rowids, 1000-row transactions ----
    std::vector<int64_t> order;
    for (int64_t i = 0; i < n; ++i) order.push_back(i + 1);
    Sample load{};
    {
        std::remove(R.path.c_str());
        auto db = Database::open(R.path);
        (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");
        auto t0 = Clock::now();
        for (int64_t batch = 0; batch < n; batch += 1000) {
            (void)db->execute("BEGIN");
            for (int64_t i = batch; i < std::min(batch + 1000, n); ++i)
                (void)db->execute("INSERT INTO t VALUES (" + std::to_string(order[size_t(i)]) +
                                  ", " + std::to_string(order[size_t(i)] % 9973) +
                                  ", 'row-" + std::to_string(order[size_t(i)]) + "')");
            (void)db->execute("COMMIT");
        }
        load = {elapsed_s(t0), uint64_t(n), "rows"};
    }

    // ---- load: random rowid order into a second table ----
    Sample load_rand{};
    {
        auto db = Database::open(R.path);
        (void)db->execute("CREATE TABLE r (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");
        auto shuffled = shuffled_1_to_n(0x9e3779b9u, n);
        auto t0 = Clock::now();
        for (int64_t batch = 0; batch < n; batch += 1000) {
            (void)db->execute("BEGIN");
            for (int64_t i = batch; i < std::min(batch + 1000, n); ++i)
                (void)db->execute("INSERT INTO r VALUES (" + std::to_string(shuffled[size_t(i)]) +
                                  ", " + std::to_string(shuffled[size_t(i)] % 9973) +
                                  ", 'row-" + std::to_string(shuffled[size_t(i)]) + "')");
            (void)db->execute("COMMIT");
        }
        load_rand = {elapsed_s(t0), uint64_t(n), "rows"};
    }
    print_rate("load sequential", load);
    print_rate("load random", load_rand);

    // everything after this runs on the sequential table
    auto db = Database::open(R.path);
    (void)db->execute("CREATE INDEX t_a ON t(a)");

    // ---- full scan ----
    {
        auto t0 = Clock::now();
        uint64_t c = count_of(*db);
        if (c != uint64_t(n)) std::printf("  !! scan count mismatch: %" PRIu64 "\n", c);
        print_rate("scan (COUNT *)", {elapsed_s(t0), uint64_t(n), "rows"});
    }

    // ---- point lookups: primary key ----
    {
        auto shuffled = shuffled_1_to_n(0x1234567u, n);
        auto t0 = Clock::now();
        uint64_t found = 0;
        for (int64_t i = 0; i < n; ++i) {
            auto rs = db->execute("SELECT b FROM t WHERE id = " +
                                   std::to_string(shuffled[size_t(i)]));
            if (rs[0].rows.size() == 1) ++found;
        }
        if (found != uint64_t(n))
            std::printf("  !! pk lookup misses: %" PRIu64 "/%" PRId64 "\n", found, n);
        print_rate("point (pk)", {elapsed_s(t0), uint64_t(n), "lookups"});
    }

    // ---- point lookups: secondary index ----
    {
        auto shuffled = shuffled_1_to_n(0xdeadbeefu, n);
        auto t0 = Clock::now();
        for (int64_t i = 0; i < n; ++i) {
            // a = k % 9973 matches ~n/9973 rows each; still one index seek
            (void)db->execute("SELECT id FROM t WHERE a = " +
                              std::to_string(shuffled[size_t(i)] % 9973));
        }
        print_rate("point (index)", {elapsed_s(t0), uint64_t(n), "lookups"});
    }

    // ---- updates: n/10 rows ----
    {
        auto shuffled = shuffled_1_to_n(0xcafeu, n);
        auto t0 = Clock::now();
        (void)db->execute("BEGIN");
        for (int64_t i = 0; i < n / 10; ++i)
            (void)db->execute("UPDATE t SET a = a + 1000000 WHERE id = " +
                              std::to_string(shuffled[size_t(i)]));
        (void)db->execute("COMMIT");
        print_rate("update", {elapsed_s(t0), uint64_t(n / 10), "rows"});
    }

    // ---- deletes: n/10 rows ----
    {
        auto shuffled = shuffled_1_to_n(0xf00du, n);
        auto t0 = Clock::now();
        (void)db->execute("BEGIN");
        for (int64_t i = n / 10; i < n / 5; ++i)
            (void)db->execute("DELETE FROM t WHERE id = " +
                              std::to_string(shuffled[size_t(i)]));
        (void)db->execute("COMMIT");
        print_rate("delete", {elapsed_s(t0), uint64_t(n / 10), "rows"});
    }

    // ---- tree height + page count (informational) ----
    {
        auto& pager = db->pager_for_tools();
        const TableDef* t = db->catalog().find_table("t");
        uint32_t h = BTree<RowIdKey>(pager, t->root_page).height();
        std::printf("  %-18s %u levels, %u pages, page size %u\n", "table b-tree", h,
                    pager.page_count(), pager.page_size());
    }

    // ---- structural validation ----
    {
        auto t0 = Clock::now();
        db->validate(true);
        print_rate("validate (deep)", {elapsed_s(t0), uint64_t(n), "rows"});
    }

    // ---- restart + scan (startup cost is dominated by the open + scan) ----
    {
        db.reset();
        auto t0 = Clock::now();
        auto db2 = Database::open(R.path);
        uint64_t c = count_of(*db2);
        double open_scan = elapsed_s(t0);
        if (c != uint64_t(n - n / 10))
            std::printf("  !! post-restart count mismatch: %" PRIu64 "\n", c);
        print_rate("restart + scan", {open_scan, uint64_t(n - n / 10), "rows"});
        db = std::move(db2);
    }

    // ---- cache behavior: bounded cache, then warm ----
    {
        auto& pager = db->pager_for_tools();
        pager.set_cache_capacity(64);   // 64 pages = 256 KiB
        uint64_t before_h = pager.stats().cache_hits, before_m = pager.stats().cache_misses;
        (void)count_of(*db);
        uint64_t h = pager.stats().cache_hits - before_h, m = pager.stats().cache_misses - before_m;
        std::printf("  %-18s cache=%zu pages: %6.2f%% hits (%" PRIu64 " hit / %" PRIu64
                    " miss)\n",
                    "scan, 64-page cache", pager.cache_capacity(),
                    100.0 * double(h) / double(h + m + (h + m == 0)), h, m);

        pager.set_cache_capacity(1u << 20);   // effectively unbounded
        before_h = pager.stats().cache_hits;
        before_m = pager.stats().cache_misses;
        (void)count_of(*db);
        (void)count_of(*db);   // second pass: everything resident
        h = pager.stats().cache_hits - before_h;
        m = pager.stats().cache_misses - before_m;
        std::printf("  %-18s 2 scans, big cache: %6.2f%% hits (%" PRIu64 " hit / %" PRIu64
                    " miss)\n",
                    "scan, big cache", 100.0 * double(h) / double(h + m + (h + m == 0)), h, m);
    }

    db.reset();
    std::remove(R.path.c_str());
    std::remove((R.path + "-journal").c_str());
}

} // namespace

int main(int argc, char** argv) {
    std::vector<int64_t> sizes;
    for (int i = 1; i < argc; ++i) sizes.push_back(std::atoll(argv[i]));
    if (sizes.empty()) sizes = {10000, 50000, 100000};

    std::printf("sc-bench: whole-engine benchmarks through the SQL layer\n");
    std::printf("          (g++ %d.%d, -O2)\n\n", __GNUC__, __GNUC_MINOR__);
    for (int64_t n : sizes) run_size({n, temp_path()});
    std::printf("\ndone.\n");
    return 0;
}

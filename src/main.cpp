// main.cpp — dbx: the interactive SQL shell.
//
//   usage: dbx [database-file]        (no file = in-memory database)
//
// Dot commands (must start at the beginning of a line):
//   .help            show this help
//   .tables          list tables
//   .schema          show stored CREATE statements
//   .dump            dump the whole database as SQL
//   .pages           page-by-page inventory of the database file
//   .btree NAME      render the B-Tree of a table or index
//   .validate        run full structural + consistency validation
//   .stats           pager/cache statistics
//   .explain ON|OFF  print the query plan before every SELECT/UPDATE/DELETE
//   .quit / .exit    leave the shell
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "database.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "util.hpp"

using namespace sc;

namespace {

void print_help() {
    std::printf(
        "dot commands:\n"
        "  .help            show this help\n"
        "  .tables          list tables\n"
        "  .schema          show stored CREATE statements\n"
        "  .dump            dump the whole database as SQL\n"
        "  .pages           page-by-page inventory of the database file\n"
        "  .btree NAME      render the B-Tree of a table or index\n"
        "  .validate        run full structural + consistency validation\n"
        "  .stats           pager/cache statistics\n"
        "  .explain ON|OFF  print the query plan before SELECT/UPDATE/DELETE\n"
        "  .quit | .exit    leave the shell\n");
}

void print_rows(const ExecResult& r) {
    if (r.columns.empty() && r.rows.empty()) {
        std::printf("%s\n", r.message.c_str());
        return;
    }
    // column widths
    std::vector<size_t> w(r.columns.size(), 0);
    for (size_t i = 0; i < r.columns.size(); ++i) w[i] = r.columns[i].size();
    for (const auto& row : r.rows)
        for (size_t i = 0; i < row.size() && i < w.size(); ++i)
            w[i] = std::max(w[i], row[i].display().size());

    for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) std::printf(" | ");
        std::printf("%-*s", int(w[i]), r.columns[i].c_str());
    }
    std::putchar('\n');
    for (size_t i = 0; i < w.size(); ++i) {
        if (i) std::putchar('+');
        std::printf("%.*s", int(w[i] + 2), "--------------------------------");
    }
    std::putchar('\n');
    for (const auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) std::printf(" | ");
            std::printf("%-*s", int(w[i]), row[i].display().c_str());
        }
        std::putchar('\n');
    }
}

void print_stats(const Database& db) {
    const PagerStats& s = db.pager().stats();
    uint64_t total = s.cache_hits + s.cache_misses;
    double hit_rate = total ? (100.0 * double(s.cache_hits) / double(total)) : 0.0;
    std::printf(
        "page size        : %u\n"
        "pages in file    : %u\n"
        "cached pages     : %zu (capacity %zu)\n"
        "cache hits       : %llu\n"
        "cache misses     : %llu\n"
        "cache hit rate   : %.1f%%\n"
        "disk reads       : %llu\n"
        "disk writes      : %llu\n"
        "evictions        : %llu\n"
        "journal snapshots: %llu\n",
        db.pager().page_size(), db.pager().page_count(), db.pager().cached_pages(),
        db.pager().cache_capacity(), (unsigned long long)s.cache_hits,
        (unsigned long long)s.cache_misses, hit_rate, (unsigned long long)s.disk_reads,
        (unsigned long long)s.disk_writes, (unsigned long long)s.evictions,
        (unsigned long long)s.txn_page_writes);
}

void print_pages(Database& db) {
    auto inv = db.page_inventory();
    std::map<std::string, size_t> counts;
    for (const auto& p : inv) counts[p.kind]++;
    std::printf("page size %u, %zu pages total\n", db.pager().page_size(), inv.size());
    for (const auto& [kind, n] : counts) std::printf("  %-16s %zu\n", kind.c_str(), n);
}

// Returns true to continue, false to exit.
bool run_dot_command(Database& db, const std::string& line, bool& explain_on,
                     bool& quit) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::string arg;
    std::getline(in >> std::ws, arg);

    if (cmd == ".quit" || cmd == ".exit") {
        quit = true;
        return false;
    }
    if (cmd == ".help") {
        print_help();
        return true;
    }
    if (cmd == ".tables") {
        auto names = db.table_names();
        if (names.empty()) std::printf("(no tables)\n");
        for (const auto& n : names) std::printf("%s\n", n.c_str());
        return true;
    }
    if (cmd == ".schema") {
        std::string s = db.schema_sql();
        std::fputs(s.c_str(), stdout);
        return true;
    }
    if (cmd == ".dump") {
        std::string s = db.dump_sql();
        std::fputs(s.c_str(), stdout);
        return true;
    }
    if (cmd == ".pages") {
        print_pages(db);
        return true;
    }
    if (cmd == ".btree") {
        if (arg.empty()) {
            std::printf("usage: .btree TABLE_OR_INDEX\n");
            return true;
        }
        try {
            std::fputs(db.render_btree(arg).c_str(), stdout);
        } catch (const DbError& e) {
            std::printf("Error: %s\n", e.message.c_str());
        }
        return true;
    }
    if (cmd == ".validate") {
        try {
            db.validate(/*deep=*/true);
            std::printf("database is valid (structure, freelist, orphans, "
                        "index/table consistency)\n");
        } catch (const DbError& e) {
            std::printf("VALIDATION FAILED [%s]: %s\n", err_name(e.code), e.message.c_str());
        }
        return true;
    }
    if (cmd == ".stats") {
        print_stats(db);
        return true;
    }
    if (cmd == ".explain") {
        std::string a = lower(arg);
        if (a == "on") {
            explain_on = true;
            std::printf("explain mode ON\n");
        } else if (a == "off") {
            explain_on = false;
            std::printf("explain mode OFF\n");
        } else {
            std::printf("usage: .explain ON|OFF\n");
        }
        return true;
    }
    std::printf("unknown command '%s'; try .help\n", cmd.c_str());
    return true;
}

void run_sql(Database& db, const std::string& sql, bool explain_on) {
    try {
        if (explain_on) {
            // print plans for the plannable statements in this batch
            try {
                auto stmts = parse(sql);
                for (const auto& s : stmts) {
                    if (s->kind == Statement::Kind::Explain) continue;
                    if (s->kind == Statement::Kind::Select && s->select.has_table) {
                        const TableDef* t = db.catalog().find_table(s->select.table);
                        if (t)
                            std::printf("-- plan: %s\n",
                                        plan_query(db.catalog(), *t, s->select.where.get())
                                            .describe()
                                            .c_str());
                    }
                }
            } catch (const DbError&) {
                // the real execution below will report the error
            }
        }
        auto results = db.execute(sql);
        for (const ExecResult& r : results) print_rows(r);
    } catch (const DbError& e) {
        std::printf("Error [%s]: %s\n", err_name(e.code), e.message.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    if (argc >= 2) path = argv[1];
    if (path == ":memory:") path.clear();   // sqlite convention

    std::unique_ptr<Database> db;
    try {
        if (path.empty()) {
            db = Database::open_memory();
            std::fprintf(stderr, "dbx: no file given — using an in-memory database "
                                 "(changes will not be saved)\n");
        } else {
            db = Database::open(path);
        }
    } catch (const DbError& e) {
        std::fprintf(stderr, "dbx: cannot open '%s': [%s] %s\n", path.c_str(),
                     err_name(e.code), e.message.c_str());
        return 1;
    }

    bool interactive = isatty(0);
    bool explain_on = false;
    bool quit = false;
    std::string buffer;

    if (interactive)
        std::printf("dbx %s — a miniature SQLite-compatible engine (.help for help)\n",
                    path.empty() ? ":memory:" : path.c_str());

    std::string line;
    while (!quit && std::getline(std::cin, line)) {
        // dot commands only at the start of a statement
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (!run_dot_command(*db, line, explain_on, quit)) break;
            if (interactive) std::printf("dbx> ");
            continue;
        }
        buffer += line;
        buffer += '\n';
        if (buffer.find(';') != std::string::npos) {
            run_sql(*db, buffer, explain_on);
            buffer.clear();
        }
        if (interactive) std::printf(buffer.empty() ? "dbx> " : "   ...> ");
        std::fflush(stdout);
    }
    if (!buffer.empty() && !quit) run_sql(*db, buffer, explain_on);
    return 0;
}

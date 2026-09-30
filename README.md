# sqlite-clone

An educational, SQLite-compatible embedded database engine written from
scratch: SQL text → lexer → parser → AST → planner → executor → B-Tree →
pager → fixed-size pages on disk. No existing database engine, ORM, or
storage library is used anywhere — the storage layer, B-Trees, query
execution, and transaction machinery are all implemented in this repo.

**Language:** C++20 (system `g++ 12.2`, `-std=c++20`), zero external
dependencies — standard library only. The project spec left the language
slot open; C++20 was chosen for this implementation and every
subsystem was built and verified against that choice (see §31 of the
spec: decide, document, continue).

~10k lines of C++ across the engine, CLI, test suite, and benchmark
harness.

## Build & run

```sh
make                # build the CLI shell (build/dbx) and benchmarks (build/bench)
./build/dbx test.db # open (or create) a database file
./build/dbx :memory:# fully in-memory database
```

Inside the shell:

```
dbx> CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT, age INTEGER);
dbx> CREATE INDEX users_email ON users(email);
dbx> INSERT INTO users VALUES (1, 'alice@example.com', 29);
dbx> SELECT id, email FROM users WHERE age > 21 ORDER BY email LIMIT 5;
dbx> EXPLAIN SELECT id FROM users WHERE email = 'alice@example.com';
dbx> .tables      .schema      .dump      .pages      .btree users
dbx> .help        .quit        .exit      .stats      .validate
```

## Supported SQL

```sql
CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, ...);
CREATE INDEX i ON t(col);
INSERT INTO t VALUES (...), (...);
INSERT INTO t (a, b) VALUES (...);
SELECT exprs FROM t
  [WHERE expr] [ORDER BY col [ASC|DESC], ...] [LIMIT n [OFFSET m]];
SELECT COUNT(*) | COUNT(col) | MIN(x) | MAX(x) | SUM(x) FROM t [WHERE ...];
UPDATE t SET col = expr [WHERE ...];
DELETE FROM t [WHERE ...];
BEGIN; COMMIT; ROLLBACK;
EXPLAIN <statement>;
```

Expressions: literals, column refs, arithmetic (`+ - * / %`),
comparisons (`= != < <= > >=`), `AND` / `OR` / `NOT`, `IS NULL` /
`IS NOT NULL`, SQL three-valued logic (comparisons with NULL never
match), and type affinity on insert.

## Storage & transactions (short version)

* Fixed-size pages (default 4096 B); page 0 is a checksummed file header.
* Table B-Trees keyed by rowid; secondary indexes are separate B-Trees
  over (key, rowid) pairs. Both share one generic B-Tree
  implementation with splitting, borrowing, merging, and an independent
  structural validator (`.validate`, `BTree::validate`).
* A page cache with LRU eviction sits between the B-Trees and the file;
  `PagerStats` exposes hits/misses/evictions (`.stats`).
* The catalog (tables, indexes, roots) is itself stored in a B-Tree on
  page 1 — schema survives restart by construction.
* Rollback journal: every transaction writes original page images to a
  journal before modifying pages; commit flushes data then deletes the
  journal, rollback (or crash recovery at open) restores the originals.
  The journal header carries a record count and checksum; a corrupt
  journal is a hard error, never silently applied.
* Errors are typed (`LexError`, `ParseError`, `SchemaError`,
  `PlanError`, `TypeError`, `ConstraintError`, `ExecutionError`,
  `StorageError`, `BTreeError`) with actionable messages; every
  on-disk structure is bounds-checked and checksummed, and malformed
  disk data fails loudly.

## Tests

```sh
make test       # full suite under AddressSanitizer + UndefinedBehaviorSanitizer
make test-fast  # same suite without sanitizers (fast iteration)
```

126 tests: lexer, parser, value/record codec, page, pager, B-Tree
(structure, split/borrow/merge, overflow chains, differential testing
against a reference model), SQL end-to-end, transactions (durability,
statement-level rollback, journal recovery, crash abandonment,
corruption), restart persistence, disk corruption (13 scenarios, each
verified to fail loudly with a typed error), property-based
differential testing against an in-memory reference model, and the §30
end-to-end acceptance scenario.

## Benchmarks

```sh
./build/bench              # 10k / 50k / 100k rows
./build/bench 200000       # custom size
```

Whole-engine throughput through the SQL layer (parse → plan → execute →
B-Tree → pager), g++ 12.2 -O2:

| N      | load seq (rows/s) | load random (rows/s) | scan (rows/s) | point pk (lookups/s) | validate (rows/s) |
|--------|------------------:|---------------------:|--------------:|---------------------:|------------------:|
| 10k    | ~199,000          | ~125,000             | ~2,060,000    | ~297,000             | ~511,000          |
| 50k    | ~184,000          | ~97,000              | ~1,700,000    | ~133,000             | ~306,000          |
| 100k   | ~161,000          | ~62,000              | ~1,710,000    | ~108,000             | ~263,000          |

A 100k-row table is a 3-level B-Tree in ~3,000 pages; restart + full
scan of 100k rows takes ~50 ms. Full details from `./build/bench`.

## Repository layout

```
src/     engine: lexer, parser, planner, executor, catalog,
         record codec, B-Tree, pager, journal, database facade,
         CLI shell (main.cpp), benchmark harness (bench.cpp)
tests/   12 self-registering test binaries, one Makefile target
Makefile build: dbx, bench, test (ASan+UBSan), test-fast
```

## Known limitations

* Only `INTEGER PRIMARY KEY` (rowid alias) — no TEXT/composite primary
  keys and no UNIQUE constraints on other columns.
* No joins, no subqueries, no GROUP BY/HAVING, no aggregates beyond
  COUNT/MIN/MAX/SUM (no AVG).
* No expression indexes, no partial indexes; single-column indexes only.
* Transactions provide atomicity + durability via the rollback journal;
  no concurrency (single process), no WAL.
* 64-bit integers and TEXT only (no REAL, BLOB, or large TEXT beyond
  overflow pages' limits).
* The planner chooses between full scans and index lookups/scans; no
  cost model, no join ordering (nothing to order).

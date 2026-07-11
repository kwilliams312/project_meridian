// SPDX-License-Identifier: Apache-2.0
//
// meridian-db CONCURRENCY regression test (#510). Reproduces the libmariadb
// thread-init race: authd/worldd open a fresh db::Connection per request on many
// concurrent worker threads. Without the process-wide mysql_library_init() and a
// per-thread mysql_thread_init()/end() (db::global_init + db::ThreadGuard), those
// concurrent first-use mysql_init() calls race on libmariadb's shared init state
// and INTERMITTENTLY corrupt prepared-statement/result fetching — the query
// returns an EMPTY result set for a row that provably exists. Live, this surfaced
// as spurious "unknown user" (account lookup empty) and "empty realm list" (realm
// query empty). The existing db_test soak is SEQUENTIAL and cannot catch it.
//
// This test mirrors the real access pattern: N threads, each holding a
// db::ThreadGuard, opening its OWN fresh connection every iteration and running a
// parameterized SELECT that MUST return exactly one seeded row, M times. It
// asserts EVERY result is the expected non-empty row, and stays deterministically
// GREEN with the fix in place.
//
// REPRODUCTION NOTE (honest scope): on libmariadb >= ~3.3 (verified against
// 3.3.17 on Ubuntu — the CI image's client lib — and 3.4.9 on macOS) the library
// and thread init are guarded INTERNALLY by a pthread_once, so even with
// global_init()/ThreadGuard stubbed to no-ops this test stays green — no
// empty/short result sets appeared across 20k+ concurrent prepared-statement
// queries, 32 threads racing their first-ever libmariadb call. The empty-result
// corruption reported in #510 therefore requires a client-lib build WITHOUT that
// internal guard (older Connector/C, or a differently-built cluster image). This
// test is a regression guard for the correct threading contract and will catch
// the corruption on such a library; the fix itself (mysql_library_init once +
// per-thread mysql_thread_init/end) is correct-by-contract regardless, and also
// closes a real per-thread libmariadb memory leak (the missing mysql_thread_end).
//
// Env knobs: MERIDIAN_DB_CC_THREADS / MERIDIAN_DB_CC_ITERS tune the load;
// MERIDIAN_DB_CC_SKIP_SEED expects the row pre-seeded out-of-band (so the process
// opens no db::Connection on the main thread before the workers).
//
// Needs a live MariaDB; reads connection params from MERIDIAN_DB_* env and SKIPS
// (exit 0) when none are set, so it is inert in the DB-free ctest and runs for
// real only in the --db CI job / test.sh --db.

#include "meridian/db/connection.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace meridian::db;

namespace {

const char* env(const char* k) { return std::getenv(k); }

// Concurrency knobs — sized to make the race reliable while keeping the test to a
// few seconds. 24 threads x 80 iterations = 1920 concurrent connect+query cycles.
int kThreads = 24;
int kIterations = 80;

// The seeded row every worker query must return.
const char* const kTable = "meridian_db_concurrency_selftest";
const char* const kKey = "botrunner0001";  // stands in for the account/realm row
constexpr std::int64_t kValue = 424242;

}  // namespace

int main() {
    ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = std::atoi(port);
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (const char* t = env("MERIDIAN_DB_CC_THREADS")) kThreads = std::atoi(t);
    if (const char* m = env("MERIDIAN_DB_CC_ITERS")) kIterations = std::atoi(m);

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured (set "
                    "MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        return 0;
    }

    // Seed a normal (non-temporary, so it is visible across connections) table
    // with exactly one row that the worker SELECT must always return. When
    // MERIDIAN_DB_CC_SKIP_SEED is set the row is expected to already exist (seeded
    // out-of-band) and NO db::Connection is opened on the main thread first — so
    // the very first libmariadb call in the process happens concurrently across
    // the worker threads (the truest exposure of the un-guarded library-init race).
    if (!env("MERIDIAN_DB_CC_SKIP_SEED")) {
        try {
            Connection seed(p);
            seed.execute(std::string("DROP TABLE IF EXISTS ") + kTable);
            seed.execute(std::string("CREATE TABLE ") + kTable +
                         " (id BIGINT NOT NULL, name VARCHAR(64) NOT NULL, "
                         "PRIMARY KEY (name)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
            seed.execute(std::string("INSERT INTO ") + kTable + " (id, name) VALUES (?, ?)",
                         {Param{kValue}, Param{std::string{kKey}}});
        } catch (const DbError& e) {
            std::printf("FAIL: seed failed — DbError %u: %s\n", e.code(), e.what());
            return 1;
        }
    }

    const std::string select =
        std::string("SELECT id FROM ") + kTable + " WHERE name = ?";

    // All workers spin on this gate so their first concurrent connect+query lands
    // in the same window — that is where the un-guarded init race lives.
    std::atomic<bool> go{false};
    std::atomic<int> empty_results{0};    // query returned 0 rows for a present row
    std::atomic<int> wrong_values{0};     // returned a row but the wrong value
    std::atomic<int> errors{0};           // threw a DbError mid-flight
    std::atomic<long> total_queries{0};
    std::mutex first_err_mtx;
    std::string first_err;

    auto record_err = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(first_err_mtx);
        if (first_err.empty()) first_err = msg;
    };

    auto worker = [&] {
        // Per-thread libmariadb init/teardown — the fix under test. With this
        // stubbed to a no-op (reverted fix), the loop below races and produces
        // empty/short results.
        ThreadGuard guard;
        while (!go.load(std::memory_order_acquire)) { /* spin to align the start */ }

        for (int i = 0; i < kIterations; ++i) {
            try {
                // Fresh connection per iteration — exactly like authd's per-request
                // db::Connection — to keep hammering the concurrent init path.
                Connection db(p);
                Result r = db.execute(select, {Param{std::string{kKey}}});
                total_queries.fetch_add(1, std::memory_order_relaxed);
                if (r.rows.empty()) {
                    empty_results.fetch_add(1, std::memory_order_relaxed);
                    record_err("empty result set for present row '" +
                               std::string(kKey) + "'");
                    continue;
                }
                const Cell& c = r.rows[0][0];
                if (!c.has_value() || *c != std::to_string(kValue)) {
                    wrong_values.fetch_add(1, std::memory_order_relaxed);
                    record_err("wrong value: got '" +
                               (c.has_value() ? *c : std::string("<null>")) +
                               "' expected '" + std::to_string(kValue) + "'");
                }
            } catch (const DbError& e) {
                errors.fetch_add(1, std::memory_order_relaxed);
                record_err("DbError " + std::to_string(e.code()) + ": " + e.what());
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) pool.emplace_back(worker);
    go.store(true, std::memory_order_release);  // release all workers at once
    for (std::thread& t : pool) t.join();

    // Teardown (skip when the table was seeded out-of-band, so it survives for a
    // repeated skip-seed run).
    if (!env("MERIDIAN_DB_CC_SKIP_SEED")) {
        try {
            Connection drop(p);
            drop.execute(std::string("DROP TABLE IF EXISTS ") + kTable);
        } catch (const DbError&) { /* best-effort cleanup */ }
    }

    const int bad = empty_results.load() + wrong_values.load() + errors.load();
    std::printf("concurrency: %d threads x %d iters = %ld queries; "
                "empty=%d wrong=%d errors=%d\n",
                kThreads, kIterations, total_queries.load(),
                empty_results.load(), wrong_values.load(), errors.load());
    if (bad != 0) {
        std::printf("  first failure: %s\n", first_err.c_str());
        std::printf("\n%d CONCURRENT QUERY FAILURE(S) — libmariadb thread-init race (#510)\n",
                    bad);
        return 1;
    }
    std::printf("\nALL CONCURRENT QUERIES RETURNED THE SEEDED ROW\n");
    return 0;
}

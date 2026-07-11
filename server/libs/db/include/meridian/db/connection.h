// SPDX-License-Identifier: Apache-2.0
//
// meridian-db — MariaDB access for the server (server SAD §2.2, TD-05).
//
// M0 scope: a synchronous connection with parameterized (prepared-statement)
// queries. authd is not the tick loop, so synchronous DB calls are fine here;
// the async worker-pool + per-DB connection pools the SAD describes for worldd
// (§2.2, "no synchronous DB calls from the update loop") wrap this layer later.
//
// Safety rule (backend standards): user input is NEVER concatenated into SQL —
// every value binds through a prepared-statement parameter.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace meridian::db {

// Connection target. If `unix_socket` is set it takes precedence over host/port.
struct ConnectParams {
    std::string host = "127.0.0.1";
    unsigned port = 3306;
    std::string user;
    std::string password;
    std::string database;
    std::string unix_socket;
};

// Bytes for blob params / results (BINARY/VARBINARY columns).
using Bytes_t = std::vector<std::uint8_t>;

// A bound parameter. NULL is std::monostate; integers, doubles, text and blobs
// bind by type. Strings are sent as-is (no escaping needed — they are values,
// not SQL) which is exactly what makes prepared statements injection-proof.
using Param = std::variant<std::monostate, std::int64_t, double, std::string, Bytes_t>;

// A result column value: text/blob as string, or null. Callers parse numerics
// from the string (kept minimal for M0; a typed accessor can come later).
using Cell = std::optional<std::string>;
struct Row {
    std::vector<Cell> cols;
    const Cell& operator[](std::size_t i) const { return cols.at(i); }
};

struct Result {
    std::vector<std::string> columns;     // column names, in order
    std::vector<Row> rows;                // empty for non-SELECT
    std::uint64_t affected_rows = 0;      // for INSERT/UPDATE/DELETE
    std::uint64_t last_insert_id = 0;     // AUTO_INCREMENT id after INSERT
};

// Thrown on connect/query failure — carries the MariaDB error.
class DbError : public std::runtime_error {
public:
    DbError(unsigned code, const std::string& msg)
        : std::runtime_error(msg), code_(code) {}
    unsigned code() const { return code_; }
private:
    unsigned code_;
};

// Process-wide libmariadb initialization (#510). libmariadb (Connector/C) keeps
// GLOBAL and THREAD-LOCAL state; its contract requires mysql_library_init() to
// run ONCE before any thread opens a connection, and mysql_thread_init() on
// every thread that then touches the client library. authd and worldd open a
// fresh Connection per request on many concurrent worker threads; without these
// calls, concurrent first-use mysql_init() races on libmariadb's shared init
// state and intermittently corrupts prepared-statement/result fetching, yielding
// EMPTY result sets for queries that must return rows (#510: intermittent
// "unknown user" and "empty realm list" under concurrent load).
//
// global_init() is idempotent (std::call_once): the first call runs
// mysql_library_init(); later calls are no-ops. Connection's constructor and
// ThreadGuard both call it, so it is automatic — but every daemon should ALSO
// call it explicitly from main() before spawning worker threads, so the one-time
// global init can never first happen concurrently on a worker's first connect.
// Throws DbError if mysql_library_init() fails.
void global_init();

// Release process-wide libmariadb resources (mysql_library_end). Call once at
// clean daemon/tool shutdown, after all Connections are destroyed and every
// worker thread (and its ThreadGuard) has exited. Safe to omit on a hard process
// exit, but required for a leak-clean teardown.
void global_end();

// RAII per-thread libmariadb initialization (#510). Every thread OTHER than the
// one that called global_init() must run mysql_thread_init() before its first
// DB use and mysql_thread_end() before it exits — otherwise it races on / leaks
// libmariadb thread-local state. Construct exactly one ThreadGuard at the top of
// each worker thread that opens or uses a Connection (e.g. authd's per-connection
// handler thread, worldd's IO-worker threads); its lifetime must bracket ALL DB
// use on that thread. Placing the guard in the db layer means no call site can
// forget the thread_end pairing. The ctor also calls global_init(), so a guard
// alone guarantees correct init order even if main() forgot the explicit call.
class ThreadGuard {
public:
    ThreadGuard();
    ~ThreadGuard();
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
    ThreadGuard(ThreadGuard&&) = delete;
    ThreadGuard& operator=(ThreadGuard&&) = delete;
};

// One MariaDB connection. RAII: connects on construction, closes on destruction.
// Not copyable; movable.
class Connection {
public:
    explicit Connection(const ConnectParams& params);
    ~Connection();

    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Execute `sql` with `params` bound to its ? placeholders. Returns rows for
    // a SELECT, or affected_rows / last_insert_id for a mutation.
    Result execute(std::string_view sql, const std::vector<Param>& params = {});

    // Liveness check (mysql_ping); reconnects transparently if configured.
    bool ping();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace meridian::db

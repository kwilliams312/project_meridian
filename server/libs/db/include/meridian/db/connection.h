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

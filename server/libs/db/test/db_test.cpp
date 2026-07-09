// SPDX-License-Identifier: Apache-2.0
//
// meridian-db integration test. Needs a live MariaDB; reads connection params
// from MERIDIAN_DB_* env and SKIPS (exit 0) when none are set. Proves the
// round-trip and — critically — that parameterized queries are injection-safe:
// a value containing a quote / SQL fragment is stored and returned verbatim.

#include "meridian/db/connection.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian::db;

namespace {
int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
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

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured (set "
                    "MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        return 0;
    }

    try {
        Connection db(p);
        std::printf("connected to MariaDB\n");
        check("ping", db.ping());

        db.execute("DROP TEMPORARY TABLE IF EXISTS meridian_db_selftest");
        db.execute(
            "CREATE TEMPORARY TABLE meridian_db_selftest ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY, "
            "name VARCHAR(128) NOT NULL, n BIGINT, blob_col VARBINARY(32)) "
            "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        // Insert via bound params — including a value that would break naive
        // string concatenation. If it survives verbatim, injection is impossible.
        const std::string evil = "Robert'); DROP TABLE students;--";
        Bytes_t blob{0x00, 0xFF, 0x10, 0x00, 0x42};  // embedded NULs
        Result ins = db.execute(
            "INSERT INTO meridian_db_selftest (name, n, blob_col) VALUES (?, ?, ?)",
            {Param{evil}, Param{std::int64_t{-1234567890123}}, Param{blob}});
        check("insert affected 1 row", ins.affected_rows == 1);
        check("last_insert_id > 0", ins.last_insert_id > 0);

        db.execute("INSERT INTO meridian_db_selftest (name, n) VALUES (?, ?)",
                   {Param{std::string{"second"}}, Param{std::monostate{}}});  // NULL n

        // Read back the evil row by parameterized lookup.
        Result r = db.execute(
            "SELECT id, name, n, blob_col FROM meridian_db_selftest WHERE name = ?",
            {Param{evil}});
        check("select returned exactly 1 row", r.rows.size() == 1);
        check("columns named", r.columns.size() == 4 && r.columns[1] == "name");
        if (r.rows.size() == 1) {
            const Row& row = r.rows[0];
            check("injection string stored verbatim", row[1].has_value() && *row[1] == evil);
            check("negative int round-trips", row[2].has_value() && *row[2] == "-1234567890123");
            check("blob round-trips (with NULs)",
                  row[3].has_value() && row[3]->size() == blob.size() &&
                      static_cast<unsigned char>((*row[3])[1]) == 0xFF);
        }

        // NULL handling.
        Result nr = db.execute(
            "SELECT n FROM meridian_db_selftest WHERE name = ?", {Param{std::string{"second"}}});
        check("NULL column reads as nullopt",
              nr.rows.size() == 1 && !nr.rows[0][0].has_value());

        // Aggregate.
        Result cnt = db.execute("SELECT COUNT(*) FROM meridian_db_selftest");
        check("count = 2", cnt.rows.size() == 1 && *cnt.rows[0][0] == "2");

        db.execute("DROP TEMPORARY TABLE meridian_db_selftest");

        // FLOAT / DOUBLE / DECIMAL result columns must read back non-empty and
        // round-trip (#393). Before the fix the result layer bound every column
        // as a zero-length string buffer; FLOAT/DOUBLE reported length 0 and came
        // back as an empty cell, silently yielding 0 for any FLOAT SELECT.
        db.execute("DROP TEMPORARY TABLE IF EXISTS meridian_db_numtest");
        db.execute(
            "CREATE TEMPORARY TABLE meridian_db_numtest ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY, "
            "f FLOAT, d DOUBLE, dec_col DECIMAL(12,4)) "
            "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        // 2.5 is exactly representable as FLOAT; the DOUBLE and DECIMAL carry
        // more digits so the round-trip is a real conversion, not a lucky zero.
        const double kFloat = 2.5;
        const double kDouble = 123.4567890123;
        db.execute(
            "INSERT INTO meridian_db_numtest (f, d, dec_col) VALUES (?, ?, ?)",
            {Param{kFloat}, Param{kDouble}, Param{std::string{"1234.5678"}}});

        Result fr = db.execute("SELECT f, d, dec_col FROM meridian_db_numtest");
        check("float select returned 1 row", fr.rows.size() == 1);
        if (fr.rows.size() == 1) {
            const Row& row = fr.rows[0];
            check("FLOAT column non-empty", row[0].has_value() && !row[0]->empty());
            check("DOUBLE column non-empty", row[1].has_value() && !row[1]->empty());
            check("DECIMAL column non-empty", row[2].has_value() && !row[2]->empty());
            double f = (row[0].has_value() && !row[0]->empty()) ? std::stod(*row[0]) : 0.0;
            double d = (row[1].has_value() && !row[1]->empty()) ? std::stod(*row[1]) : 0.0;
            check("FLOAT round-trips (2.5)", std::fabs(f - kFloat) < 1e-4);
            check("DOUBLE round-trips (123.4567890123)", std::fabs(d - kDouble) < 1e-9);
            check("DECIMAL round-trips (1234.5678)",
                  row[2].has_value() && *row[2] == "1234.5678");
        }
        db.execute("DROP TEMPORARY TABLE meridian_db_numtest");
    } catch (const DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL DB TESTS PASSED\n" : "\n%d DB TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

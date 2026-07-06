// SPDX-License-Identifier: Apache-2.0
//
// meridian-db implementation over the MariaDB Connector/C (libmariadb).
// Prepared statements throughout — parameters bind by value, never concatenated.

#include "meridian/db/connection.h"

#include <mysql.h>

#include <cstring>

namespace meridian::db {

namespace {

[[noreturn]] void throw_mysql(MYSQL* c) {
    throw DbError(mysql_errno(c), mysql_error(c));
}

[[noreturn]] void throw_stmt(MYSQL_STMT* s) {
    throw DbError(mysql_stmt_errno(s), mysql_stmt_error(s));
}

}  // namespace

struct Connection::Impl {
    MYSQL* conn = nullptr;
};

Connection::Connection(const ConnectParams& p) {
    impl_ = new Impl();
    impl_->conn = mysql_init(nullptr);
    if (!impl_->conn) throw DbError(0, "mysql_init failed");

    // Reconnect + utf8mb4 to match the schema charset.
    bool reconnect = true;
    mysql_options(impl_->conn, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(impl_->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    const char* host = p.unix_socket.empty() ? p.host.c_str() : nullptr;
    const char* sock = p.unix_socket.empty() ? nullptr : p.unix_socket.c_str();
    if (!mysql_real_connect(impl_->conn, host, p.user.c_str(), p.password.c_str(),
                            p.database.empty() ? nullptr : p.database.c_str(),
                            p.port, sock, 0)) {
        DbError e(mysql_errno(impl_->conn), mysql_error(impl_->conn));
        mysql_close(impl_->conn);
        delete impl_;
        throw e;
    }
}

Connection::~Connection() {
    if (impl_) {
        if (impl_->conn) mysql_close(impl_->conn);
        delete impl_;
    }
}

Connection::Connection(Connection&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
Connection& Connection::operator=(Connection&& o) noexcept {
    if (this != &o) {
        if (impl_) {
            if (impl_->conn) mysql_close(impl_->conn);
            delete impl_;
        }
        impl_ = o.impl_;
        o.impl_ = nullptr;
    }
    return *this;
}

bool Connection::ping() { return mysql_ping(impl_->conn) == 0; }

Result Connection::execute(std::string_view sql, const std::vector<Param>& params) {
    MYSQL* c = impl_->conn;
    MYSQL_STMT* stmt = mysql_stmt_init(c);
    if (!stmt) throw_mysql(c);

    struct StmtGuard {
        MYSQL_STMT* s;
        ~StmtGuard() { mysql_stmt_close(s); }
    } guard{stmt};

    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
        throw_stmt(stmt);

    // ---- Bind input parameters -------------------------------------------
    std::vector<MYSQL_BIND> in(params.size());
    // Stable storage for scalar params through execute().
    std::vector<std::int64_t> ints(params.size());
    std::vector<double> dbls(params.size());
    std::vector<unsigned long> lens(params.size());
    if (!params.empty()) std::memset(in.data(), 0, in.size() * sizeof(MYSQL_BIND));

    for (std::size_t i = 0; i < params.size(); ++i) {
        MYSQL_BIND& b = in[i];
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    b.buffer_type = MYSQL_TYPE_NULL;
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    ints[i] = v;
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.buffer = &ints[i];
                } else if constexpr (std::is_same_v<T, double>) {
                    dbls[i] = v;
                    b.buffer_type = MYSQL_TYPE_DOUBLE;
                    b.buffer = &dbls[i];
                } else if constexpr (std::is_same_v<T, std::string>) {
                    lens[i] = static_cast<unsigned long>(v.size());
                    b.buffer_type = MYSQL_TYPE_STRING;
                    b.buffer = const_cast<char*>(v.data());
                    b.buffer_length = lens[i];
                    b.length = &lens[i];
                } else {  // Bytes_t
                    lens[i] = static_cast<unsigned long>(v.size());
                    b.buffer_type = MYSQL_TYPE_BLOB;
                    b.buffer = const_cast<std::uint8_t*>(v.data());
                    b.buffer_length = lens[i];
                    b.length = &lens[i];
                }
            },
            params[i]);
    }
    if (!params.empty() && mysql_stmt_bind_param(stmt, in.data()) != 0)
        throw_stmt(stmt);

    if (mysql_stmt_execute(stmt) != 0) throw_stmt(stmt);

    Result result;
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) {
        // Non-SELECT: report affected rows + last insert id.
        result.affected_rows = mysql_stmt_affected_rows(stmt);
        result.last_insert_id = mysql_stmt_insert_id(stmt);
        return result;
    }

    struct MetaGuard {
        MYSQL_RES* m;
        ~MetaGuard() { mysql_free_result(m); }
    } mguard{meta};

    unsigned ncols = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
    for (unsigned i = 0; i < ncols; ++i)
        result.columns.emplace_back(fields[i].name, fields[i].name_length);

    // Bind outputs with zero-length buffers to learn each cell's length, then
    // fetch each column into a right-sized buffer (handles arbitrary widths).
    std::vector<MYSQL_BIND> out(ncols);
    std::vector<unsigned long> out_len(ncols, 0);
    std::vector<my_bool> out_null(ncols, 0);
    std::vector<my_bool> out_err(ncols, 0);
    std::memset(out.data(), 0, out.size() * sizeof(MYSQL_BIND));
    for (unsigned i = 0; i < ncols; ++i) {
        out[i].buffer_type = MYSQL_TYPE_STRING;
        out[i].buffer = nullptr;
        out[i].buffer_length = 0;
        out[i].length = &out_len[i];
        out[i].is_null = &out_null[i];
        out[i].error = &out_err[i];
    }
    if (mysql_stmt_bind_result(stmt, out.data()) != 0) throw_stmt(stmt);

    for (;;) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1) throw_stmt(stmt);
        // rc == 0 or MYSQL_DATA_TRUNCATED (expected: zero-length output buffers).
        Row row;
        row.cols.resize(ncols);
        for (unsigned i = 0; i < ncols; ++i) {
            if (out_null[i]) {
                row.cols[i] = std::nullopt;
                continue;
            }
            std::string cell(out_len[i], '\0');
            MYSQL_BIND fetch{};
            fetch.buffer_type = MYSQL_TYPE_STRING;
            fetch.buffer = cell.data();
            fetch.buffer_length = out_len[i];
            unsigned long real_len = 0;
            fetch.length = &real_len;
            if (mysql_stmt_fetch_column(stmt, &fetch, i, 0) != 0) throw_stmt(stmt);
            row.cols[i] = std::move(cell);
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

}  // namespace meridian::db

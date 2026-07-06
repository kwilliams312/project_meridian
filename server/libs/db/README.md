# `meridian-db` — MariaDB access (#75)

Server-side database access over the MariaDB Connector/C (libmariadb),
server SAD §2.2 / TD-05.

**M0 scope:** a synchronous `Connection` with **parameterized (prepared-statement)
queries**. `authd` is not the 20 Hz tick loop, so synchronous DB calls are fine
here — the async worker-pool + per-DB connection pools the SAD describes for
`worldd` ("no synchronous DB calls from the update loop") wrap this layer later.

## Safety

Every value binds through a prepared-statement parameter — **user input is never
concatenated into SQL** (backend standards). The test proves it: a value
containing `Robert'); DROP TABLE students;--` is stored and returned verbatim.

## API

```cpp
meridian::db::Connection db({.unix_socket = "/run/mysqld/mysqld.sock",
                             .user = "meridian", .database = "auth"});
auto r = db.execute("SELECT srp_salt, srp_verifier FROM account WHERE username = ?",
                    {std::string{name}});
if (!r.rows.empty()) { /* r.rows[0][0], r.rows[0][1] ... */ }
```

`execute()` returns `rows` for a SELECT (each cell an `optional<string>`, NULL →
`nullopt`) or `affected_rows` / `last_insert_id` for a mutation.

## Build / test

Needs libmariadb (`pkg-config libmariadb`; Homebrew `mariadb` or apt
`libmariadb-dev`). The integration test reads `MERIDIAN_DB_*` env vars and
**skips (exit 0) when none are set**, so it is inert in the plain server build's
`ctest` and runs for real only in the DB CI job (a `mariadb:11` service) or
locally:

```
MERIDIAN_DB_SOCKET=/tmp/mmdb.sock MERIDIAN_DB_USER=root MERIDIAN_DB_NAME=auth \
  ctest --test-dir build -R db --output-on-failure
```

## Not yet

Async worker pool + connection pools (worldd, §2.2); typed column accessors; the
`save_epoch` compare-and-set write helpers (§4.7, M2). This is the connection +
safe-query core the auth flow (#77/#79) builds on.

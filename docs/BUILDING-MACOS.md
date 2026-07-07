<!-- SPDX-License-Identifier: Apache-2.0 -->
# Building & running Project Meridian natively on macOS

This is the **native macOS build/test/run path** for the server, `mcc`, and
(optionally) the client GDExtension — a developer on an Apple-Silicon Mac can
build, test, and run the whole M0 stack locally with a couple of commands.

> **Why this exists.** There is no macOS CI runner yet (that's **#61 / A-16**,
> owner-assigned, not provisioned). Until it lands, **these scripts are the macOS
> verification path** — the local substitute for CI. The Linux/CI path uses
> Docker (`deploy/docker/`, #215); this is the non-Docker alternative for Mac
> devs. When #61 lands it will wire the same steps into a runner.

Everything lives under [`scripts/dev/`](../scripts/dev/):

| Script | Does |
|--------|------|
| `setup-macos.sh` | Checks/installs Homebrew deps + `uv`, verifies versions, one-time git-lfs + (optional) godot-cpp submodule setup. Idempotent. |
| `build.sh` | Builds server (`authd`, `worldd`, `meridian-account`) + `mcc` (+ `--client`). |
| `test.sh` | DB-free suite always; `--db` spins up a throwaway MariaDB, loads all schemas, runs the DB-backed tests, tears it down. |
| `run-local.sh` | Brings up a local realm (MariaDB + cert + test account + `authd` + `worldd`). `--smoke` verifies + tears down; `--stop` stops a backgrounded realm. |

---

## Prerequisites

- **macOS on Apple Silicon (arm64).** The server/`mcc` build works on Intel Macs
  too, but the **client GDExtension is arm64-only** (Client SAD §9.6, D-28).
- **[Homebrew](https://brew.sh).**
- **Xcode Command Line Tools** (`xcode-select --install`) for the AppleClang C++20
  toolchain.

`setup-macos.sh` installs the rest:

| Formula | Used for |
|---------|----------|
| `cmake`, `ninja` | build system + generator |
| `openssl@3` | `meridian-srp`, `meridian-net` (TLS 1.3) |
| `flatbuffers` | `meridian-proto` (`flatc` codegen from `schema/net/*.fbs`) |
| `mariadb` | `meridian-db` (libmariadb) + the local throwaway DB (`mariadbd`) |
| `git-lfs` | LFS-tracked assets |
| `uv` (astral) | Python content validator + pytest suite |

Client-only (`--client`): the pinned **godot-cpp** submodule (#158).

---

## Quick start

```bash
# 1. One-time setup (idempotent — safe to re-run).
scripts/dev/setup-macos.sh              # add --client to also init godot-cpp

# 2. Build server + mcc.
scripts/dev/build.sh                    # add --client for the GDExtension

# 3. Test.
scripts/dev/test.sh                     # DB-free suite (fast)
scripts/dev/test.sh --db                # + throwaway-MariaDB-backed tests

# 4. Run a local realm and smoke it.
scripts/dev/run-local.sh --smoke        # start → verify reachable → tear down
scripts/dev/run-local.sh                # start in background (Stop: --stop)
```

Build trees land under `build/` (gitignored). Throwaway DB datadirs and daemon
logs land under `.dev-run/` (gitignored).

---

## The build recipe (what the scripts do under the hood)

If you want to build by hand, this is exactly what `build.sh` runs. The two
environment variables are the load-bearing part on macOS (see **Troubleshooting**).

```bash
# libmariadb ships a pkg-config file (not a CMake config):
export PKG_CONFIG_PATH="$(brew --prefix mariadb)/lib/pkgconfig"
# openssl@3 + flatbuffers are found via find_package(CONFIG):
export CMAKE_PREFIX_PATH="$(brew --prefix openssl@3);$(brew --prefix flatbuffers)"

# Server → authd, worldd, meridian-account
cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/server

# mcc
cmake -S tools/mcc -B build/mcc -G Ninja
cmake --build build/mcc
```

Produced binaries (note the per-target subdirs):

```
build/server/authd/authd
build/server/worldd/worldd
build/server/tools/meridian-account/meridian-account
build/mcc/mcc
```

### Client GDExtension (optional, arm64-only)

```bash
git submodule update --init --recursive client/godot-cpp   # or setup-macos.sh --client
cmake -S client -B build/client -DGODOTCPP_TARGET=template_debug
cmake --build build/client          # compiles godot-cpp too — slow the first time
```

See [`client/README.md`](../client/README.md) for the full client story (engine
pins, SCons parity path, Godot export).

---

## Testing

`test.sh` (no flag) runs the **DB-free** suite:

- `ctest --test-dir build/server` — the DB integration tests (`authd-login`,
  `worldd-session`, `worldd-relay`, `meridian-db`, `account`) **self-SKIP** when
  no `MERIDIAN_DB_*` env is set, so this is fast and hermetic. Pure tests
  (`srp`, `net-tls`, `worldd-dispatch`, `worldd-movement`, `worldd-aoi`) run.
- `ctest --test-dir build/mcc` — `mcc` unit tests.
- `uv run pytest -q` — Python validator/doc-sync suite.
- `uv run tools/validate_content.py --assets error` — content-tree validation.
- `uv run tools/check_traceability.py` — PRD ↔ baseline traceability.

`test.sh --db` **additionally**:

1. Spins up a throwaway `mariadbd` (temp datadir, port **3307**, socket
   `/tmp/mmdb.sock`) — never touches a system MySQL/MariaDB.
2. Creates + loads `meridian_auth`, `meridian_characters`, `meridian_world`
   (auth/characters `*.up.sql` migrations + the `schema/sql/world/*.sql` DDL).
3. Re-runs `ctest --test-dir build/server` with `MERIDIAN_DB_*` set so the
   DB-backed tests run **for real** (they connect, insert, and assert).
4. Tears the DB down and removes the datadir (`--keep-db` to keep it).

---

## Running a local realm

`run-local.sh` brings up a complete M0 realm:

1. Throwaway MariaDB + all three schemas (as above).
2. A self-signed TLS cert (`scripts/dev/certs/server.{crt,key}`, generated once,
   gitignored).
3. A test account via `meridian-account` (`devtester` / `devpassword`).
4. `authd` (IF-1) and `worldd` (IF-2), backgrounded, each confirmed reachable
   with a real **TLS 1.3** handshake.

Modes:

```bash
scripts/dev/run-local.sh --smoke        # start, verify both reachable, tear down, exit
scripts/dev/run-local.sh                # start in background, print ports, return
scripts/dev/run-local.sh --foreground   # start and block until Ctrl-C (then tear down)
scripts/dev/run-local.sh --stop         # stop a backgrounded realm + remove datadir
```

---

## Ports & environment

| Component | Bind | Port | Notes |
|-----------|------|------|-------|
| `authd` (IF-1 login) | `127.0.0.1` | **7100** | TLS 1.3; SAD §5.1 default |
| `worldd` (IF-2 world) | `127.0.0.1` | **7200** | TLS 1.3 at M0; SAD §5.2 default |
| local MariaDB | `127.0.0.1` | **3307** | throwaway; socket `/tmp/mmdb.sock` |

The daemons read DB connection details from the environment (same variable names
as the tests):

| Env var | Meaning | Value the scripts use |
|---------|---------|-----------------------|
| `MERIDIAN_DB_HOST` | auth DB host | `127.0.0.1` |
| `MERIDIAN_DB_PORT` | auth DB port | `3307` |
| `MERIDIAN_DB_USER` | auth DB user | `root` |
| `MERIDIAN_DB_NAME` | auth DB name | `meridian_auth` |
| `MERIDIAN_DB_PASS` | auth DB password | *(unset — local root has none)* |
| `MERIDIAN_DB_SOCKET` | auth DB unix socket | *(unset — TCP used)* |
| `MERIDIAN_CHARDB_*` | characters DB (worldd, enter-world) | `…_NAME=meridian_characters` |

Toolkit-local overrides (rarely needed):

| Env var | Default | Meaning |
|---------|---------|---------|
| `MERIDIAN_DEV_DB_PORT` | `3307` | throwaway MariaDB port |
| `MERIDIAN_DEV_DB_SOCKET` | `/tmp/mmdb.sock` | throwaway MariaDB socket (keep it short — see below) |
| `MERIDIAN_DEV_DB_DATADIR` | `.dev-run/mariadb-data` | throwaway datadir |
| `MERIDIAN_DEV_DB_KEEP` | `0` | `1` keeps the datadir on teardown |

---

## Troubleshooting

**`Could NOT find PkgConfig` / libmariadb not found.**
libmariadb ships a `pkg-config` file, not a CMake config. Export
`PKG_CONFIG_PATH="$(brew --prefix mariadb)/lib/pkgconfig"` before configuring
(the scripts do this for you). Verify with `pkg-config --exists libmariadb`.

**`Could NOT find OpenSSL` / `Could NOT find FlatBuffers`.**
`openssl@3` is keg-only and `flatbuffers` is found via `find_package(CONFIG)`.
Pass both prefixes on `CMAKE_PREFIX_PATH`:
`CMAKE_PREFIX_PATH="$(brew --prefix openssl@3);$(brew --prefix flatbuffers)"`
(semicolon-separated — it's a CMake list, not a shell PATH).

**`Can't create UNIX socket` / socket path errors from MariaDB.**
macOS caps a Unix domain socket path at ~104 characters (`sockaddr_un.sun_path`).
A datadir deep under a worktree easily blows past that, so the toolkit forces a
short `/tmp/mmdb.sock`. If you override `MERIDIAN_DEV_DB_SOCKET`, keep it short
and in `/tmp` — `db_start` refuses paths ≥ 104 chars.

**`another MariaDB is already answering on /tmp/mmdb.sock`.**
A previous realm/test DB is still up (or crashed leaving a live process). Run
`scripts/dev/run-local.sh --stop`, or `rm -f /tmp/mmdb.sock` and kill the stray
`mariadbd`, then retry.

**`--cert and --key are required to serve`.**
`authd`/`worldd` are TLS listeners and need a cert+key. `run-local.sh` generates
a self-signed pair automatically; to build one by hand see
[`scripts/dev/certs/README.md`](../scripts/dev/certs/README.md).

**Client build fails on an Intel Mac.**
The client GDExtension is **Apple-Silicon-only** (Client SAD §9.6, D-28 rule 2).
The server and `mcc` still build on Intel; only `build.sh --client` is gated.

**`godot-cpp submodule missing`.**
Run `scripts/dev/setup-macos.sh --client` (or
`git submodule update --init --recursive client/godot-cpp`).

---

## Relationship to the Docker path

`deploy/docker/` (#215) is the reference **Linux/CI** bring-up: a `docker
compose` that runs `mariadb:11` + `authd` + `worldd` from published GHCR images.
This native path mirrors it without Docker — same schemas, same ports, same
`MERIDIAN_DB_*` contract — so a Mac dev gets an equivalent local realm. Prefer
whichever fits your box; the contracts are identical.

Until the macOS CI runner (**#61 / A-16**) exists, running `scripts/dev/build.sh`
+ `scripts/dev/test.sh --db` + `scripts/dev/run-local.sh --smoke` on a Mac **is**
the macOS verification.

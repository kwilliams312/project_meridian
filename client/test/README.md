# Client login flow — IF-1/IF-2 (issue #99)

The client half of IT-M0 authentication: connect to **authd** over TLS 1.3, run
the SRP-6a login, fetch the realm list, select a realm to receive a single-use
**SessionGrant**, then build the **IF-2 WorldHello** the client sends to worldd.
This is the client counterpart to the whole server auth path (authd #79 /
worldd #84).

## Design

Engine-free C++ core + a thin GDExtension/GDScript wrapper — the #102/#168/#107
pattern (Client SAD §9.2 "engine-agnostic cores").

| Layer | Files | Role |
|-------|-------|------|
| SRP-6a client | `src/srp_client_core.{h,cpp}` | Client side of SRP-6a: random `a`, compute `A`, derive `x`/`S`/`K`, produce `M1`, verify server `M2`. Mirrors `server/libs/srp` params **exactly** — RFC 5054 2048-bit group, g=2, SHA-256 — so it interoperates bit-for-bit. |
| IF-1 state machine + IF-2 kickoff | `src/login_core.{h,cpp}` | Drives `ClientHello→ServerHello`, `SrpStart→SrpChallenge`, `SrpProof→AuthResult` (verifies M2 — mutual auth), `RealmListRequest→RealmList`, `RealmSelect→SessionGrant`; then builds `WorldHello{grant_id, build, nonce, HMAC-SHA256(session_key, build‖nonce)}`. Transport is an injected `ILoginTransport` seam so it's testable without a socket. Encodes/decodes via `meridian::proto` (the same auth.fbs/world.fbs codegen the server uses). |
| TLS 1.3 transport | `src/login_transport.{h,cpp}` | `ILoginTransport` over an OpenSSL TLS 1.3 client socket + IF-1 u32-LE length framing (max 8 KiB), mirroring the server's `meridian-net` listener. |
| GDExtension binding | `src/meridian_login.{h,cpp}` | `MeridianLogin` (RefCounted): `login(host, port, user, password, realm_id) -> Dictionary{ok, grant_id, session_key, realm_id, realms, …}` + `build_world_hello_frame()`. All policy is in the core; the wrapper only marshals Godot types. |
| GDScript flow | `project/scenes/login/login_flow.gd`, `login_screen.{gd,tscn}` | Runs the login off the main thread (`Thread`), surfaces success (grant + realm) / failure (STATUS_* code) to a minimal login screen. UI is deliberately minimal — the FLOW + net core is the deliverable. |

### SRP client approach
Reuses the **algorithm** from `server/libs/srp` (same OpenSSL BIGNUM/EVP math,
same group/hash/PAD/notation) but implements the true client side with a
**cryptographically random** ephemeral `a` per login (the server's
`testing::client_side()` helper only takes a fixed `a` for RFC vectors). Mutual
auth is enforced: the client aborts (`kServerProofFailed`) if the server's `M2`
does not verify.

### IF-2 / M0 honesty
worldd at M0 validates the grant and **ignores** `WorldHello.proof`, and the IF-2
payload is not AEAD-wrapped on the wire yet (the `seal()` seam runs but writes
plaintext — `server/worldd/world_state.cpp`). The proof this core builds is
well-formed and future-proof, but not yet checked server-side. The TLS client
does **not** verify authd's cert chain at M0 (SRP mutual-auth authenticates the
server independently); cert pinning is a one-call follow-up.

## Tests

Engine-free unit test (no Godot, no socket, no DB — a mock server + a **real**
server-side `srp::ServerSession`):

```
cmake -S client -B build-test -DMERIDIAN_CLIENT_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/openssl@3
cmake --build build-test -j && ctest --test-dir build-test -R login-core --output-on-failure
```

**Killer interop test** — the client core driven over real TLS against a **real
authd** backed by a **real MariaDB** (boots a throwaway MariaDB on a UNIQUE
socket/port, seeds an account + realm, launches the real authd, drives the client
login core, verifies the grant is persisted + single-use):

```
client/test/run_authd_login_it.sh
```

Requires `mariadbd`/`mariadb-install-db`/`mariadb`, `cmake`, `flatc`, `openssl`,
`nc` on PATH. Inert as a plain ctest (`authd-login-it` SKIPs without the harness's
`--host`/`--port` args).

**Single-bot full-client harness (#111)** — the headless bot (`meridian-bot`)
driven over real TLS against a **real authd AND worldd**: login → enter-world →
MOVE. Boots a throwaway MariaDB (unique socket/port), seeds an account + realm,
launches both daemons, runs the bot, asserts `handshake_ok=1` + `moves_accepted>0`
+ the grant is consumed:

```
client/test/run_bot_client_it.sh
```

**Two-bot AoI E2E harness (#248) — the IT-M0 see-each-other-move capstone**
(§Step 3 / DC-4). Runs `meridian-two-bot`: TWO headless bots log into the real
authd, both enter the real worldd at the bootstrap spawn (within the 40 m AoI
enter radius, #87), rendezvous at a barrier so both are in-world, then both walk a
square path. Each bot **captures the OTHER's relayed EntityEnter (saw the peer on
login) + EntityUpdate (saw the peer MOVE)** via the #87 AoI relay. Asserts mutual
enter **and** mutual move both ways, distinct per-session guids (#87 fix), and both
grants consumed:

```
client/test/run_two_bot_it.sh
```

Both harnesses use a UNIQUE PID-keyed MariaDB socket/port (never the dev default
`/tmp/mmdb.sock:3307`) so they never collide with a dev DB or each other. They are
env-guarded integration tests — not wired into a fast CI job. The two-bot driver
prints a machine-readable `TWO_BOT_RESULT` line with each visibility leg
(`a_saw_b_enter`, `b_saw_a_move`, …) so the run is honest about full vs partial
mutual visibility.

The bot's inbound-entity CAPTURE (EntityEnter/Update/Leave → guid + position) is
proven WITHOUT a server by section 6 of the `bot-world-session` unit test (ctest,
`-DMERIDIAN_BOT=ON`); the two-bot harness is the live-server proof.

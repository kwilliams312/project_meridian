# Network Protocol Schemas (`/schema/net`)

FlatBuffers IDL for the two client-facing wire protocols the server **owns** and the client
**consumes**. These `.fbs` files are the single source of truth — every process boundary uses
code generated from them (Overview §3 principle 2: *schema-generated edges*, D-01). **No
hand-written serializers, no committed generated code.** `flatc`/`mcc` generate C++ (server,
worldd, gateway) and the client GDExtension bindings; golden round-trip fixtures (client SAD
§5.1 conformance) live alongside the schemas.

| File | Interface | Contract | Status |
|------|-----------|----------|--------|
| [`auth.fbs`](auth.fbs)  | IF-1 auth protocol  | server SAD §5.1 | M0 |
| [`world.fbs`](world.fbs) | IF-2 world protocol | server SAD §5.2 (+§5.2.1) | M0 skeleton |

`bus.fbs` (internal mesh envelope, server SAD §5.6) is a separate, server-internal contract and
is **not** part of this deliverable.

## Framing

Both protocols length-prefix each message with a **`u32` little-endian** length. The length
**excludes itself** and covers the entire rest of the frame.

### IF-1 (auth) — `auth.fbs`

```
┌──────────────┬───────────────────────────┐
│ u32 LE length │ FlatBuffer root table      │
└──────────────┴───────────────────────────┘
```

- Transport: **TLS 1.3 over TCP**, default port `7100`. Standard client cert verification.
- Max frame: **8 KiB** (auth messages are tiny).
- The message type is implicit in the FlatBuffer root table — the handshake is strictly
  sequenced (ClientHello → SrpStart → SrpProof → RealmSelect), so IF-1 carries no opcode field.

### IF-2 (world) — `world.fbs`

```
┌──────────────┬───────────┬─────────┬──────────────────────────┐
│ u32 LE length │ u16 opcode │ u64 seq │ AEAD(payload)            │
└──────────────┴───────────┴─────────┴──────────────────────────┘
                                        payload = FlatBuffer table for `opcode`
```

- Transport: **TCP**, default port `7200`, terminating at `gatewayd` from M2. **Not TLS** —
  confidentiality/integrity come from per-session AEAD (below).
- Max frame: **64 KiB client→server**, **256 KiB server→client** (entity-create + AoI-refresh
  bursts). Every inbound buffer passes the FlatBuffers **verifier** at the gateway before forwarding.
- `opcode` (u16) selects the payload table via the `Opcode` enum in `world.fbs`.
- `seq` (u64) is the per-direction sequence counter; it also feeds the AEAD nonce (below).

## Opcode-range map (`world.fbs Opcode`)

One FlatBuffer table per message; the u16 opcode encodes the domain in its high nibble. Values
are **stable wire identifiers** — append within a range, never renumber. Only M0 ranges have
concrete tables; the rest are reserved with commented placeholders in `world.fbs`.

| Range    | Domain            | M0? | Owner / future ref |
|----------|-------------------|-----|--------------------|
| `0x0xxx` | session / system  | ✅ M0 | WorldHello, HandshakeOk, Disconnect, **ClockSync (#65)** |
| `0x1xxx` | movement          | ✅ M0 | MovementIntent, MovementState (OPS-03 validation, SAD §5.5) |
| `0x2xxx` | entity state      | ✅ M0 | EntityEnter, EntityUpdate, EntityLeave |
| `0x3xxx` | combat            | ⛔ stub | ability cast / GCD (SAD §3.3, D-10) |
| `0x4xxx` | quest / gossip    | ⛔ stub | quest state, gossip menus |
| `0x5xxx` | inventory / loot / economy | ⛔ stub | inventory, loot rolls, vendor |
| `0x6xxx` | chat / social     | ⛔ stub | channels, whisper, social (SAD §3.8) |
| `0x7xxx` | group / instance  | ⛔ stub | party/raid, instance lifecycle |
| `0x8xxx` | shard / transfer  | ⛔ stub | ShardTransfer*, AoIRefresh*, ShardPresence (SAD §5.2.1, D-23, M3) |
| `0x9xxx` | world / area      | ✅ M1 | PoiDiscovered (area triggers + POI discovery; #368 WLD-01/03, epic #20) |
| `0xExxx` | GM                | ⛔ stub | GM commands / audit |
| `0xFxxx` | hot-reload        | ⛔ stub | IF-7 editor channel (SAD §5.4, M1) |

Generated dispatcher metadata per opcode (required state, thread, rate class) is enforced
worker-side; it is derived from `world.fbs`, not carried on the wire.

## Session handshake & keys (IF-3 → IF-2)

1. IF-1 ends by issuing a `SessionGrant{grant_id, session_key, reconnect_window_ms}` (auth.fbs).
   `reconnect_window_ms` is **issue #66** — the server-owned reconnect window, surfaced to the
   client (client SAD §5.1).
2. The client opens IF-2 and sends `WorldHello{grant_id, client_build, nonce, proof}` where
   `proof = HMAC-SHA256(session_key, client_build ‖ nonce)`.
3. The gateway replies `HandshakeOk{content_hash, server_proof}` where
   `server_proof = HMAC-SHA256(session_key, nonce ‖ content_hash)` — so the client authenticates
   the realm too.
4. `content_hash` compat: mismatch is a warning (M0–M1), a reject (M1+ test realm).

`ClockSync` (**issue #65**, client SAD §5.1) runs over the established session: the client sends
`{client_time_ms, server_time_ms=0}`; the server echoes both timestamps. The client filters the
offset samples into a server-clock estimate that keys all snapshot buffering and interpolation.

## Per-session AEAD (server SAD §5.2)

IF-2 payloads are encrypted with **ChaCha20-Poly1305**. Keys are derived per session, per
direction, from the IF-1 `session_key`:

```
k_c2s, k_s2c = HKDF-SHA256(session_key, info = "meridian-world-v1", direction)
nonce        = direction ‖ 64-bit sequence counter   (the frame's u64 seq)
```

- The `seq` field in the frame header **is** the AEAD nonce counter — it must be monotonic per
  direction and never reused under a given key (nonce-reuse is fatal for ChaCha20-Poly1305).
- Workers/services never see `session_key`; from M2 the **gateway** holds it and terminates AEAD
  (IF-3). D-22 clock-sync / reconnect-window / nonce-budget items remain in A-11 scope; transport
  stays abstracted in `libmeridian-proto`.

## Versioning & evolution

- `proto_ver` (u16) is checked at ClientHello (IF-1) and re-asserted at the IF-2 handshake.
- **Additive-field FlatBuffers evolution within a major version.** New fields are *appended* to a
  table with a default, so old readers skip them and new readers see the default from old writers.
  Rules: never remove or reorder existing fields; never change a field's type or id; never
  renumber an `Opcode`; deprecate rather than delete. Enums are append-only within their backing
  type.
- **Major versions are incompatible** — a mismatch is a hard reject (IF-1 `PROTOCOL_MISMATCH`,
  IF-2 `Disconnect{PROTOCOL_MISMATCH}`).

## Conventions

- `namespace meridian.net;` in every file. One **root table per message**.
- Explicit backing types on enums (`: uint16`) so wire size is pinned across generators.
- Byte blobs (keys, salts, proofs, hashes) are `[ubyte]`; identifiers are fixed-width unsigned
  scalars (`uint32`/`uint64`); world-space coordinates are `float`.
- Timestamps are `uint64` milliseconds, interpreted against the `ClockSync` server-clock estimate.

## Compiling / verifying

```sh
flatc --cpp -o build/gen schema/net/*.fbs      # server / worldd / gateway bindings
```

`flatc` is not required to be present locally; CI (and `mcc`) compile these and run the golden
round-trip fixtures. These schemas were authored and self-reviewed without a local `flatc` —
compilation is enforced in CI.

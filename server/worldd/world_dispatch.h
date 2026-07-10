// SPDX-License-Identifier: Apache-2.0
//
// worldd — IF-2 opcode dispatcher + world-process scaffold (issues #82, #83).
//
// CLEAN-ROOM: designed from the server SAD only — §2 / §2.5 (worldd component
// decomposition: the single world/update thread + a map/IO worker pool, "no
// synchronous DB calls from the update loop"), §5.2 (IF-2 framing + the u16
// Opcode registry with reserved per-domain ranges), §6 (concurrency model:
// world thread owns game state, map workers never touch sockets). The wire
// contract is schema/net/world.fbs. No GPL source (CMaNGOS / TrinityCore or
// otherwise) was consulted. See CONTRIBUTING.md.
//
// SCOPE (M0, this story): stand worldd up as a PROCESS that
//   1. accepts a TLS connection (meridian::net) and reads IF-2 length-framed
//      messages,
//   2. decodes the leading u16 Opcode (world.fbs), verifies the FlatBuffer
//      payload table, and dispatches to a handler in a dispatch table,
//   3. carries the world-thread + map/IO worker-pool STRUCTURE (accept/IO on
//      workers, game state owned by the world thread, a queue between them).
//
// DELIBERATELY NOT HERE (later stories): grant/session validation (#84),
// movement simulation (#86), AoI (#87), the real 20 Hz tick body, the message
// bus, AEAD session crypto. The M0 opcode handlers are STUBS that log/echo or
// return a not-yet-implemented marker; the real bodies land in those stories.
//
// M0 TRANSPORT NOTE: the SAD §5.2 IF-2 frame is
//   u32 LE length ‖ u16 opcode ‖ u64 seq ‖ AEAD(payload)
// At M0 the transport is meridian::net's TLS 1.3 Session, which owns the
// `u32 LE length` prefix (8 KiB cap). So the bytes we put INSIDE one net frame
// are the IF-2 message header + payload MINUS the AEAD wrap (TLS provides
// confidentiality/integrity at M0; per-session AEAD is #84):
//   [ u16 opcode (LE) ‖ u64 seq (LE) ‖ FlatBuffer table ]
// Encode/decode of this in-frame header lives in this file (encode_frame /
// decode_frame) so #84 can swap the AEAD wrap in at exactly one seam.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"
#include "meridian/trace/exporter.h"

#include <memory>

#include "ability_store.h"
#include "active_sessions.h"
#include "buyback.h"  // vendor::BuybackQueue — per-session buyback state (ECO-01, #370)
#include "combat_resolver.h"
#include "item_template.h"   // items::TemplateStore (content-store install seam, #390)
#include "loot_registry.h"  // shared corpse loot registry (ITM-02 wire; #388)
#include "loot_table.h"      // loot::LootTableStore (WorldServer::set_loot_tables, #390)
#include "map_tick.h"        // TickEvent — route_tick_events kill-credit routing (#396)
#include "movement_validation.h"
#include "npc_def.h"         // npc::NpcStore (content-store install seam, #390)
#include "quest_credit.h"  // MapTick→session quest-kill credit registry (QST-01 event-bus, #396)
#include "quest_log.h"  // QuestLog — per-session quest state machine (QST-01 #371)
#include "rate_class.h"  // OPS-03b per-opcode rate classes + per-session gate (#421)
#include "trainer.h"    // npc::LearnedAbilitySet — per-session learned abilities (NPC-02 #372)
#include "vendor_catalog.h"  // vendor::VendorCatalog (content-store install seam, #390)
#include "world_generated.h"
#include "world_metrics.h"
#include "world_session.h"
#include "world_state.h"

namespace meridian::worldd {

using net::Bytes;

// Install world-DB-backed content stores behind the M1 seams the QUEST/GOSSIP/VENDOR
// dispatch handlers read (item templates, vendor catalog, quest defs, npc defs),
// replacing the placeholder set (#390). Call ONCE at boot (main()) AFTER the IF-4
// manifest check, before serving; the referenced stores must outlive every served
// connection (main() owns them for the process lifetime). A nullptr leaves that seam
// on its placeholder default. Not thread-safe — a boot-time, single-threaded set.
void install_content_stores(const items::TemplateStore* item_store,
                            const vendor::VendorCatalog* vendor,
                            const QuestStore* quests,
                            const npc::NpcStore* npcs);

// IF-2 in-frame header size: u16 opcode + u64 seq, little-endian, ahead of the
// FlatBuffer payload (see the transport note above).
inline constexpr std::size_t kFrameHeaderBytes = sizeof(std::uint16_t) + sizeof(std::uint64_t);

// A decoded IF-2 frame: the opcode selector, the per-message sequence number,
// and the FlatBuffer payload bytes (the table for that opcode). `payload` is a
// view offset into the frame the caller still owns for the duration of the
// handler call.
struct Frame {
    net::Opcode opcode{};
    std::uint64_t seq = 0;
    const std::uint8_t* payload = nullptr;  // -> first payload byte
    std::size_t payload_len = 0;
};

// Encode an IF-2 frame body (header + payload) for Session::write_frame. The net
// layer prepends the u32 LE length; this prepends the u16 opcode + u64 seq.
Bytes encode_frame(net::Opcode opcode, std::uint64_t seq, const Bytes& payload);

// Decode the IF-2 in-frame header from a net frame body. Returns nullopt when the
// buffer is too short to even hold the header (a malformed/hostile frame) — the
// caller treats that as a protocol error and disconnects.
std::optional<Frame> decode_frame(const Bytes& frame);

// Convenience: build a Disconnect{reason,message} frame body (opcode DISCONNECT,
// seq echoed) ready for Session::write_frame. Used for the unknown/out-of-range
// opcode path and any clean server-initiated close.
Bytes make_disconnect(net::DisconnectReason reason, const std::string& message,
                      std::uint64_t seq = 0);

// Whether an opcode value falls in a range that is RESERVED (declared in
// world.fbs but with no M0 tables — combat 0x3xxx, quest 0x4xxx, … hot-reload
// 0xFxxx). Reserved opcodes are rejected cleanly (Disconnect), distinct from a
// truly unknown value, so a client probing a not-yet-implemented domain gets a
// defined answer rather than a crash.
bool is_reserved_range(std::uint16_t opcode);

// The outcome of dispatching one frame — drives the serve loop (keep the
// connection open, or send a Disconnect and close).
enum class DispatchOutcome {
    kHandled,        // a handler ran; keep serving
    kUnknownOpcode,  // opcode not in the table and not a known reserved range
    kReservedOpcode, // opcode is in a reserved (not-yet-implemented) range
    kBadPayload,     // opcode known but the FlatBuffer payload failed verify
    kMalformedFrame, // frame too short to hold the IF-2 header
};

// Per-connection context threaded through the dispatcher to each handler. It
// holds the state a handler needs that is scoped to ONE connection (never shared
// across connections, never on the stateless Dispatcher):
//   - `db` / `char_db`: this connection's DB handles (a worldd IO worker owns its
//     own DB connection, like authd — SAD §2.2). `db` is the auth DB (grants);
//     `char_db` is the characters DB (nullable at M0 — enter-world falls back to
//     a placeholder when absent). Either may be null in tests that stub the flow.
//   - `realm_id`: the realm this worldd serves (grants are realm-bound, SAD §5.3).
//   - `session`: the AEAD channel, established by the WORLD_HELLO handler after a
//     valid grant. std::nullopt until the handshake completes; post-handshake
//     frames are sealed/opened through it (the #82/#83 codec seam).
//   - `authenticated`: set true once WORLD_HELLO validated a grant. A second
//     WORLD_HELLO or a pre-handshake game opcode can be rejected on this.
//   - `disconnect`: a handler sets this (with a reason/message) to ask the serve
//     loop to send a Disconnect and close — the grant-reject path uses it so the
//     handler does not have to own socket-close policy (SAD §5.2 keeps that in
//     the serve loop).
//   - `movement` / `intake`: the per-session AUTHORITATIVE movement state (#86)
//     and the ≤ 10/s intent-rate gate. std::nullopt until a valid WORLD_HELLO —
//     movement is only processed AFTER a HandshakeOk (SAD §5.5: MovementIntent is
//     trusted only post-auth), so the MOVEMENT_INTENT handler rejects a
//     pre-handshake intent. The state is emplaced at enter-world with the
//     placeholder character's spawn position + guid. THREADING: at M0 one
//     connection is served start-to-finish by ONE IO worker (serve_connection
//     runs on the calling worker), so this per-connection movement state is
//     single-threaded by construction — the SAD's "game state on the world
//     thread" invariant (§2.5/§6) is honoured because no OTHER thread touches
//     this ConnCtx. The seam where authoritative state migrates onto the shared
//     world thread (keyed by session id, drained from the inbound queue) is the
//     WorldServer scaffold's queue; #87's AoI relay consumes `movement` from
//     there. At M0 it lives here, next to the WorldSession it is scoped to.
// Two-phase in-world session (server-authoritative characters, D-35 / #341).
// A valid WORLD_HELLO authenticates the session and leaves it at kCharSelect —
// it may list/create/delete its characters and ENTER_WORLD, but it is NOT
// spawned (no entity, no movement). An explicit ENTER_WORLD_REQUEST naming a
// character the account OWNS promotes it to kInWorld (spawned, AoI-registered,
// movement-enabled, holding the account's single in-world slot #326). The server
// never fabricates a character, so there is no implicit spawn at handshake.
enum class SessionPhase {
    kConnected,   // pre-handshake (no valid WORLD_HELLO yet)
    kCharSelect,  // authenticated, at character select — NOT spawned
    kInWorld,     // spawned via an ENTER_WORLD_RESPONSE(OK)
};

struct ConnCtx {
    db::Connection* db = nullptr;        // auth DB (session_grant) — may be null in tests
    db::Connection* char_db = nullptr;   // characters DB — nullable (ENTER_WORLD -> INTERNAL when null)
    std::uint32_t realm_id = 0;          // realm this worldd serves (0 = accept any)

    // Two-phase session state (D-35 / #341). Advances kConnected -> kCharSelect
    // (valid WORLD_HELLO) -> kInWorld (ENTER_WORLD OK). Movement + entity relay
    // are legal only at kInWorld; the ENTER_WORLD handler refuses a double-enter.
    SessionPhase phase = SessionPhase::kConnected;

    // OPS-05 session-flow traces (#166). When set, the WORLD_HELLO handler emits a
    // "worldd.enter_world" span (grant validate → session establish → HandshakeOk)
    // parented onto the authd login span via the grant-derived trace context
    // (trace::flow::trace_context_from_grant) — the two hops stitch into ONE
    // session-flow trace (D-29 §9 rule 5). NULL => no tracing (graceful
    // degradation; the handshake is unaffected). Borrowed; set by serve_connection.
    meridian::trace::Exporter* tracer = nullptr;

    // OPS-05 map-scoped metric labels (#164). Stamped by serve_connection from the
    // WorldServerConfig; every handler's metric call uses this so the emitted
    // series carry the {realm,zone,shard,map} the dashboards group by. Defaults to
    // the reference/0/0/bootstrap M0 labels (fine for the DB-less dispatch test).
    MetricsLabels labels;

    std::optional<WorldSession> session; // established on a valid WORLD_HELLO
    bool authenticated = false;

    std::optional<SessionMovementState> movement;  // authoritative movement (#86), post-auth
    MovementIntake intake;                          // ≤ 10/s intent-rate gate (#86)
    ChatIntake chat_intake;                         // chat rate class (OPS-03; #367)

    // OPS-03b per-opcode RATE CLASSES (#421). The connection-wide anti-flood gate
    // the dispatcher runs at the dispatch entry: every client opcode is assigned a
    // rate class (rate_class_for) with a per-session sliding-window ceiling. A frame
    // over its class ceiling is DROPPED (not disconnected) + flagged on the anti-
    // cheat audit stream (kRateLimited). Single-threaded on this connection's IO
    // worker (like `intake`/`chat_intake`), so it needs no locking. This is the
    // COARSE flood backstop above the finer per-feature caps (MovementIntake/
    // ChatIntake), which remain the authority for their own opcode (see rate_class.h).
    RateLimiter rate;

    // Vendors (ECO-01, #370). `char_id` is this session's spawned character
    // (== character.id, the currency/inventory key), captured at ENTER_WORLD; the
    // vendor buy/sell/buyback handlers act on it against ctx.char_db (server-
    // authoritative — never a client-supplied character). `buyback` is THIS
    // session's per-connection buyback queue (recently-sold items repurchasable
    // for a bounded window; single-threaded, like `chat_intake`/`combat`). Both
    // are inert until kInWorld — char_id is 0 pre-spawn and the handlers reject.
    std::uint64_t char_id = 0;
    vendor::BuybackQueue buyback;

    // The spawned character's class + level (roster.h ids), captured at ENTER_WORLD
    // from the loaded owned character. The trainer path (NPC-02, #372) gates a learn
    // on class + level; the quest path (QST-01, #371) gates an accept on level. Both
    // are server-authoritative — read from the DB-loaded character, never a client
    // field. 0 / 1 pre-spawn (the handlers reject before kInWorld).
    std::uint8_t char_class = 0;
    std::uint16_t char_level = 1;

    // QUEST state (QST-01, #371). This session's quest state machine — accept /
    // objective progress / turn-in — a pure, in-memory per-character log (like the
    // combat/movement state above; single-threaded per connection). Emplaced at
    // ENTER_WORLD over the shared placeholder QuestStore; std::nullopt until spawned.
    // At M1 quest progress is NOT persisted (the durable character_quest store is a
    // later story / mcc #28); reward ITEMS + copper granted at turn-in ARE durable
    // (minted/credited to char_db). The reward XP is reported but not persisted
    // (CHR-03 leveling is in-memory in MapTick).
    std::optional<QuestLog> quests;

    // QUEST-KILL credit bus (QST-01 event-bus, #396). `quest_credit` is the SHARED,
    // thread-safe MapTick→session kill registry (set by serve_connection; null in
    // the DB-less smoke path). At ENTER_WORLD this session REGISTERS its entity guid
    // (`credit_guid`, saved so the serve loop can unregister on teardown); the world
    // thread PUSHES creature-kill credits keyed by that guid, and this session DRAINS
    // + applies them on its own IO worker (poll_quest_credits) — keeping ctx.quests
    // single-threaded (the world thread never touches it). `credit_guid` is 0 until
    // registered.
    QuestCreditRegistry* quest_credit = nullptr;
    ObjectGuid credit_guid = 0;
    QuestCreditToken credit_token = 0;  // this registration's holder id (ABA guard on unregister)

    // LOOT (ITM-02, #369/#388). `loot` is the SHARED, thread-safe corpse-loot
    // registry (set by serve_connection from the WorldServer; may be null in the
    // DB-less smoke path). `open_corpse` is THIS session's currently-open loot
    // window (the corpse guid), set on a LOOT_REQUEST(OK) and cleared on
    // LOOT_RELEASE / when the corpse is fully looted — the per-connection "open loot
    // session" state (mirrors the vendor buyback queue's per-session scope).
    LootRegistry* loot = nullptr;
    std::optional<std::uint64_t> open_corpse;

    // TRAINER (NPC-02, #372). This session's IN-MEMORY learned-ability set. At M1
    // there is no durable character_ability table (trainer.h file header) — the
    // COPPER debit IS durable (character.money via char_db); the learned row is
    // per-session. std::nullopt-free (empty until a first learn), like `buyback`.
    npc::LearnedAbilitySet learned;

    // Combat (CMB-01, #344/#345). `abilities` is the shared, read-only ability
    // template store (#343), set by serve_connection from the WorldServer; the
    // CAST_REQUEST handler looks the used ability up here. `combat` is THIS
    // session's per-connection GCD + cast lifecycle state (single-threaded, like
    // `movement`); the resolver reads/mutates it on each ability use.
    const AbilityStore* abilities = nullptr;
    CombatSession combat;

    // AoI relay (#87). `world` is the shared world-thread-owned session registry
    // + grid (set by serve_connection; may be null in the DB-less dispatch smoke
    // test, where no relay is wired and movement replies only to the mover).
    // `egress` is THIS connection's serialized, WorldSession-sealed s2c write
    // channel (created after a valid WORLD_HELLO). `slot`/`entered` track this
    // session's registration in `world` so MOVEMENT_INTENT can relay and the
    // serve loop can leave() on disconnect.
    WorldState* world = nullptr;
    std::shared_ptr<SessionEgress> egress;
    SessionSlot slot = 0;
    bool entered = false;

    // GM `.summon` target mailbox (OPS-02b, #418). Created at ENTER_WORLD and
    // registered with WorldState (set_session_control) so a summoning GM on ANOTHER
    // thread can post this session's forced destination; THIS session's own IO
    // worker drains it (drain_forced_move — top of MOVEMENT_INTENT + post-frame) and
    // applies force_correction, keeping ctx.movement single-threaded. Null until
    // spawned / on the DB-less smoke path (un-summonable).
    std::shared_ptr<ForcedMoveMailbox> forced_move;

    // OPS-05: true once this session bumped the CCU / active-session gauges on a
    // valid WORLD_HELLO, so the serve loop decrements them EXACTLY once on close
    // (distinct from `entered`, which tracks AoI registration — a session can be
    // authenticated/counted without a world registry in the DB-less path).
    bool entered_metrics = false;

    // OPS-05 audit stream (#92): the account + grant this session authenticated
    // as, captured on a valid WORLD_HELLO so the serve loop can attribute the
    // session-leave audit record to the same actor + correlation id as the
    // session-enter record. 0 until authenticated (an unauthenticated connection
    // that never entered emits no leave audit).
    std::uint64_t account_id = 0;
    std::uint64_t grant_id = 0;

    // GM permission level for this session's account (OPS-02a, #417). Captured on
    // a valid WORLD_HELLO from the grant-consume JOIN to account.gm_level (D-16
    // ladder: 0 player < 1 helper < 2 GM < 3 admin), so it is authoritative for
    // the whole session — the GM command framework (gm_command.h) gates every
    // `.`-command at the chat path on this value. 0 (player) until authenticated;
    // a plain player never clears any command's threshold.
    std::uint8_t gm_level = 0;

    // Single active session per account (#326). `active_sessions` is the shared,
    // account-keyed registry (set by serve_connection; null in the DB-less
    // dispatch smoke test, where no account ever authenticates). On a valid
    // WORLD_HELLO the handler ADMITS this session's account into the registry
    // (kick-old): if the account already had a live session, that one is kicked.
    // `session_token` is this admission's holder identity — the serve loop passes
    // it to release() on teardown (a compare-and-remove, so a kicked-old session
    // never evicts the session that replaced it). `admitted` guards that
    // exactly-once release. `kicked` is set true when a LATER login for this
    // account displaces THIS session — the serve loop notices it and closes.
    ActiveSessionRegistry* active_sessions = nullptr;
    SessionToken session_token = 0;
    bool admitted = false;
    std::shared_ptr<std::atomic<bool>> kicked;

    bool disconnect = false;             // handler asks the serve loop to close
    net::DisconnectReason disconnect_reason = net::DisconnectReason::UNKNOWN;
    std::string disconnect_message;
};

// A handler processes one verified frame for a given opcode. It receives the
// session (to write replies), the decoded frame, and the per-connection context
// (grant DB, realm, AEAD session — see ConnCtx). Returning normally means the
// frame was handled; handlers throw meridian::net::TlsError only on a transport
// failure (which the serve loop treats as connection loss). A handler may set
// ctx.disconnect to ask the serve loop to send a Disconnect and close.
//
// M0: WORLD_HELLO is the real IF-3 handshake (#84 — grant validation + AEAD
// session + enter-world -> HandshakeOk). CLOCK_SYNC echoes; MOVEMENT_INTENT logs
// (validation + broadcast is #86).
using Handler = std::function<void(net::Session&, const Frame&, ConnCtx&)>;

// The IF-2 opcode dispatcher: a table mapping u16 Opcode -> Handler. Decodes each
// net frame's IF-2 header, verifies the payload table for the opcode, and routes
// to the handler. Unknown / reserved / malformed / bad-payload frames do NOT
// invoke a handler — they surface as the matching DispatchOutcome so the serve
// loop can send a Disconnect and close (SAD §5.2: "wrong-state or undecodable
// messages are dropped/rejected").
//
// Instances are cheap and own only the handler table; one is shared (read-only
// after construction) across all connections.
class Dispatcher {
public:
    // Build the dispatcher and register the M0 stub handlers.
    Dispatcher();

    // Register / override the handler for one opcode. Later stories replace the
    // stubs by re-registering. Not thread-safe; call during setup only.
    void on(net::Opcode opcode, Handler handler);

    // Whether a handler is registered for `opcode`.
    bool has_handler(net::Opcode opcode) const;

    // Decode + verify + dispatch one net frame. On kHandled the handler already
    // ran (and may have written replies). On any error outcome NO reply is sent
    // here — the caller decides the Disconnect reason + message (so the serve
    // loop owns the socket-closing policy). `last_opcode`/`last_seq` are set for
    // the caller's logging + Disconnect echo even on the error paths (best
    // effort; 0 when the frame was too short to read them). `ctx` carries the
    // per-connection state (grant DB, realm, AEAD session) to the handler; a
    // handler may set ctx.disconnect to request a clean close.
    DispatchOutcome dispatch(net::Session& sess, const Bytes& frame, ConnCtx& ctx,
                             std::uint16_t& last_opcode, std::uint64_t& last_seq) const;

private:
    void register_m0_stubs();

    std::unordered_map<std::uint16_t, Handler> handlers_;
};

// ---------------------------------------------------------------------------
// WorldServer — the M0 process scaffold (SAD §2.5, §6).
//
// This is the STRUCTURE the tick, maps, and bus land on later. It models the
// three roles the SAD assigns worldd, without their real bodies yet:
//   - the WORLD/UPDATE THREAD: owns game state + the tick loop. At M0 the loop
//     is a placeholder that drains an inbound work queue at a fixed cadence and
//     does nothing else (no maps to update). It is the ONLY thread that will
//     touch game state, so the accept/IO path never does (SAD §6.1).
//   - a MAP/IO WORKER POOL: a thread pool that runs accept + per-connection IO
//     (framing) off the world thread. Workers own sockets, never game state
//     (SAD §6.1 "map workers ... never touch sockets"; conversely the IO/accept
//     work never touches game state). Decoded frames destined for the simulation
//     are enqueued to the world thread over the queue below — no worker mutates
//     game state directly.
//   - a QUEUE between them: an MPSC-style inbound queue (many IO workers ->
//     the one world thread). At M0 it carries a minimal WorldEvent; it is the
//     seam the real per-map inbound drains replace.
//
// M0 handlers run inline on the IO worker (they only log/echo over the session).
// The queue exists and is exercised so the structure is real, not aspirational:
// a handler that needs the simulation enqueues a WorldEvent rather than touching
// state, and the world thread drains it each tick.
// ---------------------------------------------------------------------------

// A unit of work handed from an IO worker to the world thread. At M0 it is a
// decoded opcode + seq + a copy of the payload (the world thread outlives the
// frame). Later stories give it a session id and typed, per-map routing.
struct WorldEvent {
    net::Opcode opcode{};
    std::uint64_t seq = 0;
    Bytes payload;  // owned copy — the frame buffer is gone by drain time
};

// Route one map tick's events to the quest-kill credit bus (QST-01 event-bus, #396):
// for each TickEventKind::kCreatureKill delta, push the credit (killer guid + victim
// template id) to `reg`, which retains it only for a registered in-world session. The
// world thread calls this each tick after MapTick::advance(); the integration test
// calls it with a real MapTick's deltas to exercise the exact same routing. Non-kill
// deltas are ignored here (they are the AoI/flush stream, handled elsewhere).
void route_tick_events(const std::vector<TickEvent>& deltas, QuestCreditRegistry& reg);

struct WorldServerConfig {
    // Map/IO worker pool size. Defaults to a small pool; main() can size it from
    // hardware_concurrency. The SAD's "M ≈ cores − 3" sizing is a later concern —
    // at M0 the pool just needs to exist and run accept/IO off the world thread.
    unsigned io_workers = 2;

    // World-thread tick cadence placeholder. The real rate is 20 Hz / 50 ms
    // (SAD §2.5); at M0 the loop just drains the inbound queue at this cadence.
    unsigned tick_interval_ms = 50;

    // IF-3 grant validation (#84). worldd consumes session_grant rows from the
    // auth DB (worldd, not gatewayd, is client-facing until the M2 split — SAD
    // §2.2/§5.3). Each IO worker opens its OWN auth-DB connection per served
    // connection (like authd) so no connection is shared across threads.
    //   - `auth_db`: connection params for the auth DB. When `user` is empty,
    //     grant validation is DISABLED — serve_connection builds a ConnCtx with a
    //     null DB, and a WORLD_HELLO with no DB is rejected (GRANT_INVALID). This
    //     keeps the plain, DB-less `worldd --version`/dispatch test buildable and
    //     the daemon usable without a DB (it simply cannot admit players).
    //   - `char_db`: characters DB params for enter-world. Optional — when empty,
    //     enter-world loads the D-11 placeholder stub (no characters DB needed).
    //   - `realm_id`: the realm this worldd serves; grants for another realm are
    //     rejected. 0 = accept a grant for any realm (single-realm M0 default).
    db::ConnectParams auth_db;
    db::ConnectParams char_db;
    std::uint32_t realm_id = 0;

    // OPS-05 metric label context (#164) — copied onto each connection's ConnCtx
    // so every worldd metric carries the {realm,zone,shard,map} the dashboards
    // query. main() fills `labels.realm` from --realm / MERIDIAN_REALM.
    MetricsLabels labels;

    // OPS-05 session-flow trace exporter (#166). Borrowed pointer (main() owns the
    // Exporter for the process lifetime); copied onto each ConnCtx by
    // serve_connection so the WORLD_HELLO handler can emit the enter-world span.
    // NULL => tracing off. Never dereferenced without an active() check.
    meridian::trace::Exporter* tracer = nullptr;
};

class WorldServer {
public:
    // Construct with the shared dispatcher and the pool/tick config. Does NOT
    // start any threads — call run() (blocking) or start()/stop() for the test.
    WorldServer(const Dispatcher& dispatcher, WorldServerConfig cfg);
    ~WorldServer();

    WorldServer(const WorldServer&) = delete;
    WorldServer& operator=(const WorldServer&) = delete;

    // Start the world/update thread (drains the inbound queue at the tick
    // cadence). Idempotent. The IO/accept side is driven by serve_connection()
    // per accepted Session (main() runs the accept loop + the pool).
    void start();

    // Stop the world thread and join it. Idempotent; also called by the dtor.
    void stop();

    // Serve one accepted connection to completion on the CALLING thread (an IO
    // worker): read frames, dispatch each, and on an error outcome send a
    // Disconnect and close. Returns when the peer closes cleanly, an error
    // outcome closes the connection, or a transport failure is caught. Never
    // touches game state — decoded simulation work is enqueued (enqueue()).
    void serve_connection(net::Session sess);

    // Enqueue a WorldEvent for the world thread (called from IO workers /
    // handlers that need the simulation). Thread-safe (MPSC).
    void enqueue(WorldEvent ev);

    // Test/diagnostic: how many WorldEvents the world thread has drained. Lets a
    // test assert the queue actually reached the world thread.
    std::uint64_t drained_count() const;

    // The shared world-thread-owned session registry + AoI grid (#87). One per
    // server, shared (thread-safe) across every serve_connection: a mover's
    // authoritative MovementState is relayed through it to the OTHER sessions in
    // range. Exposed for tests that inspect session_count().
    WorldState& world_state();

    // The shared single-active-session registry (#326). One per server, keyed by
    // account_id; the WORLD_HELLO handler admits each authenticated session into
    // it (kick-old on collision) so an account holds at most one in-world session.
    // Exposed for tests that assert active_count()/is_active().
    ActiveSessionRegistry& active_sessions();

    // The shared corpse-loot registry (ITM-02 wire; #388). One per server, keyed by
    // corpse guid; the LOOT_* dispatch handlers open/take/release against it. The
    // world-thread creature-death hook (#369) seeds it; a test seeds it directly to
    // drive the loot wire path. Exposed so tests can insert a corpse's loot session.
    LootRegistry& loot_registry();

    // The shared MapTick→session quest-kill credit registry (QST-01 event-bus, #396).
    // One per server: the world thread pushes creature-kill credits (route_tick_events)
    // keyed by killer guid; each in-world session registers its guid + drains its own
    // credits. Exposed so a test can register a guid / push a kill / assert isolation.
    QuestCreditRegistry& quest_credit();

    // Install a world-DB-backed loot-table store on the per-map tick (#390),
    // replacing the placeholder loot tables the tick rolls on creature death. The
    // store must outlive the WorldServer (main() owns the WorldContent bundle). Call
    // at boot before start(); not thread-safe.
    void set_loot_tables(const loot::LootTableStore& store);

private:
    void world_thread_main();

    const Dispatcher& dispatcher_;
    WorldServerConfig cfg_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meridian::worldd

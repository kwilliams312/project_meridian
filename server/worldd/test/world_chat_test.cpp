// SPDX-License-Identifier: Apache-2.0
//
// worldd — chat router UNIT TEST (issue #367, SOC-01). The deterministic proof
// of the server-authoritative chat router.
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 ("spatial chat stays
// here" — the AoI grid delivers say/yell; whisper/zone route through the in-
// process world-thread manager, #88 over the bus at M3) and §3.8 (whisper by
// name, zone/general broadcast), plus world_state.h / aoi_grid.h and the
// world.fbs chat contract. No GPL source consulted (CONTRIBUTING).
//
// PURE / DB-FREE / SOCKET-FREE / SEEDED: drives WorldState directly with
// CAPTURING egress sinks (the EgressFn is a std::function, so a test lambda
// records exactly the frames the router emits — no TLS, no MariaDB). Every
// position + name is fixed, so the routing is fully deterministic. Runs in the
// plain server ctest (no MERIDIAN_DB_* needed). The DB-backed two-client wire
// proof is a later integration concern; the routing LOGIC is proven here.
//
// What it proves (the story's acceptance list):
//   A. SAY reaches in-range sessions ONLY — a near session receives, a far one
//      does not; the sender sees its own echo.
//   B. YELL reaches WIDER than say — the far session that missed the say now
//      receives the yell.
//   C. WHISPER routes to the NAMED target across sessions — only the target
//      receives it (case-insensitive name), with the sender's guid/name; an
//      unknown name is reported TARGET_OFFLINE (no delivery).
//   D. ZONE broadcasts to EVERY in-world session regardless of distance.
//   E. RATE CLASS (OPS-03) — ChatIntake admits up to the per-second cap then
//      drops, and the window slides.

#include "aoi_grid.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// A decoded ChatDeliver frame captured by a session's egress sink.
struct Received {
    mn::ChatChannel channel;
    std::uint64_t sender_guid;
    std::string sender_name;
    std::string text;
};

// A capturing egress sink: records every CHAT_DELIVER the router emits to this
// session (ignores the AoI ENTITY_* frames enter() also sends). The payload is
// the bare FlatBuffer table (world_state emits table bytes; the IF-2 header is
// added by the real SessionEgress, absent here).
struct Capture {
    std::vector<Received> chats;

    EgressFn sink() {
        return [this](mn::Opcode op, const std::vector<std::uint8_t>& payload) -> bool {
            if (op != mn::Opcode::CHAT_DELIVER) return true;  // ignore entity relay frames
            fb::Verifier v(payload.data(), payload.size());
            if (!v.VerifyBuffer<mn::ChatDeliver>(nullptr)) return false;
            const auto* d = fb::GetRoot<mn::ChatDeliver>(payload.data());
            chats.push_back(Received{
                d->channel(), d->sender_guid(),
                d->sender_name() ? d->sender_name()->str() : std::string(),
                d->text() ? d->text()->str() : std::string()});
            return true;
        };
    }
};

// How many CHAT_DELIVER frames of `channel` this capture holds.
std::size_t count_of(const Capture& c, mn::ChatChannel channel) {
    std::size_t n = 0;
    for (const auto& r : c.chats)
        if (r.channel == channel) ++n;
    return n;
}

// The last CHAT_DELIVER of `channel`, or a defaulted Received if none.
Received last_of(const Capture& c, mn::ChatChannel channel) {
    for (auto it = c.chats.rbegin(); it != c.chats.rend(); ++it)
        if (it->channel == channel) return *it;
    return Received{channel, 0, {}, {}};
}

// ---------------------------------------------------------------------------
// A/B/C/D — the four routing modes over three fixed-position sessions.
// ---------------------------------------------------------------------------
void test_routing() {
    std::printf("[chat] routing (say/yell/whisper/zone)\n");

    // say radius = 25 m, yell radius = 90 m. Place B WITHIN say of A, C BEYOND
    // say but WITHIN yell of A.
    WorldState world;
    Capture ca, cb, cc;

    EntityIdentity ida;
    ida.entity_guid = 1001;
    ida.name = "Aldric";
    EntityIdentity idb;
    idb.entity_guid = 1002;
    idb.name = "Brynn";
    EntityIdentity idc;
    idc.entity_guid = 1003;
    idc.name = "Cerys";

    // Zone-01 play-area centre (#562); B/C offsets preserve the 6 m / 30 m distances.
    const SessionSlot a = world.enter(ida, at(-320.0f, -320.0f), ca.sink()).slot;   // centre
    const SessionSlot b = world.enter(idb, at(-314.0f, -320.0f), cb.sink()).slot;   //  6 m: say in
    const SessionSlot c = world.enter(idc, at(-320.0f, -290.0f), cc.sink()).slot;   // 30 m: say out, yell in
    (void)b;
    (void)c;

    // --- A. SAY reaches in-range only + sender echo ---------------------------
    world.deliver_spatial(a, mn::ChatChannel::SAY, "hail");
    check("say: sender A hears its own echo", count_of(ca, mn::ChatChannel::SAY) == 1);
    check("say: near B (6 m) hears it", count_of(cb, mn::ChatChannel::SAY) == 1);
    check("say: far C (30 m) does NOT hear it", count_of(cc, mn::ChatChannel::SAY) == 0);
    {
        const Received r = last_of(cb, mn::ChatChannel::SAY);
        check("say: B sees A's guid as sender", r.sender_guid == 1001);
        check("say: B sees A's name as sender", r.sender_name == "Aldric");
        check("say: B sees the body text", r.text == "hail");
    }

    // --- B. YELL reaches wider than say --------------------------------------
    world.deliver_spatial(a, mn::ChatChannel::YELL, "to arms");
    check("yell: near B hears it", count_of(cb, mn::ChatChannel::YELL) == 1);
    check("yell: far C (30 m) NOW hears it (wider radius)",
          count_of(cc, mn::ChatChannel::YELL) == 1);
    check("yell: sender A hears its own echo", count_of(ca, mn::ChatChannel::YELL) == 1);

    // --- C. WHISPER routes to the named target across sessions ----------------
    // Case-insensitive: whisper to "brynn" reaches Brynn (B) only.
    const ChatWhisperOutcome w1 = world.whisper(a, "brynn", "psst");
    check("whisper: outcome is delivered", w1 == ChatWhisperOutcome::kDelivered);
    check("whisper: target B receives it", count_of(cb, mn::ChatChannel::WHISPER) == 1);
    check("whisper: non-target C receives nothing",
          count_of(cc, mn::ChatChannel::WHISPER) == 0);
    check("whisper: sender A is not its own target",
          count_of(ca, mn::ChatChannel::WHISPER) == 0);
    {
        const Received r = last_of(cb, mn::ChatChannel::WHISPER);
        check("whisper: B sees A as sender", r.sender_guid == 1001 && r.sender_name == "Aldric");
        check("whisper: body delivered", r.text == "psst");
    }
    // An unknown name is offline — no delivery.
    const ChatWhisperOutcome w2 = world.whisper(a, "Nobody", "hello?");
    check("whisper: unknown name -> TARGET_OFFLINE", w2 == ChatWhisperOutcome::kTargetOffline);
    check("whisper: no stray delivery on offline target",
          count_of(cb, mn::ChatChannel::WHISPER) == 1);
    // Empty target name -> NO_TARGET.
    check("whisper: empty target -> NO_TARGET",
          world.whisper(a, "", "x") == ChatWhisperOutcome::kNoTarget);

    // --- D. ZONE broadcasts to everyone regardless of distance ----------------
    const std::size_t n = world.deliver_channel(a, "server restart in 5m");
    check("zone: delivered to all three in-world sessions", n == 3);
    check("zone: A received", count_of(ca, mn::ChatChannel::ZONE) == 1);
    check("zone: B received", count_of(cb, mn::ChatChannel::ZONE) == 1);
    check("zone: C received (out of say/yell range, still on channel)",
          count_of(cc, mn::ChatChannel::ZONE) == 1);
}

// A session that has left is unaddressable — whisper reports offline, and a
// departed name frees up (no delivery to a stale slot).
void test_leave_unregisters_name() {
    std::printf("[chat] leave unregisters the whisper name\n");
    WorldState world;
    Capture ca, cb;
    EntityIdentity ida;
    ida.entity_guid = 2001;
    ida.name = "Aldric";
    EntityIdentity idb;
    idb.entity_guid = 2002;
    idb.name = "Brynn";
    const SessionSlot a = world.enter(ida, at(-320.0f, -320.0f), ca.sink()).slot;
    const SessionSlot b = world.enter(idb, at(-320.0f, -320.0f), cb.sink()).slot;

    check("whisper before leave delivers",
          world.whisper(a, "Brynn", "hi") == ChatWhisperOutcome::kDelivered);
    world.leave(b);
    check("whisper after target left -> TARGET_OFFLINE",
          world.whisper(a, "Brynn", "still there?") == ChatWhisperOutcome::kTargetOffline);
}

// ---------------------------------------------------------------------------
// E — the OPS-03 chat rate class (ChatIntake): admit up to the cap, then drop;
// the window slides so a later second admits again.
// ---------------------------------------------------------------------------
void test_rate_class() {
    std::printf("[chat] rate class (OPS-03 ChatIntake)\n");
    ChatIntake gate;
    std::uint64_t t = 100000;  // arbitrary steady-ms base

    int admitted = 0;
    for (int i = 0; i < kChatMaxPerSecond; ++i) {
        if (gate.admit(t)) ++admitted;  // same instant — all within one window
    }
    check("rate: admits up to the per-second cap", admitted == kChatMaxPerSecond);
    check("rate: the next send in the same window is dropped", !gate.admit(t));
    check("rate: dropped count reflects the over-rate send", gate.dropped() == 1);

    // Slide past the 1 s window: the oldest stamps expire, so a send admits again.
    check("rate: a send 1 s later admits again (window slid)", gate.admit(t + 1000));
}

}  // namespace

int main() {
    std::printf("worldd chat router unit test (SOC-01 #367)\n\n");
    test_routing();
    test_leave_unregisters_name();
    test_rate_class();
    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}

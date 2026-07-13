// SPDX-License-Identifier: Apache-2.0
//
// worldd — GM COMMAND FRAMEWORK unit test (OPS-02a, #417; epic #21).
//
// CLEAN-ROOM: written from docs/prd/server-prd.md §4-M1 / §6, the D-16 permission
// model, and gm_command.h / meridian/core/audit.hpp. No GPL source consulted
// (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: drives the GM command framework directly with
// CAPTURING reply + audit sinks — no TLS, no MariaDB — so it runs in the plain
// server ctest. It proves the framework end to end WITHOUT a live session: the
// dispatch adapter in world_dispatch.cpp is a thin wrapper that only wires the
// reply sink to a CHAT_DELIVER and the audit sink to core::audit::emit, so the
// logic proven here is exactly the logic the live chat path runs. (The other half
// — that a live session LEARNS its gm_level from the grant/account JOIN — is
// proven by the DB-gated worldd-grant-test.)
//
// What it proves (the story's acceptance list):
//   A. PARSE — the '.'-prefix detector + {name,args} parser (case, spacing, edge).
//   B. REGISTRY — find + permission-scoped visibility (a player sees NO commands;
//      a helper/GM/admin sees `.help`).
//   C. AUDIT — build_command_audit emits the exact record for allowed / denied /
//      unknown, with the actor + level + no secrets.
//   D. DISPATCH — `.help` from a GM-level session lists the commands + audits a
//      SUCCESS; `.help` from a player session is DENIED + audits a FAILURE; an
//      unknown command is rejected + audited.

#include "gm_command.h"

#include "leveling.h"  // kMaxLevel — the .setlevel clamp assertion
#include "meridian/core/audit.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace gm = meridian::worldd::gm;
namespace audit = meridian::core::audit;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Capture every reply line + audit record a dispatch produces.
struct Capture {
    std::vector<std::string> replies;
    std::vector<audit::Record> audits;

    gm::ReplyFn reply_sink() {
        return [this](const std::string& line) { replies.push_back(line); };
    }
    gm::AuditFn audit_sink() {
        return [this](const audit::Record& rec) { audits.push_back(rec); };
    }
    bool any_reply_has(const std::string& needle) const {
        for (const std::string& r : replies)
            if (has(r, needle)) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// A. Parse
// ---------------------------------------------------------------------------
void test_parse() {
    std::printf("[gm] parse + prefix detection\n");

    check("is_command: '.help' is a command", gm::is_command(".help"));
    check("is_command: '.' alone is a command", gm::is_command("."));
    check("is_command: plain chat is NOT", !gm::is_command("hello there"));
    check("is_command: empty is NOT", !gm::is_command(""));
    check("is_command: leading space is chat, not a command", !gm::is_command(" .help"));

    {
        gm::ParsedCommand p = gm::parse_command(".help");
        check("parse: bare command name", p.name == "help" && p.args.empty());
    }
    {
        gm::ParsedCommand p = gm::parse_command(".tele Stormwind");
        check("parse: name + single arg", p.name == "tele" && p.args == "Stormwind");
    }
    {
        // Case-insensitive name; internal arg spacing + case preserved; the gap
        // after the name collapsed; trailing whitespace trimmed.
        gm::ParsedCommand p = gm::parse_command(".ADDItem   Sword of Truth  ");
        check("parse: name lowercased", p.name == "additem");
        check("parse: args keep case + internal spacing, trimmed",
              p.args == "Sword of Truth");
    }
    {
        gm::ParsedCommand p = gm::parse_command(".x   ");
        check("parse: name with only trailing spaces -> empty args",
              p.name == "x" && p.args.empty());
    }
    {
        gm::ParsedCommand p = gm::parse_command(".");
        check("parse: bare prefix -> empty name", p.name.empty() && p.args.empty());
    }
}

// ---------------------------------------------------------------------------
// B. Registry + permission-scoped visibility
// ---------------------------------------------------------------------------
void test_registry() {
    std::printf("[gm] registry + visibility gate\n");
    const gm::Registry& reg = gm::Registry::builtin();

    check("registry: '.help' is registered", reg.find("help") != nullptr);
    check("registry: unknown command not found", reg.find("frobnicate") == nullptr);
    if (const gm::Command* c = reg.find("help")) {
        check("registry: '.help' min level is helper",
              c->min_level == gm::Level::kHelper);
    }

    // A PLAYER (level 0) sees NO commands — the whole '.'-surface is staff-only.
    check("visibility: a player sees no commands",
          reg.visible_to(static_cast<std::uint8_t>(gm::Level::kPlayer)).empty());
    // A HELPER (level 1) sees only the read-only `.help` — the M1 effect commands
    // are all GM/admin.
    check("visibility: a helper sees only .help",
          reg.visible_to(static_cast<std::uint8_t>(gm::Level::kHelper)).size() == 1);
    // A GM (level 2) sees `.help` + the GM commands (additem/setlevel/tele/summon/
    // mute) — but NOT `.kick`/`.ban` (admin).
    {
        const auto gm_cmds = reg.visible_to(static_cast<std::uint8_t>(gm::Level::kGm));
        check("visibility: a GM sees help+additem+setlevel+tele+summon+mute (6)",
              gm_cmds.size() == 6);
        bool sees_admin_only = false;
        for (const gm::Command* c : gm_cmds)
            if (c->name == "kick" || c->name == "ban") sees_admin_only = true;
        check("visibility: a GM does NOT see .kick/.ban (admin-only)", !sees_admin_only);
    }
    // An ADMIN (level 3) sees everything, including `.kick` + `.ban`.
    {
        const auto admin_cmds = reg.visible_to(static_cast<std::uint8_t>(gm::Level::kAdmin));
        check("visibility: an admin sees all eight commands", admin_cmds.size() == 8);
        bool sees_kick = false, sees_ban = false;
        for (const gm::Command* c : admin_cmds) {
            if (c->name == "kick") sees_kick = true;
            if (c->name == "ban") sees_ban = true;
        }
        check("visibility: an admin sees .kick + .ban", sees_kick && sees_ban);
    }

    // Per-command min levels (OPS-02b #418 / OPS-02c #419).
    check("registry: .tele min level is GM",
          reg.find("tele") && reg.find("tele")->min_level == gm::Level::kGm);
    check("registry: .summon min level is GM",
          reg.find("summon") && reg.find("summon")->min_level == gm::Level::kGm);
    check("registry: .additem min level is GM",
          reg.find("additem") && reg.find("additem")->min_level == gm::Level::kGm);
    check("registry: .setlevel min level is GM",
          reg.find("setlevel") && reg.find("setlevel")->min_level == gm::Level::kGm);
    check("registry: .kick min level is ADMIN",
          reg.find("kick") && reg.find("kick")->min_level == gm::Level::kAdmin);
    check("registry: .mute min level is GM",
          reg.find("mute") && reg.find("mute")->min_level == gm::Level::kGm);
    check("registry: .ban min level is ADMIN",
          reg.find("ban") && reg.find("ban")->min_level == gm::Level::kAdmin);
}

// ---------------------------------------------------------------------------
// C. Audit record builder
// ---------------------------------------------------------------------------
void test_audit_builder() {
    std::printf("[gm] audit record builder\n");

    // Allowed: a GM (level 2) ran `.help`.
    {
        gm::ParsedCommand cmd{"help", ""};
        audit::Record rec = gm::build_command_audit(
            /*account_id=*/42, /*level=*/2, cmd, gm::CommandOutcome::kOk);
        std::string j = audit::render_json(rec);
        check("audit allowed: action is gm_command", has(j, "\"action\":\"gm_command\""));
        check("audit allowed: outcome success", has(j, "\"outcome\":\"success\""));
        check("audit allowed: actor account recorded", has(j, "\"account_id\":42"));
        check("audit allowed: target is the command", has(j, "\"target\":\"command:help\""));
        check("audit allowed: gm_level recorded (numeric)", has(j, "\"gm_level\":2"));
        check("audit allowed: gm_level_name recorded", has(j, "\"gm_level_name\":\"gm\""));
        check("audit allowed: no reason on success", !has(j, "\"reason\""));
        check("audit allowed: stream tag present", has(j, "\"stream\":\"audit\""));
    }

    // Denied: a player (level 0) tried `.help` with args.
    {
        gm::ParsedCommand cmd{"help", "extra"};
        audit::Record rec = gm::build_command_audit(
            /*account_id=*/7, /*level=*/0, cmd, gm::CommandOutcome::kDenied);
        std::string j = audit::render_json(rec);
        check("audit denied: outcome failure", has(j, "\"outcome\":\"failure\""));
        check("audit denied: reason insufficient_level",
              has(j, "\"reason\":\"insufficient_level\""));
        check("audit denied: gm_level 0 (player)", has(j, "\"gm_level\":0"));
        check("audit denied: gm_level_name player", has(j, "\"gm_level_name\":\"player\""));
        check("audit denied: args recorded", has(j, "\"args\":\"extra\""));
        // No secret material ever (the audit no-secrets guarantee).
        check("audit denied: no secret material",
              !has(j, "session_key") && !has(j, "password") && !has(j, "verifier"));
    }

    // Unknown: a GM probed a non-existent command.
    {
        gm::ParsedCommand cmd{"frobnicate", ""};
        audit::Record rec = gm::build_command_audit(
            /*account_id=*/9, /*level=*/2, cmd, gm::CommandOutcome::kUnknown);
        std::string j = audit::render_json(rec);
        check("audit unknown: outcome failure", has(j, "\"outcome\":\"failure\""));
        check("audit unknown: reason unknown_command",
              has(j, "\"reason\":\"unknown_command\""));
        check("audit unknown: target names the probed command",
              has(j, "\"target\":\"command:frobnicate\""));
    }
}

// ---------------------------------------------------------------------------
// D. Dispatch — the story's live-session acceptance (over capturing sinks)
// ---------------------------------------------------------------------------
void test_dispatch() {
    std::printf("[gm] dispatch: gate + reply + audit\n");
    const gm::Registry& reg = gm::Registry::builtin();

    // GM session runs `.help` -> OK, lists the command, audits a SUCCESS.
    {
        Capture cap;
        gm::CommandOutcome oc = gm::dispatch_command(
            reg, ".help", /*account_id=*/42,
            static_cast<std::uint8_t>(gm::Level::kGm), gm::GmEffects{}, cap.reply_sink(),
            cap.audit_sink());
        check("dispatch GM .help: outcome OK", oc == gm::CommandOutcome::kOk);
        check("dispatch GM .help: reply lists .help", cap.any_reply_has(".help"));
        check("dispatch GM .help: reply has the availability header",
              cap.any_reply_has("available to you"));
        check("dispatch GM .help: exactly one audit record", cap.audits.size() == 1);
        if (cap.audits.size() == 1) {
            check("dispatch GM .help: audit is a success",
                  cap.audits[0].outcome == audit::Outcome::kSuccess &&
                  cap.audits[0].action == audit::Action::kGmCommand);
            check("dispatch GM .help: audit attributes the account",
                  cap.audits[0].account_id == 42);
        }
    }

    // PLAYER session runs `.help` -> DENIED, and the denial IS audited.
    {
        Capture cap;
        gm::CommandOutcome oc = gm::dispatch_command(
            reg, ".help", /*account_id=*/7,
            static_cast<std::uint8_t>(gm::Level::kPlayer), gm::GmEffects{}, cap.reply_sink(),
            cap.audit_sink());
        check("dispatch player .help: outcome DENIED", oc == gm::CommandOutcome::kDenied);
        check("dispatch player .help: reply is a permission refusal",
              cap.any_reply_has("permission"));
        check("dispatch player .help: does NOT leak the command list",
              !cap.any_reply_has("available to you"));
        check("dispatch player .help: the denial is audited (one record)",
              cap.audits.size() == 1);
        if (cap.audits.size() == 1) {
            check("dispatch player .help: audit is a FAILURE with the deny reason",
                  cap.audits[0].outcome == audit::Outcome::kFailure &&
                  cap.audits[0].reason == "insufficient_level");
            check("dispatch player .help: audit attributes the denied account",
                  cap.audits[0].account_id == 7);
        }
    }

    // GM probes an unknown command -> rejected + audited.
    {
        Capture cap;
        gm::CommandOutcome oc = gm::dispatch_command(
            reg, ".frobnicate now", /*account_id=*/42,
            static_cast<std::uint8_t>(gm::Level::kGm), gm::GmEffects{}, cap.reply_sink(),
            cap.audit_sink());
        check("dispatch GM .frobnicate: outcome UNKNOWN", oc == gm::CommandOutcome::kUnknown);
        check("dispatch GM .frobnicate: reply says unknown",
              cap.any_reply_has("Unknown command"));
        check("dispatch GM .frobnicate: audited as a failure",
              cap.audits.size() == 1 &&
              cap.audits[0].outcome == audit::Outcome::kFailure &&
              cap.audits[0].reason == "unknown_command");
    }
}

// ---------------------------------------------------------------------------
// E. Command set effect wiring + arg validation (OPS-02b, #418)
// ---------------------------------------------------------------------------
// Records every effect seam invocation so a test can assert the SERVER computes
// each command from validated args + reports the outcome — without a live world
// (the world_dispatch integration test proves the seams over a real WorldState).
struct FakeEffects {
    // .tele
    int teleport_calls = 0;
    meridian::worldd::Position last_dest;
    // .summon
    int summon_calls = 0;
    std::string last_summon_name;
    gm::EffectStatus summon_status = gm::EffectStatus::kApplied;
    // .additem
    int additem_calls = 0;
    std::uint32_t last_template = 0, last_count = 0;
    gm::EffectStatus additem_status = gm::EffectStatus::kApplied;
    // .setlevel
    int setlevel_calls = 0;
    std::uint32_t last_level = 0;
    // .kick
    int kick_calls = 0;
    std::string last_kick_name;
    gm::EffectStatus kick_status = gm::EffectStatus::kApplied;
    // .ban
    int ban_calls = 0;
    gm::BanSubject last_ban_subject = gm::BanSubject::kAccount;
    std::string last_ban_target;
    std::optional<std::uint64_t> last_ban_dur;
    std::string last_ban_reason;
    gm::EffectStatus ban_status = gm::EffectStatus::kApplied;
    // .mute
    int mute_calls = 0;
    std::string last_mute_name;
    std::optional<std::uint64_t> last_mute_dur;
    std::string last_mute_reason;
    gm::EffectStatus mute_status = gm::EffectStatus::kApplied;

    gm::GmEffects make() {
        gm::GmEffects fx;
        fx.teleport = [this](const meridian::worldd::Position& d) {
            ++teleport_calls;
            last_dest = d;
            return gm::EffectStatus::kApplied;
        };
        fx.summon = [this](const std::string& n) {
            ++summon_calls;
            last_summon_name = n;
            return summon_status;
        };
        fx.add_item = [this](std::uint32_t id, std::uint32_t c) {
            ++additem_calls;
            last_template = id;
            last_count = c;
            gm::AddItemResult r;
            r.status = additem_status;
            r.item_name = "Test Sword";
            r.count = c;
            return r;
        };
        fx.set_level = [this](std::uint32_t lvl) {
            ++setlevel_calls;
            last_level = lvl;
            gm::SetLevelResult r;
            r.status = gm::EffectStatus::kApplied;
            r.applied_level = static_cast<std::uint16_t>(lvl);
            return r;
        };
        fx.kick = [this](const std::string& n) {
            ++kick_calls;
            last_kick_name = n;
            return kick_status;
        };
        fx.ban = [this](gm::BanSubject s, const std::string& t,
                        std::optional<std::uint64_t> d, const std::string& reason) {
            ++ban_calls;
            last_ban_subject = s;
            last_ban_target = t;
            last_ban_dur = d;
            last_ban_reason = reason;
            gm::BanResult r;
            r.status = ban_status;
            r.subject_desc = t;
            return r;
        };
        fx.mute = [this](const std::string& n, std::optional<std::uint64_t> d,
                         const std::string& reason) {
            ++mute_calls;
            last_mute_name = n;
            last_mute_dur = d;
            last_mute_reason = reason;
            gm::MuteResult r;
            r.status = mute_status;
            r.subject_desc = n;
            return r;
        };
        return fx;
    }
};

const std::uint8_t kGm = static_cast<std::uint8_t>(gm::Level::kGm);
const std::uint8_t kAdmin = static_cast<std::uint8_t>(gm::Level::kAdmin);

// Dispatch `line` at `level` over a fresh capture + the given effects, returning
// the outcome. `fake` is inspected by the caller for the seam it exercises.
gm::CommandOutcome run(const std::string& line, std::uint8_t level, FakeEffects& fake,
                       Capture& cap) {
    return gm::dispatch_command(gm::Registry::builtin(), line, /*account_id=*/100, level,
                                fake.make(), cap.reply_sink(), cap.audit_sink());
}

void test_command_set() {
    std::printf("[gm] command set: effect wiring + arg validation\n");
    const gm::Registry& reg = gm::Registry::builtin();
    (void)reg;

    // --- .tele: valid coords call the seam; the server confirms ---------------
    {
        FakeEffects fake;
        Capture cap;
        // Destination inside the real Zone-01 play area [-512,-128] (#562).
        gm::CommandOutcome oc = run(".tele -320 -300 0", kGm, fake, cap);
        check(".tele valid: outcome OK", oc == gm::CommandOutcome::kOk);
        check(".tele valid: seam invoked once", fake.teleport_calls == 1);
        check(".tele valid: dest forwarded",
              fake.last_dest.x == -320.0f && fake.last_dest.y == -300.0f &&
                  fake.last_dest.z == 0.0f);
        check(".tele valid: reply confirms", cap.any_reply_has("Teleported"));
        check(".tele valid: audited as success (executed)",
              cap.audits.size() == 1 && cap.audits[0].outcome == audit::Outcome::kSuccess);
    }
    // --- .tele: out-of-bounds is refused by the server, seam NOT called -------
    {
        FakeEffects fake;
        Capture cap;
        run(".tele 9999 1 0", kGm, fake, cap);
        check(".tele OOB: seam NOT invoked", fake.teleport_calls == 0);
        check(".tele OOB: reply says out of bounds", cap.any_reply_has("out of bounds"));
    }
    // --- .tele: non-numeric coords rejected -----------------------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".tele here now please", kGm, fake, cap);
        check(".tele bad args: seam NOT invoked", fake.teleport_calls == 0);
        check(".tele bad args: reply is a usage/validation error",
              cap.any_reply_has("Invalid") || cap.any_reply_has("Usage"));
    }
    // --- .tele: a GM is permitted; a PLAYER is denied (no effect) --------------
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc =
            run(".tele 1 1 0", static_cast<std::uint8_t>(gm::Level::kPlayer), fake, cap);
        check(".tele as player: DENIED", oc == gm::CommandOutcome::kDenied);
        check(".tele as player: seam NOT invoked", fake.teleport_calls == 0);
    }

    // --- .additem: id + count parsed + forwarded ------------------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".additem 900001 5", kGm, fake, cap);
        check(".additem: seam invoked with id+count",
              fake.additem_calls == 1 && fake.last_template == 900001 && fake.last_count == 5);
        check(".additem: reply confirms grant", cap.any_reply_has("Added"));
    }
    // --- .additem: default count is 1 -----------------------------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".additem 900001", kGm, fake, cap);
        check(".additem default count: seam gets count 1", fake.last_count == 1);
    }
    // --- .additem: non-numeric id rejected before the seam --------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".additem sword", kGm, fake, cap);
        check(".additem bad id: seam NOT invoked", fake.additem_calls == 0);
        check(".additem bad id: reply is a validation error", cap.any_reply_has("Invalid"));
    }
    // --- .additem: unknown item status rendered -------------------------------
    {
        FakeEffects fake;
        fake.additem_status = gm::EffectStatus::kUnknownItem;
        Capture cap;
        run(".additem 424242", kGm, fake, cap);
        check(".additem unknown: reply says no such item", cap.any_reply_has("No such item"));
    }

    // --- .setlevel: clamps an over-cap request to kMaxLevel (server is law) ----
    {
        FakeEffects fake;
        Capture cap;
        run(".setlevel 999", kGm, fake, cap);
        check(".setlevel over cap: seam gets the clamped level",
              fake.setlevel_calls == 1 && fake.last_level == meridian::worldd::kMaxLevel);
        check(".setlevel over cap: reply reports the applied level",
              cap.any_reply_has(std::to_string(meridian::worldd::kMaxLevel)));
    }
    // --- .setlevel: in-range value passes through ------------------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".setlevel 5", kGm, fake, cap);
        check(".setlevel in range: seam gets 5", fake.last_level == 5);
    }
    // --- .setlevel: zero / non-numeric rejected -------------------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".setlevel 0", kGm, fake, cap);
        check(".setlevel 0: seam NOT invoked", fake.setlevel_calls == 0);
    }

    // --- .summon: name forwarded; offline status rendered ---------------------
    {
        FakeEffects fake;
        Capture cap;
        run(".summon Aragorn", kGm, fake, cap);
        check(".summon: seam gets the name",
              fake.summon_calls == 1 && fake.last_summon_name == "Aragorn");
        check(".summon: reply confirms", cap.any_reply_has("Summoned"));
    }
    {
        FakeEffects fake;
        fake.summon_status = gm::EffectStatus::kTargetOffline;
        Capture cap;
        run(".summon Nobody", kGm, fake, cap);
        check(".summon offline: reply says no online player",
              cap.any_reply_has("No online player"));
    }
    {
        FakeEffects fake;
        Capture cap;
        run(".summon", kGm, fake, cap);
        check(".summon no arg: seam NOT invoked", fake.summon_calls == 0);
    }

    // --- .kick: ADMIN-gated ---------------------------------------------------
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc = run(".kick Loki", kGm, fake, cap);  // GM < admin
        check(".kick as GM: DENIED", oc == gm::CommandOutcome::kDenied);
        check(".kick as GM: seam NOT invoked", fake.kick_calls == 0);
    }
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc = run(".kick Loki", kAdmin, fake, cap);
        check(".kick as admin: OK", oc == gm::CommandOutcome::kOk);
        check(".kick as admin: seam gets the name",
              fake.kick_calls == 1 && fake.last_kick_name == "Loki");
        check(".kick as admin: reply confirms", cap.any_reply_has("Kicked"));
        check(".kick as admin: audited as executed",
              cap.audits.size() == 1 && cap.audits[0].outcome == audit::Outcome::kSuccess);
    }
    {
        FakeEffects fake;
        fake.kick_status = gm::EffectStatus::kTargetOffline;
        Capture cap;
        run(".kick Ghost", kAdmin, fake, cap);
        check(".kick offline: reply says no online player",
              cap.any_reply_has("No online player"));
    }

    // --- .ban: ADMIN-gated; kind + subject + duration + reason parsed (OPS-02c) --
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc = run(".ban account Griefer 1h being a jerk", kGm, fake, cap);
        check(".ban as GM: DENIED (admin-only)", oc == gm::CommandOutcome::kDenied);
        check(".ban as GM: seam NOT invoked", fake.ban_calls == 0);
    }
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc = run(".ban account Griefer 2h spamming trade", kAdmin, fake, cap);
        check(".ban account: OK", oc == gm::CommandOutcome::kOk);
        check(".ban account: seam gets account subject + name",
              fake.ban_calls == 1 && fake.last_ban_subject == gm::BanSubject::kAccount &&
                  fake.last_ban_target == "Griefer");
        check(".ban account: duration parsed (2h = 7200s)",
              fake.last_ban_dur.has_value() && *fake.last_ban_dur == 7200u);
        check(".ban account: reason is the tail after the duration",
              fake.last_ban_reason == "spamming trade");
        check(".ban account: reply confirms + says temporary", cap.any_reply_has("temporary"));
        check(".ban account: audited as executed",
              cap.audits.size() == 1 && cap.audits[0].outcome == audit::Outcome::kSuccess);
    }
    // No duration -> permanent; the whole tail is the reason.
    {
        FakeEffects fake;
        Capture cap;
        run(".ban ip 203.0.113.5 open proxy", kAdmin, fake, cap);
        check(".ban ip: seam gets ip subject + address",
              fake.last_ban_subject == gm::BanSubject::kIp &&
                  fake.last_ban_target == "203.0.113.5");
        check(".ban ip: no duration -> permanent", !fake.last_ban_dur.has_value());
        check(".ban ip: reason is the whole tail", fake.last_ban_reason == "open proxy");
        check(".ban ip: reply says permanent", cap.any_reply_has("permanent"));
    }
    // 'char' alias maps to the character subject.
    {
        FakeEffects fake;
        Capture cap;
        run(".ban char Loki 7d exploiting", kAdmin, fake, cap);
        check(".ban char: alias maps to character subject",
              fake.last_ban_subject == gm::BanSubject::kCharacter &&
                  fake.last_ban_target == "Loki" && *fake.last_ban_dur == 604800u);
    }
    // Unknown subject / usage errors -> seam not invoked.
    {
        FakeEffects fake;
        Capture cap;
        run(".ban wat Loki", kAdmin, fake, cap);
        check(".ban bad kind: seam NOT invoked", fake.ban_calls == 0);
        check(".ban bad kind: reply names the valid kinds", cap.any_reply_has("account"));
    }
    {
        FakeEffects fake;
        Capture cap;
        run(".ban account", kAdmin, fake, cap);
        check(".ban missing subject: seam NOT invoked + usage", fake.ban_calls == 0);
    }
    // Unknown account name surfaced as "no such".
    {
        FakeEffects fake;
        fake.ban_status = gm::EffectStatus::kTargetOffline;
        Capture cap;
        run(".ban account Ghost", kAdmin, fake, cap);
        check(".ban unknown subject: reply says no such", cap.any_reply_has("No such"));
    }

    // --- .mute: GM-gated; char + duration + reason parsed (OPS-02c) -------------
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc = run(".mute Chatterbox 30m flooding", kGm, fake, cap);
        check(".mute as GM: OK", oc == gm::CommandOutcome::kOk);
        check(".mute: seam gets the name + duration + reason",
              fake.mute_calls == 1 && fake.last_mute_name == "Chatterbox" &&
                  fake.last_mute_dur.has_value() && *fake.last_mute_dur == 1800u &&
                  fake.last_mute_reason == "flooding");
        check(".mute: reply confirms", cap.any_reply_has("Muted"));
        check(".mute: audited as executed",
              cap.audits.size() == 1 && cap.audits[0].outcome == audit::Outcome::kSuccess);
    }
    // A PLAYER is denied.
    {
        FakeEffects fake;
        Capture cap;
        gm::CommandOutcome oc =
            run(".mute Chatterbox", static_cast<std::uint8_t>(gm::Level::kPlayer), fake, cap);
        check(".mute as player: DENIED", oc == gm::CommandOutcome::kDenied);
        check(".mute as player: seam NOT invoked", fake.mute_calls == 0);
    }
    // No duration -> permanent mute; default reason path (no reason tail).
    {
        FakeEffects fake;
        Capture cap;
        run(".mute Chatterbox", kGm, fake, cap);
        check(".mute no duration: permanent", !fake.last_mute_dur.has_value());
        check(".mute no reason: a default reason is supplied", !fake.last_mute_reason.empty());
    }
    // Unknown character -> "no such character".
    {
        FakeEffects fake;
        fake.mute_status = gm::EffectStatus::kTargetOffline;
        Capture cap;
        run(".mute Ghost", kGm, fake, cap);
        check(".mute unknown: reply says no such character", cap.any_reply_has("No such character"));
    }
    {
        FakeEffects fake;
        Capture cap;
        run(".mute", kGm, fake, cap);
        check(".mute no arg: seam NOT invoked", fake.mute_calls == 0);
    }
}

}  // namespace

int main() {
    std::printf("worldd GM command framework unit test (OPS-02a #417 / OPS-02b #418)\n\n");
    test_parse();
    test_registry();
    test_audit_builder();
    test_dispatch();
    test_command_set();
    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}

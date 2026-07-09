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
    // A helper/GM/admin sees `.help`.
    check("visibility: a helper sees .help",
          reg.visible_to(static_cast<std::uint8_t>(gm::Level::kHelper)).size() == 1);
    check("visibility: a GM sees .help",
          reg.visible_to(static_cast<std::uint8_t>(gm::Level::kGm)).size() == 1);
    check("visibility: an admin sees .help",
          reg.visible_to(static_cast<std::uint8_t>(gm::Level::kAdmin)).size() == 1);
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
            static_cast<std::uint8_t>(gm::Level::kGm), cap.reply_sink(), cap.audit_sink());
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
            static_cast<std::uint8_t>(gm::Level::kPlayer), cap.reply_sink(),
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
            static_cast<std::uint8_t>(gm::Level::kGm), cap.reply_sink(), cap.audit_sink());
        check("dispatch GM .frobnicate: outcome UNKNOWN", oc == gm::CommandOutcome::kUnknown);
        check("dispatch GM .frobnicate: reply says unknown",
              cap.any_reply_has("Unknown command"));
        check("dispatch GM .frobnicate: audited as a failure",
              cap.audits.size() == 1 &&
              cap.audits[0].outcome == audit::Outcome::kFailure &&
              cap.audits[0].reason == "unknown_command");
    }
}

}  // namespace

int main() {
    std::printf("worldd GM command framework unit test (OPS-02a #417)\n\n");
    test_parse();
    test_registry();
    test_audit_builder();
    test_dispatch();
    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}

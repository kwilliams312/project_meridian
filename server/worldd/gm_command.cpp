// SPDX-License-Identifier: Apache-2.0
//
// worldd — GM COMMAND FRAMEWORK implementation (OPS-02a, #417).
// See gm_command.h for the provenance + design.

#include "gm_command.h"

#include <algorithm>
#include <cctype>

namespace meridian::worldd::gm {
namespace audit = meridian::core::audit;
namespace log = meridian::core::log;

namespace {

// ASCII space/tab — the only argument separators a chat command uses. Kept local
// (not std::isspace, which is locale-dependent + UB on negative chars).
bool is_sep(char c) { return c == ' ' || c == '\t'; }

char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

const char* level_name(std::uint8_t raw) {
    switch (raw) {
        case 0:  return "player";
        case 1:  return "helper";
        case 2:  return "gm";
        default: return "admin";  // 3+ => admin (a super-admin still ranks as admin)
    }
}

bool is_command(std::string_view text) {
    return !text.empty() && text.front() == kCommandPrefix;
}

ParsedCommand parse_command(std::string_view text) {
    ParsedCommand out;
    std::size_t i = 0;
    // Drop the leading prefix (the caller confirms text[0]==prefix; be defensive).
    if (i < text.size() && text[i] == kCommandPrefix) ++i;

    // Name: from here up to the first separator. Lowercased for a case-insensitive
    // command vocabulary (".HELP" == ".help").
    const std::size_t name_start = i;
    while (i < text.size() && !is_sep(text[i])) ++i;
    out.name.reserve(i - name_start);
    for (std::size_t k = name_start; k < i; ++k) out.name.push_back(to_lower_ascii(text[k]));

    // Skip the separator run between the name and the args.
    while (i < text.size() && is_sep(text[i])) ++i;

    // Args: the remainder with trailing separators trimmed (leading already
    // skipped). Case + internal spacing are preserved (an arg may be a name).
    std::size_t end = text.size();
    while (end > i && is_sep(text[end - 1])) --end;
    out.args.assign(text.substr(i, end - i));
    return out;
}

const char* outcome_reason(CommandOutcome outcome) {
    switch (outcome) {
        case CommandOutcome::kOk:      return "";
        case CommandOutcome::kDenied:  return "insufficient_level";
        case CommandOutcome::kUnknown: return "unknown_command";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void Registry::add(Command cmd) {
    auto it = std::lower_bound(
        commands_.begin(), commands_.end(), cmd.name,
        [](const Command& c, const std::string& n) { return c.name < n; });
    if (it != commands_.end() && it->name == cmd.name) {
        *it = std::move(cmd);  // override a same-named command
    } else {
        commands_.insert(it, std::move(cmd));  // insert in name order
    }
}

const Command* Registry::find(std::string_view name) const {
    auto it = std::lower_bound(
        commands_.begin(), commands_.end(), name,
        [](const Command& c, std::string_view n) { return c.name.compare(n) < 0; });
    if (it != commands_.end() && it->name == name) return &*it;
    return nullptr;
}

std::vector<const Command*> Registry::visible_to(std::uint8_t level) const {
    std::vector<const Command*> out;
    for (const Command& c : commands_) {
        if (level >= static_cast<std::uint8_t>(c.min_level)) out.push_back(&c);
    }
    return out;  // commands_ is name-sorted, so `out` is too
}

Registry::Registry() {
    // M1: register the single `.help` command that proves the framework end to
    // end. It is a GM command (min level = helper), so a plain PLAYER is denied +
    // audited — the whole '.'-command surface is staff-only. `.help` lists the
    // commands the caller's level unlocks (so a helper and a GM see different
    // sets as #418 adds higher-tier commands).
    add(Command{
        "help",
        Level::kHelper,
        "list the GM commands available at your level",
        [](const Registry& reg, const ParsedCommand& /*cmd*/, std::uint8_t level,
           const ReplyFn& reply) {
            reply(std::string("GM commands available to you (") + level_name(level) + "):");
            for (const Command* c : reg.visible_to(level)) {
                reply(std::string(1, kCommandPrefix) + c->name + " - " + c->help);
            }
        },
    });
}

const Registry& Registry::builtin() {
    static const Registry reg;  // one per process; thread-safe init (C++11 statics)
    return reg;
}

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------

audit::Record build_command_audit(std::uint64_t account_id, std::uint8_t level,
                                  const ParsedCommand& cmd, CommandOutcome outcome) {
    audit::Record rec;
    rec.action = audit::Action::kGmCommand;
    // A denied / unknown attempt is the interesting security signal — a warn-level
    // failure; an executed command is an info-level success.
    rec.outcome = (outcome == CommandOutcome::kOk) ? audit::Outcome::kSuccess
                                                   : audit::Outcome::kFailure;
    rec.account_id = account_id;                 // 0 => omitted
    rec.target = std::string("command:") + cmd.name;  // the acted-on command
    if (const char* why = outcome_reason(outcome); why[0] != '\0') {
        rec.reason = why;                        // "insufficient_level"/"unknown_command"
    }
    // Forensic context: the actor's level (numeric + name) and the command args.
    // Args are player-supplied but NOT secret (a command target/coords); never a
    // credential — the audit no-secrets rule still holds.
    rec.extra.push_back(log::field("gm_level", static_cast<std::int64_t>(level)));
    rec.extra.push_back(log::field("gm_level_name", std::string(level_name(level))));
    if (!cmd.args.empty()) {
        rec.extra.push_back(log::field("args", cmd.args));
    }
    return rec;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

CommandOutcome dispatch_command(const Registry& reg, std::string_view text,
                                std::uint64_t account_id, std::uint8_t level,
                                const ReplyFn& reply, const AuditFn& audit) {
    const ParsedCommand cmd = parse_command(text);
    const Command* c = reg.find(cmd.name);

    CommandOutcome outcome;
    if (c == nullptr) {
        outcome = CommandOutcome::kUnknown;
    } else if (level < static_cast<std::uint8_t>(c->min_level)) {
        outcome = CommandOutcome::kDenied;
    } else {
        outcome = CommandOutcome::kOk;
    }

    // Audit EVERY attempt (allowed AND denied AND unknown) FIRST — the append-only
    // GM audit stream is the authoritative record even if the reply is later
    // dropped. A denied probe is exactly the event a GM dashboard wants to see.
    audit(build_command_audit(account_id, level, cmd, outcome));

    switch (outcome) {
        case CommandOutcome::kOk:
            c->handler(reg, cmd, level, reply);
            break;
        case CommandOutcome::kDenied:
            reply(std::string("You do not have permission to use ") + kCommandPrefix +
                  cmd.name + ".");
            break;
        case CommandOutcome::kUnknown:
            reply(std::string("Unknown command: ") + kCommandPrefix + cmd.name + ".");
            break;
    }
    return outcome;
}

}  // namespace meridian::worldd::gm

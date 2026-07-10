// SPDX-License-Identifier: Apache-2.0
//
// worldd — GM COMMAND FRAMEWORK implementation (OPS-02a, #417).
// See gm_command.h for the provenance + design.

#include "gm_command.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "leveling.h"            // kMaxLevel — the .setlevel clamp range
#include "movement_constants.h"  // kZoneMinXY/kZoneMaxXY/kFlatGroundZ/kHeightTolerance — .tele bounds

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

// Split an argument tail into whitespace-separated tokens (case + content
// preserved). Empty input ⇒ no tokens. Used by the commands that take positional
// args (.tele x y z, .additem id count).
std::vector<std::string> split_args(const std::string& args) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && is_sep(args[i])) ++i;
        const std::size_t start = i;
        while (i < args.size() && !is_sep(args[i])) ++i;
        if (i > start) out.push_back(args.substr(start, i - start));
    }
    return out;
}

// Parse `s` as a finite float. Returns false (leaving `out` untouched) on any
// trailing garbage, empty input, or a non-finite value (NaN/Inf) — a GM coord must
// be a clean, real number (a NaN would slip past a naive bounds compare).
bool parse_finite_float(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;  // trailing garbage
    if (!std::isfinite(v)) return false;
    out = static_cast<float>(v);
    return true;
}

// Parse a `.ban`/`.mute` [duration] token: a bare count of seconds ("3600") or a
// suffixed value ("30s"/"15m"/"2h"/"7d"). Returns the seconds, or nullopt when the
// token is not a valid duration form (so the command parser treats it as the first
// word of the reason instead). Zero and overflow are rejected. Kept LOCAL (the
// framework stays free of the meridian::bans dependency — the live seam maps to
// bans::parse_duration_seconds, which shares this grammar).
std::optional<std::uint64_t> parse_duration(const std::string& s) {
    if (s.empty()) return std::nullopt;
    std::uint64_t mult = 1;
    std::size_t len = s.size();
    switch (s.back()) {
        case 's': mult = 1;     --len; break;
        case 'm': mult = 60;    --len; break;
        case 'h': mult = 3600;  --len; break;
        case 'd': mult = 86400; --len; break;
        default: break;  // no suffix => bare seconds
    }
    if (len == 0) return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (next < value) return std::nullopt;  // overflow
        value = next;
    }
    if (value == 0) return std::nullopt;
    if (value > (0xFFFF'FFFF'FFFF'FFFFULL / mult)) return std::nullopt;
    return value * mult;
}

// Join tokens[from..] with single spaces (the free-text reason tail of .ban/.mute).
std::string join_from(const std::vector<std::string>& toks, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < toks.size(); ++i) {
        if (!out.empty()) out.push_back(' ');
        out += toks[i];
    }
    return out;
}

// Parse `s` as a base-10 unsigned 32-bit value. Returns false on empty, non-digit,
// or overflow. Used for the .additem template id + count.
bool parse_u32(const std::string& s, std::uint32_t& out) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return false;
    if (errno == ERANGE || v > 0xFFFF'FFFFULL) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
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

namespace {

// Render the SERVER-side generic effect failures (the ones every effectful command
// shares) to a reply line, or return "" for kApplied / a command-specific status
// the handler renders itself (kTargetOffline / kUnknownItem / kNoSpace). Keeps the
// wording of "not in world" / "internal error" / "unavailable" consistent.
std::string generic_effect_failure(EffectStatus status) {
    switch (status) {
        case EffectStatus::kNotInWorld:
            return "You must be in the world to use that command.";
        case EffectStatus::kInternalError:
            return "The command failed on the server (logged).";
        case EffectStatus::kUnavailable:
            return "That command is unavailable on this server.";
        default:
            return "";  // kApplied or a command-specific status
    }
}

// Format a coordinate compactly for a reply (no trailing noise). One decimal is
// plenty for a GM confirmation line.
std::string fmt_coord(float v) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << v;
    return os.str();
}

}  // namespace

Registry::Registry() {
    // `.help` (OPS-02a, #417) — the read-only introspection command. Min level =
    // helper, so a plain PLAYER is denied + audited (the whole '.'-surface is
    // staff-only). Lists the commands the caller's level unlocks.
    add(Command{
        "help",
        Level::kHelper,
        "list the GM commands available at your level",
        [](const Registry& reg, const ParsedCommand& /*cmd*/, std::uint8_t level,
           const GmEffects& /*fx*/, const ReplyFn& reply) {
            reply(std::string("GM commands available to you (") + level_name(level) + "):");
            for (const Command* c : reg.visible_to(level)) {
                reply(std::string(1, kCommandPrefix) + c->name + " - " + c->help);
            }
        },
    });

    // ── M1 GM COMMAND SET (OPS-02b, #418) ──────────────────────────────────
    // Each command validates ITS OWN args (the server trusts nothing beyond them),
    // then invokes an effect seam that performs the server-authoritative change.
    // Per-command min level (D-16 ladder): the everyday build/test commands are GM;
    // .kick — the most disruptive (it disconnects a player) — is ADMIN ("dangerous
    // ones = admin"), which also satisfies the story's "admin/GM only" for kick.

    // .additem <template_id> [count] — GM. Grant a server-minted, DB-persisted item.
    add(Command{
        "additem",
        Level::kGm,
        "grant an item to yourself: .additem <itemId> [count]",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::vector<std::string> toks = split_args(cmd.args);
            if (toks.empty() || toks.size() > 2) {
                reply("Usage: .additem <itemId> [count]");
                return;
            }
            std::uint32_t template_id = 0;
            if (!parse_u32(toks[0], template_id) || template_id == 0) {
                reply("Invalid item id: '" + toks[0] + "' (expected a positive number).");
                return;
            }
            std::uint32_t count = 1;
            if (toks.size() == 2 && (!parse_u32(toks[1], count) || count == 0)) {
                reply("Invalid count: '" + toks[1] + "' (expected a positive number).");
                return;
            }
            if (!fx.add_item) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const AddItemResult r = fx.add_item(template_id, count);
            switch (r.status) {
                case EffectStatus::kApplied:
                    reply("Added " + std::to_string(r.count) + "x " + r.item_name +
                          " (item " + std::to_string(template_id) + ").");
                    break;
                case EffectStatus::kUnknownItem:
                    reply("No such item: " + std::to_string(template_id) + ".");
                    break;
                case EffectStatus::kNoSpace:
                    reply("Your inventory is full.");
                    break;
                default:
                    reply(generic_effect_failure(r.status));
                    break;
            }
        },
    });

    // .setlevel <n> — GM. Set the caller's server-authoritative level (clamped).
    add(Command{
        "setlevel",
        Level::kGm,
        "set your level: .setlevel <n>",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::vector<std::string> toks = split_args(cmd.args);
            std::uint32_t requested = 0;
            if (toks.size() != 1 || !parse_u32(toks[0], requested) || requested == 0) {
                reply("Usage: .setlevel <n> (1.." + std::to_string(kMaxLevel) + ")");
                return;
            }
            // Server clamps to the valid range (SAD "server is law") — an above-cap
            // request is honoured AT the cap, not rejected. Done here (pure) so the
            // seam just applies the final level.
            const std::uint16_t clamped =
                requested > kMaxLevel ? kMaxLevel : static_cast<std::uint16_t>(requested);
            if (!fx.set_level) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const SetLevelResult r = fx.set_level(clamped);
            if (r.status == EffectStatus::kApplied) {
                reply("Level set to " + std::to_string(r.applied_level) + ".");
            } else {
                reply(generic_effect_failure(r.status));
            }
        },
    });

    // .tele <x> <y> <z> — GM. Teleport the caller to a validated in-bounds point.
    add(Command{
        "tele",
        Level::kGm,
        "teleport yourself: .tele <x> <y> <z>",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::vector<std::string> toks = split_args(cmd.args);
            if (toks.size() != 3) {
                reply("Usage: .tele <x> <y> <z>");
                return;
            }
            Position dest;
            if (!parse_finite_float(toks[0], dest.x) ||
                !parse_finite_float(toks[1], dest.y) ||
                !parse_finite_float(toks[2], dest.z)) {
                reply("Invalid coordinates (expected three numbers: x y z).");
                return;
            }
            // Server validates map bounds (SAD §5.5 R4/R5) — a GM teleport is still
            // constrained to the play area + ground tolerance; the client never
            // dictates an out-of-world position.
            if (dest.x < movement::kZoneMinXY || dest.x > movement::kZoneMaxXY ||
                dest.y < movement::kZoneMinXY || dest.y > movement::kZoneMaxXY) {
                reply("Destination is out of bounds (x,y must be within " +
                      fmt_coord(movement::kZoneMinXY) + ".." +
                      fmt_coord(movement::kZoneMaxXY) + ").");
                return;
            }
            if (std::fabs(dest.z - movement::kFlatGroundZ) > movement::kHeightTolerance) {
                reply("Destination z is too far from the ground.");
                return;
            }
            if (!fx.teleport) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const EffectStatus st = fx.teleport(dest);
            if (st == EffectStatus::kApplied) {
                reply("Teleported to (" + fmt_coord(dest.x) + ", " + fmt_coord(dest.y) +
                      ", " + fmt_coord(dest.z) + ").");
            } else {
                reply(generic_effect_failure(st));
            }
        },
    });

    // .summon <name> — GM. Bring an online player to the caller's position.
    add(Command{
        "summon",
        Level::kGm,
        "summon a player to you: .summon <name>",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::string name = cmd.args;  // a name may contain no spaces at M1
            if (name.empty()) { reply("Usage: .summon <name>"); return; }
            if (!fx.summon) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const EffectStatus st = fx.summon(name);
            switch (st) {
                case EffectStatus::kApplied:
                    reply("Summoned " + name + " to your position.");
                    break;
                case EffectStatus::kTargetOffline:
                    reply("No online player named '" + name + "'.");
                    break;
                default:
                    reply(generic_effect_failure(st));
                    break;
            }
        },
    });

    // .kick <name> — ADMIN. Disconnect an online player's session.
    add(Command{
        "kick",
        Level::kAdmin,
        "disconnect a player: .kick <name>",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::string name = cmd.args;
            if (name.empty()) { reply("Usage: .kick <name>"); return; }
            if (!fx.kick) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const EffectStatus st = fx.kick(name);
            switch (st) {
                case EffectStatus::kApplied:
                    reply("Kicked " + name + ".");
                    break;
                case EffectStatus::kTargetOffline:
                    reply("No online player named '" + name + "'.");
                    break;
                default:
                    reply(generic_effect_failure(st));
                    break;
            }
        },
    });

    // ── MODERATION (OPS-02c, #419) ─────────────────────────────────────────
    // .ban is the most destructive moderation action (it locks a player out), so
    // it is ADMIN, mirroring .kick. .mute (silences chat, reversible on expiry) is
    // GM. Both PARSE their args here, then invoke a seam that resolves the subject
    // + writes the durable record; the server trusts nothing beyond the parsed args.

    // .ban <account|char|ip> <subject> [duration] [reason] — ADMIN.
    add(Command{
        "ban",
        Level::kAdmin,
        "ban a subject: .ban <account|char|ip> <subject> [duration] [reason]",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::vector<std::string> toks = split_args(cmd.args);
            if (toks.size() < 2) {
                reply("Usage: .ban <account|char|ip> <subject> [duration] [reason]");
                return;
            }
            BanSubject subject;
            if (toks[0] == "account") {
                subject = BanSubject::kAccount;
            } else if (toks[0] == "char" || toks[0] == "character") {
                subject = BanSubject::kCharacter;
            } else if (toks[0] == "ip") {
                subject = BanSubject::kIp;
            } else {
                reply("Unknown ban kind '" + toks[0] +
                      "' (expected account, char, or ip).");
                return;
            }
            const std::string& target = toks[1];
            // Optional [duration] then [reason]: token[2] is a duration IFF it parses
            // as one, else it is the first word of the reason (permanent ban).
            std::optional<std::uint64_t> duration;
            std::size_t reason_from = 2;
            if (toks.size() >= 3) {
                if (std::optional<std::uint64_t> d = parse_duration(toks[2])) {
                    duration = d;
                    reason_from = 3;
                }
            }
            std::string reason = join_from(toks, reason_from);
            if (reason.empty()) reason = "banned by GM";
            if (!fx.ban) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const BanResult r = fx.ban(subject, target, duration, reason);
            switch (r.status) {
                case EffectStatus::kApplied:
                    reply("Banned " + r.subject_desc +
                          (duration ? " (temporary)." : " (permanent)."));
                    break;
                case EffectStatus::kTargetOffline:
                    reply("No such " + std::string(toks[0]) + ": '" + target + "'.");
                    break;
                default:
                    reply(generic_effect_failure(r.status));
                    break;
            }
        },
    });

    // .mute <char> [duration] [reason] — GM.
    add(Command{
        "mute",
        Level::kGm,
        "silence a character's chat: .mute <char> [duration] [reason]",
        [](const Registry& /*reg*/, const ParsedCommand& cmd, std::uint8_t /*level*/,
           const GmEffects& fx, const ReplyFn& reply) {
            const std::vector<std::string> toks = split_args(cmd.args);
            if (toks.empty()) {
                reply("Usage: .mute <char> [duration] [reason]");
                return;
            }
            const std::string& name = toks[0];
            std::optional<std::uint64_t> duration;
            std::size_t reason_from = 1;
            if (toks.size() >= 2) {
                if (std::optional<std::uint64_t> d = parse_duration(toks[1])) {
                    duration = d;
                    reason_from = 2;
                }
            }
            std::string reason = join_from(toks, reason_from);
            if (reason.empty()) reason = "muted by GM";
            if (!fx.mute) { reply(generic_effect_failure(EffectStatus::kUnavailable)); return; }
            const MuteResult r = fx.mute(name, duration, reason);
            switch (r.status) {
                case EffectStatus::kApplied:
                    reply("Muted " + r.subject_desc +
                          (duration ? " (temporary)." : " (permanent)."));
                    break;
                case EffectStatus::kTargetOffline:
                    reply("No such character: '" + name + "'.");
                    break;
                default:
                    reply(generic_effect_failure(r.status));
                    break;
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
                                const GmEffects& fx, const ReplyFn& reply,
                                const AuditFn& audit) {
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
            c->handler(reg, cmd, level, fx, reply);
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

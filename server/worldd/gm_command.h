// SPDX-License-Identifier: Apache-2.0
//
// worldd — GM COMMAND FRAMEWORK (OPS-02a, #417; epic #21).
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md §4-M1 (OPS-02 GM commands) +
// §6 ("separate append-only audit streams for GM actions"), the D-16 permission
// model (player < helper < GM < admin), and the OPS-05 audit facility
// (meridian/core/audit.hpp, #92). No GPL source consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. GM commands are typed by players as chat lines prefixed with a
// '.' (e.g. `.tele`), so they ride the EXISTING CHAT_MESSAGE opcode — there is NO
// new wire opcode and no world.fbs change. worldd intercepts a '.'-prefixed line
// at the chat path (world_dispatch's CHAT_MESSAGE handler) BEFORE normal chat
// routing, parses {command, args}, and dispatches it here: the registry looks the
// command up, the permission gate checks it against the SESSION's gm_level (learnt
// at the WORLD_HELLO handshake from account.gm_level), and EVERY attempt — allowed
// AND denied AND unknown — is written to the append-only GM audit stream (PRD §6).
//
// This is a PURE, socket-free, DB-free library: `dispatch_command` takes the raw
// line + the caller's level + two sinks (a reply sink and an audit sink) and does
// all the parse/gate/audit/execute logic, so a unit test drives the exact path the
// live dispatch runs without standing up a TLS session. The dispatch adapter in
// world_dispatch.cpp only wires the reply sink to a CHAT_DELIVER and the audit
// sink to core::audit::emit.
//
// M1 SCOPE. Exactly ONE command is registered — `.help` (lists the commands
// available at the caller's level) — to prove the framework end to end. The full
// command set (.tele/.summon/.additem/…) is the next story (#418) and slots in by
// registering more Commands; the framework here does not change.

#ifndef MERIDIAN_WORLDD_GM_COMMAND_H
#define MERIDIAN_WORLDD_GM_COMMAND_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "meridian/core/audit.hpp"

namespace meridian::worldd::gm {

// The chat prefix that marks a line as a GM command rather than a chat message.
inline constexpr char kCommandPrefix = '.';

// D-16 GM permission ladder. The underlying values MATCH the auth DB
// account.gm_level (TINYINT UNSIGNED) so a raw column value casts directly and a
// numeric compare is the permission test. Ordered: a higher value outranks a
// lower one.
enum class Level : std::uint8_t {
    kPlayer = 0,
    kHelper = 1,
    kGm     = 2,
    kAdmin  = 3,
};

// The stable, lowercase name for a raw gm_level ("player"/"helper"/"gm"/"admin").
// A value above kAdmin still names as "admin" (a super-admin ranks as admin for
// display). Used in the audit record + the `.help` header.
const char* level_name(std::uint8_t raw);
inline const char* level_name(Level level) {
    return level_name(static_cast<std::uint8_t>(level));
}

// Whether `text` is a GM command line — its first character is the prefix. A
// leading space is NOT trimmed: ".x" is a command, " .x" is ordinary chat (the
// client convention that a command starts at column 0).
bool is_command(std::string_view text);

// A parsed command line: the lowercased command name (prefix stripped) + the raw
// argument tail (the remainder after the whitespace run following the name, with
// leading/trailing whitespace removed). `.tele  Stormwind 1 2` -> {name:"tele",
// args:"Stormwind 1 2"}; `.help` -> {name:"help", args:""}; "." -> {name:"",…}.
struct ParsedCommand {
    std::string name;
    std::string args;
};
ParsedCommand parse_command(std::string_view text);

// The outcome of a command attempt — drives both the reply and the audit outcome.
enum class CommandOutcome {
    kOk,       // recognised + permitted + executed
    kDenied,   // recognised but the caller's level is below the command's minimum
    kUnknown,  // no such command (still audited — a probe is security-relevant)
};

// The machine-readable audit `reason` for a non-OK outcome ("insufficient_level"
// / "unknown_command"); the empty string for kOk (a success carries no reason).
const char* outcome_reason(CommandOutcome outcome);

class Registry;  // fwd — a Command handler reads the registry (e.g. `.help`).

// A server→sender reply sink: invoked once per reply line the command produces.
using ReplyFn = std::function<void(const std::string& line)>;

// A command handler: a PURE function of the parsed args + the caller's level that
// emits its reply through `reply`. It runs ONLY when the framework has already
// permitted the caller — a handler never does its own permission check. `reg` is
// the owning registry (so `.help` can enumerate visible commands).
using Handler = std::function<void(const Registry& reg, const ParsedCommand& cmd,
                                   std::uint8_t caller_level, const ReplyFn& reply)>;

// A registered GM command: its name, the D-16 threshold the caller must meet, the
// one-line `.help` description, and the handler that produces its reply.
struct Command {
    std::string name;
    Level min_level;
    std::string help;
    Handler handler;
};

// The server-side GM command REGISTRY. Immutable after construction; the process
// shares one via builtin(). M1 registers exactly one command (`.help`).
class Registry {
public:
    Registry();

    // The process-wide builtin registry (the M1 command set: just `.help`).
    static const Registry& builtin();

    // The command named `name` (lowercase), or nullptr if unregistered.
    const Command* find(std::string_view name) const;

    // Every command whose min_level the caller at raw `level` meets, in stable
    // name-sorted order — the set `.help` lists for that caller.
    std::vector<const Command*> visible_to(std::uint8_t level) const;

    // Register (or override a same-named) command. Setup only; not thread-safe.
    void add(Command cmd);

private:
    std::vector<Command> commands_;  // kept sorted by name
};

// Build the append-only GM-audit record for one command attempt (PRD §6). PURE —
// no sink, no clock — so a unit test can assert the EXACT record the live dispatch
// emits. `account_id` attributes the actor (0 => omitted); `level` is the caller's
// raw gm_level; `cmd` the parsed attempt; `outcome` allowed/denied/unknown. The
// record renders with action=gm_command, outcome success (kOk) / failure (else),
// target="command:<name>", reason=the denial code, and extra {gm_level,
// gm_level_name, args?}. NEVER carries secret material (audit no-secrets rule).
core::audit::Record build_command_audit(std::uint64_t account_id, std::uint8_t level,
                                        const ParsedCommand& cmd,
                                        CommandOutcome outcome);

// An audit sink: invoked once with the attempt's record. The live dispatch passes
// core::audit::emit; a test passes a capturing sink.
using AuditFn = std::function<void(const core::audit::Record& rec)>;

// PARSE + PERMISSION-GATE + AUDIT + EXECUTE one GM command line against `reg`.
// `text` is the raw chat line (the caller has already confirmed it starts with the
// prefix). `level` is the caller's raw gm_level; `account_id` attributes the audit.
// `reply` emits system lines back to the sender; `audit` records the attempt and
// is ALWAYS called — for an allowed, denied, AND unknown command. Returns the
// outcome. No wire or DB dependency.
CommandOutcome dispatch_command(const Registry& reg, std::string_view text,
                                std::uint64_t account_id, std::uint8_t level,
                                const ReplyFn& reply, const AuditFn& audit);

}  // namespace meridian::worldd::gm

#endif  // MERIDIAN_WORLDD_GM_COMMAND_H

// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-opcode rate classes implementation (OPS-03b, #421).
// See rate_class.h for the provenance + design.

#include "rate_class.h"

#include <cstdio>
#include <string>

#include "world_generated.h"  // net::EnumNameOpcode

namespace meridian::worldd {
namespace audit = meridian::core::audit;
namespace mn = meridian::net;

RateClass rate_class_for(net::Opcode op) {
    switch (op) {
        // --- session / system + character management (0x0xxx) ------------------
        case net::Opcode::WORLD_HELLO:
        case net::Opcode::DISCONNECT:
        case net::Opcode::CLOCK_SYNC:
        case net::Opcode::CHAR_LIST_REQUEST:
        case net::Opcode::CHAR_CREATE_REQUEST:
        case net::Opcode::CHAR_DELETE_REQUEST:
        case net::Opcode::ENTER_WORLD_REQUEST:
            return RateClass::kSession;

        // --- movement (0x1xxx) -------------------------------------------------
        case net::Opcode::MOVEMENT_INTENT:
            return RateClass::kMove;

        // --- chat (0x6xxx) — GM commands ride CHAT_MESSAGE (#417) ---------------
        case net::Opcode::CHAT_MESSAGE:
            return RateClass::kChat;

        // --- gameplay actions: combat / death / quest / loot / vendor / npc ----
        case net::Opcode::CAST_REQUEST:
        case net::Opcode::RELEASE_REQUEST:
        case net::Opcode::RESURRECT_REQUEST:
        case net::Opcode::QUEST_ACCEPT:
        case net::Opcode::QUEST_TURN_IN:
        case net::Opcode::QUEST_LOG:
        case net::Opcode::LOOT_REQUEST:
        case net::Opcode::LOOT_TAKE:
        case net::Opcode::LOOT_RELEASE:
        case net::Opcode::VENDOR_BUY_REQUEST:
        case net::Opcode::VENDOR_SELL_REQUEST:
        case net::Opcode::VENDOR_BUYBACK_REQUEST:
        case net::Opcode::EQUIPMENT_CHANGE_REQUEST:
        case net::Opcode::GOSSIP_HELLO:
        case net::Opcode::TRAINER_LEARN:
            return RateClass::kAction;

        default:
            // Any other (e.g. a newly-handled opcode not yet classified, or an
            // S→C opcode that somehow has a handler) gets the most generous
            // ceiling so it is never accidentally throttled before classification.
            return RateClass::kSession;
    }
}

int rate_class_limit(RateClass rc) {
    switch (rc) {
        case RateClass::kSession: return kSessionRatePerWindow;
        case RateClass::kMove:    return kMoveRatePerWindow;
        case RateClass::kChat:    return kChatRatePerWindow;
        case RateClass::kAction:  return kActionRatePerWindow;
    }
    return kSessionRatePerWindow;
}

const char* rate_class_name(RateClass rc) {
    switch (rc) {
        case RateClass::kSession: return "session";
        case RateClass::kMove:    return "move";
        case RateClass::kChat:    return "chat";
        case RateClass::kAction:  return "action";
    }
    return "session";
}

bool RateLimiter::admit(RateClass rc, std::uint64_t now_ms) {
    Bucket& b = buckets_[static_cast<std::size_t>(rc)];
    // Prune stamps older than the trailing window (a stamp at exactly now-window is
    // expired; the window is the last kRateWindowMs, exclusive of the boundary).
    while (!b.window.empty() && now_ms - b.window.front() >= kRateWindowMs) {
        b.window.pop_front();
    }
    if (static_cast<int>(b.window.size()) >= rate_class_limit(rc)) {
        ++b.dropped;
        return false;
    }
    b.window.push_back(now_ms);
    ++b.admitted;
    return true;
}

std::uint64_t RateLimiter::dropped(RateClass rc) const {
    return buckets_[static_cast<std::size_t>(rc)].dropped;
}

std::uint64_t RateLimiter::admitted(RateClass rc) const {
    return buckets_[static_cast<std::size_t>(rc)].admitted;
}

core::audit::Record build_rate_limited_audit(std::uint64_t account_id,
                                             std::uint64_t grant_id, net::Opcode op,
                                             RateClass rc) {
    audit::Record rec;
    rec.action = audit::Action::kRateLimited;
    rec.outcome = audit::Outcome::kFailure;  // a flood drop is a warn-level flag
    rec.account_id = account_id;             // 0 => omitted
    // The offending opcode as a stable name (or a hex fallback for an unknown value).
    const char* name = mn::EnumNameOpcode(op);
    std::string opname;
    if (name != nullptr && name[0] != '\0') {
        opname = name;
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(op));
        opname = buf;
    }
    rec.target = "opcode:" + opname;
    rec.reason = rate_class_name(rc);  // the rate class (low-cardinality)
    rec.correlation_id = grant_id;     // 0 => omitted; pivots audit↔trace↔log
    return rec;
}

}  // namespace meridian::worldd

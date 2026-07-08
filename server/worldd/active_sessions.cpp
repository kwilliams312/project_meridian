// SPDX-License-Identifier: Apache-2.0
//
// worldd — single active session per account (issue #326). See active_sessions.h
// for the design (kick-old policy + the token compare-and-remove guarantee).

#include "active_sessions.h"

#include <utility>

namespace meridian::worldd {

AdmitResult ActiveSessionRegistry::admit(std::uint64_t account_id, KickFn kick) {
    KickFn displaced;  // the evicted session's kick, run AFTER we drop the lock.
    AdmitResult result;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const SessionToken token = next_token_++;
        result.token = token;
        auto it = by_account_.find(account_id);
        if (it != by_account_.end()) {
            // An account already in-world: take over the slot (kick-old) and hand
            // the previous holder's kick out to fire below, off the lock.
            displaced = std::move(it->second.kick);
            it->second.token = token;
            it->second.kick = std::move(kick);
            result.kicked_previous = true;
        } else {
            by_account_.emplace(account_id, Entry{token, std::move(kick)});
        }
    }
    // Evict the displaced session OUTSIDE the lock: its kick may write a Disconnect
    // to a socket and drop the session from the AoI world, neither of which may run
    // under the registry mutex (deadlock-free by construction — file header).
    if (displaced) displaced();
    return result;
}

bool ActiveSessionRegistry::release(std::uint64_t account_id, SessionToken token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = by_account_.find(account_id);
    // Compare-and-remove: only the CURRENT holder may drop the entry. A session
    // that was kicked (token overwritten by its replacement) fails this check and
    // leaves the replacement's entry untouched.
    if (it != by_account_.end() && it->second.token == token) {
        by_account_.erase(it);
        return true;
    }
    return false;
}

std::size_t ActiveSessionRegistry::active_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return by_account_.size();
}

bool ActiveSessionRegistry::is_active(std::uint64_t account_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return by_account_.find(account_id) != by_account_.end();
}

}  // namespace meridian::worldd

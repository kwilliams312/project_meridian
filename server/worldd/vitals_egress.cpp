// SPDX-License-Identifier: Apache-2.0
//
// worldd — VitalsEgressRegistry implementation (issue #437). See vitals_egress.h.

#include "vitals_egress.h"

#include <utility>

namespace meridian::worldd {

VitalsEgressToken VitalsEgressRegistry::register_session(ObjectGuid guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    Entry& e = pending_[guid];  // keeps any pending snapshot (same-guid re-login)
    e.token = next_token_++;    // fresh holder — a prior holder's unregister now no-ops
    return e.token;
}

void VitalsEgressRegistry::unregister_session(ObjectGuid guid, VitalsEgressToken token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(guid);
    if (it == pending_.end() || it->second.token != token) return;  // stale / absent — no-op
    pending_.erase(it);
}

void VitalsEgressRegistry::push_vitals(const VitalsSnapshot& snap) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(snap.guid);
    if (it == pending_.end()) return;  // not a registered session — drop the snapshot
    it->second.pending = snap;         // coalesce onto the latest absolute state
}

std::optional<VitalsSnapshot> VitalsEgressRegistry::drain_vitals(ObjectGuid guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(guid);
    if (it == pending_.end() || !it->second.pending) return std::nullopt;
    std::optional<VitalsSnapshot> out;
    out.swap(it->second.pending);  // hand off + clear, leaving the registration in place
    return out;
}

bool VitalsEgressRegistry::is_registered(ObjectGuid guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.find(guid) != pending_.end();
}

}  // namespace meridian::worldd

// SPDX-License-Identifier: Apache-2.0
//
// worldd — QuestCreditRegistry implementation (issue #396). See quest_credit.h.

#include "quest_credit.h"

#include <utility>

namespace meridian::worldd {

QuestCreditToken QuestCreditRegistry::register_session(ObjectGuid guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    Entry& e = pending_[guid];  // keeps any queue already present (same-guid re-login)
    e.token = next_token_++;    // fresh holder — a prior holder's unregister now no-ops
    return e.token;
}

void QuestCreditRegistry::unregister_session(ObjectGuid guid, QuestCreditToken token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(guid);
    if (it == pending_.end() || it->second.token != token) return;  // stale / absent — no-op
    pending_.erase(it);
}

void QuestCreditRegistry::push_kill(ObjectGuid killer, std::uint32_t npc_template_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(killer);
    if (it == pending_.end()) return;  // not a registered session — drop the kill
    it->second.kills.push_back(npc_template_id);
}

std::vector<std::uint32_t> QuestCreditRegistry::drain_kills(ObjectGuid guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(guid);
    if (it == pending_.end() || it->second.kills.empty()) return {};
    std::vector<std::uint32_t> out;
    out.swap(it->second.kills);  // hand off the queue, leaving the registration in place
    return out;
}

bool QuestCreditRegistry::is_registered(ObjectGuid guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.find(guid) != pending_.end();
}

}  // namespace meridian::worldd

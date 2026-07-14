// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters — runtime roster (roster.h). Clean-room, original code.

#include "roster.h"

namespace meridian::characters {

void Roster::add_race(std::uint8_t id, std::string name) {
    if (id == 0) return;  // 0 reserved as unset/invalid — never a roster entry
    races_[id] = std::move(name);
}

void Roster::add_class(std::uint8_t id, std::string name) {
    if (id == 0) return;
    classes_[id] = std::move(name);
}

std::string_view Roster::race_name(std::uint8_t id) const {
    const auto it = races_.find(id);
    return it == races_.end() ? std::string_view{} : std::string_view{it->second};
}

std::string_view Roster::class_name(std::uint8_t id) const {
    const auto it = classes_.find(id);
    return it == classes_.end() ? std::string_view{} : std::string_view{it->second};
}

const Roster& Roster::compiled_fallback() {
    // ONLY the four M0 entries the pack cannot author yet (see roster.h). Everything
    // else in the runtime roster comes from pack data.
    static const Roster kFallback = [] {
        Roster r;
        r.add_race(kRaceSylvane, "Sylvane");    // forest folk (needs SP5 appearance)
        r.add_race(kRaceEmberkin, "Emberkin");  // fire-touched folk (needs SP5 appearance)
        r.add_class(kClassRuncaller, "Runcaller");  // arcane caster (needs SP5 abilities)
        r.add_class(kClassMender, "Mender");        // healer (needs SP5 abilities)
        return r;
    }();
    return kFallback;
}

const Roster& Roster::offline_full() {
    // compiled_fallback() ∪ a mirror of the seed pack's four entries. Offline/test
    // convenience only; the pack is the runtime source of truth for these four.
    static const Roster kFull = [] {
        Roster r = compiled_fallback();
        r.add_race(kRaceArdent, "Ardent");
        r.add_race(kRaceDolmen, "Dolmen");
        r.add_class(kClassVanguard, "Vanguard");
        r.add_class(kClassWarden, "Warden");
        return r;
    }();
    return kFull;
}

}  // namespace meridian::characters

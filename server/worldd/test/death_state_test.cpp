// SPDX-License-Identifier: Apache-2.0
//
// worldd — player death state machine UNIT test (issue #359, CMB-03). See
// death_state.h.
//
// PURE (no DB, no socket): drives DeathStateMachine directly — on_death spawns a
// corpse + enters kCorpse; requested + auto release → kGhost; corpse-run distance
// gating for resurrect (NOT_RELEASED / TOO_FAR / OK); resurrect despawns the
// corpse + reports the health-% seam. Deterministic (dt passed in).

#include "death_state.h"

#include "combat_unit.h"
#include "movement_validation.h"

#include <cstdio>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    return p;
}

constexpr ObjectGuid kPlayer = 42;

}  // namespace

int main() {
    std::printf("worldd death state machine (#359)\n");

    // --- on_death: corpse spawned at the death spot, enters kCorpse. -------------
    {
        DeathStateMachine dsm;  // default config (auto_release 6000ms, rez 50%)
        const Position death = at(10, 20);
        const Position grave = at(100, 100);
        const ObjectGuid corpse = dsm.on_death(kPlayer, death, grave);

        check("corpse guid is in the reserved corpse band", corpse >= kCorpseGuidBase);
        check("player is now kCorpse", dsm.phase_of(kPlayer) == DeathPhase::kCorpse);
        check("dead_count = 1", dsm.dead_count() == 1);
        const Corpse* c = dsm.corpse(corpse);
        check("corpse object exists", c != nullptr);
        check("corpse owner = the dead player", c != nullptr && c->owner_guid() == kPlayer);
        check("corpse is at the death spot",
              c != nullptr && c->position().x == 10 && c->position().y == 20);
        const DeathRecord* r = dsm.record(kPlayer);
        check("release timer armed to auto_release_ms",
              r != nullptr && r->release_remaining_ms == dsm.config().auto_release_ms);

        // Cannot resurrect while still a corpse (must release first).
        ResurrectReject why = ResurrectReject::kNone;
        check("kCorpse cannot resurrect (NOT_RELEASED)",
              !dsm.can_resurrect(kPlayer, death, why) && why == ResurrectReject::kNotReleased);
    }

    // --- requested release: kCorpse → kGhost immediately. ------------------------
    {
        DeathStateMachine dsm;
        dsm.on_death(kPlayer, at(0, 0), at(50, 0));
        check("request_release applies to a corpse", dsm.request_release(kPlayer));
        check("player is now kGhost", dsm.phase_of(kPlayer) == DeathPhase::kGhost);
        check("re-releasing a ghost is a no-op", !dsm.request_release(kPlayer));
    }

    // --- auto release: timer elapses → kGhost, reported once. --------------------
    {
        DeathConfig cfg;
        cfg.auto_release_ms = 3000;
        DeathStateMachine dsm(cfg);
        dsm.on_death(kPlayer, at(0, 0), at(50, 0));

        std::vector<ObjectGuid> released;
        dsm.advance(1000, released);
        check("not yet auto-released after 1s", released.empty() &&
                                                    dsm.phase_of(kPlayer) == DeathPhase::kCorpse);
        dsm.advance(1000, released);
        dsm.advance(1000, released);  // total 3000ms → elapses
        check("auto-released once the timer elapses", released.size() == 1 &&
                                                          released[0] == kPlayer);
        check("player is kGhost after auto-release", dsm.phase_of(kPlayer) == DeathPhase::kGhost);

        // A second advance does not re-report an already-released ghost.
        released.clear();
        dsm.advance(1000, released);
        check("released ghost is not re-reported", released.empty());
    }

    // --- corpse-run gating + resurrect. ------------------------------------------
    {
        DeathStateMachine dsm;  // corpse_run_radius_m = 2.0
        const Position death = at(10, 10);
        const ObjectGuid corpse = dsm.on_death(kPlayer, death, at(50, 50));
        dsm.request_release(kPlayer);  // → ghost

        ResurrectReject why = ResurrectReject::kNone;
        check("ghost far from corpse cannot resurrect (TOO_FAR)",
              !dsm.can_resurrect(kPlayer, at(50, 50), why) && why == ResurrectReject::kTooFar);
        check("ghost AT the corpse may resurrect (corpse-run complete)",
              dsm.can_resurrect(kPlayer, at(10, 10), why) && why == ResurrectReject::kNone);
        check("just within the corpse-run radius may resurrect",
              dsm.can_resurrect(kPlayer, at(11.0f, 10.0f), why));

        const ObjectGuid despawned = dsm.resurrect(kPlayer);
        check("resurrect returns the corpse to despawn", despawned == corpse);
        check("player is alive again (no record)", dsm.phase_of(kPlayer) == DeathPhase::kAlive);
        check("dead_count back to 0", dsm.dead_count() == 0);
        check("corpse object is despawned", dsm.corpse(corpse) == nullptr);
    }

    // --- resurrect health %: config pct of max, clamped to >= 1. -----------------
    {
        DeathConfig cfg;
        cfg.resurrect_health_pct = 50;
        DeathStateMachine dsm(cfg);
        check("50% of 200 = 100", dsm.resurrect_health(200) == 100);
        check("50% of 1 clamps up to 1", dsm.resurrect_health(1) == 1);
    }

    // --- unknown guid: not dead. -------------------------------------------------
    {
        DeathStateMachine dsm;
        ResurrectReject why = ResurrectReject::kNone;
        check("unknown guid is NOT_DEAD",
              !dsm.can_resurrect(999, at(0, 0), why) && why == ResurrectReject::kNotDead);
        check("resurrect of unknown guid returns 0", dsm.resurrect(999) == 0);
    }

    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}

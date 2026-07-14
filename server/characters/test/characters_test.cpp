// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters integration test (CHR-01 stub, D-11; issue #85).
//
// Needs a live MariaDB; reads connection params from MERIDIAN_DB_* env (the same
// var names as meridian-db's test) and SKIPS (exit 0) when none are set, so it is
// inert in the plain server build's ctest and runs for real only in the DB CI
// job or locally via `scripts/dev/test.sh --db`.
//
// What it proves end-to-end against the characters DB `character` table:
//   1. create -> list shows the new character (fields round-trip).
//   2. a duplicate name is refused (DuplicateName / uq_character_name).
//   3. an invalid race id is refused (InvalidRace / M0-frozen roster).
//   4. an invalid class id is refused (InvalidClass / M0-frozen roster).
//   5. deleting ANOTHER account's character is refused (ownership predicate) —
//      the row survives.
//   6. deleting your OWN character removes it (list no longer shows it).
//   7. a > INT64_MAX account_id round-trips through create + list, proving the
//      BIGINT-UNSIGNED decimal-string binding (the meridian-db signed-bind
//      gotcha) is handled.
//
// The test is self-contained like the account test: it CREATE-TABLE-IF-NOT-EXISTS
// the `character` table (standalone — the character table has no outgoing FK, so
// it stands alone in any schema), operates only on its own randomised
// account_ids, and cleans those rows up at the end. It never drops a table.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "characters.h"
#include "meridian/db/connection.h"
#include "roster.h"

using namespace meridian;

namespace {
int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Standalone `character` table DDL — mirrors the columns of
// server/db/characters/migrations/0001_init_characters.up.sql. The character
// table has NO outgoing foreign keys (account_id is a soft ref, §4.4), so this
// stands alone in a fresh schema; CREATE ... IF NOT EXISTS is a no-op when the
// real migration already loaded it (the --db harness loads it into
// meridian_characters).
constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id            BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,"
    "  account_id    BIGINT UNSIGNED   NOT NULL,"
    "  name          VARCHAR(32)       NOT NULL,"
    "  race          TINYINT UNSIGNED  NOT NULL,"
    "  class         TINYINT UNSIGNED  NOT NULL,"
    "  appearance    JSON              NULL,"
    "  level         SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp            INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  money         BIGINT UNSIGNED   NOT NULL DEFAULT 0,"
    "  map_id        INT UNSIGNED      NOT NULL,"
    "  instance_id   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  pos_x         FLOAT             NOT NULL,"
    "  pos_y         FLOAT             NOT NULL,"
    "  pos_z         FLOAT             NOT NULL,"
    "  pos_o         FLOAT             NOT NULL DEFAULT 0,"
    "  played_time   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  logout_at     DATETIME          NULL,"
    "  save_epoch    BIGINT            NOT NULL DEFAULT 0,"
    "  created_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// True iff `list` contains a character with the given id.
bool has_id(const std::vector<characters::CharacterSummary>& list, std::uint64_t id) {
    for (const auto& c : list) {
        if (c.id == id) return true;
    }
    return false;
}

// --- Concurrency (scenario 9) ------------------------------------------------
// MariaDB error number for a detected lock cycle (ER_LOCK_DEADLOCK). Mirrors the
// constant create_character retries on; used here to recognise the deadlock we
// are deliberately reproducing.
constexpr unsigned kErLockDeadlock = 1213;

// Minimal single-use two-party rendezvous. Both parties call sync(); neither
// returns until BOTH have arrived — used to align two threads at a precise point
// so their transactions interleave deterministically. Hand-rolled (not
// std::barrier) so the test does not depend on <barrier>, which some macOS libc++
// versions still omit. A party that errors out before its sync() calls arrive()
// instead so its peer is never left spinning.
struct TwoGate {
    std::atomic<int> n{0};
    void arrive() { n.fetch_add(1, std::memory_order_acq_rel); }
    void sync() {
        arrive();
        while (n.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    }
};

// Outcome of one concurrent create attempt (each runs on its own thread + its
// own connection — meridian-db Connections are not shared across threads).
struct ConcOutcome {
    bool ok = false;       // create + COMMIT succeeded
    unsigned db_code = 0;  // db::DbError code that escaped (0 = none)
    std::string err;       // exception message, if any
};

// The RAW check-then-insert transaction create_character ran BEFORE the fix,
// with NO deadlock retry. `gate` is synced AFTER the FOR UPDATE so both threads
// hold their idx_character_account gap lock before either INSERTs — making the
// cross-account insert-intention deadlock deterministic. Forces REPEATABLE READ
// so the gap lock (hence the deadlock) does not hinge on the server default.
// This is the CONTROL: it proves the pattern is deadlock-prone.
ConcOutcome raw_create_no_retry(const db::ConnectParams& p, std::uint64_t account_id,
                                const std::string& name, TwoGate& gate) {
    ConcOutcome o;
    bool synced = false;
    try {
        db::Connection conn(p);
        conn.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
        conn.execute("START TRANSACTION");
        try {
            conn.execute(
                "SELECT COUNT(*) FROM `character` WHERE account_id = ? FOR UPDATE",
                {db::Param{std::to_string(account_id)}});
            gate.sync();  // both gap locks held -> now race the INSERT
            synced = true;
            conn.execute(
                "INSERT INTO `character` "
                "(account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
                "VALUES (?, ?, 1, 1, 0, 0, 0, 0)",
                {db::Param{std::to_string(account_id)}, db::Param{name}});
            conn.execute("COMMIT");
            o.ok = true;
        } catch (const db::DbError& e) {
            try { conn.execute("ROLLBACK"); } catch (...) { /* already rolled back */ }
            o.db_code = e.code();
            o.err = e.what();
        }
    } catch (const std::exception& e) {
        o.err = e.what();
    }
    if (!synced) gate.arrive();  // never leave the peer spinning
    return o;
}

// Runs create_character (the FIXED path, with retry) on its own thread/connection.
// `gate` aligns both threads at create_character's entry so their internal
// gap-lock windows overlap; the fix must absorb any resulting deadlock and still
// succeed for both distinct accounts.
ConcOutcome conc_create(const db::ConnectParams& p, characters::CreateRequest req,
                        TwoGate& gate) {
    ConcOutcome o;
    bool synced = false;
    try {
        db::Connection conn(p);
        gate.sync();  // align both threads at the create entry point
        synced = true;
        characters::CreateResult r = characters::create_character(conn, req);
        o.ok = r.character_id > 0;
    } catch (const db::DbError& e) {
        o.db_code = e.code();
        o.err = e.what();
    } catch (const std::exception& e) {
        o.err = e.what();
    }
    if (!synced) gate.arrive();
    return o;
}
}  // namespace

// Pure (no-DB) unit checks for AppearanceRecord::from_json — the durable-read
// fallback path (contract ① §5.2/§9). from_json takes a db::Cell (an
// optional<string> holding the raw `character.appearance` JSON column), so these
// exercise it directly with no MariaDB, and therefore run in the PLAIN server
// ctest (not only the --db job). A garbage/truncated JSON string must fall back to
// the versioned default {v:1,hair:1,face:1,skin:1} and never throw (#464).
void run_from_json_unit_checks() {
    using characters::AppearanceRecord;
    auto is_default = [](const AppearanceRecord& a) {
        return a.version == 1 && a.hair == 1 && a.face == 1 && a.skin == 1;
    };

    // Total garbage — not JSON at all. No recognisable key/number pairs, so every
    // field keeps its default; must not crash.
    check("from_json: non-JSON garbage falls back to defaults",
          is_default(AppearanceRecord::from_json(db::Cell{"@@@ not json at all !!!"})));

    // Structurally broken JSON (unterminated object, keys but no values).
    check("from_json: truncated/broken JSON falls back to defaults",
          is_default(AppearanceRecord::from_json(db::Cell{"{\"hair\": , \"face\""})));

    // A key present but immediately followed by a non-digit → that field stays
    // default (the purpose-built reader returns nullopt), the rest too.
    check("from_json: key with non-numeric value falls back to default",
          is_default(AppearanceRecord::from_json(db::Cell{"{\"skin\":\"oops\"}"})));

    // Empty string and NULL cell → the versioned default (spec §5.2).
    check("from_json: empty column falls back to defaults",
          is_default(AppearanceRecord::from_json(db::Cell{std::string{}})));
    check("from_json: NULL column falls back to defaults",
          is_default(AppearanceRecord::from_json(db::Cell{})));

    // A well-formed but OUT-OF-BOUNDS record is clamped, not rejected: version!=1
    // -> 1, preset id 0 -> 1, and an in-range id survives (normalise on read, §9).
    {
        AppearanceRecord a =
            AppearanceRecord::from_json(db::Cell{"{\"v\":9,\"hair\":0,\"face\":7,\"skin\":0}"});
        check("from_json: out-of-bounds record clamps to bounds on read",
              a.version == 1 && a.hair == 1 && a.face == 7 && a.skin == 1);
    }
}

// Pure (no-DB) unit checks for the Roster class `race_limits` gate (SP2.6 #696),
// exercising Roster::is_race_allowed_for_class directly. Runs in the PLAIN server
// ctest (no MariaDB) — the create-path enforcement is proven DB-backed below.
void run_roster_unit_checks() {
    using characters::Roster;

    // (1) offline_full() mirrors the seed pack's gate: Vanguard(1) is limited to
    //     Ardent(1) + Dolmen(2); every other class omits race_limits (all races).
    const Roster& full = Roster::offline_full();
    check("roster gate: Vanguard permits Ardent",
          full.is_race_allowed_for_class(characters::kClassVanguard, characters::kRaceArdent));
    check("roster gate: Vanguard permits Dolmen",
          full.is_race_allowed_for_class(characters::kClassVanguard, characters::kRaceDolmen));
    check("roster gate: Vanguard REFUSES Sylvane (∉ race_limits)",
          !full.is_race_allowed_for_class(characters::kClassVanguard, characters::kRaceSylvane));
    check("roster gate: Vanguard REFUSES Emberkin (∉ race_limits)",
          !full.is_race_allowed_for_class(characters::kClassVanguard, characters::kRaceEmberkin));
    // Warden(3) omits race_limits → permits every race, including the fallback ones.
    check("roster gate: Warden (no race_limits) permits Ardent",
          full.is_race_allowed_for_class(characters::kClassWarden, characters::kRaceArdent));
    check("roster gate: Warden (no race_limits) permits Sylvane",
          full.is_race_allowed_for_class(characters::kClassWarden, characters::kRaceSylvane));
    check("roster gate: Runcaller (no race_limits) permits Emberkin",
          full.is_race_allowed_for_class(characters::kClassRuncaller, characters::kRaceEmberkin));

    // (2) A hand-built Roster: a class GAINS a limit set the first time a limit is
    //     added; a class never given one permits all races. id 0 is rejected.
    Roster r;
    r.add_race(1, "R1");
    r.add_race(2, "R2");
    r.add_class(10, "Gated");
    r.add_class(11, "Open");
    r.add_class_race_limit(10, 1);  // Gated permits only race 1
    check("built gate: gated class permits its listed race", r.is_race_allowed_for_class(10, 1));
    check("built gate: gated class refuses an unlisted race", !r.is_race_allowed_for_class(10, 2));
    check("built gate: class with no limit permits any race", r.is_race_allowed_for_class(11, 2));
    r.add_class_race_limit(0, 1);   // id 0 rejected — no phantom gate created
    r.add_class_race_limit(10, 0);  // race 0 rejected — not added to the set
    check("built gate: add_class_race_limit rejects class id 0 (still all-permit)",
          r.is_race_allowed_for_class(0, 5));
    check("built gate: add_class_race_limit rejects race id 0 (not added)",
          !r.is_race_allowed_for_class(10, 0));
}

int main() {
    run_from_json_unit_checks();
    run_roster_unit_checks();

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured (set "
                    "MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        // The DB scenarios skip, but the pure from_json checks above already ran —
        // surface their result rather than an unconditional pass.
        return g_fail == 0 ? 0 : 1;
    }

    // Randomised, distinct account_ids + name prefix so repeated/parallel local
    // runs never collide. account_big is > INT64_MAX to exercise the unsigned
    // decimal-string binding path (scenario 7).
    const int salt = std::rand();
    const std::uint64_t account_a = 4'000'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_b = 4'100'000'000ULL + static_cast<unsigned>(salt);
    // account_c / account_d exercise the per-account character cap (#329, 8):
    // account_c fills to the cap then is refused one past it; account_d (a
    // different account) can still create its first.
    const std::uint64_t account_c = 4'200'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_d = 4'300'000'000ULL + static_cast<unsigned>(salt);
    // account_e / account_f exercise the appearance bounds rule (contract ① §9):
    // account_e creates WITHOUT appearance (server default {1,1,1,1}); account_f
    // creates with an out-of-bounds record (version!=1, preset id 0) that clamps.
    const std::uint64_t account_e = 4'400'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_f = 4'500'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_big =
        18'000'000'000'000'000'000ULL + static_cast<unsigned>(salt % 100000);
    const std::string name_a = "Chr_" + std::to_string(salt) + "_a";
    const std::string name_dup = name_a;  // same name -> duplicate
    const std::string name_b = "Chr_" + std::to_string(salt) + "_b";
    const std::string name_big = "Chr_" + std::to_string(salt) + "_big";
    const std::string name_c1 = "Chr_" + std::to_string(salt) + "_c1";
    const std::string name_c2 = "Chr_" + std::to_string(salt) + "_c2";
    const std::string name_d = "Chr_" + std::to_string(salt) + "_d";
    const std::string name_e = "Chr_" + std::to_string(salt) + "_e";
    const std::string name_f = "Chr_" + std::to_string(salt) + "_f";

    // Concurrency scenario (9) reserves a contiguous id block [conc_base,
    // conc_base + kConcSpan) so two-thread rounds always target FRESH, empty
    // accounts (the gap-lock-vulnerable state). The control phase uses the first
    // kControlSpan ids; the fixed-path phase uses ids from kControlSpan onward.
    constexpr std::uint64_t kControlSpan = 100;
    constexpr std::uint64_t kConcSpan = 300;
    const std::uint64_t conc_base =
        5'000'000'000ULL + static_cast<std::uint64_t>(static_cast<unsigned>(salt)) * kConcSpan;
    const std::string conc_name = "Cc_" + std::to_string(salt) + "_";

    auto cleanup = [&](db::Connection& db) {
        for (std::uint64_t acct :
             {account_a, account_b, account_c, account_d, account_e, account_f,
              account_big}) {
            db.execute("DELETE FROM `character` WHERE account_id = ?",
                       {db::Param{std::to_string(acct)}});
        }
        // Range-delete the whole concurrency id block (scenario 9).
        db.execute("DELETE FROM `character` WHERE account_id >= ? AND account_id < ?",
                   {db::Param{std::to_string(conc_base)},
                    db::Param{std::to_string(conc_base + kConcSpan)}});
    };

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        cleanup(db);  // clear any stray rows from a prior aborted run

        // ---- 1. create -> list shows it ------------------------------------
        characters::CreateRequest req;
        req.account_id = account_a;
        req.name = name_a;
        req.race = static_cast<std::uint8_t>(characters::kRaceArdent);
        req.char_class = static_cast<std::uint8_t>(characters::kClassVanguard);
        // Explicit, in-bounds appearance record (contract ① §5.2): a non-default
        // preset per channel so "list returns what create stored" is meaningful.
        req.appearance = characters::AppearanceRecord{/*version=*/1, /*hair=*/2,
                                                      /*face=*/3, /*skin=*/4};
        characters::CreateResult created = characters::create_character(db, req);
        check("create returns a server-minted id", created.character_id > 0);

        std::vector<characters::CharacterSummary> listed =
            characters::list_characters(db, account_a);
        check("list shows exactly one character for the account", listed.size() == 1);
        if (listed.size() == 1) {
            const auto& c = listed[0];
            check("listed id matches created id", c.id == created.character_id);
            check("listed account_id matches", c.account_id == account_a);
            check("listed name matches", c.name == name_a);
            check("listed race matches",
                  c.race == static_cast<std::uint8_t>(characters::kRaceArdent));
            check("listed class matches",
                  c.char_class == static_cast<std::uint8_t>(characters::kClassVanguard));
            check("new character starts at level 1", c.level == 1);
            // Explicit appearance round-trips exactly (stored as JSON, parsed back).
            check("listed appearance version round-trips", c.appearance.version == 1);
            check("listed appearance hair round-trips", c.appearance.hair == 2);
            check("listed appearance face round-trips", c.appearance.face == 3);
            check("listed appearance skin round-trips", c.appearance.skin == 4);
        }

        // ---- 2. duplicate name is refused ----------------------------------
        {
            characters::CreateRequest dup;
            dup.account_id = account_b;  // different account, same name
            dup.name = name_dup;
            dup.race = static_cast<std::uint8_t>(characters::kRaceArdent);
            dup.char_class = static_cast<std::uint8_t>(characters::kClassRuncaller);
            bool threw = false;
            try {
                characters::create_character(db, dup);
            } catch (const characters::DuplicateName&) {
                threw = true;
            }
            check("duplicate name refused (DuplicateName)", threw);
        }

        // ---- 3. invalid race is refused ------------------------------------
        {
            characters::CreateRequest bad;
            bad.account_id = account_b;
            bad.name = name_b;
            bad.race = 0;  // 0 is reserved-invalid in the M0-frozen roster
            bad.char_class = static_cast<std::uint8_t>(characters::kClassVanguard);
            bool threw = false;
            try {
                characters::create_character(db, bad);
            } catch (const characters::InvalidRace&) {
                threw = true;
            }
            check("invalid race refused (InvalidRace)", threw);
        }

        // ---- 4. invalid class is refused -----------------------------------
        {
            characters::CreateRequest bad;
            bad.account_id = account_b;
            bad.name = name_b;
            bad.race = static_cast<std::uint8_t>(characters::kRaceArdent);
            bad.char_class = static_cast<std::uint8_t>(characters::kClassCount + 1);  // out of range
            bool threw = false;
            try {
                characters::create_character(db, bad);
            } catch (const characters::InvalidClass&) {
                threw = true;
            }
            check("invalid class refused (InvalidClass)", threw);
        }

        // ---- 4b. race ∈ class race_limits gate (#696) ----------------------
        // The default offline_full() roster mirrors the seed pack: Vanguard(1) is
        // gated to Ardent(1)+Dolmen(2); Warden(3) omits race_limits (all races).
        // (a) a VALID race that is NOT permitted for the chosen class is refused
        //     with the DISTINCT InvalidRaceForClass (not InvalidRace) — Sylvane(3)
        //     is a valid roster race but ∉ Vanguard's limits.
        {
            characters::CreateRequest gated;
            gated.account_id = account_b;
            gated.name = "GateVanguardBad_" + std::to_string(salt);
            gated.race = static_cast<std::uint8_t>(characters::kRaceSylvane);   // valid race
            gated.char_class = static_cast<std::uint8_t>(characters::kClassVanguard);  // gated
            bool gate_refused = false;
            bool wrong_exc = false;
            try {
                characters::create_character(db, gated);
            } catch (const characters::InvalidRaceForClass&) {
                gate_refused = true;
            } catch (const characters::InvalidRace&) {
                wrong_exc = true;  // must NOT collapse to unknown-race
            } catch (const characters::InvalidClass&) {
                wrong_exc = true;
            }
            check("race∉class race_limits refused (InvalidRaceForClass)", gate_refused);
            check("race_limits refusal is DISTINCT from InvalidRace/InvalidClass",
                  !wrong_exc);

            // (b) a class WITHOUT race_limits (Warden) accepts ANY valid race,
            //     including the Sylvane that Vanguard rejects — the "no gate = all
            //     races" rule, proven through the real create path + INSERT.
            characters::CreateRequest open;
            open.account_id = account_b;
            open.name = "GateWardenOk_" + std::to_string(salt);
            open.race = static_cast<std::uint8_t>(characters::kRaceSylvane);
            open.char_class = static_cast<std::uint8_t>(characters::kClassWarden);
            characters::CreateResult r = characters::create_character(db, open);
            check("any race accepted for a class without race_limits (Sylvane/Warden)",
                  r.character_id > 0);
        }

        // ---- 5. deleting another account's character is refused ------------
        // account_a owns `created`. account_b tries to delete it -> no-op.
        {
            bool deleted = characters::delete_character(db, account_b, created.character_id);
            check("delete of another account's character refused (returns false)", !deleted);
            std::vector<characters::CharacterSummary> still =
                characters::list_characters(db, account_a);
            check("victim character still present after refused delete",
                  has_id(still, created.character_id));
        }

        // ---- 6. deleting your own character removes it ---------------------
        {
            bool deleted = characters::delete_character(db, account_a, created.character_id);
            check("delete of own character succeeds (returns true)", deleted);
            std::vector<characters::CharacterSummary> after =
                characters::list_characters(db, account_a);
            check("character gone from list after own delete", after.empty());
        }

        // ---- 7. > INT64_MAX account_id round-trips (unsigned binding) ------
        {
            characters::CreateRequest big;
            big.account_id = account_big;  // > 9.22e18, i.e. > INT64_MAX
            big.name = name_big;
            big.race = static_cast<std::uint8_t>(characters::kRaceSylvane);
            big.char_class = static_cast<std::uint8_t>(characters::kClassMender);
            characters::CreateResult r = characters::create_character(db, big);
            check("create with > INT64_MAX account_id succeeds", r.character_id > 0);
            std::vector<characters::CharacterSummary> listed_big =
                characters::list_characters(db, account_big);
            check("list by > INT64_MAX account_id finds the character",
                  listed_big.size() == 1);
            check("> INT64_MAX account_id round-trips exactly",
                  listed_big.size() == 1 && listed_big[0].account_id == account_big);
        }

        // ---- 8. per-account cap (#329, raised to 8 in #629) ----------------
        // (a) an account may create up to kMaxCharactersPerAccount characters;
        // (b) the create ONE PAST the cap for the SAME account is refused with
        // the cap reason (CharacterLimitReached), and the account still owns
        // exactly kMaxCharactersPerAccount; (c) a DIFFERENT account can still
        // create its 1st character. Driven off the constant so the test tracks
        // any future retune of the cap.
        {
            bool all_filled = true;
            for (std::uint64_t i = 0; i < characters::kMaxCharactersPerAccount; ++i) {
                characters::CreateRequest fill;
                fill.account_id = account_c;
                fill.name = name_c1 + "_" + std::to_string(i);  // unique per create
                fill.race = static_cast<std::uint8_t>(characters::kRaceArdent);
                fill.char_class =
                    static_cast<std::uint8_t>(characters::kClassVanguard);
                characters::CreateResult rf = characters::create_character(db, fill);
                all_filled = all_filled && rf.character_id > 0;
            }
            check("cap: account may create up to kMaxCharactersPerAccount characters",
                  all_filled);

            characters::CreateRequest over;
            over.account_id = account_c;     // same account -> one past the cap
            over.name = name_c2;             // unique name: the refusal is the cap
            over.race = static_cast<std::uint8_t>(characters::kRaceSylvane);
            over.char_class = static_cast<std::uint8_t>(characters::kClassMender);
            bool capped = false;
            try {
                characters::create_character(db, over);
            } catch (const characters::CharacterLimitReached&) {
                capped = true;
            }
            check("cap: create past the cap refused (CharacterLimitReached)", capped);

            std::vector<characters::CharacterSummary> owned =
                characters::list_characters(db, account_c);
            check("cap: account still owns exactly kMaxCharactersPerAccount",
                  owned.size() == characters::kMaxCharactersPerAccount);

            characters::CreateRequest other;
            other.account_id = account_d;    // different account -> allowed
            other.name = name_d;
            other.race = static_cast<std::uint8_t>(characters::kRaceArdent);
            other.char_class = static_cast<std::uint8_t>(characters::kClassRuncaller);
            characters::CreateResult rd = characters::create_character(db, other);
            check("cap: a different account can still create its 1st character",
                  rd.character_id > 0);
        }

        // ---- 9. concurrency: distinct-account creates must not deadlock -----
        // The self-create path (worldd CHAR_CREATE_REQUEST) lets two DIFFERENT
        // accounts create at once. Their check-then-insert transactions gap-lock
        // the same slice of idx_character_account (both accounts still empty), so
        // their INSERTs deadlock (ER_LOCK_DEADLOCK, 1213). Two facets:
        //   (a) CONTROL — the raw non-retrying pattern deterministically
        //       reproduces the 1213 deadlock (proving the bug is real);
        //   (b) FIX — create_character, run under the same two-thread load,
        //       ALWAYS succeeds for both accounts because it retries the loser.
        {
            // (a) control — deterministic deadlock reproduction. Adjacent ids
            // (a1, a1+1) are DISTINCT accounts guaranteed to share one index gap.
            constexpr int kControlRounds = 16;
            int control_deadlocks = 0;
            for (int r = 0; r < kControlRounds; ++r) {
                const std::uint64_t a1 = conc_base + static_cast<std::uint64_t>(2 * r);
                const std::uint64_t a2 = a1 + 1;
                const std::string n1 = conc_name + std::to_string(r) + "x";
                const std::string n2 = conc_name + std::to_string(r) + "y";
                TwoGate gate;
                ConcOutcome o1, o2;
                std::thread t1([&] { o1 = raw_create_no_retry(p, a1, n1, gate); });
                std::thread t2([&] { o2 = raw_create_no_retry(p, a2, n2, gate); });
                t1.join();
                t2.join();
                if (o1.db_code == kErLockDeadlock || o2.db_code == kErLockDeadlock)
                    ++control_deadlocks;
            }
            check("concurrency control: raw check-then-insert reproduces the "
                  "distinct-account deadlock (ER_LOCK_DEADLOCK)",
                  control_deadlocks > 0);

            // (b) fix — create_character retries the transient loser, so both
            // distinct-account creates succeed under the same concurrency.
            constexpr int kFixRounds = 40;
            int fix_failures = 0;
            int fix_deadlocks_escaped = 0;
            for (int r = 0; r < kFixRounds; ++r) {
                const std::uint64_t a1 =
                    conc_base + kControlSpan + static_cast<std::uint64_t>(2 * r);
                const std::uint64_t a2 = a1 + 1;
                characters::CreateRequest req1;
                req1.account_id = a1;
                req1.name = conc_name + "f" + std::to_string(r) + "x";
                req1.race = static_cast<std::uint8_t>(characters::kRaceArdent);
                req1.char_class = static_cast<std::uint8_t>(characters::kClassVanguard);
                characters::CreateRequest req2 = req1;
                req2.account_id = a2;
                req2.name = conc_name + "f" + std::to_string(r) + "y";
                TwoGate gate;
                ConcOutcome o1, o2;
                std::thread t1([&] { o1 = conc_create(p, req1, gate); });
                std::thread t2([&] { o2 = conc_create(p, req2, gate); });
                t1.join();
                t2.join();
                for (const ConcOutcome* o : {&o1, &o2}) {
                    if (!o->ok) {
                        ++fix_failures;
                        std::printf("      round %d: create failed (code %u: %s)\n",
                                    r, o->db_code, o->err.c_str());
                    }
                    if (o->db_code == kErLockDeadlock) ++fix_deadlocks_escaped;
                }
            }
            check("concurrency fix: every distinct-account create_character "
                  "succeeded under two-thread load",
                  fix_failures == 0);
            check("concurrency fix: no ER_LOCK_DEADLOCK escaped the retry",
                  fix_deadlocks_escaped == 0);
        }

        // ---- 10. appearance ABSENT -> server default {1,1,1,1} --------------
        // A create that supplies no appearance (the CHR-01 stub / old client)
        // stores the NULL column; list must materialise the versioned default
        // (contract ① §5.2: absent ⇒ {v:1,hair:1,face:1,skin:1}).
        {
            characters::CreateRequest noapp;
            noapp.account_id = account_e;
            noapp.name = name_e;
            noapp.race = static_cast<std::uint8_t>(characters::kRaceDolmen);
            noapp.char_class = static_cast<std::uint8_t>(characters::kClassWarden);
            // noapp.appearance intentionally left unset (std::nullopt).
            characters::create_character(db, noapp);
            std::vector<characters::CharacterSummary> got =
                characters::list_characters(db, account_e);
            check("absent-appearance create succeeds + lists one", got.size() == 1);
            if (got.size() == 1) {
                const auto& a = got[0].appearance;
                check("absent appearance defaults version=1", a.version == 1);
                check("absent appearance defaults hair=1", a.hair == 1);
                check("absent appearance defaults face=1", a.face == 1);
                check("absent appearance defaults skin=1", a.skin == 1);
            }
        }

        // ---- 11. appearance out-of-bounds -> clamped -----------------------
        // The record is opaque-but-bounded, never gameplay-authoritative
        // (contract ① §9): version!=1 clamps to 1 (only v1 exists at M1) and a
        // preset id of 0 clamps to 1 (ids are 1-based). No new failure taxonomy —
        // the create still SUCCEEDS, the stored record is just normalised.
        {
            characters::CreateRequest oob;
            oob.account_id = account_f;
            oob.name = name_f;
            oob.race = static_cast<std::uint8_t>(characters::kRaceEmberkin);
            oob.char_class = static_cast<std::uint8_t>(characters::kClassMender);
            oob.appearance = characters::AppearanceRecord{/*version=*/7, /*hair=*/0,
                                                          /*face=*/5, /*skin=*/0};
            characters::create_character(db, oob);
            std::vector<characters::CharacterSummary> got =
                characters::list_characters(db, account_f);
            check("out-of-bounds-appearance create still succeeds", got.size() == 1);
            if (got.size() == 1) {
                const auto& a = got[0].appearance;
                check("version!=1 clamped to 1", a.version == 1);
                check("preset id 0 (hair) clamped to 1", a.hair == 1);
                check("in-bounds preset id (face=5) preserved", a.face == 5);
                check("preset id 0 (skin) clamped to 1", a.skin == 1);
            }
        }

        cleanup(db);  // remove this run's rows
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL CHARACTER TESTS PASSED\n"
                            : "\n%d CHARACTER TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

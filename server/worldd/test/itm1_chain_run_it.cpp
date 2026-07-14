// SPDX-License-Identifier: Apache-2.0
//
// worldd — IT-M1 10-QUEST CHAIN server-side HARNESS (issue #399, epic #20). THE
// exit criterion for epic #20: "the 10-quest IT-M1 chain runs fully server-side
// with zero client-trusted outcomes." This is the capstone that drives the
// hand-authored 'Emberfall Hollow' chain (content/core, compiled by mcc emit-sql
// and loaded through the #390 DB stores) to completion over the LIVE session
// dispatch path, asserting every outcome is SERVER-COMPUTED.
//
// WHAT IT DOES (a fusion of the three merged templates it is modeled on):
//   * boot like worldd-itm1-chain-load (#394): `mcc emit-sql content/core` -> a
//     throwaway MariaDB it owns, loaded with the real world DDL + the emitted DML,
//     then load_world_content() (#390 DB stores) + install_content_stores();
//   * stand up a real worldd session like worldd-quest-loot-npc-db (#388): the same
//     throwaway DB also carries the auth + characters schemas (their up-migrations),
//     so it serves as auth_db AND char_db; a real WORLD_HELLO(grant) -> HANDSHAKE_OK
//     -> ENTER_WORLD(owned character) spawns the session in-world;
//   * complete objectives on the SERVER-AUTHORITATIVE paths like worldd-quest-live-
//     credit (#396): kill via a real MapTick -> route_tick_events -> the quest-credit
//     bus -> poll_quest_credits; collect via LOOT_TAKE -> sync_collect; deliver via
//     GOSSIP_HELLO at the to-NPC -> on_deliver; explore via a synthetic TriggerVolume
//     carrying the content POI key crossed by a MOVEMENT_INTENT.
//
// The harness issues ONLY player intents (accept / kill-in-a-tick / loot / gossip /
// move / turn-in) and reads back the server's own QUEST_PROGRESS / QUEST_LOG / turn-
// in results. It NEVER pokes ctx.quests completion by fiat and never asserts a quest
// done that the server did not report — a successful QUEST_TURN_IN is itself proof
// the server computed every objective complete (turn_in returns kIncomplete
// otherwise). It asserts:
//   1. ZERO client-trusted outcomes — only intents in, server results out;
//   2. PREREQ GATING is server-side — accepting the 3-prereq convergence quest
//      (Heart of the Cindermaw) before its prerequisites are complete is REJECTED
//      (MISSING_PREREQUISITE), and accepted once they are;
//   3. the FULL chain completes in a valid topological order, the convergence quest
//      completes LAST, and reward money lands durably in character.money;
//   4. every objective type (kill / collect / deliver / explore) is exercised by a
//      real quest step.
//
// DEFERRED (documented, NOT faked): the DB area-trigger `poi` load is #398 — until
// it lands the explore volume is SYNTHESIZED here from the CONTENT objective's
// (zone_id, poi), exactly as worldd-quest-live-credit does, so the join is still
// against real content ids. Corpses are seeded into the loot registry directly (the
// M0 world tick does not yet spawn content creatures on whose death #369 would seed
// loot) — the same shortcut the sibling loot integration tests take; the COLLECT
// outcome (LOOT_TAKE -> inventory -> sync_collect -> complete) is still fully server-
// computed.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*), needs a
// mariadb/mysql client + an mcc binary; SKIPS (exit 0) when any is absent — inert in
// the plain server ctest, real only under scripts/dev/test.sh --db / the DB CI job.
// Creates + drops the throwaway database it owns, touching nothing else.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world/auth/character
// DDL + the #388/#390/#396 seams + the world.fbs wire contract; no GPL/AGPL/CMaNGOS/
// TrinityCore/leaked source consulted).

#include "world_dispatch.h"
#include "world_session.h"

#include "ability_store.h"
#include "area_triggers.h"
#include "characters.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "db_content_store.h"
#include "item_template.h"
#include "loot_roll.h"
#include "loot_session.h"
#include "map_tick.h"
#include "movement_validation.h"
#include "quest_def.h"

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

#include "world_generated.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace db = meridian::db;
namespace chr = meridian::characters;
namespace lo = meridian::loot;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void checks(const std::string& what, bool ok) { check(what.c_str(), ok); }

using Bytes = std::vector<std::uint8_t>;

const char* env(const char* k) { return std::getenv(k); }
const char* pick(const char* world_key, const char* fallback_key) {
    if (const char* v = env(world_key)) return v;
    return env(fallback_key);
}

// The 10 authored chain quests, by display NAME (stable identity — the numeric
// IF-9 ids shift with unrelated content; see worldd-itm1-chain-load #394).
const std::vector<std::string> kChainQuestNames = {
    "Culling the Kobolds", "Ears for Evidence",       "Sparks in the Dark",
    "Warden's Watch",      "Emberblight",             "Trail of the Digmasters",
    "The Deep Tally",      "Silence the Firecallers",  "Charter of the Hollow",
    "Heart of the Cindermaw",
};
// The 3-prerequisite convergence quest — must complete LAST (epic-#20 shape).
const std::string kConvergenceQuest = "Heart of the Cindermaw";

// ---- MariaDB client + mcc discovery (parity with worldd-itm1-chain-load) ------
std::string find_client() {
    for (const char* name : {"mariadb", "mysql"}) {
        std::string cmd = std::string(name) + " --version >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return name;
    }
    return "";
}
std::string find_mcc() {
    std::vector<std::string> candidates;
    if (const char* e = env("MERIDIAN_MCC_BIN")) candidates.emplace_back(e);
#ifdef ITM1_MCC_BIN
    candidates.emplace_back(ITM1_MCC_BIN);
#endif
    for (const std::string& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    if (std::system("mcc --help >/dev/null 2>&1") == 0 ||
        std::system("command -v mcc >/dev/null 2>&1") == 0) {
        return "mcc";
    }
    return "";
}
std::string conn_flags() {
    std::string f;
    auto add = [&](const std::string& s) { f += " " + s; };
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET"))
        add("--socket=" + std::string(s));
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST"))
        add("--host=" + std::string(h));
    if (const char* p = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        add("--port=" + std::string(p));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER"))
        add("--user=" + std::string(u));
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS"))
        add("--password=" + std::string(pw));
    return f;
}
int run_sql_file(const std::string& client, const std::string& flags,
                 const std::string& dbname, const fs::path& file) {
    std::string cmd = client + flags;
    if (!dbname.empty()) cmd += " " + dbname;
    cmd += " < " + file.string();
    return std::system(cmd.c_str());
}
db::ConnectParams conn_params(const std::string& dbname) {
    db::ConnectParams p;
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) p.unix_socket = s;
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST")) p.host = h;
    if (const char* port = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS")) p.password = pw;
    p.database = dbname;
    return p;
}

// Concatenate every *.sql in `dir` (ascending) into `out`.
void concat_dir_sql(const fs::path& dir, const fs::path& out) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".sql") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::ofstream o(out);
    for (const auto& f : files) {
        std::ifstream in(f);
        o << in.rdbuf() << "\n";
    }
}
// Concatenate only the *.up.sql migrations in `dir` (ascending) into `out`.
void concat_up_migrations(const fs::path& dir, const fs::path& out) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 7 && n.substr(n.size() - 7) == ".up.sql") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::ofstream o(out);
    for (const auto& f : files) {
        std::ifstream in(f);
        o << in.rdbuf() << "\n";
    }
}

// ---- Throwaway self-signed cert (OpenSSL public API; mirrors the wire tests) --
bool generate_self_signed(const std::string& cert_path, const std::string& key_path) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    FILE* fk = nullptr;
    FILE* fc = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) goto done;
    if (EVP_PKEY_keygen_init(pctx) != 1) goto done;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) != 1) goto done;
    if (EVP_PKEY_keygen(pctx, &pkey) != 1) goto done;
    x509 = X509_new();
    if (!x509) goto done;
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L);
    if (X509_set_pubkey(x509, pkey) != 1) goto done;
    {
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("meridian-itm1-run"), -1, -1, 0);
        if (X509_set_issuer_name(x509, name) != 1) goto done;
    }
    if (X509_sign(x509, pkey, EVP_sha256()) == 0) goto done;
    fk = std::fopen(key_path.c_str(), "wb");
    if (!fk) goto done;
    if (PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) goto done;
    fc = std::fopen(cert_path.c_str(), "wb");
    if (!fc) goto done;
    if (PEM_write_X509(fc, x509) != 1) goto done;
    ok = true;
done:
    if (fk) std::fclose(fk);
    if (fc) std::fclose(fc);
    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    if (pctx) EVP_PKEY_CTX_free(pctx);
    if (!ok) ERR_print_errors_fp(stderr);
    return ok;
}

// ---- Minimal TLS 1.3 IF-2 client --------------------------------------------
class Client {
public:
    explicit Client(std::uint16_t port) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        // A receive timeout so a MISSING expected frame surfaces as a visible test
        // FAIL (recv_frame -> nullopt) rather than hanging the whole harness — the
        // difference between a diagnosable failure and a 400 s ctest timeout.
        timeval tv{};
        tv.tv_sec = 8;
        tv.tv_usec = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        connected_ = (SSL_connect(ssl_) == 1);
        if (!connected_) ERR_print_errors_fp(stderr);
    }
    ~Client() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (fd_ >= 0) ::close(fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return connected_; }

    bool send_frame(const Bytes& payload) {
        std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        Bytes f{static_cast<std::uint8_t>(len & 0xFF),
                static_cast<std::uint8_t>((len >> 8) & 0xFF),
                static_cast<std::uint8_t>((len >> 16) & 0xFF),
                static_cast<std::uint8_t>((len >> 24) & 0xFF)};
        f.insert(f.end(), payload.begin(), payload.end());
        return write_all(f.data(), f.size());
    }
    std::optional<Bytes> recv_frame() {
        std::uint8_t lenbuf[4];
        if (!read_all(lenbuf, 4)) return std::nullopt;
        std::uint32_t len = static_cast<std::uint32_t>(lenbuf[0]) |
                            (static_cast<std::uint32_t>(lenbuf[1]) << 8) |
                            (static_cast<std::uint32_t>(lenbuf[2]) << 16) |
                            (static_cast<std::uint32_t>(lenbuf[3]) << 24);
        Bytes payload(len);
        if (len > 0 && !read_all(payload.data(), len)) return std::nullopt;
        return payload;
    }

private:
    bool write_all(const std::uint8_t* buf, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            int w = SSL_write(ssl_, buf + sent, static_cast<int>(n - sent));
            if (w <= 0) return false;
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }
    bool read_all(std::uint8_t* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            int r = SSL_read(ssl_, buf + got, static_cast<int>(n - got));
            if (r <= 0) return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    bool connected_ = false;
};

// ---- world.fbs payload builders ---------------------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateWorldHello(b, grant_id, build, 0, 0));
    return bytes_of(b);
}
Bytes enc_enter_world_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return bytes_of(b);
}
Bytes enc_quest_accept(std::uint32_t quest_id, std::uint64_t giver) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAccept(b, quest_id, giver));
    return bytes_of(b);
}
Bytes enc_quest_turn_in(std::uint32_t quest_id, std::uint64_t turn_in, std::int32_t choice) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestTurnIn(b, quest_id, turn_in, choice));
    return bytes_of(b);
}
Bytes enc_quest_log_req() {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestLogEntry>> none;
    b.Finish(mn::CreateQuestLog(b, b.CreateVector(none)));
    return bytes_of(b);
}
Bytes enc_loot_request(std::uint64_t corpse) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRequest(b, corpse));
    return bytes_of(b);
}
Bytes enc_loot_take(std::uint64_t corpse, std::uint32_t slot, bool money) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootTake(b, corpse, slot, money));
    return bytes_of(b);
}
Bytes enc_gossip_hello(std::uint64_t npc) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGossipHello(b, npc));
    return bytes_of(b);
}
Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y, float z,
                          std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, flags, x, y, z, /*orientation=*/0.0f,
                                      client_time_ms));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

struct RxFrame {
    mn::Opcode opcode{};
    Bytes payload;
};
std::optional<RxFrame> recv_decoded(Client& c) {
    std::optional<Bytes> raw = c.recv_frame();
    if (!raw) return std::nullopt;
    std::optional<mw::Frame> rf = mw::decode_frame(*raw);
    if (!rf) return std::nullopt;
    return RxFrame{rf->opcode, Bytes(rf->payload, rf->payload + rf->payload_len)};
}
// Send `req` under `opcode`, then read (bounded) until the expected reply opcode,
// skipping unrelated server-pushed frames (a trailing LOOT_CLOSED after the corpse
// empties, a GOSSIP_MENU ahead of a QUEST_PROGRESS, etc.) so a straggler never
// desyncs the request/reply cursor.
std::optional<Bytes> round_trip(Client& c, mn::Opcode opcode, const Bytes& req,
                                mn::Opcode resp_opcode, std::uint64_t seq, int budget = 16) {
    if (!c.send_frame(mw::encode_frame(opcode, seq, req))) return std::nullopt;
    for (int i = 0; i < budget; ++i) {
        std::optional<RxFrame> rf = recv_decoded(c);
        if (!rf) return std::nullopt;
        if (rf->opcode == resp_opcode) return rf->payload;
    }
    return std::nullopt;
}
// Read frames (bounded) until one of `opcode` arrives, returning its payload.
std::optional<Bytes> read_until(Client& c, mn::Opcode opcode, int budget = 12) {
    for (int i = 0; i < budget; ++i) {
        std::optional<RxFrame> rf = recv_decoded(c);
        if (!rf) return std::nullopt;
        if (rf->opcode == opcode) return rf->payload;
    }
    return std::nullopt;
}

// The QUEST_PROGRESS of interest for one objective step.
struct Progress {
    std::uint32_t quest_id = 0;
    std::uint8_t index = 0;
    std::uint16_t have = 0;
    bool complete = false;
    bool got = false;
};
// Read frames until a QUEST_PROGRESS for (quest_id, objective_index) arrives.
Progress read_progress_for(Client& c, std::uint32_t quest_id, std::uint8_t index, int budget = 16) {
    for (int i = 0; i < budget; ++i) {
        std::optional<RxFrame> rf = recv_decoded(c);
        if (!rf) break;
        if (rf->opcode != mn::Opcode::QUEST_PROGRESS) continue;  // skip reply frames
        const auto* qp = decode<mn::QuestProgress>(rf->payload);
        if (qp == nullptr) break;
        if (qp->quest_id() == quest_id && qp->objective_index() == index)
            return Progress{qp->quest_id(), qp->objective_index(), qp->have(),
                            qp->complete(), true};
        // A different objective advanced first — keep reading for ours.
    }
    return Progress{};
}

std::uint64_t cell_u64(const db::Cell& c) {
    return c.has_value() ? std::strtoull(c->c_str(), nullptr, 10) : 0;
}
std::int64_t cell_i64(const db::Cell& c) {
    return c.has_value() ? std::strtoll(c->c_str(), nullptr, 10) : 0;
}
db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }
db::Param blob32(const Bytes& b) { return db::Param{db::Bytes_t(b.begin(), b.end())}; }

std::int64_t money_of(db::Connection& conn, std::uint64_t char_id) {
    db::Result r = conn.execute("SELECT money FROM `character` WHERE id = ?", {sid(char_id)});
    return r.rows.empty() ? -1 : cell_i64(r.rows.at(0)[0]);
}
std::uint64_t backpack_used(db::Connection& conn, std::uint64_t char_id) {
    db::Result r = conn.execute("SELECT COUNT(*) FROM character_inventory WHERE char_id = ?",
                                {sid(char_id)});
    return r.rows.empty() ? 0 : cell_u64(r.rows.at(0)[0]);
}
void seed_grant(db::Connection& conn, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key, std::uint32_t build) {
    conn.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, DATE_ADD(UTC_TIMESTAMP(), INTERVAL 120 SECOND))",
        {sid(grant_id), sid(account_id), db::Param{static_cast<std::int64_t>(realm_id)},
         blob32(session_key), db::Param{static_cast<std::int64_t>(build)}});
}

// Seed a corpse loot session dropping `count` of `item_id`, owned by `owner`, with a
// range large enough that the looter is always in range (the harness moves the
// character for explore steps; a wide range decouples collect from position).
void seed_corpse(mw::WorldServer& world, std::uint64_t corpse, std::uint64_t owner,
                 std::uint32_t item_id, std::uint32_t count) {
    lo::LootRoll roll;
    roll.stacks.push_back(lo::LootStack{item_id, count, /*required_quest_id=*/0});
    roll.copper = 0;
    const lo::LootPoint pos{-320.0f, -320.0f, 0.0f};  // == ENTER_WORLD spawn (kZoneSpawnXY, #562)
    world.loot_registry().insert(
        lo::LootSession(corpse, pos, std::move(roll), {owner}, /*loot_range=*/1.0e6f));
}

// Drive a real MapTick to kill ONE creature of `template_id` by `killer`, returning
// the tick deltas (carrying the typed kCreatureKill the world loop routes; #396).
std::vector<mw::TickEvent> map_tick_one_kill(mw::ObjectGuid killer, std::uint32_t template_id) {
    const mw::AbilityStore abilities = mw::load_placeholder_ability_store();
    mw::MapTick mt(abilities, /*rng_seed=*/0x0399ULL, /*dt_ms=*/1600);
    mt.set_report_kills(true);

    mw::UnitStats st;
    st.level = 60;
    st.max_health = 100000;
    st.resource_type = mw::ResourceType::kMana;
    st.max_resource = 1000;
    st.faction = mw::Faction::kPlayer;
    mw::Position home;
    home.x = 0.0f; home.y = 0.0f; home.z = 0.0f;
    mt.add_player(killer, home, st);

    mw::CreatureSpawnDef d;
    d.template_id = template_id;
    d.level = 1;
    d.faction = mw::Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = 0;
    d.leash_radius = 1000;
    d.respawn_ms = 999999;
    d.move_speed = 0;
    d.patrol_mode = mw::PatrolMode::kStationary;
    const mw::ObjectGuid crt = mt.add_creature(d);
    mw::Unit* cu = mt.unit_for_guid(crt);
    cu->set_max_health(8);  // one melee strike is lethal

    std::vector<mw::TickEvent> all;
    for (int t = 0; t < 12 && cu->is_alive(); ++t) {
        mt.enqueue_cast(mw::AbilityUseCmd{killer, mw::kPlaceholderMeleeStrikeId, crt});
        std::vector<mw::TickEvent> deltas = mt.advance();
        for (mw::TickEvent& ev : deltas) all.push_back(std::move(ev));
    }
    return all;
}

// ---- A loaded chain quest + the topological drive order ----------------------
struct ChainQuest {
    const mw::QuestDef* def = nullptr;
    std::string name;
};

// Kahn's algorithm over the chain quests (edge prereq -> quest), restricted to the
// chain members. Deterministic: ready nodes drained in ascending quest id. Returns
// an empty vector if the restricted graph is not a DAG (would leave nodes unqueued).
std::vector<ChainQuest> topo_order(const std::vector<ChainQuest>& chain) {
    std::unordered_map<mw::QuestId, const ChainQuest*> by_id;
    for (const ChainQuest& q : chain) by_id[q.def->id] = &q;

    std::unordered_map<mw::QuestId, int> indeg;
    for (const ChainQuest& q : chain) {
        indeg.try_emplace(q.def->id, 0);
        for (mw::QuestId pre : q.def->prerequisites)
            if (by_id.count(pre)) indeg[q.def->id]++;
    }
    std::vector<ChainQuest> out;
    // Repeatedly take the ready node (indeg 0) with the smallest id.
    while (out.size() < chain.size()) {
        mw::QuestId pick = 0;
        bool found = false;
        for (const ChainQuest& q : chain) {
            if (indeg[q.def->id] != 0) continue;
            if (!found || q.def->id < pick) { pick = q.def->id; found = true; }
        }
        if (!found) break;  // cycle / stuck
        indeg[pick] = -1;   // consumed
        out.push_back(*by_id[pick]);
        // Decrement successors.
        for (const ChainQuest& q : chain)
            for (mw::QuestId pre : q.def->prerequisites)
                if (pre == pick && indeg[q.def->id] > 0) indeg[q.def->id]--;
    }
    return out;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd IT-M1 10-quest chain SERVER-SIDE harness (#399, epic #20 exit criterion)\n");

    // --- Guards: DB env, client, mcc. SKIP (inert) if any is missing. ----------
    if (!pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST") &&
        !pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured\n");
        return 0;
    }
    const std::string client = find_client();
    if (client.empty()) { std::printf("SKIP: no mariadb/mysql client on PATH\n"); return 0; }
    const std::string mcc = find_mcc();
    if (mcc.empty()) {
        std::printf("SKIP: no mcc binary found (set MERIDIAN_MCC_BIN, or build it)\n");
        return 0;
    }
    const std::string flags = conn_flags();

    fs::path scratch = fs::temp_directory_path() / "itm1_chain_run_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    const std::string dbname = "meridian_itm1_chain_run";

    // --- 1. mcc emit-sql content/core -> world.sql. ----------------------------
    const fs::path world_sql = scratch / "world.sql";
    {
        std::string cmd = "\"" + mcc + "\" emit-sql \"" + std::string(ITM1_CONTENT_DIR) +
                          "\" --out \"" + world_sql.string() + "\" >" +
                          (scratch / "emit.log").string() + " 2>&1";
        const int rc = std::system(cmd.c_str());
        check("mcc emit-sql produced world.sql", rc == 0 && fs::exists(world_sql));
        if (rc != 0 || !fs::exists(world_sql)) {
            std::printf("FAIL: emit-sql failed (rc=%d); see %s\n", rc,
                        (scratch / "emit.log").string().c_str());
            return 1;
        }
    }

    // --- 2. Assemble the three schemas: world DDL + auth + characters. ---------
    const fs::path world_ddl = scratch / "world_ddl.sql";
    const fs::path auth_ddl = scratch / "auth.sql";
    const fs::path char_ddl = scratch / "characters.sql";
    concat_dir_sql(ITM1_WORLD_DDL_DIR, world_ddl);
    concat_up_migrations(ITM1_AUTH_MIGRATIONS_DIR, auth_ddl);
    concat_up_migrations(ITM1_CHAR_MIGRATIONS_DIR, char_ddl);
    check("assembled world DDL + auth + characters schemas",
          fs::exists(world_ddl) && fs::exists(auth_ddl) && fs::exists(char_ddl));

    // --- 3. Create the throwaway DB + load DDL, content DML, auth, characters. --
    {
        const fs::path create = scratch / "create.sql";
        std::ofstream(create) << "DROP DATABASE IF EXISTS " << dbname << ";\n"
                              << "CREATE DATABASE " << dbname
                              << " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n";
        const int rc = run_sql_file(client, flags, "", create);
        check("created a fresh throwaway database", rc == 0);
        if (rc != 0) { std::printf("FAIL: could not create database\n"); return 1; }
    }
    check("world DDL loads", run_sql_file(client, flags, dbname, world_ddl) == 0);
    check("mcc-emitted content DML loads", run_sql_file(client, flags, dbname, world_sql) == 0);
    check("auth schema loads", run_sql_file(client, flags, dbname, auth_ddl) == 0);
    check("characters schema loads", run_sql_file(client, flags, dbname, char_ddl) == 0);

    char tmpl[] = "/tmp/meridian-itm1-run-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    std::uint64_t account_id = 0, realm_id = 0, char_id = 0, grant_ok = 0;
    std::int64_t start_money = 500;

    try {
        db::ConnectParams params = conn_params(dbname);
        db::Connection conn(params);
        std::printf("connected to the throwaway world+auth+characters DB\n");

        // --- 4. Load the #390 DB content stores + install them behind the seams. ---
        static mw::WorldContent content = mw::load_world_content(conn);
        mw::install_content_stores(content.items.get(), content.vendor.get(),
                                   content.quests.get(), content.npcs.get());

        // Index quests by name; resolve the 10 chain quests.
        std::unordered_map<std::string, const mw::QuestDef*> by_name;
        for (mw::QuestId qid : content.quests->ids())
            if (const mw::QuestDef* q = content.quests->find(qid)) by_name[q->name] = q;

        std::vector<ChainQuest> chain;
        {
            bool all = true;
            for (const std::string& nm : kChainQuestNames) {
                auto it = by_name.find(nm);
                const bool present = it != by_name.end();
                checks("chain quest loaded: " + nm, present);
                if (present) chain.push_back(ChainQuest{it->second, nm});
                else all = false;
            }
            check("all 10 chain quests present", all && chain.size() == 10);
        }
        if (chain.size() != 10) throw std::runtime_error("chain incomplete — cannot drive");

        // Topological drive order from the LOADED prerequisites (server data).
        const std::vector<ChainQuest> order = topo_order(chain);
        check("computed a valid topological order over the chain DAG", order.size() == 10);
        {
            // Every quest's chain-prerequisites appear before it in the order.
            std::unordered_map<mw::QuestId, std::size_t> pos;
            for (std::size_t i = 0; i < order.size(); ++i) pos[order[i].def->id] = i;
            bool valid = true;
            for (std::size_t i = 0; i < order.size(); ++i)
                for (mw::QuestId pre : order[i].def->prerequisites)
                    if (pos.count(pre) && pos[pre] >= i) valid = false;
            check("topological order respects every prerequisite edge", valid);
            check("the 3-prerequisite convergence quest is LAST",
                  !order.empty() && order.back().name == kConvergenceQuest &&
                      order.back().def->prerequisites.size() >= 3);
        }

        // --- 5. Seed account + realm + grant + the session's character. --------
        const std::string username = "itm1_run_" + std::to_string(salt);
        conn.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                     {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        account_id = cell_u64(conn.execute("SELECT id FROM account WHERE username = ?",
                                           {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "ITM1 Realm " + std::to_string(salt);
        conn.execute("INSERT INTO realm (name, address, port, build_min, build_max) "
                     "VALUES (?, '127.0.0.1', 7200, 0, 100000)", {db::Param{realm_name}});
        realm_id = cell_u64(conn.execute("SELECT id FROM realm WHERE name = ?",
                                         {db::Param{realm_name}}).rows.at(0)[0]);
        check("test realm seeded", realm_id > 0);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        grant_ok = rand_u64();
        seed_grant(conn, grant_ok, account_id, static_cast<std::uint32_t>(realm_id),
                   Bytes(32, 0xAB), client_build);

        chr::CreateRequest cr;
        cr.account_id = account_id;
        cr.name = "Itm1Hero" + std::to_string(salt % 100000);
        cr.race = static_cast<std::uint8_t>(chr::kRaceSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::kClassRuncaller);
        char_id = chr::create_character(conn, cr).character_id;
        check("session character created", char_id > 0);
        // Level 60 clears every chain quest's required_level gate (so only PREREQ
        // gating is exercised); a known starting purse tracks reward money.
        conn.execute("UPDATE `character` SET level = 60, money = ? WHERE id = ?",
                     {db::Param{start_money}, sid(char_id)});
        check("character starts at the expected money", money_of(conn, char_id) == start_money);

        // --- 6. Stand up the real listener + serve loop (auth + char DB). ------
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t sport = listener.local_port();
        check("listener bound to ephemeral port", sport != 0);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = params;
        wcfg.char_db = params;
        wcfg.realm_id = static_cast<std::uint32_t>(realm_id);
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, wcfg);
        world.set_loot_tables(*content.loot);

        // NOTE (explore): the DB area-trigger `poi` load is deferred (#398). Until
        // then, each explore objective is driven by SYNTHESIZING one fresh discovery
        // TriggerVolume carrying the CONTENT objective's (zone_id, poi) just ahead of
        // the character's current position and taking a single legal step into it (see
        // the kExplore case below) — the join is still against real content ids.

        world.start();

        std::thread server([&] {
            try {
                net::Session s = listener.accept();
                world.serve_connection(std::move(s));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  server thread error: %s\n", e.what());
            }
        });

        {
            Client c(sport);
            check("client connected (TLS)", c.connected());
            std::uint64_t seq = 1;

            // Handshake -> HANDSHAKE_OK.
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++,
                                          enc_world_hello(grant_ok, client_build)));
            bool hs = false;
            if (std::optional<RxFrame> rf = recv_decoded(c))
                hs = rf->opcode == mn::Opcode::HANDSHAKE_OK;
            check("handshake ok", hs);

            // ENTER_WORLD(owned character) -> OK (spawns in-world, registers on the
            // quest-credit bus with credit_guid == char_id).
            if (auto pl = round_trip(c, mn::Opcode::ENTER_WORLD_REQUEST,
                                     enc_enter_world_request(char_id),
                                     mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD -> OK", m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }

            // === Assertion 2: PREREQ GATING is server-side ======================
            // Accepting the convergence quest before its prerequisites are complete
            // is REJECTED by the server (MISSING_PREREQUISITE), not by the harness.
            {
                const ChainQuest& conv = order.back();
                if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                         enc_quest_accept(conv.def->id, conv.def->giver_npc_id),
                                         mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                    const auto* m = decode<mn::QuestAcceptResult>(*pl);
                    check("accept convergence quest before prereqs -> MISSING_PREREQUISITE",
                          m && m->status() == mn::QuestAcceptStatus::MISSING_PREREQUISITE);
                } else {
                    check("got a QuestAcceptResult (early convergence)", false);
                }
            }

            // === Assertion 3+4: drive the full chain in topological order =======
            std::int64_t expected_money = start_money;
            std::uint32_t intent_seq = 1;
            std::uint64_t corpse_seq = 0xC0FFEE00ULL;
            std::uint64_t move_time = 0;
            float player_x = -320.0f;  // authoritative spawn x (Zone-01 centre, #562); advances one step per explore
            mw::TriggerId explore_vol_id = 900;
            int kills_seen = 0, collects_seen = 0, delivers_seen = 0, explores_seen = 0;
            bool reward_items_landed = false;
            std::vector<std::string> completed;

            for (const ChainQuest& cq : order) {
                const mw::QuestDef* q = cq.def;
                const std::string& nm = cq.name;

                // --- accept -> OK (server validates level + prereqs) -------------
                bool accepted = false;
                if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                         enc_quest_accept(q->id, q->giver_npc_id),
                                         mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                    const auto* m = decode<mn::QuestAcceptResult>(*pl);
                    accepted = m && m->status() == mn::QuestAcceptStatus::OK;
                }
                checks("accept '" + nm + "' -> OK (prereqs satisfied server-side)", accepted);
                read_until(c, mn::Opcode::QUEST_LOG);  // drain the post-accept snapshot

                // --- complete every objective on its server-authoritative path ---
                for (std::size_t i = 0; i < q->objectives.size(); ++i) {
                    const mw::QuestObjective& o = q->objectives[i];
                    const std::uint8_t idx = static_cast<std::uint8_t>(i);
                    switch (o.type) {
                        case mw::ObjectiveType::kKill: {
                            ++kills_seen;
                            const std::uint16_t goal = o.required();
                            std::vector<mw::TickEvent> deltas;
                            for (std::uint16_t k = 0; k < goal; ++k) {
                                std::vector<mw::TickEvent> one =
                                    map_tick_one_kill(char_id, o.target_npc_id);
                                for (mw::TickEvent& ev : one) deltas.push_back(std::move(ev));
                            }
                            mw::route_tick_events(deltas, world.quest_credit());
                            // Any handled frame drains pending kill credits + emits progress.
                            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_LOG, seq++,
                                                          enc_quest_log_req()));
                            Progress p = read_progress_for(c, q->id, idx);
                            checks("kill objective credited server-side: '" + nm + "'",
                                   p.got && p.have == goal && p.complete);
                            break;
                        }
                        case mw::ObjectiveType::kCollect: {
                            ++collects_seen;
                            const std::uint16_t goal = o.required();
                            const std::uint64_t corpse = ++corpse_seq;
                            seed_corpse(world, corpse, char_id, o.item_id, goal);
                            // Open the corpse.
                            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST,
                                                     enc_loot_request(corpse),
                                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                                const auto* m = decode<mn::LootResponse>(*pl);
                                checks("loot window opens on the seeded corpse: '" + nm + "'",
                                       m && m->status() == mn::LootStatus::OK);
                            } else {
                                checks("got a LootResponse: '" + nm + "'", false);
                            }
                            // Take the quest item -> lands in DB inventory -> sync_collect.
                            if (auto pl = round_trip(c, mn::Opcode::LOOT_TAKE,
                                                     enc_loot_take(corpse, 0, false),
                                                     mn::Opcode::LOOT_RESULT, seq++)) {
                                const auto* m = decode<mn::LootResult>(*pl);
                                checks("LOOT_TAKE the collect item -> OK: '" + nm + "'",
                                       m && m->status() == mn::LootTakeStatus::OK);
                            } else {
                                checks("got a LootResult: '" + nm + "'", false);
                            }
                            Progress p = read_progress_for(c, q->id, idx);
                            checks("collect objective credited server-side: '" + nm + "'",
                                   p.got && p.have == goal && p.complete);
                            break;
                        }
                        case mw::ObjectiveType::kDeliver: {
                            ++delivers_seen;
                            // Talk to the deliver-target NPC (the item was granted on accept).
                            c.send_frame(mw::encode_frame(mn::Opcode::GOSSIP_HELLO, seq++,
                                                          enc_gossip_hello(o.to_npc_id)));
                            Progress p = read_progress_for(c, q->id, idx);
                            checks("deliver objective credited server-side: '" + nm + "'",
                                   p.got && p.complete);
                            break;
                        }
                        case mw::ObjectiveType::kExplore: {
                            ++explores_seen;
                            // Synthesize ONE fresh discovery volume carrying this
                            // objective's CONTENT (zone_id, poi) just ahead of the
                            // character, then take a single small legal step INTO it so
                            // the crossing fires on the MOVE (a fresh volume id means the
                            // character is 'outside' it at the current position). A tiny
                            // step keeps the move well inside the speed budget.
                            const float target = player_x + 0.25f;
                            mw::TriggerVolume v;
                            v.id = explore_vol_id++;
                            v.kind = mw::TriggerKind::kDiscovery;
                            v.area_id = o.zone_id;
                            v.poi = o.poi;
                            v.min_x = player_x + 0.10f; v.max_x = target + 0.10f;
                            v.min_y = -321.0f;          v.max_y = -319.0f;
                            world.world_state().load_area_triggers({v});
                            move_time += 1000;
                            // state_flags low 3 bits = MoveMode (Idle=0/Walk=1/Run=2);
                            // Run gives the largest speed budget (6 m/s) — flags=0 is Idle
                            // (cap 0) and would reject every move.
                            c.send_frame(mw::encode_frame(
                                mn::Opcode::MOVEMENT_INTENT, seq++,
                                enc_movement_intent(intent_seq++, /*Run=*/2u, target, -320.0f,
                                                    0.0f, move_time)));
                            Progress p = read_progress_for(c, q->id, idx);
                            checks("explore objective credited server-side: '" + nm + "'",
                                   p.got && p.complete);
                            player_x = target;  // server accepted the step -> advance cursor
                            break;
                        }
                    }
                }

                // --- turn in at the turn-in NPC -> OK + server-computed rewards ---
                const std::uint64_t used_before = backpack_used(conn, char_id);
                const std::int32_t choice = q->choice_items.empty() ? 0 : 0;
                bool turned_in = false;
                if (auto pl = round_trip(c, mn::Opcode::QUEST_TURN_IN,
                                         enc_quest_turn_in(q->id, q->turn_in_npc(), choice),
                                         mn::Opcode::QUEST_TURN_IN_RESULT, seq++)) {
                    const auto* m = decode<mn::QuestTurnInResult>(*pl);
                    turned_in = m && m->status() == mn::QuestTurnInStatus::OK &&
                                m->reward_money() == static_cast<std::int64_t>(q->reward_money) &&
                                m->reward_xp() == q->reward_xp;
                }
                checks("turn in '" + nm + "' -> OK (server computed all objectives complete)",
                       turned_in);
                read_until(c, mn::Opcode::QUEST_LOG);  // drain the post-turn-in snapshot

                expected_money += static_cast<std::int64_t>(q->reward_money);
                checks("reward money credited to character.money after '" + nm + "'",
                       money_of(conn, char_id) == expected_money);
                // Reward ITEMS land durably too: a quest granting always-granted items
                // grows the backpack over its pre-turn-in count (server minted them).
                if (turned_in && !q->reward_items.empty() &&
                    backpack_used(conn, char_id) > used_before)
                    reward_items_landed = true;
                if (turned_in) completed.push_back(nm);
                // NOTE: no backpack housekeeping — the server now CONSUMES each quest's
                // collect/deliver objective items at turn-in (quest_log.cpp), so the
                // 16-slot backpack no longer overflows across the 10-quest chain.
            }

            // === Chain-completion assertions ====================================
            check("all 10 quests completed server-side", completed.size() == 10);
            check("the convergence quest completed LAST",
                  !completed.empty() && completed.back() == kConvergenceQuest);
            check("every objective type exercised: kill", kills_seen > 0);
            check("every objective type exercised: collect", collects_seen > 0);
            check("every objective type exercised: deliver", delivers_seen > 0);
            check("every objective type exercised: explore", explores_seen > 0);
            check("reward items landed durably in the character's inventory",
                  reward_items_landed);

            std::printf("  chain completion order (server-authoritative):\n");
            for (std::size_t i = 0; i < completed.size(); ++i)
                std::printf("    %2zu. %s\n", i + 1, completed[i].c_str());
        }  // client closes

        server.join();
        world.stop();

        // Cleanup: the character's items (JOIN so the FK cascade clears placements),
        // then the character. The throwaway DB is dropped wholesale below.
        conn.execute("DELETE ii FROM item_instance ii "
                     "JOIN character_inventory ci ON ci.item_guid = ii.item_guid "
                     "WHERE ci.char_id = ?", {sid(char_id)});
        conn.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    // Drop the throwaway database it owns.
    {
        const fs::path drop = scratch / "drop.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
        run_sql_file(client, flags, "", drop);
    }

    if (g_fail == 0) {
        std::printf("\nPASS: the IT-M1 10-quest chain ran fully server-side (zero client-trusted "
                    "outcomes)\n");
        return 0;
    }
    std::printf("\nFAIL: %d IT-M1 chain-run check(s) failed\n", g_fail);
    return 1;
}

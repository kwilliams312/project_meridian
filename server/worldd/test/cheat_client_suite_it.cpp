// SPDX-License-Identifier: Apache-2.0
//
// worldd — INSTRUMENTED CHEAT-CLIENT regression suite (issue #422, epic #21 EXIT
// CRITERION). The capstone that PROVES the epic-#21 exit line:
//   "speedhack/teleport/dupe attempts from the cheat client are all rejected and
//    logged."
//
// WHAT IT DOES. It stands up a LIVE worldd session (real TLS listener + serve loop,
// a throwaway MariaDB owning the world + auth + characters schemas + mcc-emitted
// content) exactly like the #399 IT-M1 chain-run harness it is modeled on — then,
// instead of playing the game legitimately, it acts as a CHEAT CLIENT: it drives a
// battery of deliberate exploit attempts over the wire and asserts, for EACH one,
// BOTH halves of the anti-cheat contract:
//
//   (1) the attempt is REJECTED server-side — the authoritative state is unchanged
//       or corrected (a snap-back MovementState / a typed error reply), and no
//       durable state records an illegal gain; AND
//   (2) the attempt is RECORDED on the append-only OPS-05 ANTI-CHEAT / ECONOMY
//       AUDIT stream (meridian/core/audit.hpp) — a `{event="audit"}` JSON line with
//       the right `action` + `reason`.
//
// The audit half is observed by CAPTURING the process's stdout (the audit sink is
// the shared OPS-05 log pipeline — a JSON line on stdout, log::write_always) into a
// file for the duration of the attack phase, then scanning the bytes each attack
// produced for the exact audit record the live dispatch emitted. The harness's own
// pass/fail lines go to stderr so they stay visible under ctest while stdout carries
// the machine audit stream.
//
// THE ATTACK BATTERY (each MUST be rejected AND logged; a SUCCESS is a real
// anti-cheat gap — the suite fails loudly, it never weakens an assertion to go
// green):
//   A. SPEEDHACK   — a MOVEMENT_INTENT whose displacement exceeds the #420 per-packet
//                    speed envelope (but stays under the teleport hard budget).
//                    Expect: snap-back to spawn (no displacement gained) + an audit
//                    action="movement_rejected" reason="speed_per_packet".
//   B. TELEPORT    — a single-packet position jump beyond the #420 teleport hard
//                    budget (kTeleportHardBudget = 13.8 m). Expect: snap-back to
//                    spawn + audit action="movement_rejected" reason="teleport".
//   C. FLY / FLAG  — a MOVEMENT_INTENT carrying a fabricated reserved state-flag bit
//                    (a "fly" hack, #420 flag legality). Expect: snap-back to spawn +
//                    audit action="movement_rejected" reason="illegal_flag".
//   D. DUPE (loot) — double-take the SAME loot slot. Expect: the FIRST take grants the
//                    item exactly once (durable inventory +1); the REPLAY is rejected
//                    ALREADY_LOOTED and the durable inventory is UNCHANGED (grants at
//                    most once — no duplication).
//   E. DUPE (econ) — an impossible economy delta over the wire (a VENDOR_BUY with an
//                    absurd quantity, an int overflow probe). Expect: a BAD_QUANTITY
//                    reject, the character's money UNCHANGED, and an audit
//                    action="economy_rejected" target="vendor_buy" reason="bad_quantity".
// Plus a CONTROL: a single LEGAL move is ACCEPTED (the envelope is not a blanket
// reject — a green from D/E/A/B/C is meaningful only if a legit client still moves).
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*), needs a
// mariadb/mysql client + an mcc binary; SKIPS (exit 0) when any is absent — inert in
// the plain server ctest, real only under scripts/dev/test.sh --db / the DB CI job.
// Creates + drops the throwaway database it owns, touching nothing else.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world/auth/character
// DDL + the #420 movement envelope (movement_constants.h / movement_validation.h) +
// the #421 economy-sanity path + the OPS-05 audit stream (core::audit) + the world.fbs
// wire contract; modeled structurally on the #399 IT-M1 chain-run harness. No
// GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "world_dispatch.h"
#include "world_session.h"

#include "db_content_store.h"
#include "loot_roll.h"
#include "loot_session.h"
#include "movement_constants.h"

#include "characters.h"

#include "meridian/core/log.hpp"
#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

#include "world_generated.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace db = meridian::db;
namespace chr = meridian::characters;
namespace lo = meridian::loot;
namespace mc = meridian::worldd::movement;
namespace mlog = meridian::core::log;

namespace {

int g_fail = 0;

// Harness pass/fail lines go to STDERR: stdout is redirected to the audit capture
// file for the attack phase (below), so keeping our own diagnostics on stderr means
// they stay visible under ctest while stdout carries only the machine audit stream.
void check(const char* what, bool ok) {
    std::fprintf(stderr, "  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void checks(const std::string& what, bool ok) { check(what.c_str(), ok); }

using Bytes = std::vector<std::uint8_t>;

const char* env(const char* k) { return std::getenv(k); }
const char* pick(const char* world_key, const char* fallback_key) {
    if (const char* v = env(world_key)) return v;
    return env(fallback_key);
}

// ---- MariaDB client + mcc discovery (parity with itm1_chain_run) --------------
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
#ifdef CHEAT_MCC_BIN
    candidates.emplace_back(CHEAT_MCC_BIN);
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
            reinterpret_cast<const unsigned char*>("meridian-cheat-suite"), -1, -1, 0);
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
Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y, float z,
                          std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, flags, x, y, z, /*orientation=*/0.0f,
                                      client_time_ms));
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
Bytes enc_vendor_buy(std::uint32_t vendor_id, std::uint32_t template_id, std::uint32_t quantity) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuyRequest(b, vendor_id, template_id, quantity));
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
// skipping unrelated server-pushed frames so a straggler never desyncs the cursor.
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

// ---- DB helpers --------------------------------------------------------------
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

// Seed a corpse with TWO item slots (so taking slot 0 leaves slot 1 — the corpse is
// NOT fully looted, the loot session is retained, and a REPLAY of slot 0 resolves to
// ALREADY_LOOTED rather than NO_SUCH_CORPSE). Wide loot_range so the looter is always
// in range (position is decoupled from the collect check).
void seed_corpse(mw::WorldServer& world, std::uint64_t corpse, std::uint64_t owner,
                 std::uint32_t item_id) {
    lo::LootRoll roll;
    roll.stacks.push_back(lo::LootStack{item_id, 1, /*required_quest_id=*/0});  // slot 0
    roll.stacks.push_back(lo::LootStack{item_id, 1, /*required_quest_id=*/0});  // slot 1
    roll.copper = 0;
    const lo::LootPoint pos{64.0f, 64.0f, 0.0f};
    world.loot_registry().insert(
        lo::LootSession(corpse, pos, std::move(roll), {owner}, /*loot_range=*/1.0e6f));
}

// ---- Audit-stream capture (stdout redirect) ---------------------------------
// The OPS-05 audit sink is a JSON line on stdout (log::write_always). To OBSERVE the
// live audit stream from inside the same process, redirect the process's stdout fd
// to a capture file for the attack phase and scan the bytes each attack appends. The
// harness's own diagnostics stay on stderr (see check()), so ctest still shows them.
std::string g_cap_path;
int g_saved_stdout = -1;
std::size_t g_scan_offset = 0;

void begin_audit_capture(const std::string& path) {
    g_cap_path = path;
    std::fflush(stdout);
    g_saved_stdout = ::dup(fileno(stdout));
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ::dup2(fd, fileno(stdout));
        ::close(fd);
    }
}
void end_audit_capture() {
    std::fflush(stdout);
    if (g_saved_stdout >= 0) {
        ::dup2(g_saved_stdout, fileno(stdout));
        ::close(g_saved_stdout);
        g_saved_stdout = -1;
    }
}
// Return the audit bytes written since the previous drain (advances the cursor).
std::string drain_audit() {
    std::fflush(stdout);
    std::ifstream in(g_cap_path, std::ios::binary);
    if (!in) return "";
    in.seekg(static_cast<std::streamoff>(g_scan_offset));
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string chunk = ss.str();
    g_scan_offset += chunk.size();
    return chunk;
}
bool contains_all(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (hay.find(n) == std::string::npos) return false;
    return true;
}

// A snap-back MovementState reply holds the LAST AUTHORITATIVE position (spawn here,
// the play-area centre). "Rejected + no displacement gained" ⇔ the reply is at spawn
// and NOT at the attack target.
constexpr float kSpawnX = mc::kZoneMaxXY * 0.5f;  // 64 m
constexpr float kSpawnY = mc::kZoneMaxXY * 0.5f;  // 64 m
bool near_spawn(float x, float y) {
    return std::fabs(x - kSpawnX) < 0.01f && std::fabs(y - kSpawnY) < 0.01f;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    // Force JSON so the audit stream is a machine-parseable line on stdout regardless
    // of MERIDIAN_LOG_FORMAT (test.sh sets text for dev readability) — the capture
    // scans for `"action":"..."` / `"reason":"..."` JSON tokens.
    mlog::set_format(mlog::Format::Json);

    std::fprintf(stderr,
                 "worldd CHEAT-CLIENT regression suite (#422, epic #21 EXIT CRITERION)\n");

    // --- Guards: DB env, client, mcc. SKIP (inert) if any is missing. ----------
    if (!pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST") &&
        !pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        std::fprintf(stderr, "SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured\n");
        return 0;
    }
    const std::string client = find_client();
    if (client.empty()) { std::fprintf(stderr, "SKIP: no mariadb/mysql client on PATH\n"); return 0; }
    const std::string mcc = find_mcc();
    if (mcc.empty()) {
        std::fprintf(stderr, "SKIP: no mcc binary found (set MERIDIAN_MCC_BIN, or build it)\n");
        return 0;
    }
    const std::string flags = conn_flags();

    fs::path scratch = fs::temp_directory_path() / "cheat_client_suite_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    const std::string dbname = "meridian_cheat_suite";
    const fs::path cap_path = scratch / "audit_stream.jsonl";

    // --- 1. mcc emit-sql content/core -> world.sql (a valid item_template set). --
    const fs::path world_sql = scratch / "world.sql";
    {
        std::string cmd = "\"" + mcc + "\" emit-sql \"" + std::string(CHEAT_CONTENT_DIR) +
                          "\" --out \"" + world_sql.string() + "\" >" +
                          (scratch / "emit.log").string() + " 2>&1";
        const int rc = std::system(cmd.c_str());
        check("mcc emit-sql produced world.sql", rc == 0 && fs::exists(world_sql));
        if (rc != 0 || !fs::exists(world_sql)) {
            std::fprintf(stderr, "FAIL: emit-sql failed (rc=%d); see %s\n", rc,
                         (scratch / "emit.log").string().c_str());
            return 1;
        }
    }

    // --- 2. Assemble the three schemas: world DDL + auth + characters. ---------
    const fs::path world_ddl = scratch / "world_ddl.sql";
    const fs::path auth_ddl = scratch / "auth.sql";
    const fs::path char_ddl = scratch / "characters.sql";
    concat_dir_sql(CHEAT_WORLD_DDL_DIR, world_ddl);
    concat_up_migrations(CHEAT_AUTH_MIGRATIONS_DIR, auth_ddl);
    concat_up_migrations(CHEAT_CHAR_MIGRATIONS_DIR, char_ddl);
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
        if (rc != 0) { std::fprintf(stderr, "FAIL: could not create database\n"); return 1; }
    }
    check("world DDL loads", run_sql_file(client, flags, dbname, world_ddl) == 0);
    check("mcc-emitted content DML loads", run_sql_file(client, flags, dbname, world_sql) == 0);
    check("auth schema loads", run_sql_file(client, flags, dbname, auth_ddl) == 0);
    check("characters schema loads", run_sql_file(client, flags, dbname, char_ddl) == 0);

    char tmpl[] = "/tmp/meridian-cheat-suite-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    std::uint64_t account_id = 0, realm_id = 0, char_id = 0, grant_ok = 0;
    const std::int64_t start_money = 500;

    try {
        db::ConnectParams params = conn_params(dbname);
        db::Connection conn(params);
        std::fprintf(stderr, "connected to the throwaway world+auth+characters DB\n");

        // --- 4. Load the #390 DB content stores + install them behind the seams. ---
        static mw::WorldContent content = mw::load_world_content(conn);
        mw::install_content_stores(content.items.get(), content.vendor.get(),
                                   content.quests.get(), content.npcs.get());

        // A real item_template id for the seeded corpse loot (so the mint/place lands
        // durably against a known template — the same join the live loot path uses).
        std::uint32_t loot_item_id = 0;
        {
            db::Result r = conn.execute("SELECT id FROM item_template ORDER BY id LIMIT 1", {});
            if (!r.rows.empty()) loot_item_id = static_cast<std::uint32_t>(cell_u64(r.rows.at(0)[0]));
        }
        check("resolved a real item_template id for the corpse loot", loot_item_id != 0);

        // --- 5. Seed account + realm + grant + the session's character. --------
        const std::string username = "cheat_" + std::to_string(salt);
        conn.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                     {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        account_id = cell_u64(conn.execute("SELECT id FROM account WHERE username = ?",
                                           {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "Cheat Realm " + std::to_string(salt);
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
        cr.name = "CheatHero" + std::to_string(salt % 100000);
        cr.race = static_cast<std::uint8_t>(chr::Race::kSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::Class::kRuncaller);
        char_id = chr::create_character(conn, cr).character_id;
        check("session character created", char_id > 0);
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

            // ENTER_WORLD(owned character) -> OK (spawns at the play-area centre).
            if (auto pl = round_trip(c, mn::Opcode::ENTER_WORLD_REQUEST,
                                     enc_enter_world_request(char_id),
                                     mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD -> OK", m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }

            // ===================================================================
            // ATTACK PHASE — capture the audit stream from here.
            // ===================================================================
            begin_audit_capture(cap_path.string());
            std::uint32_t intent_seq = 1;

            // Helper: send a movement intent, read back the authoritative MovementState,
            // and assert BOTH halves of the anti-cheat contract for a REJECTED move.
            auto attack_move = [&](const char* name, std::uint32_t flags, float tx, float ty,
                                   std::uint64_t t_ms, const char* audit_reason) {
                auto pl = round_trip(c, mn::Opcode::MOVEMENT_INTENT,
                                     enc_movement_intent(intent_seq, flags, tx, ty, 0.0f, t_ms),
                                     mn::Opcode::MOVEMENT_STATE, seq++);
                const std::string tag = name;
                if (!pl) { checks(tag + ": got a MovementState reply", false); return; }
                const auto* ms = decode<mn::MovementState>(*pl);
                if (!ms) { checks(tag + ": decoded MovementState", false); return; }
                // (1) REJECTED server-side: the authoritative reply snapped back to
                // spawn — the illegal displacement was NOT granted.
                checks(tag + ": snap-back to spawn (no illegal displacement gained)",
                       near_spawn(ms->x(), ms->y()));
                checks(tag + ": MovementState.ack_seq echoes the offending intent",
                       ms->ack_seq() == intent_seq);
                // (2) LOGGED: an anti-cheat audit record with the right action + reason.
                const std::string a = drain_audit();
                checks(tag + ": audit movement_rejected reason=" + std::string(audit_reason),
                       contains_all(a, {"\"action\":\"movement_rejected\"",
                                        (std::string("\"reason\":\"") + audit_reason + "\"").c_str()}));
                ++intent_seq;
            };

            // --- ATTACK A: SPEEDHACK -----------------------------------------
            // Run mode; dt = 200 ms => per-packet budget = 6.0*0.2*1.15 = 1.38 m. A 10 m
            // step is far over the envelope but UNDER the 13.8 m teleport budget, so it
            // is classified as a per-packet speed violation (a "speedhack").
            attack_move("speedhack", /*Run=*/2u, kSpawnX + 10.0f, kSpawnY, /*t=*/200,
                        "speed_per_packet");

            // --- ATTACK B: TELEPORT ------------------------------------------
            // A single-packet 30 m jump (> kTeleportHardBudget 13.8 m, still in bounds)
            // is a warp — rejected as its own kind.
            attack_move("teleport", /*Run=*/2u, kSpawnX + 30.0f, kSpawnY, /*t=*/400, "teleport");

            // --- ATTACK C: FLY / FABRICATED FLAG -----------------------------
            // A reserved state-flag bit (bit 9 = 0x200) is a fabricated "fly" flag no
            // legit M0 client can send — rejected on flag legality (before the speed
            // checks), so the tiny step's target is irrelevant.
            attack_move("fly-flag", /*Run|reserved bit9=*/2u | 0x200u, kSpawnX + 5.0f, kSpawnY,
                        /*t=*/600, "illegal_flag");

            // --- CONTROL: a LEGAL move is ACCEPTED ---------------------------
            // dt = 50 ms from the previous intent => budget = 6.0*0.05*1.15 = 0.345 m; a
            // 0.30 m step is legal. Proves the envelope is not a blanket reject (so the
            // rejections above are meaningful, not a dead handler).
            {
                const float legal_x = kSpawnX + 0.30f;
                auto pl = round_trip(c, mn::Opcode::MOVEMENT_INTENT,
                                     enc_movement_intent(intent_seq, 2u, legal_x, kSpawnY, 0.0f,
                                                         /*t=*/650),
                                     mn::Opcode::MOVEMENT_STATE, seq++);
                bool advanced = false;
                if (pl) {
                    const auto* ms = decode<mn::MovementState>(*pl);
                    advanced = ms && std::fabs(ms->x() - legal_x) < 0.01f &&
                               std::fabs(ms->y() - kSpawnY) < 0.01f;
                }
                check("CONTROL: a legal move is ACCEPTED (envelope is not blanket-reject)",
                      advanced);
                (void)drain_audit();  // a legal move emits no anti-cheat audit
                ++intent_seq;
            }

            // --- ATTACK D: DUPE (loot double-take) ---------------------------
            // Seed a corpse (two slots), open it, take slot 0 ONCE (the item lands
            // durably), then REPLAY the take of slot 0. The replay must be rejected
            // ALREADY_LOOTED and the durable inventory must be UNCHANGED — the item is
            // granted AT MOST ONCE (no duplication).
            {
                const std::uint64_t corpse = 0xC0FFEE01ULL;
                seed_corpse(world, corpse, char_id, loot_item_id);

                bool opened = false;
                if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(corpse),
                                         mn::Opcode::LOOT_RESPONSE, seq++)) {
                    const auto* m = decode<mn::LootResponse>(*pl);
                    opened = m && m->status() == mn::LootStatus::OK;
                }
                check("dupe: loot window opens on the seeded corpse", opened);

                const std::uint64_t backpack_before = backpack_used(conn, char_id);

                // First (legitimate) take -> OK, item minted into durable inventory.
                bool first_ok = false;
                if (auto pl = round_trip(c, mn::Opcode::LOOT_TAKE, enc_loot_take(corpse, 0, false),
                                         mn::Opcode::LOOT_RESULT, seq++)) {
                    const auto* m = decode<mn::LootResult>(*pl);
                    first_ok = m && m->status() == mn::LootTakeStatus::OK;
                }
                check("dupe: FIRST take of slot 0 -> OK (granted once)", first_ok);
                const std::uint64_t backpack_after_first = backpack_used(conn, char_id);
                check("dupe: the legitimate take landed exactly one item durably",
                      backpack_after_first == backpack_before + 1);

                // Replay the SAME slot -> ALREADY_LOOTED (rejected), no second grant.
                bool replay_rejected = false;
                if (auto pl = round_trip(c, mn::Opcode::LOOT_TAKE, enc_loot_take(corpse, 0, false),
                                         mn::Opcode::LOOT_RESULT, seq++)) {
                    const auto* m = decode<mn::LootResult>(*pl);
                    replay_rejected = m && m->status() == mn::LootTakeStatus::ALREADY_LOOTED;
                }
                check("dupe: REPLAY take of slot 0 -> ALREADY_LOOTED (rejected)", replay_rejected);
                const std::uint64_t backpack_after_replay = backpack_used(conn, char_id);
                check("dupe: NO duplication — durable inventory unchanged by the replay",
                      backpack_after_replay == backpack_after_first);
            }

            // --- ATTACK E: DUPE (impossible economy delta) -------------------
            // A VENDOR_BUY with an absurd quantity (a 32-bit overflow probe) is an
            // impossible economy delta — rejected BAD_QUANTITY, money UNCHANGED, and
            // flagged on the economy audit stream (action="economy_rejected").
            {
                const std::int64_t money_before = money_of(conn, char_id);
                bool bad_quantity = false;
                if (auto pl = round_trip(c, mn::Opcode::VENDOR_BUY_REQUEST,
                                         enc_vendor_buy(/*vendor_id=*/1, loot_item_id,
                                                        /*quantity=*/0xFFFFFFFFu),
                                         mn::Opcode::VENDOR_BUY_RESULT, seq++)) {
                    const auto* m = decode<mn::VendorBuyResult>(*pl);
                    bad_quantity = m && m->status() == mn::VendorBuyStatus::BAD_QUANTITY;
                }
                check("econ-dupe: impossible VENDOR_BUY quantity -> BAD_QUANTITY (rejected)",
                      bad_quantity);
                check("econ-dupe: character money UNCHANGED (no money duplication/loss)",
                      money_of(conn, char_id) == money_before);
                const std::string a = drain_audit();
                check("econ-dupe: audit economy_rejected target=vendor_buy reason=bad_quantity",
                      contains_all(a, {"\"action\":\"economy_rejected\"", "vendor_buy",
                                       "\"reason\":\"bad_quantity\""}));
            }

            end_audit_capture();
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
        end_audit_capture();
        std::fprintf(stderr, "  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        end_audit_capture();
        std::fprintf(stderr, "  FAIL  exception: %s\n", e.what());
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
        std::fprintf(stderr,
                     "\nPASS: every speedhack/teleport/fly/dupe attempt was REJECTED and LOGGED "
                     "(zero successful exploit)\n");
        return 0;
    }
    std::fprintf(stderr, "\nFAIL: %d cheat-client suite check(s) failed\n", g_fail);
    return 1;
}

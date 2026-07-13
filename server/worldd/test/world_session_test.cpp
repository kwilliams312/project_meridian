// SPDX-License-Identifier: Apache-2.0
//
// worldd IF-3 session establishment integration test (IT-M0 enter-world; #84).
//
// CLEAN-ROOM: written from the server SAD (§5.2 IF-2 framing + per-session AEAD,
// §5.3 IF-3 session handoff + single-use atomic consume, §4.1 session_grant DDL,
// §4.2 characters DB, decision D-11 placeholder character), the world.fbs wire
// contract, and the OpenSSL public API docs only. No GPL source consulted
// (CONTRIBUTING).
//
// Composes meridian::world-dispatch (the real serve/dispatch loop + the real
// WORLD_HELLO handler) against a REAL MariaDB + a REAL TLS 1.3 socket. It seeds a
// session_grant row exactly as authd writes it (decimal-string binding for the
// BIGINT UNSIGNED grant_id/account_id), then drives the real worldd handler.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env (same vars as the db/account/authd tests) and SKIPS (exit 0)
// when none are set, so it is inert in the plain server build's ctest and runs
// for real only in the worldd-session CI job (or locally with env set).
//
// What it proves end-to-end:
//   A. HAPPY PATH — a TLS 1.3 client sends WorldHello{grant_id} -> the server
//      validates + atomically consumes the grant, establishes the AEAD session,
//      loads the placeholder character, and replies HandshakeOk. The grant row
//      is consumed_at != NULL afterward.
//   B. REPLAY — a second WorldHello with the SAME grant_id -> Disconnect
//      {GRANT_INVALID}, and the DB shows NO second consume (consumed_at unchanged).
//   C. EXPIRED — a grant whose expires_at is in the past -> Disconnect, not
//      consumed.
//   D. UNKNOWN — a grant_id with no row -> Disconnect.
//   E. AEAD — WorldSession seals a frame that opens back to the same plaintext,
//      a tampered ciphertext fails to open, HKDF gives the two directions
//      different keys, and the nonce counter advances per seal.

#include "world_dispatch.h"
#include "world_session.h"

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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// ---- Throwaway self-signed cert (OpenSSL API; mirrors the net/authd tests) ---
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
            reinterpret_cast<const unsigned char*>("meridian-worldd-session-test"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-2 client (mirrors world_dispatch_test) --------------
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
    bool is_tls13() const {
        return connected_ && std::string(SSL_get_version(ssl_)) == "TLSv1.3";
    }

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

// ---- world.fbs payload builders --------------------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0);
    b.Finish(h);
    return bytes_of(b);
}

Bytes enc_enter_world_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

std::uint64_t cell_u64(const db::Cell& c) {
    return c.has_value() ? std::strtoull(c->c_str(), nullptr, 10) : 0;
}

db::Param blob32(const Bytes& b) {
    return db::Param{db::Bytes_t(b.begin(), b.end())};
}

// Seed a session_grant row EXACTLY as authd writes it (SAD §4.1; decimal-string
// binding for the BIGINT UNSIGNED grant_id/account_id — the int64/BIGINT-UNSIGNED
// gotcha). `expires_sql` is the DATETIME expression (e.g. a future or past time).
void seed_grant(db::Connection& db, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key,
                std::uint32_t client_build, const std::string& expires_sql) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, " + expires_sql + ")",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob32(session_key),
         db::Param{static_cast<std::int64_t>(client_build)}});
}

Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y,
                          float z, std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, flags, x, y, z, /*orientation=*/0.0f,
                                      client_time_ms));
    return bytes_of(b);
}

// Handshake over a fresh connection, then send ONE MovementIntent on the SAME
// (now-authenticated) connection and decode the MovementState reply. Proves the
// #86 authoritative path end-to-end on an established session: the server
// validates the intent, advances authoritative state, and replies MovementState.
struct MoveResult {
    bool handshake_ok = false;
    bool got_state = false;      // the FIRST MovementState reply (legal move) came back
    float state_x = 0.0f, state_y = 0.0f, state_z = 0.0f;
    std::uint32_t ack_seq = 0;
    // OPS-03a (#420): an OPTIONAL SECOND intent on the SAME in-world connection —
    // used to prove an ILLEGAL move is rejected + snapped back on the live path,
    // WITHOUT a second ENTER_WORLD (which would hit single-active-session, #326).
    bool got_state2 = false;     // the second MovementState (correction) came back
    float state2_x = 0.0f, state2_y = 0.0f, state2_z = 0.0f;
    std::uint32_t ack_seq2 = 0;
};

// Decode a MOVEMENT_STATE reply frame into (x,y,z,ack). Returns false if the next
// frame is not a MovementState (or none arrived).
bool recv_movement_state(Client& c, float& x, float& y, float& z, std::uint32_t& ack) {
    // Skip the unsolicited self VITALS_UPDATE the server now pushes at ENTER_WORLD
    // (#439: the HUD player-frame snapshot). It arrives after ENTER_WORLD_RESPONSE
    // but before this MovementState, so drain it here to reach the awaited state.
    // `ms` MUST outlive the decode below — rf->payload points INTO it.
    std::optional<Bytes> ms;
    std::optional<mw::Frame> rf;
    for (;;) {
        ms = c.recv_frame();
        if (!ms) return false;
        rf = mw::decode_frame(*ms);
        if (rf && rf->opcode == mn::Opcode::VITALS_UPDATE) continue;

        if (rf && rf->opcode == mn::Opcode::INVENTORY_SNAPSHOT) continue;  // #453 unsolicited bags snapshot
        if (rf && rf->opcode == mn::Opcode::KNOWN_ABILITIES) continue;     // #457 unsolicited spellbook
        break;
    }
    if (!rf || rf->opcode != mn::Opcode::MOVEMENT_STATE) return false;
    Bytes pl(rf->payload, rf->payload + rf->payload_len);
    const auto* st = decode<mn::MovementState>(pl);
    if (!st) return false;
    x = st->x(); y = st->y(); z = st->z(); ack = st->ack_seq();
    return true;
}

MoveResult drive_hello_then_move(std::uint16_t port, std::uint64_t grant_id,
                                 std::uint32_t build, std::uint64_t character_id,
                                 std::uint32_t flags, float x,
                                 float y, float z, std::uint64_t client_time_ms,
                                 // OPTIONAL second intent (default: none). When
                                 // second_move is true, a further MovementIntent is
                                 // sent on the SAME connection after the first reply.
                                 bool second_move = false, std::uint32_t flags2 = 0,
                                 float x2 = 0.0f, float y2 = 0.0f, float z2 = 0.0f,
                                 std::uint32_t intent_seq2 = 8,
                                 std::uint64_t client_time_ms2 = 0) {
    MoveResult r;
    Client c(port);
    if (!c.connected()) return r;
    c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, /*seq=*/1,
                                  enc_world_hello(grant_id, build)));
    std::optional<Bytes> hs = c.recv_frame();
    if (hs) {
        std::optional<mw::Frame> rf = mw::decode_frame(*hs);
        if (rf && rf->opcode == mn::Opcode::HANDSHAKE_OK) {
            Bytes pl(rf->payload, rf->payload + rf->payload_len);
            r.handshake_ok = (decode<mn::HandshakeOk>(pl) != nullptr);
        }
    }
    if (!r.handshake_ok) return r;

    // Server-authoritative characters (D-35): movement is legal only IN WORLD, so
    // the session must ENTER_WORLD as its owned character before a MovementIntent
    // is accepted. Enter, expect OK, THEN move.
    c.send_frame(mw::encode_frame(mn::Opcode::ENTER_WORLD_REQUEST, /*seq=*/2,
                                  enc_enter_world_request(character_id)));
    if (std::optional<Bytes> ew = c.recv_frame()) {
        std::optional<mw::Frame> rf = mw::decode_frame(*ew);
        if (!rf || rf->opcode != mn::Opcode::ENTER_WORLD_RESPONSE) return r;
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        const auto* resp = decode<mn::EnterWorldResponse>(pl);
        if (!resp || resp->status() != mn::EnterWorldStatus::OK) return r;
    } else {
        return r;
    }

    // First MovementIntent (the legal move) on the in-world connection.
    c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, /*seq=*/3,
                                  enc_movement_intent(7, flags, x, y, z, client_time_ms)));
    r.got_state = recv_movement_state(c, r.state_x, r.state_y, r.state_z, r.ack_seq);

    // OPS-03a (#420): an OPTIONAL second intent on the SAME session — the server
    // ALWAYS replies a MovementState (accept = advance; reject = snap-back), so the
    // client can block on exactly one reply. Reusing the session avoids a second
    // ENTER_WORLD (single-active-session, #326).
    if (second_move && r.got_state) {
        c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, /*seq=*/4,
                                      enc_movement_intent(intent_seq2, flags2, x2, y2, z2,
                                                          client_time_ms2)));
        r.got_state2 =
            recv_movement_state(c, r.state2_x, r.state2_y, r.state2_z, r.ack_seq2);
    }
    return r;
}

// Drive one WorldHello over a fresh TLS connection; return the server's first
// reply frame (HandshakeOk on success, Disconnect on reject) decoded, plus a
// flag for whether the connection then closed.
struct HelloResult {
    bool connected = false;
    bool got_reply = false;
    bool is_handshake_ok = false;
    bool is_disconnect = false;
    bool closed_after = false;
};

HelloResult drive_hello(std::uint16_t port, std::uint64_t grant_id,
                        std::uint32_t build) {
    HelloResult r;
    Client c(port);
    r.connected = c.connected();
    if (!r.connected) return r;
    c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, /*seq=*/1,
                                  enc_world_hello(grant_id, build)));
    std::optional<Bytes> reply = c.recv_frame();
    if (reply) {
        r.got_reply = true;
        std::optional<mw::Frame> rf = mw::decode_frame(*reply);
        if (rf) {
            if (rf->opcode == mn::Opcode::HANDSHAKE_OK) {
                Bytes pl(rf->payload, rf->payload + rf->payload_len);
                r.is_handshake_ok = (decode<mn::HandshakeOk>(pl) != nullptr);
            } else if (rf->opcode == mn::Opcode::DISCONNECT) {
                Bytes pl(rf->payload, rf->payload + rf->payload_len);
                r.is_disconnect = (decode<mn::Disconnect>(pl) != nullptr);
            }
        }
    }
    // After a Disconnect the server closes; after HandshakeOk it stays open (we
    // just close from our side). Only the reject path guarantees an EOF here.
    if (r.is_disconnect) r.closed_after = !c.recv_frame().has_value();
    return r;
}

// ===== E. AEAD unit checks (pure, no DB/socket) =============================
void test_aead() {
    Bytes key(mw::kAeadKeyBytes);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);

    mw::WorldSession s(key);

    // Different derived keys per direction (HKDF direction separation).
    check("E: c2s and s2c keys differ",
          s.key(mw::Direction::kClientToServer) != s.key(mw::Direction::kServerToClient));

    // Seal c2s, open c2s -> round-trips.
    const Bytes plaintext = {'h', 'e', 'l', 'l', 'o', '-', 'w', 'o', 'r', 'l', 'd'};
    const Bytes aad = {0x01, 0x00, 0xDE, 0xAD};  // e.g. opcode+seq header bytes
    std::uint64_t seq0 = 12345;  // overwritten by seal (out_seq)
    check("E: first seal uses seq 0", s.next_seq(mw::Direction::kClientToServer) == 0);
    Bytes sealed = s.seal(mw::Direction::kClientToServer, plaintext, aad, seq0);
    check("E: seal returned out_seq 0", seq0 == 0);
    check("E: counter advanced to 1", s.next_seq(mw::Direction::kClientToServer) == 1);
    check("E: ciphertext = plaintext+tag length",
          sealed.size() == plaintext.size() + mw::kAeadTagBytes);

    // A second session with the SAME key opens it (the two ends share session_key).
    mw::WorldSession peer(key);
    std::optional<Bytes> opened =
        peer.open(mw::Direction::kClientToServer, sealed, seq0, aad);
    check("E: open recovers plaintext", opened.has_value() && *opened == plaintext);

    // Tampered ciphertext fails to open (flip one byte).
    Bytes tampered = sealed;
    tampered[0] ^= 0x80;
    check("E: tampered ciphertext fails to open",
          !peer.open(mw::Direction::kClientToServer, tampered, seq0, aad).has_value());

    // Wrong AAD fails to open (auth binds the header).
    Bytes wrong_aad = aad;
    wrong_aad[0] ^= 0x01;
    check("E: wrong AAD fails to open",
          !peer.open(mw::Direction::kClientToServer, sealed, seq0, wrong_aad).has_value());

    // Wrong seq (nonce) fails to open.
    check("E: wrong seq fails to open",
          !peer.open(mw::Direction::kClientToServer, sealed, seq0 + 1, aad).has_value());

    // Second seal uses the next nonce (seq 1) — distinct ciphertext for same input.
    std::uint64_t seq1 = 0;
    Bytes sealed2 = s.seal(mw::Direction::kClientToServer, plaintext, aad, seq1);
    check("E: second seal uses seq 1", seq1 == 1);
    check("E: distinct nonce -> distinct ciphertext", sealed2 != sealed);
    check("E: second frame opens at seq 1",
          peer.open(mw::Direction::kClientToServer, sealed2, seq1, aad) == plaintext);
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd IF-3 session establishment test (IT-M0 enter-world, #84)\n");

    // AEAD checks run WITHOUT a DB (pure crypto) so they exercise even in a
    // DB-less environment. Run them first.
    test_aead();

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — AEAD checks ran; "
                    "grant-consume DB checks skipped (set MERIDIAN_DB_SOCKET or "
                    "MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        std::printf(g_fail == 0 ? "\nALL WORLDD SESSION (AEAD) TESTS PASSED\n"
                                : "\n%d WORLDD SESSION TEST(S) FAILED\n", g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    // Self-signed cert into a temp dir.
    char tmpl[] = "/tmp/meridian-worldd-sess-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    std::uint64_t account_id = 0;
    std::uint32_t realm_id = 0;
    std::uint64_t grant_ok = 0, grant_expired = 0, grant_wrong_realm = 0, grant_move = 0;
    std::uint64_t char_move = 0;  // owned character the movement leg enters as
    const std::uint64_t grant_unknown = 0xFEEDFACECAFEBEEFULL;  // never inserted

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        // --- Seed an account + realm (session_grant FKs both, ON DELETE CASCADE).
        const std::string username = "worldd_it_" + std::to_string(std::rand());
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) "
            "VALUES (?, ?, ?)",
            {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        account_id = cell_u64(ar.rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        // Seed one owned character (server-authoritative entry needs a real owned
        // character now, D-35 — the placeholder fabrication is gone). The `character`
        // table doubles into this DB for the test; CREATE IF NOT EXISTS is a no-op
        // when the real migration already loaded it.
        db.execute(
            "CREATE TABLE IF NOT EXISTS `character` ("
            "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "  account_id BIGINT UNSIGNED NOT NULL,"
            "  name VARCHAR(32) NOT NULL,"
            "  race TINYINT UNSIGNED NOT NULL,"
            "  class TINYINT UNSIGNED NOT NULL,"
            "  appearance JSON NULL,"  // 0003 (§5.2): opaque visual record
            "  level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
            "  xp INT UNSIGNED NOT NULL DEFAULT 0,"
            "  money BIGINT UNSIGNED NOT NULL DEFAULT 0,"
            "  map_id INT UNSIGNED NOT NULL,"
            "  instance_id INT UNSIGNED NOT NULL DEFAULT 0,"
            "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
            "  pos_o FLOAT NOT NULL DEFAULT 0,"
            "  played_time INT UNSIGNED NOT NULL DEFAULT 0,"
            "  logout_at DATETIME NULL, save_epoch BIGINT NOT NULL DEFAULT 0,"
            "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "  PRIMARY KEY (id), UNIQUE KEY uq_character_name (name),"
            "  KEY idx_character_account (account_id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        const std::string char_name = "WdIt_" + std::to_string(std::rand());
        db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{char_name}});
        db.execute(
            "INSERT INTO `character` "
            "(account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
            "VALUES (?, ?, 1, 1, 0, 0, 0, 0)",
            {db::Param{std::to_string(account_id)}, db::Param{char_name}});
        char_move = cell_u64(
            db.execute("SELECT id FROM `character` WHERE name = ?",
                       {db::Param{char_name}}).rows.at(0)[0]);
        check("test character seeded", char_move > 0);

        const std::string realm_name = "WD IT Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7200, 0, 100000)",
            {db::Param{realm_name}});
        db::Result rr = db.execute("SELECT id FROM realm WHERE name = ?",
                                   {db::Param{realm_name}});
        realm_id = static_cast<std::uint32_t>(cell_u64(rr.rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        // Grants: one valid (expires +30s, as authd), one already-expired.
        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        // A SECOND realm, so we can seed a grant bound to the wrong realm.
        const std::string realm2_name = "WD IT Realm2 " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7201, 0, 100000)",
            {db::Param{realm2_name}});
        db::Result rr2 = db.execute("SELECT id FROM realm WHERE name = ?",
                                    {db::Param{realm2_name}});
        std::uint32_t realm2_id = static_cast<std::uint32_t>(cell_u64(rr2.rows.at(0)[0]));

        grant_ok = rand_u64();
        grant_expired = rand_u64();
        grant_wrong_realm = rand_u64();
        grant_move = rand_u64();
        const Bytes session_key(32, 0xAB);  // deterministic 32-byte key

        seed_grant(db, grant_ok, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        // A second valid grant used by the movement leg (section G): handshake +
        // ENTER_WORLD + a LEGAL then an ILLEGAL MovementIntent on the SAME
        // connection (#86 authoritative-state proof + #420 reject/snap-back proof).
        seed_grant(db, grant_move, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        seed_grant(db, grant_expired, account_id, realm_id, session_key, client_build,
                   "DATE_SUB(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        // Valid + unexpired, but bound to realm2 — this worldd serves realm_id.
        seed_grant(db, grant_wrong_realm, account_id, realm2_id, session_key,
                   client_build, "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        check("valid + expired + wrong-realm grants seeded", true);

        // --- Stand up the real TLS listener + the worldd serve loop with the
        //     auth DB wired (so the REAL WORLD_HELLO handler validates grants).
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);
        std::printf("  worldd session listener on 127.0.0.1:%u\n", port);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;          // each served connection opens its own auth DB conn
        wcfg.char_db = p;          // characters DB (same instance) — ENTER_WORLD ownership load
        wcfg.realm_id = realm_id;  // grants for another realm are rejected
        mw::Dispatcher dispatcher;  // the REAL WORLD_HELLO handler (not overridden)
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        // Serve six connections: happy, replay, expired, unknown, wrong-realm,
        // and the movement leg (handshake + MOVEMENT_INTENT on one connection).
        std::thread server([&] {
            for (int i = 0; i < 6; ++i) {
                try {
                    net::Session s = listener.accept();
                    world.serve_connection(std::move(s));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server thread error: %s\n", e.what());
                }
            }
        });

        // ===== A. HAPPY PATH: valid grant -> HandshakeOk + consumed ==========
        HelloResult a = drive_hello(port, grant_ok, client_build);
        check("A: client connected (TLS)", a.connected);
        check("A: server replied", a.got_reply);
        check("A: reply is HandshakeOk", a.is_handshake_ok);

        db::Result c1 = db.execute(
            "SELECT consumed_at FROM session_grant WHERE grant_id = ?",
            {db::Param{std::to_string(grant_ok)}});
        check("A: grant row still exists", c1.rows.size() == 1);
        check("A: grant is now consumed (consumed_at set)",
              c1.rows.size() == 1 && c1.rows[0][0].has_value());

        // ===== B. REPLAY: same grant again -> Disconnect, NO second consume ===
        db::Result before = db.execute(
            "SELECT consumed_at FROM session_grant WHERE grant_id = ?",
            {db::Param{std::to_string(grant_ok)}});
        std::string consumed_before = before.rows.at(0)[0].value_or("");

        HelloResult b = drive_hello(port, grant_ok, client_build);
        check("B: replay got a Disconnect", b.is_disconnect);
        check("B: connection closed after Disconnect", b.closed_after);

        db::Result after = db.execute(
            "SELECT consumed_at FROM session_grant WHERE grant_id = ?",
            {db::Param{std::to_string(grant_ok)}});
        std::string consumed_after = after.rows.at(0)[0].value_or("");
        check("B: consumed_at unchanged (no second consume)",
              consumed_after == consumed_before && !consumed_after.empty());

        // ===== C. EXPIRED grant -> Disconnect, not consumed ==================
        HelloResult cc = drive_hello(port, grant_expired, client_build);
        check("C: expired grant got a Disconnect", cc.is_disconnect);
        db::Result ec = db.execute(
            "SELECT consumed_at FROM session_grant WHERE grant_id = ?",
            {db::Param{std::to_string(grant_expired)}});
        check("C: expired grant NOT consumed",
              ec.rows.size() == 1 && !ec.rows[0][0].has_value());

        // ===== D. UNKNOWN grant_id -> Disconnect =============================
        HelloResult d = drive_hello(port, grant_unknown, client_build);
        check("D: unknown grant got a Disconnect", d.is_disconnect);

        // ===== F. WRONG REALM: valid grant for realm2 -> Disconnect ==========
        // The grant is consumed (spent) but rejected — it cannot be replayed
        // against realm2 either (single-use preserved even on a wrong-realm hit).
        HelloResult f = drive_hello(port, grant_wrong_realm, client_build);
        check("F: wrong-realm grant got a Disconnect", f.is_disconnect);

        // ===== G. MOVEMENT after handshake -> authoritative MovementState =====
        // + OPS-03a (#420) REJECT -> live SNAP-BACK correction, on the SAME session.
        // Handshake on grant_move, ENTER_WORLD once, then send TWO intents on the
        // one in-world connection:
        //   (1) a LEGAL 0.20 m walk step from the (-320,-320) spawn (#562) — accepted,
        //       the authoritative position advances to ~-319.80 (#86 authoritative path).
        //   (2) an ILLEGAL move — a Swim mode flag (selector value 4) on the flat
        //       bootstrap map, which has NO liquid volume: "swim on dry land" (SAD
        //       §5.5 flag legality). The full envelope REJECTS it and replies a
        //       snap-back MovementState at the LAST authoritative position (~-319.80),
        //       NOT the cheated position — reject + snap-back on the wired serve
        //       loop, end-to-end (the pure unit test proves the rule; this proves
        //       the wiring). Reusing the one session avoids a second ENTER_WORLD
        //       (single-active-session, #326).
        {
            MoveResult g = drive_hello_then_move(
                port, grant_move, client_build, char_move,
                /*flags=Walk*/ 1, /*x=*/-319.80f, /*y=*/-320.0f, /*z=*/0.0f,
                /*client_time_ms=*/100,
                // Second, ILLEGAL intent on the same session: swim on dry land.
                /*second_move=*/true, /*flags2=Swim*/ mw::movement::kModeSwim,
                /*x2=*/-319.60f, /*y2=*/-320.0f, /*z2=*/0.0f,
                /*intent_seq2=*/8, /*client_time_ms2=*/200);
            check("G: handshake ok on the movement connection", g.handshake_ok);
            check("G: server replied MovementState to the legal intent", g.got_state);
            check("G: MovementState acks the intent seq", g.ack_seq == 7);
            check("G: authoritative advanced to the validated position",
                  g.got_state && g.state_x > -319.81f && g.state_x < -319.79f);
            // The illegal second move: server still replies (snap-back correction).
            check("G: server replied a MovementState to the illegal move", g.got_state2);
            check("G: the correction acks the illegal intent seq", g.ack_seq2 == 8);
            // Snap-back holds the LAST authoritative position (~-319.80, from move 1),
            // NOT the cheated -319.60 — reject + snap-back proven on the live path.
            check("G: illegal move corrected back to last authoritative (~-319.80, not -319.60)",
                  g.got_state2 && g.state2_x > -319.81f && g.state2_x < -319.79f);
        }

        server.join();
        world.stop();

        // --- Cleanup: grants + both realms + account. Deleting the account
        //     CASCADEs its grants; delete both seeded realms explicitly. --------
        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
        db.execute("DELETE FROM realm WHERE id IN (?, ?)",
                   {db::Param{static_cast<std::int64_t>(realm_id)},
                    db::Param{static_cast<std::int64_t>(realm2_id)}});
        db.execute("DELETE FROM `character` WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(char_move)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
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

    std::printf(g_fail == 0 ? "\nALL WORLDD SESSION TESTS PASSED\n"
                            : "\n%d WORLDD SESSION TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

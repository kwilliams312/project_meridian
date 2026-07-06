// SPDX-License-Identifier: Apache-2.0
//
// authd end-to-end login integration test (IT-M0 auth path; issue #79).
//
// CLEAN-ROOM: written from the server SAD (§5.1 IF-1 message flow, §3.1 login->
// grant sequence + single-use consume, §4.1 auth DB), the auth.fbs wire contract,
// and the OpenSSL public API docs only. No GPL source consulted (CONTRIBUTING).
//
// Composes all five merged libs against a REAL MariaDB + a REAL TLS 1.3 socket:
//   - meridian::account  — create the test account (stores {salt, verifier}).
//   - meridian::authd-login — run_login() (the daemon's exact login flow).
//   - meridian::net (transitively) — the real TlsListener behind run_login.
//   - meridian::proto (transitively) — auth.fbs encode/decode on the client side.
//   - meridian::srp — testing::client_side() to compute the real client proof.
//   - meridian::db — read back + atomically consume the session_grant.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env (same vars as the db/account tests) and SKIPS (exit 0) when
// none are set, so it is inert in the plain server build's ctest and runs for
// real only in the authd CI job (or locally with env set).
//
// What it proves end-to-end:
//   A. FULL LOGIN — a TLS 1.3 client drives ClientHello -> ServerHello ->
//      SrpStart -> SrpChallenge -> SrpProof -> AuthResult (M2 verified) ->
//      RealmListRequest -> RealmList (the seeded realm present) -> RealmSelect ->
//      SessionGrant (32-byte key), against the credential meridian::account stored.
//   B. GRANT PERSISTED + SINGLE-USE — the grant row exists with the right
//      {account, realm} and a 32-byte key; the atomic consume UPDATE affects 1
//      row the first time and 0 the second (§3.1 single-use rule).
//   C. WRONG PASSWORD REJECTED — a second login with a bad password yields
//      AuthResult error and writes NO grant.

#include "login_session.h"

#include "account.h"
#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"
#include "meridian/srp/srp.h"

#include "auth_generated.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// ---- Throwaway self-signed cert (OpenSSL API; mirrors the net test) ---------
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
            reinterpret_cast<const unsigned char*>("meridian-authd-test"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-1 client -------------------------------------------
// Wraps an SSL* and speaks the IF-1 length framing (u32 LE prefix + payload) so
// the test can send/receive auth.fbs messages exactly as a real client would.
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

// ---- auth.fbs encode helpers (client side) ----------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_client_hello(std::uint32_t build, std::uint16_t proto) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateClientHello(b, build, proto));
    return bytes_of(b);
}
Bytes enc_srp_start(const std::string& account) {
    fb::FlatBufferBuilder b;
    auto acc = b.CreateString(account);
    b.Finish(mn::CreateSrpStart(b, acc));
    return bytes_of(b);
}
Bytes enc_srp_proof(const Bytes& A, const Bytes& M1) {
    fb::FlatBufferBuilder b;
    auto av = b.CreateVector(A);
    auto mv = b.CreateVector(M1);
    b.Finish(mn::CreateSrpProof(b, av, mv));
    return bytes_of(b);
}
Bytes enc_realm_list_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateRealmListRequest(b));
    return bytes_of(b);
}
Bytes enc_realm_select(std::uint32_t realm_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateRealmSelect(b, realm_id));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}
Bytes vec_to_bytes(const fb::Vector<std::uint8_t>* v) {
    if (v == nullptr) return {};
    return Bytes(v->data(), v->data() + v->size());
}

srp::Bytes to_srp(const Bytes& b) { return srp::Bytes(b.begin(), b.end()); }
Bytes from_srp(const srp::Bytes& b) { return Bytes(b.begin(), b.end()); }

// Fixed client ephemeral a (deterministic; production picks random). SRP only
// requires a != 0.
const Bytes kFixedA = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0xA7, 0xB8,
    0xC9, 0xD0, 0xE1, 0xF2, 0xA3, 0xB4, 0xC5, 0xD6,
    0xE7, 0xF8, 0xA9, 0xB0, 0xC1, 0xD2, 0xE3, 0xF4,
    0xA5, 0xB6, 0xC7, 0xD8, 0xE9, 0xF0, 0xA1, 0xB2};

struct LoginRun {
    bool tls13 = false;
    bool server_hello_ok = false;
    bool challenge_ok = false;
    bool auth_success = false;
    bool auth_m2_ok = false;
    bool realm_present = false;
    std::uint64_t grant_id = 0;
    std::size_t key_len = 0;
};

// Drive the FULL login flow with the given password. `expect_auth_ok` chooses
// whether we continue to realm select (correct pw) or stop at AuthResult (wrong).
LoginRun drive_login(std::uint16_t port, const std::string& account,
                     const std::string& password, std::uint32_t client_build,
                     std::uint32_t realm_id, const srp::Parameters& params,
                     bool expect_auth_ok) {
    LoginRun r;
    Client c(port);
    if (!c.connected()) return r;
    r.tls13 = c.is_tls13();

    // 1) ClientHello -> ServerHello
    c.send_frame(enc_client_hello(client_build, /*proto=*/1));
    auto f = c.recv_frame();
    if (!f) return r;
    const mn::ServerHello* sh = decode<mn::ServerHello>(*f);
    r.server_hello_ok = (sh != nullptr && sh->proto_ver() == 1);
    if (!r.server_hello_ok) return r;

    // 2) SrpStart -> SrpChallenge
    c.send_frame(enc_srp_start(account));
    f = c.recv_frame();
    if (!f) return r;
    const mn::SrpChallenge* ch = decode<mn::SrpChallenge>(*f);
    if (ch == nullptr || ch->salt() == nullptr || ch->b() == nullptr) return r;
    r.challenge_ok = true;
    Bytes salt = vec_to_bytes(ch->salt());
    Bytes B = vec_to_bytes(ch->b());

    // Compute the client proof against the challenge (real SRP-6a).
    srp::testing::ClientProof cp = srp::testing::client_side(
        account, password, to_srp(salt), to_srp(kFixedA), to_srp(B), params);

    // 3) SrpProof -> AuthResult
    c.send_frame(enc_srp_proof(from_srp(cp.A), from_srp(cp.M1)));
    f = c.recv_frame();
    if (!f) return r;
    const mn::AuthResult* ar = decode<mn::AuthResult>(*f);
    if (ar == nullptr) return r;
    r.auth_success = ar->success();
    if (r.auth_success && ar->m2() != nullptr) {
        r.auth_m2_ok = (vec_to_bytes(ar->m2()) == from_srp(cp.expected_M2));
    }
    if (!expect_auth_ok) return r;      // wrong-password path: stop here
    if (!r.auth_success) return r;

    // 4) RealmListRequest -> RealmList
    c.send_frame(enc_realm_list_request());
    f = c.recv_frame();
    if (!f) return r;
    const mn::RealmList* rl = decode<mn::RealmList>(*f);
    if (rl != nullptr && rl->realms() != nullptr) {
        for (const mn::RealmRow* row : *rl->realms()) {
            if (row->id() == realm_id) r.realm_present = true;
        }
    }

    // 5) RealmSelect -> SessionGrant
    c.send_frame(enc_realm_select(realm_id));
    f = c.recv_frame();
    if (!f) return r;
    const mn::SessionGrant* g = decode<mn::SessionGrant>(*f);
    if (g != nullptr) {
        r.grant_id = g->grant_id();
        r.key_len = (g->session_key() ? g->session_key()->size() : 0);
    }
    return r;
}

std::uint64_t cell_u64(const db::Cell& c) {
    return c.has_value() ? std::strtoull(c->c_str(), nullptr, 10) : 0;
}

}  // namespace

int main() {
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
        return 0;
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("authd end-to-end login test (IT-M0 auth path, #79)\n");

    // Self-signed cert into a temp dir.
    char tmpl[] = "/tmp/meridian-authd-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::string username = "authd_it_" + std::to_string(std::rand());
    const std::string password = "correct horse battery staple";
    const std::string wrong_password = "Tr0ub4dour&3";
    const std::uint32_t client_build = 1000;
    srp::Parameters params;  // {Rfc5054_2048, Sha256} — authd + account defaults

    std::uint32_t realm_id = 0;
    std::uint64_t account_id = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        // --- Test-data setup: an account + a realm the client will select. ----
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        account::CreateRequest req;
        req.username = username;
        req.password = password;
        account::CreateResult created = account::create_account(db, req);
        account_id = created.account_id;
        check("test account created", account_id > 0);

        // A realm this build is in range for. Unique name so parallel runs don't
        // collide on uq_realm_name; cleaned (with grants via CASCADE) at the end.
        const std::string realm_name = "IT Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max, "
            " population, flags) VALUES (?, '127.0.0.1', 7200, ?, ?, 0, 0)",
            {db::Param{realm_name},
             db::Param{std::int64_t{0}},
             db::Param{std::int64_t{100000}}});
        db::Result rr = db.execute("SELECT id FROM realm WHERE name = ?",
                                   {db::Param{realm_name}});
        realm_id = static_cast<std::uint32_t>(cell_u64(rr.rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        // --- Stand up the real TLS listener + login state machine. ------------
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);
        std::printf("  authd login listener on 127.0.0.1:%u\n", port);

        authd::LoginConfig login_cfg;
        login_cfg.proto_ver = 1;
        login_cfg.server_build = client_build;
        login_cfg.build_floor = 0;
        login_cfg.srp_params = params;

        // Server thread: serve exactly TWO logins (good, then wrong-password),
        // each on its own connection + DB connection.
        std::thread server([&] {
            for (int i = 0; i < 2; ++i) {
                try {
                    net::Session s = listener.accept();
                    db::Connection sdb(p);
                    authd::run_login(s, sdb, login_cfg);
                    s.close();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server thread error: %s\n", e.what());
                }
            }
        });

        // ===== A. FULL LOGIN with the CORRECT password =====================
        LoginRun good = drive_login(port, username, password, client_build,
                                    realm_id, params, /*expect_auth_ok=*/true);
        check("A: TLS 1.3 negotiated (client)", good.tls13);
        check("A: ServerHello received", good.server_hello_ok);
        check("A: SrpChallenge received", good.challenge_ok);
        check("A: AuthResult success (correct password)", good.auth_success);
        check("A: server M2 verifies (client authenticates server)", good.auth_m2_ok);
        check("A: seeded realm present in RealmList", good.realm_present);
        check("A: SessionGrant received (grant_id != 0)", good.grant_id != 0);
        check("A: session_key is 32 bytes", good.key_len == 32);

        // ===== B. GRANT PERSISTED + SINGLE-USE =============================
        db::Result grow = db.execute(
            "SELECT account_id, realm_id, LENGTH(session_key), consumed_at "
            "FROM session_grant WHERE grant_id = ?",
            {db::Param{std::to_string(good.grant_id)}});
        check("B: grant row exists", grow.rows.size() == 1);
        if (grow.rows.size() == 1) {
            const db::Row& g = grow.rows[0];
            check("B: grant bound to the right account", cell_u64(g[0]) == account_id);
            check("B: grant bound to the selected realm", cell_u64(g[1]) == realm_id);
            check("B: stored session_key is 32 bytes", cell_u64(g[2]) == 32);
            check("B: grant is initially unconsumed", !g[3].has_value());
        }

        // Atomic single-use consume (§3.1). First consume: 1 row; second: 0.
        const char* kConsume =
            "UPDATE session_grant SET consumed_at = UTC_TIMESTAMP() "
            "WHERE grant_id = ? AND consumed_at IS NULL";
        db::Result c1 = db.execute(kConsume,
            {db::Param{std::to_string(good.grant_id)}});
        db::Result c2 = db.execute(kConsume,
            {db::Param{std::to_string(good.grant_id)}});
        check("B: first consume affects exactly 1 row", c1.affected_rows == 1);
        check("B: second consume affects 0 rows (single-use)", c2.affected_rows == 0);

        // ===== C. WRONG PASSWORD -> rejected, NO grant =====================
        db::Result before = db.execute(
            "SELECT COUNT(*) FROM session_grant WHERE account_id = ?",
            {db::Param{static_cast<std::int64_t>(account_id)}});
        std::uint64_t grants_before = cell_u64(before.rows.at(0)[0]);

        LoginRun bad = drive_login(port, username, wrong_password, client_build,
                                   realm_id, params, /*expect_auth_ok=*/false);
        check("C: ServerHello received (wrong-pw run)", bad.server_hello_ok);
        check("C: SrpChallenge received (wrong-pw run)", bad.challenge_ok);
        check("C: AuthResult is FAILURE for wrong password", !bad.auth_success);
        check("C: no SessionGrant issued on wrong password", bad.grant_id == 0);

        server.join();

        db::Result after = db.execute(
            "SELECT COUNT(*) FROM session_grant WHERE account_id = ?",
            {db::Param{static_cast<std::int64_t>(account_id)}});
        std::uint64_t grants_after = cell_u64(after.rows.at(0)[0]);
        check("C: wrong password wrote NO new grant row",
              grants_after == grants_before);

        // --- Cleanup: our grant(s) + realm (CASCADE) + account. ---------------
        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
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

    std::printf(g_fail == 0 ? "\nALL AUTHD LOGIN TESTS PASSED\n"
                            : "\n%d AUTHD LOGIN TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

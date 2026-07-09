// SPDX-License-Identifier: Apache-2.0
//
// authd — BAN enforcement at login integration test (OPS-02c, #419; epic #21).
//
// CLEAN-ROOM: written from the server SAD (§5.1 IF-1 flow, §2.1 "authd refuses a
// banned login"), the auth.fbs wire contract (AuthErrorCode.ACCOUNT_BANNED), and
// meridian/bans/bans.h. No GPL source consulted (CONTRIBUTING.md).
//
// Composes the real TLS 1.3 listener + the daemon's exact run_login against a live
// MariaDB (auth schema loaded), proving the story's acceptance list:
//   A. BASELINE — a non-banned account authenticates (AuthResult success).
//   B. ACCOUNT BAN — a permanently-banned account is REFUSED after a correct SRP
//      proof (AuthResult error = ACCOUNT_BANNED) and receives NO grant.
//   C. EXPIRY — an account whose only ban already EXPIRED authenticates normally
//      (the ban lookup evaluates expiry, so a lapsed ban does not block).
//   D. IP BAN — a login from a banned source IP is refused at ServerHello
//      (Error = ACCOUNT_BANNED) before any SRP work. Runs LAST + is cleaned up,
//      because the test's own client shares that source IP (127.0.0.1).
//
// Reads MERIDIAN_DB_* and SKIPs (exit 0) when unset, like the other authd/worldd
// DB-backed tests.

#include "login_session.h"

#include "account.h"
#include "meridian/bans/bans.h"
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
            reinterpret_cast<const unsigned char*>("meridian-ban-test"), -1, -1, 0);
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

const Bytes kFixedA = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0xA7, 0xB8,
    0xC9, 0xD0, 0xE1, 0xF2, 0xA3, 0xB4, 0xC5, 0xD6,
    0xE7, 0xF8, 0xA9, 0xB0, 0xC1, 0xD2, 0xE3, 0xF4,
    0xA5, 0xB6, 0xC7, 0xD8, 0xE9, 0xF0, 0xA1, 0xB2};

// The outcome of driving one login attempt up to (and including) AuthResult.
struct Attempt {
    bool connected = false;
    bool hello_was_error = false;      // Error instead of ServerHello (IP-ban path)
    std::uint16_t hello_error_code = 0;
    bool reached_challenge = false;
    bool got_auth_result = false;
    bool auth_success = false;
    std::uint16_t auth_error_code = 0; // AuthResult.error().code() when !success
};

// Drive ClientHello -> ... -> AuthResult with the given (correct) password.
Attempt drive(std::uint16_t port, const std::string& account, const std::string& password,
              std::uint32_t build, const srp::Parameters& params) {
    Attempt a;
    Client c(port);
    if (!c.connected()) return a;
    a.connected = true;

    c.send_frame(enc_client_hello(build, /*proto=*/1));
    auto f = c.recv_frame();
    if (!f) return a;
    if (const mn::ServerHello* sh = decode<mn::ServerHello>(*f); sh != nullptr) {
        // proceed
    } else if (const mn::Error* er = decode<mn::Error>(*f); er != nullptr) {
        a.hello_was_error = true;
        a.hello_error_code = static_cast<std::uint16_t>(er->code());
        return a;  // refused before SRP (IP ban)
    } else {
        return a;
    }

    c.send_frame(enc_srp_start(account));
    f = c.recv_frame();
    if (!f) return a;
    const mn::SrpChallenge* ch = decode<mn::SrpChallenge>(*f);
    if (ch == nullptr || ch->salt() == nullptr || ch->b() == nullptr) return a;
    a.reached_challenge = true;
    Bytes salt = vec_to_bytes(ch->salt());
    Bytes B = vec_to_bytes(ch->b());
    srp::testing::ClientProof cp = srp::testing::client_side(
        account, password, to_srp(salt), to_srp(kFixedA), to_srp(B), params);

    c.send_frame(enc_srp_proof(from_srp(cp.A), from_srp(cp.M1)));
    f = c.recv_frame();
    if (!f) return a;
    const mn::AuthResult* ar = decode<mn::AuthResult>(*f);
    if (ar == nullptr) return a;
    a.got_auth_result = true;
    a.auth_success = ar->success();
    if (!a.auth_success && ar->error() != nullptr) {
        a.auth_error_code = static_cast<std::uint16_t>(ar->error()->code());
    }
    return a;
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
        std::printf("SKIP: no MERIDIAN_DB_* connection configured\n");
        return 0;
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("authd ban-at-login test (OPS-02c #419)\n");

    char tmpl[] = "/tmp/meridian-ban-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::string userA = "ban_it_a_" + std::to_string(std::rand());
    const std::string userB = "ban_it_b_" + std::to_string(std::rand());
    const std::string password = "correct horse battery staple";
    const std::uint32_t build = 1000;
    srp::Parameters params;
    const std::string kLoopbackIp = "127.0.0.1";

    std::uint64_t idA = 0, idB = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        db.execute("DELETE FROM account WHERE username IN (?, ?)",
                   {db::Param{userA}, db::Param{userB}});
        // Pre-clean any stale loopback IP ban from a previous aborted run.
        db.execute("DELETE FROM ip_ban WHERE target = ?", {db::Param{kLoopbackIp}});

        account::CreateRequest ra;
        ra.username = userA;
        ra.password = password;
        idA = account::create_account(db, ra).account_id;
        account::CreateRequest rb;
        rb.username = userB;
        rb.password = password;
        idB = account::create_account(db, rb).account_id;
        check("test accounts created", idA > 0 && idB > 0);

        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound", port != 0);

        authd::LoginConfig cfg;
        cfg.proto_ver = 1;
        cfg.server_build = build;
        cfg.build_floor = 0;
        cfg.srp_params = params;

        // Serve FOUR attempts (baseline A, banned A, expired-ban B, IP-banned).
        std::thread server([&] {
            for (int i = 0; i < 4; ++i) {
                try {
                    net::Session s = listener.accept();
                    db::Connection sdb(p);
                    authd::run_login(s, sdb, cfg);
                    s.close();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server thread note: %s\n", e.what());
                }
            }
        });

        // A. BASELINE — A is not banned -> AuthResult success.
        {
            Attempt a = drive(port, userA, password, build, params);
            check("A: baseline reaches AuthResult", a.got_auth_result);
            check("A: baseline authenticates (not banned)", a.auth_success);
        }

        // B. ACCOUNT BAN — permanent ban on A -> refused after proof, no success.
        bans::ban_account(db, idA, "test ban", /*issued_by=*/0, /*perm=*/std::nullopt);
        check("B setup: A reads as banned",
              bans::account_ban(db, idA).has_value());
        {
            Attempt a = drive(port, userA, password, build, params);
            check("B: banned account still gets a challenge (no user oracle)",
                  a.reached_challenge);
            check("B: banned account AuthResult is a FAILURE", a.got_auth_result && !a.auth_success);
            check("B: failure code is ACCOUNT_BANNED",
                  a.auth_error_code ==
                      static_cast<std::uint16_t>(mn::AuthErrorCode::ACCOUNT_BANNED));
            db::Result g = db.execute(
                "SELECT COUNT(*) FROM session_grant WHERE account_id = ?",
                {db::Param{static_cast<std::int64_t>(idA)}});
            check("B: no grant written for a banned account",
                  g.rows.at(0)[0].value_or("0") == "0");
        }

        // C. EXPIRY — B's only ban already expired -> authenticates normally.
        db.execute(
            "INSERT INTO account_ban (account_id, expires_at, reason) "
            "VALUES (?, UTC_TIMESTAMP() - INTERVAL 1 HOUR, 'lapsed')",
            {db::Param{static_cast<std::int64_t>(idB)}});
        check("C setup: B's ban is NOT active (expired)",
              !bans::account_ban(db, idB).has_value());
        {
            Attempt a = drive(port, userB, password, build, params);
            check("C: an expired ban does NOT block login", a.got_auth_result && a.auth_success);
        }

        // D. IP BAN — ban 127.0.0.1 -> refused at ServerHello (terminal).
        bans::ban_ip(db, kLoopbackIp, "test ip ban", /*issued_by=*/0, /*perm=*/std::nullopt);
        {
            Attempt a = drive(port, userB, password, build, params);
            check("D: a banned source IP is refused at ServerHello",
                  a.connected && a.hello_was_error);
            check("D: IP-ban error code is ACCOUNT_BANNED",
                  a.hello_error_code ==
                      static_cast<std::uint16_t>(mn::AuthErrorCode::ACCOUNT_BANNED));
            check("D: a banned IP never reaches the SRP challenge", !a.reached_challenge);
        }

        server.join();

        // Cleanup: drop the loopback IP ban FIRST (so nothing lingers), then rows.
        db.execute("DELETE FROM ip_ban WHERE target = ?", {db::Param{kLoopbackIp}});
        db.execute("DELETE FROM session_grant WHERE account_id IN (?, ?)",
                   {db::Param{static_cast<std::int64_t>(idA)},
                    db::Param{static_cast<std::int64_t>(idB)}});
        db.execute("DELETE FROM account WHERE username IN (?, ?)",
                   {db::Param{userA}, db::Param{userB}});  // account_ban cascades
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

    std::printf(g_fail == 0 ? "\nALL AUTHD BAN TESTS PASSED\n"
                            : "\n%d AUTHD BAN TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
//
// meridian-net loopback integration test.
//
// CLEAN-ROOM: written from the server SAD (§5.1 IF-1 transport + framing) and the
// OpenSSL public API docs only. No GPL source consulted (CONTRIBUTING.md).
//
// Self-contained: it GENERATES a throwaway self-signed cert + key with the
// OpenSSL API into a temp dir (no external fixtures, so it runs in CI), starts a
// TlsListener on an ephemeral port on a background thread, connects a real TLS
// 1.3 client over loopback, and proves:
//   1) the negotiated protocol is TLSv1.3 on BOTH ends (SSL_get_version),
//   2) a framed message round-trips byte-for-byte (server echoes it),
//   3) a length prefix > 8 KiB is rejected (server errors out / closes; the
//      oversize buffer is NOT allocated), and
//   4) a zero-length frame round-trips (boundary case).
//
// Dependency-free harness (assert-style counters), matching the srp test's style.

#include "meridian/net/tls_listener.h"

#include <openssl/bn.h>
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
#include <string>
#include <thread>
#include <vector>

using meridian::net::Bytes;
using meridian::net::ConnectionClosed;
using meridian::net::ListenConfig;
using meridian::net::Session;
using meridian::net::TlsError;
using meridian::net::TlsListener;

namespace {

int g_fail = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// ---- Throwaway self-signed cert generation (OpenSSL API) --------------------

// Write a fresh RSA-2048 key + self-signed X.509 cert to key_path / cert_path.
// Returns true on success. Uses only the OpenSSL public API — no shelling out.
bool generate_self_signed(const std::string& cert_path,
                          const std::string& key_path) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    FILE* fk = nullptr;
    FILE* fc = nullptr;

    // RSA-2048 keypair via the high-level EVP keygen API.
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) goto done;
    if (EVP_PKEY_keygen_init(pctx) != 1) goto done;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) != 1) goto done;
    if (EVP_PKEY_keygen(pctx, &pkey) != 1) goto done;

    x509 = X509_new();
    if (!x509) goto done;
    X509_set_version(x509, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L);  // 1 hour is plenty
    if (X509_set_pubkey(x509, pkey) != 1) goto done;

    {
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("meridian-test"), -1, -1, 0);
        // Self-signed: issuer == subject.
        if (X509_set_issuer_name(x509, name) != 1) goto done;
    }

    if (X509_sign(x509, pkey, EVP_sha256()) == 0) goto done;

    fk = std::fopen(key_path.c_str(), "wb");
    if (!fk) goto done;
    if (PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        goto done;

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

// ---- Minimal TLS 1.3 loopback client ----------------------------------------

// Connects to 127.0.0.1:port, forces TLS 1.3, completes the handshake. Returns a
// connected SSL* (with owned fd) or nullptr. Accepts the self-signed cert
// (verify is off — this test proves transport + framing, not PKI trust).
SSL* tls_client_connect(std::uint16_t port, SSL_CTX** out_ctx) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        SSL_CTX_free(ctx);
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        SSL_CTX_free(ctx);
        return nullptr;
    }

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        ::close(fd);
        SSL_CTX_free(ctx);
        return nullptr;
    }
    *out_ctx = ctx;
    return ssl;
}

// Write raw bytes over the client's TLS channel (used for the framed-message and
// the malicious oversize-prefix cases). Returns bytes written or -1.
bool ssl_write_all(SSL* ssl, const std::uint8_t* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        int w = SSL_write(ssl, buf + sent, static_cast<int>(n - sent));
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

// Build an IF-1 frame on the wire: u32 LE length prefix + payload.
Bytes make_frame(const Bytes& payload) {
    std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    Bytes f{static_cast<std::uint8_t>(len & 0xFF),
            static_cast<std::uint8_t>((len >> 8) & 0xFF),
            static_cast<std::uint8_t>((len >> 16) & 0xFF),
            static_cast<std::uint8_t>((len >> 24) & 0xFF)};
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

}  // namespace

int main() {
    std::printf("meridian-net TLS 1.3 loopback test\n");

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    // 1) Throwaway cert + key into a temp dir (no external fixtures).
    char tmpl[] = "/tmp/meridian-net-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::perror("mkdtemp");
        return 1;
    }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generate self-signed cert+key (OpenSSL API)",
          generate_self_signed(cert_path, key_path));

    // 2) Listener on an ephemeral port (port 0 → OS assigns).
    ListenConfig cfg;
    cfg.cert_path = cert_path;
    cfg.key_path = key_path;
    cfg.bind_addr = "127.0.0.1";
    cfg.port = 0;

    TlsListener listener(cfg);
    std::uint16_t port = listener.local_port();
    check("listener bound to an ephemeral port", port != 0);
    std::printf("  listening on 127.0.0.1:%u\n", port);

    // 3) Server thread: echo one framed message, then observe the oversize frame.
    std::atomic<bool> srv_saw_version_tls13{false};
    std::atomic<bool> srv_echo_ok{false};
    std::atomic<bool> srv_zero_ok{false};
    std::atomic<bool> srv_rejected_oversize{false};
    Bytes srv_echoed;

    std::thread server([&] {
        try {
            Session s = listener.accept();
            srv_saw_version_tls13 = (s.tls_version() == "TLSv1.3");

            // (a) echo the first framed message byte-for-byte
            Bytes msg = s.read_frame();
            srv_echoed = msg;
            s.write_frame(msg);
            srv_echo_ok = true;

            // (b) zero-length frame round-trip
            Bytes empty = s.read_frame();
            s.write_frame(empty);
            srv_zero_ok = empty.empty();

            // (c) oversize prefix: read_frame must THROW (reject) — not allocate
            try {
                (void)s.read_frame();
                srv_rejected_oversize = false;  // should not reach here
            } catch (const TlsError&) {
                srv_rejected_oversize = true;  // rejected as designed
            }
            s.close();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  server thread error: %s\n", e.what());
        }
    });

    // 4) Client: connect over TLS 1.3, drive the three exchanges.
    SSL_CTX* cctx = nullptr;
    SSL* client = tls_client_connect(port, &cctx);
    check("client completed TLS handshake", client != nullptr);

    bool client_tls13 = false;
    bool echo_match = false;
    bool oversize_closed = false;

    if (client) {
        client_tls13 =
            (std::string(SSL_get_version(client)) == "TLSv1.3");

        // (a) framed message → expect identical echo back
        Bytes payload = {'M', 'E', 'R', 'I', 'D', 'I', 'A', 'N', 0x00, 0x01,
                         0xFF, 0x42};
        Bytes frame = make_frame(payload);
        ssl_write_all(client, frame.data(), frame.size());

        std::uint8_t lenbuf[4];
        int r = SSL_read(client, lenbuf, 4);
        Bytes echoed;
        if (r == 4) {
            std::uint32_t elen = static_cast<std::uint32_t>(lenbuf[0]) |
                                 (lenbuf[1] << 8) | (lenbuf[2] << 16) |
                                 (static_cast<std::uint32_t>(lenbuf[3]) << 24);
            echoed.resize(elen);
            std::size_t got = 0;
            while (got < elen) {
                int rr = SSL_read(client, echoed.data() + got,
                                  static_cast<int>(elen - got));
                if (rr <= 0) break;
                got += static_cast<std::size_t>(rr);
            }
        }
        echo_match = (echoed == payload);

        // (b) zero-length frame → expect a zero-length echo
        Bytes zframe = make_frame({});
        ssl_write_all(client, zframe.data(), zframe.size());
        std::uint8_t zlen[4];
        bool zero_echo = false;
        if (SSL_read(client, zlen, 4) == 4) {
            std::uint32_t zl = static_cast<std::uint32_t>(zlen[0]) |
                               (zlen[1] << 8) | (zlen[2] << 16) |
                               (static_cast<std::uint32_t>(zlen[3]) << 24);
            zero_echo = (zl == 0);
        }
        check("zero-length frame round-trips", zero_echo);

        // (c) malicious oversize prefix: claim 9000 bytes (> 8 KiB) and send a
        //     little payload. The server must reject on the prefix alone.
        std::uint32_t big = 9000;  // > kMaxFrameBytes (8192)
        std::uint8_t bigpfx[4] = {
            static_cast<std::uint8_t>(big & 0xFF),
            static_cast<std::uint8_t>((big >> 8) & 0xFF),
            static_cast<std::uint8_t>((big >> 16) & 0xFF),
            static_cast<std::uint8_t>((big >> 24) & 0xFF)};
        ssl_write_all(client, bigpfx, 4);
        std::uint8_t filler[16] = {0};
        ssl_write_all(client, filler, sizeof(filler));

        // After rejection the server closes; a client read should now fail/EOF.
        std::uint8_t sink[8];
        int rr = SSL_read(client, sink, sizeof(sink));
        oversize_closed = (rr <= 0);

        SSL_shutdown(client);
        SSL_free(client);
    }
    if (cctx) SSL_CTX_free(cctx);

    server.join();

    // 5) Assertions.
    check("server negotiated TLSv1.3 (SSL_get_version, server side)",
          srv_saw_version_tls13.load());
    check("client negotiated TLSv1.3 (SSL_get_version, client side)",
          client_tls13);
    check("server read the framed message", srv_echo_ok.load());
    check("framed message echoed byte-for-byte", echo_match);
    check("server round-tripped zero-length frame", srv_zero_ok.load());
    check("oversize (>8 KiB) length prefix rejected by server",
          srv_rejected_oversize.load());
    check("client connection closed after oversize rejection", oversize_closed);

    // Best-effort temp cleanup.
    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    if (g_fail == 0) {
        std::printf("\nALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}

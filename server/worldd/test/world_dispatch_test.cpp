// SPDX-License-Identifier: Apache-2.0
//
// worldd end-to-end IF-2 dispatch integration test (IT-M0 world path; #82/#83).
//
// CLEAN-ROOM: written from the server SAD (§2.5 worldd structure, §5.2 IF-2
// framing + Opcode registry, §6 concurrency model), the world.fbs wire contract,
// and the OpenSSL public API docs only. No GPL source consulted (CONTRIBUTING).
//
// Self-contained (no DB, no external service): it generates a throwaway
// self-signed cert, stands the REAL meridian::net TLS 1.3 listener + the REAL
// worldd serve/dispatch loop on an ephemeral port, and drives a TLS 1.3 client
// that speaks IF-2 length framing. It therefore always runs in the plain server
// `ctest` (unlike the DB-gated authd test).
//
// What it proves end-to-end:
//   A. FRAME CODEC round-trips (u16 opcode ‖ u64 seq ‖ FlatBuffer payload).
//   B. WORLD_HELLO routes to the WORLD_HELLO handler — asserted by an observable
//      side effect (a test-installed handler records the decoded grant_id) AND by
//      the connection staying open (a routed/handled frame is not disconnected).
//   C. CLOCK_SYNC routes to its handler and the server echoes both timestamps.
//   D. An UNKNOWN opcode -> server sends Disconnect{PROTOCOL_MISMATCH} and closes.
//   E. A RESERVED-range opcode (0x3xxx combat, not implemented) -> Disconnect +
//      close, cleanly (distinct reason detail, same reject policy).
//   F. The world-thread queue actually reaches the world thread (drained_count).

#include "world_dispatch.h"

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
            reinterpret_cast<const unsigned char*>("meridian-worldd-test"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-2 client -------------------------------------------
// Speaks the net-layer length framing (u32 LE prefix + payload). The payload we
// put inside is the IF-2 in-frame body (opcode ‖ seq ‖ FlatBuffer), matching
// mw::encode_frame — the test builds those with mw::encode_frame directly.
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

    // Read one net frame; nullopt on clean EOF / error (used to detect the
    // server closing the connection after a Disconnect).
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

// ---- world.fbs payload builders (client side) -------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    // nonce + proof left empty at M0 (grant validation is #84); the table still
    // verifies as WorldHello, which is all the dispatcher checks here.
    auto h = mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0);
    b.Finish(h);
    return bytes_of(b);
}
Bytes enc_clock_sync(std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateClockSync(b, client_time_ms, /*server_time_ms=*/0));
    return bytes_of(b);
}
Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y,
                          float z, std::uint64_t client_time_ms) {
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

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd IF-2 dispatch test (IT-M0 world path, #82/#83)\n");

    // --- A. Frame codec round-trip (pure, no socket). -------------------------
    {
        Bytes payload = {0xDE, 0xAD, 0xBE, 0xEF};
        Bytes frame = mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, 0x0102030405060708ULL,
                                       payload);
        std::optional<mw::Frame> dec = mw::decode_frame(frame);
        check("A: frame decodes", dec.has_value());
        if (dec) {
            check("A: opcode round-trips", dec->opcode == mn::Opcode::MOVEMENT_INTENT);
            check("A: seq round-trips", dec->seq == 0x0102030405060708ULL);
            check("A: payload round-trips",
                  dec->payload_len == payload.size() &&
                      std::memcmp(dec->payload, payload.data(), payload.size()) == 0);
        }
        // A frame shorter than the IF-2 header is malformed.
        Bytes tiny = {0x01, 0x00};  // 2 bytes < 10-byte header
        check("A: short frame -> no decode", !mw::decode_frame(tiny).has_value());
        // is_reserved_range: 0x1xxx movement + 0x3xxx combat (M1) are live; the
        // still-unimplemented domains (quest 0x4xxx, …) remain reserved.
        check("A: 0x1001 not reserved", !mw::is_reserved_range(0x1001));
        check("A: 0x4001 (quest) is reserved", mw::is_reserved_range(0x4001));
    }

    // Self-signed cert into a temp dir.
    char tmpl[] = "/tmp/meridian-worldd-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    try {
        // --- Stand up the real TLS listener + the world serve/dispatch loop. --
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);
        std::printf("  worldd dispatch listener on 127.0.0.1:%u\n", port);

        // Dispatcher with a TEST-OVERRIDE WORLD_HELLO handler that records the
        // decoded grant_id (observable routing side effect) AND enqueues a
        // WorldEvent to the world thread (exercises the IO-worker -> world-thread
        // queue). The default stub only logs; this proves the frame reached the
        // right table AND the queue seam works.
        mw::Dispatcher dispatcher;
        std::atomic<std::uint64_t> seen_hello_grant{0};
        std::atomic<int> hello_calls{0};

        mw::WorldServer world(dispatcher, mw::WorldServerConfig{});
        world.start();

        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [&](net::Session& sess, const mw::Frame& f, mw::ConnCtx& ctx) {
                          (void)sess;
                          (void)ctx;
                          const auto* h = fb::GetRoot<mn::WorldHello>(f.payload);
                          if (h) seen_hello_grant.store(h->grant_id());
                          hello_calls.fetch_add(1);
                          // Route the frame into the simulation via the queue,
                          // rather than touching game state on this IO worker.
                          mw::WorldEvent ev;
                          ev.opcode = f.opcode;
                          ev.seq = f.seq;
                          ev.payload.assign(f.payload, f.payload + f.payload_len);
                          world.enqueue(std::move(ev));
                      });

        // Server: serve exactly THREE connections (happy path, unknown opcode,
        // reserved opcode), each on the world serve loop.
        // Four connections: B+C (hello+clock), D (unknown), E (reserved), and
        // G (#86 — MOVEMENT_INTENT before handshake -> Disconnect).
        std::thread server([&] {
            for (int i = 0; i < 4; ++i) {
                try {
                    net::Session s = listener.accept();
                    world.serve_connection(std::move(s));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server thread error: %s\n", e.what());
                }
            }
        });

        // ===== B + C. HAPPY PATH: WORLD_HELLO routes, CLOCK_SYNC echoes =======
        {
            Client c(port);
            check("B: client connected", c.connected());
            check("B: TLS 1.3 negotiated", c.is_tls13());

            const std::uint64_t grant = 0xABCDEF0123456789ULL;
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, /*seq=*/1,
                                          enc_world_hello(grant, /*build=*/1000)));

            // CLOCK_SYNC after the hello — its echo is the observable proof that
            // the connection stayed open (WORLD_HELLO was handled, not rejected).
            const std::uint64_t client_time = 123456;
            c.send_frame(mw::encode_frame(mn::Opcode::CLOCK_SYNC, /*seq=*/2,
                                          enc_clock_sync(client_time)));
            std::optional<Bytes> reply = c.recv_frame();
            check("C: server replied to CLOCK_SYNC", reply.has_value());
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                check("C: reply is an IF-2 frame", rf.has_value());
                if (rf) {
                    check("C: reply opcode is CLOCK_SYNC",
                          rf->opcode == mn::Opcode::CLOCK_SYNC);
                    check("C: reply seq echoes request", rf->seq == 2);
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const mn::ClockSync* cs = decode<mn::ClockSync>(pl);
                    check("C: reply payload verifies as ClockSync", cs != nullptr);
                    if (cs) {
                        check("C: client_time_ms echoed",
                              cs->client_time_ms() == client_time);
                        check("C: server_time_ms filled", cs->server_time_ms() != 0);
                    }
                }
            }
            // The WORLD_HELLO handler ran with the right grant_id (routing proof).
            check("B: WORLD_HELLO handler invoked", hello_calls.load() == 1);
            check("B: WORLD_HELLO routed with correct grant_id",
                  seen_hello_grant.load() == grant);
        }  // client closes -> server's serve_connection returns

        // ===== D. UNKNOWN opcode -> Disconnect + close ========================
        {
            Client c(port);
            check("D: client connected", c.connected());
            // 0x00FF is in the live session/system domain (0x0xxx) but is not a
            // defined opcode — a wholly unknown value.
            Bytes junk = {0x01, 0x02, 0x03, 0x04};  // not a valid FlatBuffer either
            c.send_frame(mw::encode_frame(static_cast<mn::Opcode>(0x00FF), /*seq=*/7,
                                          junk));
            std::optional<Bytes> reply = c.recv_frame();
            check("D: server replied before closing", reply.has_value());
            bool got_disconnect = false;
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                if (rf && rf->opcode == mn::Opcode::DISCONNECT) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const mn::Disconnect* d = decode<mn::Disconnect>(pl);
                    got_disconnect = (d != nullptr);
                }
            }
            check("D: server sent a Disconnect for unknown opcode", got_disconnect);
            // After the Disconnect the server closes: the next read hits EOF.
            check("D: connection closed after Disconnect",
                  !c.recv_frame().has_value());
        }

        // ===== E. RESERVED-range opcode (0x4xxx quest) -> Disconnect + close ==
        // Combat 0x3xxx went live with CMB-01 (#344/#345), so this probes a domain
        // that is still declared-but-unimplemented (quest 0x4xxx) — the reserved-
        // range reject policy is identical.
        {
            Client c(port);
            check("E: client connected", c.connected());
            Bytes junk = {0x00};
            c.send_frame(mw::encode_frame(static_cast<mn::Opcode>(0x4001), /*seq=*/9,
                                          junk));
            std::optional<Bytes> reply = c.recv_frame();
            bool got_disconnect = false;
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                if (rf && rf->opcode == mn::Opcode::DISCONNECT) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    got_disconnect = (decode<mn::Disconnect>(pl) != nullptr);
                }
            }
            check("E: server sent a Disconnect for reserved opcode", got_disconnect);
            check("E: connection closed after Disconnect",
                  !c.recv_frame().has_value());
        }

        // ===== G. MOVEMENT_INTENT before HandshakeOk -> Disconnect + close ====
        // The DEFAULT MOVEMENT_INTENT handler (#86 — NOT the WORLD_HELLO override
        // above) enforces "movement only after auth" (SAD §5.5): an intent on an
        // unauthenticated connection is a protocol error. This runs DB-free (no
        // grant/session is established), so it lives in the plain-ctest dispatch
        // test rather than the DB-gated session test — it proves the auth-gate on
        // the real serve loop end-to-end.
        {
            Client c(port);
            check("G: client connected", c.connected());
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, /*seq=*/11,
                                          enc_movement_intent(1, /*flags=*/2, -320.0f, -320.0f,
                                                              0.0f, /*client_time_ms=*/1000)));
            std::optional<Bytes> reply = c.recv_frame();
            bool got_disconnect = false;
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                if (rf && rf->opcode == mn::Opcode::DISCONNECT) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    got_disconnect = (decode<mn::Disconnect>(pl) != nullptr);
                }
            }
            check("G: pre-handshake movement -> Disconnect", got_disconnect);
            check("G: connection closed after Disconnect",
                  !c.recv_frame().has_value());
        }

        server.join();

        // ===== F. The world-thread queue actually reached the world thread ====
        // The happy-path WORLD_HELLO handler enqueued exactly one WorldEvent.
        // Give the world thread a moment to drain, then assert it saw it.
        for (int i = 0; i < 100 && world.drained_count() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check("F: world thread drained the enqueued event",
              world.drained_count() >= 1);

        world.stop();
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD DISPATCH TESTS PASSED\n"
                            : "\n%d WORLDD DISPATCH TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
//
// worldd — combat ability-use WIRE round-trip integration test (CMB-01 #344,
// D-10). Proves the D-10 accept/reject path end-to-end over the REAL TLS listener
// + world serve/dispatch loop: a CAST_REQUEST is answered with CastStart (ACCEPT)
// or CastFailed (REJECT) within one round-trip, so the client's optimistic
// GCD/cast confirms or rolls back within one RTT (server SAD §3.3; client SAD
// §2.2/§3c).
//
// CLEAN-ROOM: written from the server SAD (§2.5/§3.3), the client SAD (§2.2/§3c),
// the world.fbs wire contract, and the OpenSSL public API only. No GPL source
// consulted (CONTRIBUTING.md).
//
// Self-contained (no DB): a test-installed WORLD_HELLO handler promotes the
// connection IN-WORLD (spawns a Player in the shared WorldState) without a grant
// DB, so the CAST_REQUEST handler runs against a real spawned Unit. It therefore
// always runs in the plain server `ctest` (like worldd-dispatch-test). The
// DB-backed grant/enter-world path is proven by the session/char-mgmt jobs; here
// we isolate the COMBAT wire contract.

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

#include <cstdio>
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

// ---- Throwaway self-signed cert (OpenSSL API; mirrors the dispatch test) -----
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
            reinterpret_cast<const unsigned char*>("meridian-combat-test"), -1, -1, 0);
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

Bytes enc_cast_request(std::uint32_t ability_id, std::uint64_t target_guid,
                       std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastRequest(b, ability_id, target_guid, client_time_ms));
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
    std::printf("worldd combat ability-use WIRE round-trip test (CMB-01 #344, D-10)\n");

    // --- Wiring proof (pure): a default Dispatcher routes CAST_REQUEST. --------
    {
        mw::Dispatcher d;
        check("CAST_REQUEST has a registered handler",
              d.has_handler(mn::Opcode::CAST_REQUEST));
        check("0x3001 is the combat range", mw::is_reserved_range(0x3001));
    }

    char tmpl[] = "/tmp/meridian-combat-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    try {
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);

        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, mw::WorldServerConfig{});
        world.start();

        // TEST WORLD_HELLO: promote the connection IN-WORLD without a grant DB —
        // spawn a Player (class 2 = mana user) in the shared WorldState so the
        // CAST_REQUEST handler has a real caster Unit. No egress is wired, so the
        // handler's replies fall back to a direct socket write (send_s2c), which
        // the client reads.
        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [](net::Session& /*sess*/, const mw::Frame& /*f*/, mw::ConnCtx& ctx) {
                          ctx.authenticated = true;
                          ctx.account_id = 1;
                          ctx.phase = mw::SessionPhase::kInWorld;
                          mw::Position spawn;
                          spawn.x = 64.0f;
                          spawn.y = 64.0f;
                          spawn.z = 0.0f;
                          ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
                          if (ctx.world != nullptr) {
                              mw::EntityIdentity id;
                              id.entity_guid = 0;   // synthetic guid assigned by enter()
                              id.type_id = 2;
                              id.char_class = 2;    // mana-using class (has resource)
                              mw::EnterResult er = ctx.world->enter(
                                  id, spawn,
                                  [](mn::Opcode, const std::vector<std::uint8_t>&) {
                                      return true;  // no other sessions -> never called
                                  });
                              ctx.slot = er.slot;
                              ctx.entered = true;
                              ctx.movement->set_entity_guid(er.entity_guid);
                          }
                      });

        std::thread server([&] {
            try {
                net::Session s = listener.accept();
                world.serve_connection(std::move(s));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  server thread error: %s\n", e.what());
            }
        });

        {
            Client c(port);
            check("client connected", c.connected());

            // Promote in-world.
            fb::FlatBufferBuilder hb;
            hb.Finish(mn::CreateWorldHello(hb, /*grant_id=*/1, /*build=*/1, 0, 0));
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, /*seq=*/1, bytes_of(hb)));

            // --- REJECT 1: unknown ability id -> CastFailed{UNKNOWN_ABILITY}. ---
            c.send_frame(mw::encode_frame(mn::Opcode::CAST_REQUEST, /*seq=*/2,
                                          enc_cast_request(0xDEADBEEFu, /*target=*/0,
                                                           /*client_time=*/1000)));
            {
                std::optional<Bytes> reply = c.recv_frame();
                check("R1: server replied to unknown-ability cast", reply.has_value());
                std::optional<mw::Frame> rf = reply ? mw::decode_frame(*reply) : std::nullopt;
                check("R1: reply is CastFailed",
                      rf && rf->opcode == mn::Opcode::CAST_FAILED);
                if (rf) {
                    check("R1: seq echoes request", rf->seq == 2);
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const mn::CastFailed* cf = decode<mn::CastFailed>(pl);
                    check("R1: reason UNKNOWN_ABILITY",
                          cf && cf->reason() == mn::CastFailReason::UNKNOWN_ABILITY);
                }
            }

            // --- REJECT 2: melee (ENEMY) on self -> CastFailed{WRONG_FACTION}. --
            // Self is a Player (friendly), not a legal enemy target.
            c.send_frame(mw::encode_frame(
                mn::Opcode::CAST_REQUEST, /*seq=*/3,
                enc_cast_request(mw::kPlaceholderMeleeStrikeId, /*target=*/0, 1100)));
            {
                std::optional<Bytes> reply = c.recv_frame();
                check("R2: server replied to melee-on-self cast", reply.has_value());
                std::optional<mw::Frame> rf = reply ? mw::decode_frame(*reply) : std::nullopt;
                check("R2: reply is CastFailed",
                      rf && rf->opcode == mn::Opcode::CAST_FAILED);
                if (rf) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const mn::CastFailed* cf = decode<mn::CastFailed>(pl);
                    check("R2: reason WRONG_FACTION",
                          cf && cf->reason() == mn::CastFailReason::WRONG_FACTION);
                }
            }

            // --- ACCEPT: heal (FRIENDLY) on self -> CastStart{cast_ms=2000}. ----
            c.send_frame(mw::encode_frame(
                mn::Opcode::CAST_REQUEST, /*seq=*/4,
                enc_cast_request(mw::kPlaceholderHealId, /*target=*/0, 1200)));
            {
                std::optional<Bytes> reply = c.recv_frame();
                check("A: server replied to self-heal cast", reply.has_value());
                std::optional<mw::Frame> rf = reply ? mw::decode_frame(*reply) : std::nullopt;
                check("A: reply is CastStart (ACCEPT)",
                      rf && rf->opcode == mn::Opcode::CAST_START);
                if (rf) {
                    check("A: seq echoes request", rf->seq == 4);
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const mn::CastStart* cs = decode<mn::CastStart>(pl);
                    check("A: CastStart cast_ms = heal cast time (2000, cast-time)",
                          cs && cs->cast_ms() == 2000 &&
                              cs->ability_id() == mw::kPlaceholderHealId);
                }
            }
        }  // client closes

        server.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "combat wire test exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD COMBAT-WIRE TESTS PASSED\n"
                            : "\n%d WORLDD COMBAT-WIRE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

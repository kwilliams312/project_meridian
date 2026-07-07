// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — /metrics HTTP endpoint integration test (OPS-05).
//
// Self-contained (loopback only, no DB): stands the real Exposer on an ephemeral
// 127.0.0.1 port, drives a raw HTTP/1.1 GET over a BSD socket, and asserts:
//   1. a 200 response with the Prometheus exposition Content-Type,
//   2. the body is valid exposition text carrying the EXPECTED catalog metric
//      names (the dashboard contract),
//   3. a metric MOVES when its event fires — bump a catalog counter, re-scrape,
//      and assert the rendered value increased.
// Runs DB-free in the `server` CI job's ctest.

#include "meridian/metrics/catalog.h"
#include "meridian/metrics/exposer.h"
#include "meridian/metrics/registry.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace meridian::metrics;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Do one HTTP/1.1 GET / to 127.0.0.1:port and return the full raw response
// (head + body), or "" on a connect/IO failure.
std::string http_get(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
    const char* req = "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, req, std::strlen(req), 0);

    std::string resp;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return resp;
}

// Split an HTTP response into (status_line+headers, body).
std::string body_of(const std::string& resp) {
    std::size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? "" : resp.substr(p + 4);
}

}  // namespace

int main() {
    // A private registry for the test so it does not race the process-global one.
    Registry reg;
    // Seed a couple of catalog-shaped families directly on this registry so the
    // exposition has known names to assert (mirrors what the daemons register on
    // the default registry).
    auto& ccu = reg.gauge("meridian_ccu", "Concurrent connected users",
                          {"realm", "zone", "shard"});
    ccu.with({"reference", "0", "0"}).set(7.0);
    auto& opcode = reg.counter("meridian_opcode_total", "Per-opcode message rate",
                               {"realm", "zone", "shard", "opcode"});
    opcode.with({"reference", "0", "0", "WORLD_HELLO"}).inc();

    ExposerConfig cfg;
    cfg.port = 0;                 // ephemeral
    cfg.bind_addr = "127.0.0.1";
    Exposer exposer(cfg, reg);
    exposer.start();
    std::uint16_t port = exposer.port();
    CHECK(port != 0);

    // --- 1 + 2: scrape returns 200 + valid exposition with catalog names -----
    std::string resp = http_get(port);
    CHECK(!resp.empty());
    CHECK(resp.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(has(resp, "Content-Type: text/plain; version=0.0.4"));

    std::string body = body_of(resp);
    CHECK(has(body, "# TYPE meridian_ccu gauge"));
    CHECK(has(body, "meridian_ccu{realm=\"reference\",zone=\"0\",shard=\"0\"} 7"));
    CHECK(has(body, "# TYPE meridian_opcode_total counter"));
    CHECK(has(body,
              "meridian_opcode_total{realm=\"reference\",zone=\"0\",shard=\"0\",opcode=\"WORLD_HELLO\"} 1"));

    // --- 3: a metric MOVES when its event fires ------------------------------
    // Fire two more "opcodes", re-scrape, assert the series went 1 -> 3.
    opcode.with({"reference", "0", "0", "WORLD_HELLO"}).inc();
    opcode.with({"reference", "0", "0", "WORLD_HELLO"}).inc();
    std::string body2 = body_of(http_get(port));
    CHECK(has(body2,
              "meridian_opcode_total{realm=\"reference\",zone=\"0\",shard=\"0\",opcode=\"WORLD_HELLO\"} 3"));

    exposer.stop();
    // stop() is idempotent.
    exposer.stop();

    std::printf("meridian-metrics-exposer-test: OK (%d checks, port %u)\n", g_checks, port);
    return 0;
}

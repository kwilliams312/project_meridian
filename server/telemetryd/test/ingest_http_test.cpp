// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — IN-PROCESS ingest HTTP endpoint test (OPS-05 / D-29; issue #167).
//
// Self-contained (loopback only, no DB): stands the real IngestServer on an
// ephemeral 127.0.0.1 port, drives raw HTTP/1.1 POSTs over a BSD socket, and
// asserts the full round-trip. Test matrix (issue "TEST"):
//   (a) a valid #168 batch → 200 accepted + FORWARDED (the sink ostream got it),
//   (b) malformed / oversized → 4xx, no crash,
//   (c) a PII-shaped field → 422 rejected,
//   (d) rate-limit: a flood → 429 after the cap.
// Runs DB-free in the `server` CI job's ctest.

#include "ingest_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using namespace meridian::telemetryd;

namespace {

int g_fail = 0;
void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

const char* kSdkHeader = "{\"sdk\":{\"name\":\"meridian.client.telemetry\",\"version\":\"1\"}}\n";
const char* kItemHeader = "{\"type\":\"event\",\"content_type\":\"application/json\"}\n";

std::string one_event_envelope() {
    std::string env = kSdkHeader;
    env += kItemHeader;
    env += "{\"level\":\"error\",\"message\":\"connect failed\",\"timestamp\":1720000000000,"
           "\"logger\":\"net\",\"tags\":{\"session_id\":\"sess-1\",\"build\":\"client-0.1\","
           "\"platform\":\"macos-arm64\"}}\n";
    return env;
}

// POST `body` to path on 127.0.0.1:port and return the raw HTTP response ("" on
// connect/IO failure).
std::string http_post(std::uint16_t port, const std::string& path, const std::string& body) {
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
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    req += "Content-Type: application/x-sentry-envelope\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    ::send(fd, req.data(), req.size(), 0);

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

// Send a POST with a LYING Content-Length header far above the cap WITHOUT a real
// body — proves the endpoint refuses on the header alone (never reads the body).
std::string http_post_oversized_header(std::uint16_t port, const std::string& path) {
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
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    req += "Content-Length: 999999999\r\nConnection: close\r\n\r\n";  // ~1 GB claimed
    ::send(fd, req.data(), req.size(), 0);
    std::string resp;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return resp;
}

}  // namespace

int main() {
    std::ostringstream sink;  // the forwarded-log sink (stdout in the daemon)

    IngestServerConfig cfg;
    cfg.port = 0;  // ephemeral — avoids colliding with any real service/test
    cfg.bind_addr = "127.0.0.1";
    cfg.ingest_path = "/api/1/store/";
    cfg.realm_label = "reference";
    cfg.rate_limit.max_requests = 3;   // tiny cap so the flood test is quick
    cfg.rate_limit.window_ms = 60'000;

    IngestServer server(cfg, sink, /*registry=*/nullptr);
    server.start();
    std::uint16_t port = server.port();
    check("bound to an ephemeral port", port != 0);

    // ── (a) valid #168 batch → 200 accepted + forwarded ──────────────────────
    std::printf("[a] valid #168 batch → 200 + forwarded to sink\n");
    {
        std::string resp = http_post(port, "/api/1/store/", one_event_envelope());
        check("got a response", !resp.empty());
        check("200 OK", resp.rfind("HTTP/1.1 200", 0) == 0);
        check("body says accepted", contains(resp, "\"status\":\"accepted\""));
        // The forwarded event landed in the sink (Loki-compatible JSON line).
        std::string forwarded = sink.str();
        check("sink received a line", !forwarded.empty());
        check("sink line is client_log_ingest", contains(forwarded, "\"event\":\"client_log_ingest\""));
        check("sink line has the build", contains(forwarded, "\"build\":\"client-0.1\""));
        check("sink line has the message", contains(forwarded, "connect failed"));
        check("sink line has NO email/PII", !contains(forwarded, "email"));
    }

    // ── (b) malformed → 400, no crash ────────────────────────────────────────
    std::printf("[b] malformed / oversized → 4xx, no crash\n");
    {
        std::string resp = http_post(port, "/api/1/store/", "not an envelope\n");
        check("malformed → 400", resp.rfind("HTTP/1.1 400", 0) == 0);

        // Oversized via a lying Content-Length header — refused on the header.
        std::string big = http_post_oversized_header(port, "/api/1/store/");
        check("oversized header → 413", big.rfind("HTTP/1.1 413", 0) == 0);

        // Wrong method + wrong path.
        // (Reuse http_post but to a bogus path.)
        std::string wrong_path = http_post(port, "/nope", one_event_envelope());
        check("unknown path → 404", wrong_path.rfind("HTTP/1.1 404", 0) == 0);
    }

    // ── (c) PII-shaped field → 422 rejected ──────────────────────────────────
    std::printf("[c] a PII-shaped field → 422 rejected\n");
    {
        std::string pii = kSdkHeader;
        pii += kItemHeader;
        pii += "{\"level\":\"error\",\"message\":\"m\",\"timestamp\":1,"
               "\"tags\":{\"session_id\":\"s\",\"build\":\"b\",\"platform\":\"p\","
               "\"user_email\":\"a@b.com\"}}\n";
        std::string resp = http_post(port, "/api/1/store/", pii);
        check("PII field → 422", resp.rfind("HTTP/1.1 422", 0) == 0);
        check("422 reason mentions PII", contains(resp, "PII"));
    }

    // ── (d) rate-limit: a flood → 429 after the cap ──────────────────────────
    // Fresh server with a tiny cap keyed on this test's single loopback IP.
    std::printf("[d] flood → 429 after the cap\n");
    {
        std::ostringstream sink2;
        IngestServerConfig fc;
        fc.port = 0;
        fc.bind_addr = "127.0.0.1";
        fc.ingest_path = "/api/1/store/";
        fc.rate_limit.max_requests = 3;
        fc.rate_limit.window_ms = 60'000;  // wide window so the flood stays in it
        IngestServer flood_server(fc, sink2, nullptr);
        flood_server.start();
        std::uint16_t fport = flood_server.port();

        int ok_count = 0, throttled = 0;
        for (int i = 0; i < 10; ++i) {
            std::string resp = http_post(fport, "/api/1/store/", one_event_envelope());
            if (resp.rfind("HTTP/1.1 200", 0) == 0) ++ok_count;
            else if (resp.rfind("HTTP/1.1 429", 0) == 0) ++throttled;
        }
        check("exactly cap requests accepted", ok_count == 3);
        check("the flood was throttled (429)", throttled == 7);
        flood_server.stop();
    }

    server.stop();
    server.stop();  // idempotent

    if (g_fail == 0) {
        std::printf("telemetryd-http-test: OK (all checks passed, ingest port %u)\n", port);
        return 0;
    }
    std::printf("telemetryd-http-test: FAILED (%d checks)\n", g_fail);
    return 1;
}

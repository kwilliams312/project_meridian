// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — ingest HTTP/1.1 server implementation (OPS-05 / D-29; #167).
// See ingest_http.h for the design + clean-room statement. Socket/responder
// STYLE borrowed from meridian::metrics::Exposer (server/libs/metrics), extended
// to read a bounded POST body.

#include "ingest_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#include "meridian/metrics/catalog.h"
#include "meridian/metrics/registry.h"

namespace meridian::telemetryd {

struct IngestServer::Impl {
    int listen_fd = -1;
    std::uint16_t bound_port = 0;
};

namespace {

// Current wall-clock in epoch ms (the rate limiter's `now`).
std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Send a minimal HTTP/1.1 response with a JSON body (Sentry-ish {"status":...}).
void send_json(int fd, const std::string& status, const std::string& body) {
    std::string resp;
    resp.reserve(body.size() + 128);
    resp += "HTTP/1.1 " + status + "\r\n";
    resp += "Content-Type: application/json\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    write_all(fd, resp.data(), resp.size());
}

// A tiny case-insensitive header lookup over the raw request head.
// Returns the header value (trimmed) or "" if absent.
std::string header_value(const std::string& head, const std::string& name) {
    std::string lname = name;
    std::transform(lname.begin(), lname.end(), lname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while (pos < head.size()) {
        std::size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string line = head.substr(pos, eol - pos);
        pos = eol + 2;
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == lname) {
            std::string val = line.substr(colon + 1);
            std::size_t b = val.find_first_not_of(" \t");
            std::size_t e = val.find_last_not_of(" \t");
            if (b == std::string::npos) return "";
            return val.substr(b, e - b + 1);
        }
    }
    return "";
}

// Parse the request line: method + path. Returns false if there is no complete
// request line yet.
bool parse_request_line(const std::string& head, std::string& method, std::string& path) {
    std::size_t eol = head.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string line = head.substr(0, eol);
    std::size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    method = line.substr(0, sp1);
    path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    return true;
}

// Map a RejectReason to an HTTP status line. PII / unknown-tag are 422
// (Unprocessable Entity — well-formed but violates the contract); size is 413;
// everything else malformed/contract is 400.
std::string status_for(RejectReason r) {
    switch (r) {
        case RejectReason::kTooLarge:   return "413 Payload Too Large";
        case RejectReason::kPiiField:
        case RejectReason::kUnknownTag: return "422 Unprocessable Entity";
        default:                        return "400 Bad Request";
    }
}

}  // namespace

IngestServer::IngestServer(const IngestServerConfig& cfg, std::ostream& sink,
                           meridian::metrics::Registry* registry)
    : cfg_(cfg),
      sink_(sink),
      registry_(registry),
      rate_limiter_(cfg.rate_limit),
      impl_(std::make_unique<Impl>()) {}

IngestServer::~IngestServer() { stop(); }

void IngestServer::start() {
    if (running_.exchange(true)) return;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        running_.store(false);
        throw std::runtime_error(std::string("telemetryd ingest: socket() failed: ") +
                                 std::strerror(errno));
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.bind_addr.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        running_.store(false);
        throw std::runtime_error("telemetryd ingest: bad bind address '" + cfg_.bind_addr + "'");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string err = std::strerror(errno);
        ::close(fd);
        running_.store(false);
        throw std::runtime_error("telemetryd ingest: bind(" + cfg_.bind_addr + ":" +
                                 std::to_string(cfg_.port) + ") failed: " + err);
    }
    if (::listen(fd, cfg_.backlog) != 0) {
        std::string err = std::strerror(errno);
        ::close(fd);
        running_.store(false);
        throw std::runtime_error("telemetryd ingest: listen() failed: " + err);
    }

    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        impl_->bound_port = ntohs(bound.sin_port);
    } else {
        impl_->bound_port = cfg_.port;
    }

    impl_->listen_fd = fd;
    thread_ = std::thread([this] { run(); });
}

void IngestServer::stop() {
    if (!running_.exchange(false)) return;
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (thread_.joinable()) thread_.join();
}

std::uint16_t IngestServer::port() const { return impl_->bound_port; }

void IngestServer::run() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int client = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client < 0) {
            if (!running_.load()) break;
            if (errno == EINTR) continue;
            break;
        }
        char ipbuf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
        handle_client(client, ipbuf[0] ? std::string(ipbuf) : std::string("0.0.0.0"));
        ::close(client);
    }
}

void IngestServer::handle_client(int client_fd, const std::string& peer_ip) {
    // --- Read the request head (up to CRLFCRLF), header-flood guarded. --------
    std::string buf;
    char rbuf[4096];
    std::size_t header_end = std::string::npos;
    for (int i = 0; i < 32; ++i) {
        ssize_t n = ::recv(client_fd, rbuf, sizeof(rbuf), 0);
        if (n <= 0) break;
        buf.append(rbuf, static_cast<std::size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (buf.size() > 16384) {  // header flood guard
            send_json(client_fd, "431 Request Header Fields Too Large",
                      "{\"status\":\"error\",\"reason\":\"headers too large\"}");
            return;
        }
    }
    if (header_end == std::string::npos) {
        send_json(client_fd, "400 Bad Request", "{\"status\":\"error\",\"reason\":\"no request\"}");
        return;
    }

    const std::string head = buf.substr(0, header_end + 2);  // include trailing CRLF
    std::string method, path;
    if (!parse_request_line(head, method, path)) {
        send_json(client_fd, "400 Bad Request", "{\"status\":\"error\",\"reason\":\"bad request line\"}");
        return;
    }

    // Strip a query string from the path for the match.
    std::string match_path = path;
    if (std::size_t q = match_path.find('?'); q != std::string::npos) {
        match_path = match_path.substr(0, q);
    }

    if (method != "POST") {
        send_json(client_fd, "405 Method Not Allowed",
                  "{\"status\":\"error\",\"reason\":\"only POST supported\"}");
        return;
    }
    if (match_path != cfg_.ingest_path) {
        send_json(client_fd, "404 Not Found",
                  "{\"status\":\"error\",\"reason\":\"unknown ingest path\"}");
        return;
    }

    // --- Content-Length: cap BEFORE allocating / reading the body (#173). -----
    const std::string cl_str = header_value(head, "Content-Length");
    if (cl_str.empty()) {
        send_json(client_fd, "411 Length Required",
                  "{\"status\":\"error\",\"reason\":\"Content-Length required\"}");
        return;
    }
    std::uint64_t content_length = 0;
    for (char c : cl_str) {
        if (c < '0' || c > '9') {
            send_json(client_fd, "400 Bad Request",
                      "{\"status\":\"error\",\"reason\":\"bad Content-Length\"}");
            return;
        }
        content_length = content_length * 10 + static_cast<std::uint64_t>(c - '0');
        if (content_length > kMaxEnvelopeBytes) break;  // stop accumulating early
    }
    if (content_length > kMaxEnvelopeBytes) {
        // Refuse WITHOUT reading the (oversized) body — the exhaustion guard.
        send_json(client_fd, "413 Payload Too Large",
                  "{\"status\":\"error\",\"reason\":\"payload too large\"}");
        return;
    }

    // --- Read exactly content_length bytes of body. ---------------------------
    std::string body = buf.substr(header_end + 4);  // whatever already arrived
    body.reserve(static_cast<std::size_t>(content_length));
    while (body.size() < content_length) {
        ssize_t n = ::recv(client_fd, rbuf, sizeof(rbuf), 0);
        if (n <= 0) break;
        std::size_t want = static_cast<std::size_t>(content_length) - body.size();
        body.append(rbuf, std::min(static_cast<std::size_t>(n), want));
    }

    // --- Rate limit per (build, IP). We need the build to key on, which lives
    // inside the envelope; parse first, then rate-limit on the parsed build so a
    // storm of malformed bodies is still bounded (they key on build="" + IP). ---
    IngestResult result = parse_and_validate(body);

    const std::string build = (result.accepted() && result.batch)
                                  ? result.batch->context.build
                                  : std::string();
    if (!rate_limiter_.allow(build, peer_ip, now_ms())) {
        send_json(client_fd, "429 Too Many Requests",
                  "{\"status\":\"error\",\"reason\":\"rate limited\"}");
        return;
    }

    if (!result.accepted()) {
        send_json(client_fd, status_for(result.reason),
                  std::string("{\"status\":\"error\",\"reason\":\"") +
                      reject_reason_str(result.reason) + "\"}");
        return;
    }

    // --- Forward each validated event to the sink + bump metrics. -------------
    const ParsedBatch& batch = *result.batch;
    for (const ParsedEvent& ev : batch.events) {
        sink_ << forward_event_json(cfg_.realm_label, batch.context, ev) << '\n';
        if (registry_ != nullptr && ev.kind == EventKind::kLog) {
            // meridian_client_log_ingest_total{realm,severity,build,platform}
            registry_
                ->counter(
                    "meridian_client_log_ingest_total",
                    "Client ERROR/CRITICAL log events received (count only, no payload)",
                    {"realm", "severity", "build", "platform"})
                .with({cfg_.realm_label, severity_str(ev.severity), batch.context.build,
                       batch.context.platform})
                .inc();
        }
    }
    sink_.flush();

    send_json(client_fd, "200 OK",
              std::string("{\"status\":\"accepted\",\"events\":") +
                  std::to_string(batch.events.size()) + "}");
}

}  // namespace meridian::telemetryd

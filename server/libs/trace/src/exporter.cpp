// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — the async, batching OTLP/HTTP span exporter + the hand-
// rolled HTTP/1.1 POST client. See include/meridian/trace/exporter.h for the
// design + clean-room statement.

#include "meridian/trace/exporter.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "meridian/trace/otlp.h"

namespace meridian::trace {

// ---------------------------------------------------------------------------
// Endpoint normalization
// ---------------------------------------------------------------------------

std::string traces_url_for(const std::string& endpoint) {
    if (endpoint.empty()) return "";
    // Already a full traces URL?
    const std::string suffix = "/v1/traces";
    if (endpoint.size() >= suffix.size() &&
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return endpoint;
    }
    // Strip one trailing slash then append the traces path.
    std::string base = endpoint;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + suffix;
}

namespace {

// Parse "http://host:port/path" into (host, port, path). Returns false if it is
// not a plain-http URL we can dial. Defaults: port 80, path "/". Only http:// is
// supported (the collector is on the private network — see the header note).
bool parse_http_url(const std::string& url, std::string& host, std::uint16_t& port,
                    std::string& path) {
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = url.substr(scheme.size());
    std::size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (authority.empty()) return false;

    std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        host = authority;
        port = 80;
    } else {
        host = authority.substr(0, colon);
        long p = std::strtol(authority.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<std::uint16_t>(p);
    }
    return !host.empty();
}

void set_socket_timeout(int fd, std::uint32_t timeout_ms) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

}  // namespace

// ---------------------------------------------------------------------------
// HTTP/1.1 POST client (RFC 7230 message framing over BSD sockets)
// ---------------------------------------------------------------------------

bool http_post_otlp(const std::string& traces_url, const std::string& body,
                    std::uint32_t timeout_ms) {
    std::string host, path;
    std::uint16_t port = 0;
    if (!parse_http_url(traces_url, host, port, path)) return false;

    // Resolve host (numeric IP or DNS name — the collector is a compose service
    // name like "otel-collector", so DNS resolution is required).
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        return false;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return false;
    }
    set_socket_timeout(fd, timeout_ms);

    bool ok = false;
    if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        std::string req;
        req.reserve(body.size() + 256);
        req += "POST " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + port_str + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        req += "\r\n";
        req += body;

        if (write_all(fd, req.data(), req.size())) {
            // Read just enough of the response to read the status line.
            char buf[512];
            ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                // Expect "HTTP/1.1 2xx ...". A 2xx is success.
                std::string status(buf, static_cast<std::size_t>(n));
                std::size_t sp = status.find(' ');
                if (sp != std::string::npos && sp + 3 <= status.size()) {
                    ok = (status[sp + 1] == '2');
                }
            }
        }
    }

    ::close(fd);
    ::freeaddrinfo(res);
    return ok;
}

// ---------------------------------------------------------------------------
// Exporter
// ---------------------------------------------------------------------------

Exporter::Exporter(ExporterConfig cfg) : cfg_(std::move(cfg)) {
    active_ = !cfg_.endpoint.empty();
    if (active_) {
        // Default sink: POST over the built-in HTTP client to the resolved URL.
        const std::string url = traces_url_for(cfg_.endpoint);
        const std::uint32_t to = cfg_.timeout_ms;
        sink_ = [url, to](const std::string& body) { return http_post_otlp(url, body, to); };
    }
}

Exporter::Exporter(ExporterConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {
    // An injected sink makes the exporter active regardless of endpoint (test seam).
    active_ = static_cast<bool>(sink_);
}

Exporter::~Exporter() { stop(); }

void Exporter::start() {
    if (!active_) return;                     // no-op exporter: nothing to run
    if (running_.exchange(true)) return;      // already started
    stop_requested_ = false;
    thread_ = std::thread([this] { run(); });
}

void Exporter::export_span(Span span) {
    accepted_.fetch_add(1, std::memory_order_relaxed);
    if (!active_) {
        // No-op exporter: drop (graceful degradation — no collector wired).
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (span.end_unix_nano == 0) span.end();  // stamp end if the caller forgot
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.size() >= cfg_.max_queue) {
            // Backpressure: drop rather than grow unbounded (best-effort telemetry).
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(std::move(span));
    }
    cv_.notify_one();
}

std::size_t Exporter::deliver_batch(std::vector<Span>& batch) {
    if (batch.empty()) return 0;
    std::size_t delivered = 0;
    // Split into POSTs of at most cfg_.max_batch spans.
    std::size_t off = 0;
    while (off < batch.size()) {
        const std::size_t n = std::min(cfg_.max_batch, batch.size() - off);
        std::vector<Span> chunk(batch.begin() + static_cast<std::ptrdiff_t>(off),
                                batch.begin() + static_cast<std::ptrdiff_t>(off + n));
        const std::string body =
            spans_to_otlp_json(chunk, cfg_.service_name, cfg_.realm);
        if (sink_ && sink_(body)) {
            delivered += n;
            exported_.fetch_add(n, std::memory_order_relaxed);
        }
        // A failed POST drops this chunk (best-effort; never blocks the daemon).
        off += n;
    }
    return delivered;
}

void Exporter::run() {
    using namespace std::chrono;
    const auto interval = milliseconds(cfg_.flush_interval_ms);
    for (;;) {
        std::vector<Span> batch;
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, interval, [this] {
                return stop_requested_ || !queue_.empty();
            });
            batch.swap(queue_);
            stopping = stop_requested_;
        }
        // Deliver whatever we drained this pass (a final non-empty batch on stop
        // is still POSTed, so no enqueued span is silently lost).
        deliver_batch(batch);
        if (stopping) break;
    }
}

void Exporter::flush(std::uint32_t timeout_ms) {
    if (!active_) return;
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    for (;;) {
        cv_.notify_one();  // wake the worker to drain now, not at the next tick
        bool empty;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            empty = queue_.empty();
        }
        // Empty queue means the worker has swapped the batch out; give it a brief
        // moment to finish the POST (deliver_batch runs off-lock), then return.
        if (empty) {
            std::this_thread::sleep_for(milliseconds(5));
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty()) return;
        }
        if (steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(milliseconds(2));
    }
}

void Exporter::stop() {
    if (!running_.exchange(false)) return;  // never started / already stopped
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

}  // namespace meridian::trace

// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — /metrics HTTP exposer implementation.
// See include/meridian/metrics/exposer.h for the design + clean-room statement.

#include "meridian/metrics/exposer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace meridian::metrics {

struct Exposer::Impl {
    int listen_fd = -1;
    std::uint16_t bound_port = 0;
};

namespace {

// The Prometheus text exposition Content-Type (format version 0.0.4).
constexpr const char* kContentType = "text/plain; version=0.0.4; charset=utf-8";

// Read a full HTTP request head (up to the CRLFCRLF terminator) OR give up after
// a small cap — we only need the request line to decide GET vs. other. We do not
// parse a body (scrape requests have none). Returns the bytes read (may be less
// than a full head if the peer is slow; that is fine, we only sniff the verb).
std::string read_request_head(int fd) {
    std::string head;
    char buf[1024];
    // One read is enough in practice for a scrape's request line; loop a few
    // times defensively but cap total so a slow/hostile peer cannot block us.
    for (int i = 0; i < 8; ++i) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        head.append(buf, static_cast<std::size_t>(n));
        if (head.find("\r\n\r\n") != std::string::npos) break;
        if (head.size() > 8192) break;  // header flood guard
    }
    return head;
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

void send_response(int fd, const std::string& status, const std::string& content_type,
                   const std::string& body) {
    std::string resp;
    resp.reserve(body.size() + 128);
    resp += "HTTP/1.1 " + status + "\r\n";
    resp += "Content-Type: " + content_type + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    write_all(fd, resp.data(), resp.size());
}

}  // namespace

Exposer::Exposer(const ExposerConfig& cfg, Registry& registry)
    : cfg_(cfg), registry_(registry), impl_(std::make_unique<Impl>()) {}

Exposer::~Exposer() { stop(); }

void Exposer::start() {
    if (running_.exchange(true)) return;  // already started

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        running_.store(false);
        throw std::runtime_error(std::string("metrics exposer: socket() failed: ") +
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
        throw std::runtime_error("metrics exposer: bad bind address '" + cfg_.bind_addr + "'");
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string err = std::strerror(errno);
        ::close(fd);
        running_.store(false);
        throw std::runtime_error("metrics exposer: bind(" + cfg_.bind_addr + ":" +
                                 std::to_string(cfg_.port) + ") failed: " + err);
    }
    if (::listen(fd, cfg_.backlog) != 0) {
        std::string err = std::strerror(errno);
        ::close(fd);
        running_.store(false);
        throw std::runtime_error("metrics exposer: listen() failed: " + err);
    }

    // Resolve the actual bound port (for cfg_.port == 0 ephemeral).
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

void Exposer::stop() {
    if (!running_.exchange(false)) return;  // already stopped / never started
    // Close the listening socket to unblock accept().
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (thread_.joinable()) thread_.join();
}

std::uint16_t Exposer::port() const { return impl_->bound_port; }

void Exposer::run() {
    while (running_.load()) {
        int client = ::accept(impl_->listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (!running_.load()) break;      // stop() closed the listener
            if (errno == EINTR) continue;
            break;                             // listener gone / fatal
        }

        std::string head = read_request_head(client);
        // Sniff the method: everything before the first space on the request line.
        const bool is_get = head.rfind("GET ", 0) == 0;
        const bool is_head = head.rfind("HEAD ", 0) == 0;

        if (is_get || is_head) {
            std::string body = registry_.render();
            if (is_head) body.clear();
            send_response(client, "200 OK", kContentType, body);
        } else {
            send_response(client, "405 Method Not Allowed", "text/plain",
                          "only GET/HEAD supported\n");
        }

        ::close(client);
    }
}

}  // namespace meridian::metrics

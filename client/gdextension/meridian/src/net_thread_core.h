// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free NET-THREAD core (issue #97). Owns the client's
// IF-2 world session on its OWN std::thread and is the client's single seam to the
// worldd socket: the socket is created, read, and written ONLY on the net thread.
// The main thread never touches it — it communicates purely through the lock-free
// SPSC rings (spsc_ring.h):
//
//   * the net thread DECODES inbound IF-2 frames off the main thread and enqueues
//     InboundMessages into the inbound ring (net_thread_messages.h);
//   * the main thread enqueues fully-encoded outbound frames into per-priority
//     rings; the net thread drains them in priority order and writes them.
//
// ENGINE-FREE (Client SAD §9.2): no Godot types — the thin Godot binding is
// meridian_net_thread.* (a RefCounted whose _process/pre-sim hook calls
// drain_inbound()). Because the transport is injected via a factory (ConnectFn),
// the whole threaded session is unit-tested against a MOCK clientnet::ITransport
// that replays the IF-2 sequence — the same login->world->move flow the headless
// bot (client/bot/bot_core.cpp run_world_session) drives synchronously, here run on
// the dedicated thread.
//
// USES meridian-clientnet (the #95 net core) for ALL net logic: framing, the IF-2
// wire_frame codec, the FlatBuffers codec, and the AEAD WorldSession. This core
// adds only the THREADING + the cross-thread queues; it reimplements no net logic.
//
// M0 WIRE REALITY (matches the bot + worldd #84): the IF-2 body is PLAINTEXT on the
// wire (confidentiality is TLS at the auth channel; the world channel is plain TCP
// at M0). The AEAD WorldSession is still constructed + held so the seal/open seam is
// client-ready the instant worldd flips it on — exactly as run_world_session does.

#ifndef MERIDIAN_NET_THREAD_CORE_H
#define MERIDIAN_NET_THREAD_CORE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "meridian/clientnet/transport.h"  // clientnet::ITransport
#include "net_thread_messages.h"
#include "send_priority_queue.h"
#include "spsc_ring.h"

namespace meridian::net {

// Inbound ring capacity: sized for a burst of a tick's worth of server frames (our
// MovementState + AoI relay) without dropping between the main thread's drains.
inline constexpr std::size_t kInboundRingCapacity = 1024;

// How many kBulk frames one outbound-drain pass sends before yielding back to the
// read path — caps bulk so it can never delay control/movement (see SendPriority).
inline constexpr int kBulkDrainBudget = 16;

// Tuning for the net thread's read loop.
struct NetThreadConfig {
    // The IF-2 WorldHello frame body (u16 opcode ‖ u64 seq ‖ WorldHello FB) the
    // thread sends as its FIRST frame — built by the login layer from the grant
    // (meridian_login.build_world_hello_frame / login::build_world_hello). Must be
    // non-empty; an empty frame fails start().
    Bytes world_hello_frame;

    // The 32-byte SessionGrant.session_key. Held to construct the AEAD WorldSession
    // (the plaintext-at-M0 seam). May be empty at M0 (the session is simply not
    // built); a wrong-length non-empty key leaves the session not-ok (logged, still
    // plaintext).
    Bytes session_key;

    // Per-poll read timeout (ms) for the non-blocking drain — long enough that a
    // promptly-sent frame is not mistaken for "nothing pending", short enough that
    // an idle poll returns quickly. Mirrors the bot's kDrainTimeoutMs.
    unsigned drain_timeout_ms = 40;

    // Blocking read timeout (ms) for the ONE handshake reply (HandshakeOk /
    // Disconnect) right after WorldHello. 0 = block indefinitely.
    unsigned handshake_timeout_ms = 5000;
};

// A factory yielding a FRESH, already-CONNECTED clientnet::ITransport to worldd. It
// runs ON THE NET THREAD (so the connect/DNS/socket work never blocks the main
// thread). Returns nullptr on connect failure -> the thread emits kConnectFailed.
// The CLI/binding returns a clientnet::TcpTransport(host, port); a test returns a
// mock. Ownership transfers to the core for the session's lifetime.
using ConnectFn = std::function<std::unique_ptr<meridian::clientnet::ITransport>()>;

// Owns the net thread + its SPSC rings. Not copyable. Destruction stops the thread.
class NetThreadCore {
public:
    NetThreadCore() = default;
    ~NetThreadCore();

    NetThreadCore(const NetThreadCore&) = delete;
    NetThreadCore& operator=(const NetThreadCore&) = delete;

    // Spawn the net thread: it calls `connect()`, sends the WorldHello, reads the
    // handshake reply, then runs the send/receive loop until stop() (or the peer
    // closes). Returns false if already running or `cfg.world_hello_frame` is empty.
    // start() itself does NO socket work — the connect happens on the new thread.
    bool start(ConnectFn connect, NetThreadConfig cfg);

    // Signal the thread to stop, join it, and close the transport. Idempotent; also
    // runs in the destructor. Safe to call from the main thread.
    void stop();

    // True between a successful start() and the thread actually exiting. The thread
    // may set this false on its own (peer close / connect fail) before stop().
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Whether the world handshake completed (HandshakeOk seen). Set by the thread;
    // read by the main thread for a quick "am I in-world yet?" check that does not
    // require draining the ring.
    bool entered_world() const { return entered_world_.load(std::memory_order_acquire); }

    // MAIN THREAD ONLY (the SPSC consumer of the inbound ring). Pop the next decoded
    // server event. Returns false if none pending. Call at the pre-sim sync point.
    bool try_pop_inbound(InboundMessage& out) { return inbound_.pop(out); }

    // MAIN THREAD ONLY (the SPSC producer of the outbound rings). Enqueue an already-
    // encoded IF-2 frame at `prio`. Returns false if that priority's ring is full
    // (caller decides drop/back-pressure). Never blocks; never touches the socket.
    bool send(Bytes frame, SendPriority prio);

    // --- Diagnostics / test counters (atomics; safe to read from any thread) -----
    std::uint64_t frames_sent() const { return frames_sent_.load(std::memory_order_relaxed); }
    std::uint64_t frames_received() const { return frames_received_.load(std::memory_order_relaxed); }
    std::uint64_t inbound_dropped() const { return inbound_dropped_.load(std::memory_order_relaxed); }
    std::uint64_t send_dropped() const { return send_dropped_.load(std::memory_order_relaxed); }

private:
    // The net thread's entry point.
    void run_loop(ConnectFn connect, NetThreadConfig cfg);

    // Drain the outbound priority rings onto `transport` (control, then movement,
    // then bounded bulk). Returns false if a write failed (peer closed).
    bool drain_outbound(meridian::clientnet::ITransport& transport);

    // Decode one received transport-frame payload into an InboundMessage and enqueue
    // it. Increments the drop counter (never blocks) if the inbound ring is full.
    void handle_inbound_frame(const Bytes& frame_payload);

    // Push an InboundMessage to the main thread, counting a drop if the ring is full.
    void publish(InboundMessage&& msg);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> entered_world_{false};

    // The ONE inbound ring (producer = net thread, consumer = main thread).
    SpscRing<InboundMessage, kInboundRingCapacity> inbound_;

    // The outbound side: one SPSC ring per priority, drained in priority order by the
    // net thread (producer = main thread, consumer = net thread).
    SendPriorityQueue outbound_;

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> inbound_dropped_{0};
    std::atomic<std::uint64_t> send_dropped_{0};
};

}  // namespace meridian::net

#endif  // MERIDIAN_NET_THREAD_CORE_H

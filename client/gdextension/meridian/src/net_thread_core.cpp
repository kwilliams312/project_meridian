// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free NET-THREAD core implementation (issue #97). Drives
// the CLIENT side of worldd's #84 handshake + #86 movement over the #95 net core on
// a dedicated std::thread, mirroring the headless bot's synchronous
// run_world_session (client/bot/bot_core.cpp) but with the main thread decoupled by
// the SPSC rings. Clean-room from the wire contracts + this repo's own bot/worldd as
// the interop reference; no GPL source consulted (CONTRIBUTING.md).

#include "net_thread_core.h"

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/wire_frame.h"
#include "meridian/clientnet/world_session.h"

namespace meridian::net {

namespace cn = meridian::clientnet;

NetThreadCore::~NetThreadCore() { stop(); }

bool NetThreadCore::start(ConnectFn connect, NetThreadConfig cfg) {
    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return false;  // already running
    }
    if (cfg.world_hello_frame.empty() || !connect) {
        return false;  // nothing to send / no way to connect
    }

    stop_requested_.store(false, std::memory_order_release);
    entered_world_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    thread_ = std::thread(&NetThreadCore::run_loop, this, std::move(connect), std::move(cfg));
    return true;
}

void NetThreadCore::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool NetThreadCore::send(Bytes frame, SendPriority prio) {
    if (!outbound_.push(std::move(frame), prio)) {
        send_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;  // ring full — caller decides back-pressure
    }
    return true;
}

void NetThreadCore::publish(InboundMessage&& msg) {
    if (!inbound_.push(std::move(msg))) {
        inbound_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool NetThreadCore::drain_outbound(cn::ITransport& transport) {
    // Strict priority (control, then movement, then bounded bulk) lives in the
    // SendPriorityQueue; here we only supply the socket write as the drain sink.
    std::uint64_t sent = 0;
    const bool ok = outbound_.drain(
        [&transport](const Bytes& frame) { return transport.send_frame(frame); },
        kBulkDrainBudget, &sent);
    if (sent > 0) {
        frames_sent_.fetch_add(sent, std::memory_order_relaxed);
    }
    return ok;  // false iff a write failed (peer closed)
}

void NetThreadCore::handle_inbound_frame(const Bytes& frame_payload) {
    std::optional<cn::WorldFrame> f = cn::decode_world_frame(frame_payload);
    if (!f) {
        return;  // malformed IF-2 frame — drop (a hostile/broken peer)
    }
    frames_received_.fetch_add(1, std::memory_order_relaxed);

    InboundMessage msg;
    msg.opcode = f->opcode;
    msg.seq = f->seq;

    switch (f->opcode) {
        case cn::kOpHandshakeOk: {
            msg.kind = InboundKind::kHandshakeOk;
            publish(std::move(msg));
            break;
        }
        case cn::kOpMovementState: {
            auto st = cn::codec::decode_movement_state(f->payload);
            if (!st) return;  // undecodable — drop
            msg.kind = InboundKind::kMovementState;
            msg.state = *st;
            publish(std::move(msg));
            break;
        }
        case cn::kOpDisconnect: {
            msg.kind = InboundKind::kDisconnect;
            if (auto d = cn::codec::decode_disconnect(f->payload)) {
                msg.disc = *d;
                msg.detail = d->message;
            } else {
                msg.detail = "server disconnect (undecodable)";
            }
            publish(std::move(msg));
            break;
        }
        // Character-select round-trips (D-35 / #286 / #341). Decoded here off the
        // main thread; an undecodable body is dropped (never GetRoot unverified).
        case cn::kOpCharListResp: {
            auto r = cn::codec::decode_char_list_response(f->payload);
            if (!r) return;
            msg.kind = InboundKind::kCharList;
            msg.roster = std::move(*r);
            publish(std::move(msg));
            break;
        }
        case cn::kOpCharCreateResp: {
            auto r = cn::codec::decode_char_create_response(f->payload);
            if (!r) return;
            msg.kind = InboundKind::kCharCreate;
            msg.char_create = *r;
            publish(std::move(msg));
            break;
        }
        case cn::kOpCharDeleteResp: {
            auto r = cn::codec::decode_char_delete_response(f->payload);
            if (!r) return;
            msg.kind = InboundKind::kCharDelete;
            msg.char_delete = *r;
            publish(std::move(msg));
            break;
        }
        case cn::kOpEnterWorldResp: {
            auto r = cn::codec::decode_enter_world_response(f->payload);
            if (!r) return;
            msg.kind = InboundKind::kEnterWorld;
            msg.enter_world = *r;
            // NOTE: entered_world_ tracks "handshake completed" (set on HandshakeOk =
            // char-select reached). TRUE in-world entry is signalled by this kEnterWorld
            // message (status OK) — the scene keys the world transition off that, not
            // off this quick-check flag, so we do not overload it here.
            publish(std::move(msg));
            break;
        }
        default: {
            // EntityEnter/Update/Leave, ClockSync, and any other S->C opcode not
            // specially decoded here are forwarded RAW (opcode/seq/payload) for the
            // main-thread sim layer to decode — nothing is silently lost.
            msg.kind = InboundKind::kEntityFrame;
            msg.payload = f->payload;
            publish(std::move(msg));
            break;
        }
    }
}

void NetThreadCore::run_loop(ConnectFn connect, NetThreadConfig cfg) {
    // --- 1. Connect ON the net thread (the main thread never blocks on the socket). -
    std::unique_ptr<cn::ITransport> transport = connect();
    if (!transport) {
        InboundMessage m;
        m.kind = InboundKind::kConnectFailed;
        m.detail = "worldd connect failed";
        publish(std::move(m));
        running_.store(false, std::memory_order_release);
        return;
    }

    // --- 2. Send WorldHello (opcode 0x0001, seq 0) — the FIRST IF-2 frame. --------
    if (!transport->send_frame(cfg.world_hello_frame)) {
        InboundMessage m;
        m.kind = InboundKind::kConnectFailed;
        m.detail = "failed to send WorldHello";
        publish(std::move(m));
        running_.store(false, std::memory_order_release);
        return;
    }
    frames_sent_.fetch_add(1, std::memory_order_relaxed);  // WorldHello is a frame

    // --- 3. Build + hold the AEAD session (mirror of worldd #84). Plaintext on the
    //        wire at M0; held so the seal/open seam is client-ready (see the bot). --
    cn::WorldSession session(cfg.session_key);
    (void)session;  // held for the seam; unused on the plaintext-at-M0 wire

    // --- 4. Read the ONE handshake reply (HandshakeOk / Disconnect), time-bounded. -
    transport->set_recv_timeout_ms(cfg.handshake_timeout_ms);
    {
        bool would_block = false;
        std::optional<cn::Bytes> reply = transport->recv_frame_nb(would_block);
        if (!reply) {
            InboundMessage m;
            m.kind = InboundKind::kConnectFailed;
            m.detail = would_block ? "no reply to WorldHello (timeout)"
                                   : "peer closed before HandshakeOk";
            publish(std::move(m));
            running_.store(false, std::memory_order_release);
            return;
        }
        std::optional<cn::WorldFrame> f = cn::decode_world_frame(*reply);
        if (!f) {
            InboundMessage m;
            m.kind = InboundKind::kConnectFailed;
            m.detail = "malformed reply to WorldHello";
            publish(std::move(m));
            running_.store(false, std::memory_order_release);
            return;
        }
        if (f->opcode == cn::kOpDisconnect) {
            // Grant reject / server close before entering the world. Route through
            // the shared decoder so it publishes kDisconnect and counts the frame
            // exactly once (do NOT pre-increment frames_received_ here).
            handle_inbound_frame(*reply);
            running_.store(false, std::memory_order_release);
            return;
        }
        frames_received_.fetch_add(1, std::memory_order_relaxed);  // the HandshakeOk
        if (f->opcode != cn::kOpHandshakeOk) {
            InboundMessage m;
            m.kind = InboundKind::kConnectFailed;
            m.opcode = f->opcode;
            m.detail = "unexpected opcode in reply to WorldHello";
            publish(std::move(m));
            running_.store(false, std::memory_order_release);
            return;
        }
        // Entered the world.
        entered_world_.store(true, std::memory_order_release);
        InboundMessage ok;
        ok.kind = InboundKind::kHandshakeOk;
        ok.opcode = f->opcode;
        ok.seq = f->seq;
        publish(std::move(ok));
    }

    // --- 5. Steady-state loop: drain outbound (priority order), then poll inbound. -
    transport->set_recv_timeout_ms(cfg.drain_timeout_ms);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (!drain_outbound(*transport)) {
            InboundMessage m;
            m.kind = InboundKind::kTransportClosed;
            m.detail = "peer closed during send";
            publish(std::move(m));
            break;
        }

        bool would_block = false;
        std::optional<cn::Bytes> in = transport->recv_frame_nb(would_block);
        if (in) {
            handle_inbound_frame(*in);
            continue;  // decode + loop back to drain any queued outbound promptly
        }
        if (would_block) {
            continue;  // idle poll timed out — loop (the read blocked ~drain_timeout)
        }
        // Neither a frame nor a timeout -> peer closed / socket error.
        InboundMessage m;
        m.kind = InboundKind::kTransportClosed;
        m.detail = "peer closed";
        publish(std::move(m));
        break;
    }

    // --- 6. Teardown: close the socket on THIS thread; publish nothing further. ----
    transport.reset();  // RAII close
    running_.store(false, std::memory_order_release);
}

}  // namespace meridian::net

// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — integration test for the dedicated NET-THREAD core (issue #97).
// Engine-free (no Godot): drives net::NetThreadCore against a MOCK clientnet::
// ITransport that replays the IF-2 sequence, exactly as the headless bot's
// run_world_session drives the real one — here across the thread boundary via the
// lock-free SPSC rings. Plain-main style, ctest-wired (client/gdextension/meridian/
// test/CMakeLists.txt), built only when the #95 net core (meridian-clientnet) is on.
//
// Proves the #97 deliverables:
//   1. LOGIN->WORLD->MOVE over the thread: the net thread sends WorldHello, the mock
//      replies HandshakeOk (drained on the main thread from the inbound ring), the
//      main thread queues a MovementIntent, the net thread writes it, the mock echoes
//      a MovementState, and the main thread drains it — same flow as the bot.
//   2. PRIORITY drain order: control-before-movement-before-bulk, bulk budget-capped
//      (deterministic, single-threaded, over SendPriorityQueue).
//   3. LIFECYCLE: a clean start/stop join, and the connect-failure path emits
//      kConnectFailed and does not hang.

#include "net_thread_core.h"
#include "send_priority_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/transport.h"
#include "meridian/clientnet/wire_frame.h"

namespace net = meridian::net;
namespace cn = meridian::clientnet;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}
static bool near(float a, float b, float eps = 1e-3f) { return (a - b < eps) && (b - a < eps); }

// ---------------------------------------------------------------------------
// A scripted mock worldd transport (touched ONLY by the net thread).
// ---------------------------------------------------------------------------
//
// On construction it queues a HandshakeOk as the first inbound frame. Whenever the
// net thread sends a MovementIntent, it enqueues a MovementState that ECHOES the
// intent's position (so the main thread can assert the round-trip), advancing the
// authoritative x by +1 each time so a "did it move?" check is meaningful.
class MockWorldTransport final : public cn::ITransport {
public:
	MockWorldTransport() {
		// HandshakeOk carries no body the net thread decodes (it checks the opcode).
		inbox_.push_back(cn::encode_world_frame(cn::kOpHandshakeOk, /*seq=*/0, {}));
	}

	bool send_frame(const cn::Bytes& payload) override {
		auto f = cn::decode_world_frame(payload);
		if (f && f->opcode == cn::kOpMovementIntent) {
			intents_received_.fetch_add(1, std::memory_order_relaxed);
			auto mi = cn::codec::decode_movement_intent(f->payload);
			cn::codec::MovementState st;
			st.entity_guid = 42;
			st.ack_seq = mi ? mi->seq : 0;
			st.state_flags = mi ? mi->state_flags : 0;
			st.x = 64.0f + static_cast<float>(intents_received_.load(std::memory_order_relaxed));
			st.y = 64.0f;
			st.z = 0.0f;
			st.orientation = mi ? mi->orientation : 0.0f;
			st.server_time_ms = mi ? mi->client_time_ms : 0;
			cn::Bytes body = cn::codec::encode_movement_state(st);
			inbox_.push_back(cn::encode_world_frame(cn::kOpMovementState, st.ack_seq, body));
		}
		return true;
	}

	std::optional<cn::Bytes> recv_frame() override {
		if (inbox_.empty()) return std::nullopt;
		cn::Bytes b = std::move(inbox_.front());
		inbox_.pop_front();
		return b;
	}

	std::optional<cn::Bytes> recv_frame_nb(bool& would_block) override {
		if (inbox_.empty()) {
			would_block = true;
			// Emulate a real socket's read timeout so the net thread does not hot-spin.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			return std::nullopt;
		}
		would_block = false;
		return recv_frame();
	}

	void set_recv_timeout_ms(unsigned) override {}

	std::uint64_t intents_received() const {
		return intents_received_.load(std::memory_order_relaxed);
	}

private:
	std::deque<cn::Bytes> inbox_;  // frames delivered to the client (net thread reads)
	std::atomic<std::uint64_t> intents_received_{0};
};

// A transport factory that fails to connect (returns nullptr).
static std::unique_ptr<cn::ITransport> connect_fail() { return nullptr; }

// Poll the inbound ring up to `timeout_ms` for a message of `kind`. Fills `out`.
static bool wait_for_inbound(net::NetThreadCore& core, net::InboundKind kind,
                             net::InboundMessage& out, unsigned timeout_ms) {
	const auto deadline =
	    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		net::InboundMessage msg;
		while (core.try_pop_inbound(msg)) {
			if (msg.kind == kind) {
				out = std::move(msg);
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

int main() {
	std::printf("NET THREAD CORE TESTS (#97)\n");

	// --- 1. login->world->move over the dedicated thread ---------------------
	{
		// The factory news the mock on the net thread and publishes its address so the
		// test can inspect it. (A move-capturing lambda can't live in std::function —
		// ConnectFn must be copyable — so we create-and-publish instead of move-in.)
		std::atomic<MockWorldTransport*> mock_ptr{nullptr};
		net::ConnectFn connect =
		    [&mock_ptr]() -> std::unique_ptr<cn::ITransport> {
			auto t = std::make_unique<MockWorldTransport>();
			mock_ptr.store(t.get(), std::memory_order_release);
			return t;
		};

		net::NetThreadCore core;
		net::NetThreadConfig cfg;
		cfg.world_hello_frame = cn::encode_world_frame(cn::kOpWorldHello, /*seq=*/0, {});
		cfg.drain_timeout_ms = 5;
		cfg.handshake_timeout_ms = 1000;

		check("start() spawns the net thread", core.start(std::move(connect), cfg));

		net::InboundMessage hs;
		check("HandshakeOk drained from the inbound ring (entered world)",
		      wait_for_inbound(core, net::InboundKind::kHandshakeOk, hs, 2000));
		check("entered_world() flips true after HandshakeOk", core.entered_world());

		// Main thread queues a MovementIntent; the net thread writes it; the mock
		// echoes a MovementState the main thread then drains.
		cn::codec::MovementIntent mi;
		mi.seq = 7;
		mi.state_flags = 0;
		mi.x = 64.0f;
		mi.y = 64.0f;
		mi.z = 0.0f;
		mi.client_time_ms = 123456;
		cn::Bytes intent_body = cn::codec::encode_movement_intent(mi);
		cn::Bytes intent_frame =
		    cn::encode_world_frame(cn::kOpMovementIntent, mi.seq, intent_body);
		check("send(MovementIntent, kMovement) queues onto the outbound ring",
		      core.send(intent_frame, net::SendPriority::kMovement));

		net::InboundMessage st;
		check("MovementState round-trip drained from the inbound ring",
		      wait_for_inbound(core, net::InboundKind::kMovementState, st, 2000));
		check("MovementState acks our intent seq", st.state.ack_seq == 7);
		check("MovementState carries the echoed authoritative position",
		      near(st.state.y, 64.0f) && st.state.x > 64.0f);
		MockWorldTransport* mock_raw = mock_ptr.load(std::memory_order_acquire);
		check("the mock actually received our MovementIntent",
		      mock_raw != nullptr && mock_raw->intents_received() >= 1);
		check("frames_sent counts WorldHello + MovementIntent", core.frames_sent() >= 2);
		check("frames_received counts HandshakeOk + MovementState",
		      core.frames_received() >= 2);
		check("no inbound messages were dropped", core.inbound_dropped() == 0);

		core.stop();
		check("running() is false after stop()", !core.running());
		// stop() is idempotent.
		core.stop();
		check("stop() is idempotent (no hang / crash on second call)", !core.running());
	}

	// --- 2. priority drain order (deterministic, single-threaded) ------------
	{
		net::SendPriorityQueue q;
		// Enqueue interleaved so ONLY the priority policy can produce the expected
		// order: bulk first, then movement, then control (reverse priority order).
		// Byte tags per class: control 0xC*, movement 0xA*, bulk 0xB*.
		auto tag = [](std::uint8_t t) { return net::Bytes{t}; };
		q.push(tag(0xB0), net::SendPriority::kBulk);
		q.push(tag(0xB1), net::SendPriority::kBulk);
		q.push(tag(0xA0), net::SendPriority::kMovement);
		q.push(tag(0xC0), net::SendPriority::kControl);
		q.push(tag(0xA1), net::SendPriority::kMovement);
		q.push(tag(0xC1), net::SendPriority::kControl);

		std::vector<std::uint8_t> order;
		std::uint64_t sent = 0;
		bool ok = q.drain(
		    [&order](const net::Bytes& f) {
			    order.push_back(f.empty() ? 0 : f[0]);
			    return true;
		    },
		    /*bulk_budget=*/16, &sent);

		check("drain reports success when the sink accepts every frame", ok);
		check("drain sent all six frames", sent == 6);
		// control (C0,C1) -> movement (A0,A1) -> bulk (B0,B1), FIFO within each class.
		const std::vector<std::uint8_t> expect = {0xC0, 0xC1, 0xA0, 0xA1, 0xB0, 0xB1};
		check("frames drain in strict priority order (control<movement<bulk)",
		      order == expect);
	}

	// --- 2b. bulk budget caps bulk per drain pass ----------------------------
	{
		net::SendPriorityQueue q;
		for (int i = 0; i < 10; ++i) q.push(net::Bytes{static_cast<std::uint8_t>(i)},
		                                    net::SendPriority::kBulk);
		std::uint64_t sent = 0;
		q.drain([](const net::Bytes&) { return true; }, /*bulk_budget=*/4, &sent);
		check("bulk budget caps a single drain pass (4 of 10)", sent == 4);
		q.drain([](const net::Bytes&) { return true; }, /*bulk_budget=*/4, &sent);
		check("subsequent pass drains the next bulk batch (4 more)", sent == 4);
	}

	// --- 2c. drain aborts (and reports) when the sink fails -------------------
	{
		net::SendPriorityQueue q;
		q.push(net::Bytes{1}, net::SendPriority::kControl);
		q.push(net::Bytes{2}, net::SendPriority::kControl);
		std::uint64_t sent = 0;
		bool ok = q.drain([](const net::Bytes&) { return false; }, 16, &sent);
		check("drain returns false when the sink (socket write) aborts", !ok);
		check("drain reports zero sent when the first write fails", sent == 0);
	}

	// --- 3. connect-failure lifecycle (emits kConnectFailed, no hang) --------
	{
		net::NetThreadCore core;
		net::NetThreadConfig cfg;
		cfg.world_hello_frame = cn::encode_world_frame(cn::kOpWorldHello, 0, {});
		check("start() accepts a connect factory", core.start(connect_fail, cfg));

		net::InboundMessage m;
		check("connect failure surfaces kConnectFailed on the inbound ring",
		      wait_for_inbound(core, net::InboundKind::kConnectFailed, m, 2000));
		core.stop();
		check("connect-failure thread joins cleanly", !core.running());
	}

	// --- 3b. start() guards ---------------------------------------------------
	{
		net::NetThreadCore core;
		net::NetThreadConfig cfg;  // empty world_hello_frame
		check("start() rejects an empty WorldHello frame",
		      !core.start(connect_fail, cfg));
	}

	std::printf(g_fail == 0 ? "\nALL NET THREAD CORE TESTS PASSED\n"
	                        : "\n%d NET THREAD CORE TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}

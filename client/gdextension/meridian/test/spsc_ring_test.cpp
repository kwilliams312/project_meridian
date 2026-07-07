// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit test for the lock-free SPSC ring (issue #97).
// NO Godot: compiles against the header-only primitive (spsc_ring.h) only, so it
// runs in any C++17 toolchain (Client SAD §9.2). Plain-main style, mirroring the
// other client core tests (movement_controller_test.cpp), ctest-wired via
// client/gdextension/meridian/test/CMakeLists.txt.
//
// Proves the SPSC contract the net-thread binding depends on:
//   1. FIFO order + full/empty semantics (single-threaded).
//   2. WRAP-AROUND: pushing/popping far past the backing-array length preserves
//      order and never loses or duplicates an element.
//   3. CONCURRENT producer + consumer: one thread pushes a long 0..N-1 sequence,
//      another pops it; the consumer receives EXACTLY that sequence, in order, with
//      no gaps and no duplicates (the no-lost/no-duplicated-messages guarantee).
//
// Depends on the header-only primitive alone (spsc_ring.h) — no net-core headers, so
// it links nothing and runs in the plainest MERIDIAN_CLIENT_TESTS build.

#include "spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace net = meridian::net;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

int main() {
	std::printf("SPSC RING TESTS (#97)\n");

	// --- 1. FIFO + full/empty (single-threaded) ------------------------------
	{
		net::SpscRing<int, 4> ring;  // holds 4 live elements
		int out = -1;
		check("pop on empty returns false", !ring.pop(out));
		check("capacity() reports the live-element capacity", ring.capacity() == 4);

		check("push 1..4 succeeds", ring.push(1) && ring.push(2) && ring.push(3) &&
		                                ring.push(4));
		check("push on full returns false", !ring.push(5));
		check("size_approx == capacity when full", ring.size_approx() == 4);

		bool fifo = true;
		for (int expect = 1; expect <= 4; ++expect) {
			int v = -1;
			if (!ring.pop(v) || v != expect) fifo = false;
		}
		check("elements pop in FIFO order 1..4", fifo);
		check("pop past the last element returns false", !ring.pop(out));
		check("ring is empty_approx after draining", ring.empty_approx());
	}

	// --- 2. WRAP-AROUND: push/pop far past the backing-array length -----------
	{
		net::SpscRing<std::uint64_t, 4> ring;  // 4 live + 1 sentinel = 5 slots
		bool ok = true;
		std::uint64_t next_pop = 0;

		// (a) 1-in/1-out for 100000 cycles: keeps 0-1 elements resident while the
		//     head/tail indices sweep the 5-slot array 20000+ times.
		for (std::uint64_t i = 0; i < 100000; ++i) {
			if (!ring.push(i)) ok = false;
			std::uint64_t v = 0;
			if (!ring.pop(v) || v != i) ok = false;
			++next_pop;
		}
		check("wrap-around 1-in/1-out preserves FIFO with no loss/dup", ok);

		// (b) Repeatedly fill to capacity then drain fully, straddling the wrap point,
		//     so the full/empty boundary is exercised at every offset in the array.
		bool ok2 = true;
		std::uint64_t seq = next_pop;
		for (int round = 0; round < 5000; ++round) {
			for (std::size_t k = 0; k < ring.capacity(); ++k) {
				if (!ring.push(seq + k)) ok2 = false;  // fill exactly to capacity
			}
			if (ring.push(seq + ring.capacity())) ok2 = false;  // one past = full
			for (std::size_t k = 0; k < ring.capacity(); ++k) {
				std::uint64_t v = 0;
				if (!ring.pop(v) || v != seq + k) ok2 = false;
			}
			seq += ring.capacity();
		}
		check("wrap-around fill-to-full then drain preserves order at every offset",
		      ok2);
		check("ring is empty after the wrap-around rounds", ring.empty_approx());
	}

	// --- 3. CONCURRENT producer + consumer -----------------------------------
	{
		constexpr std::uint64_t kN = 2'000'000;  // long run to expose ordering races
		net::SpscRing<std::uint64_t, 1024> ring;

		std::atomic<bool> producer_ok{true};
		std::thread producer([&] {
			for (std::uint64_t i = 0; i < kN; ++i) {
				// Spin until the slot is free — bounded ring + back-pressure.
				while (!ring.push(i)) {
					std::this_thread::yield();
				}
			}
		});

		bool order_ok = true;
		std::uint64_t received = 0;
		std::uint64_t expect = 0;
		while (received < kN) {
			std::uint64_t v = 0;
			if (ring.pop(v)) {
				if (v != expect) order_ok = false;  // gap or duplicate
				++expect;
				++received;
			} else {
				std::this_thread::yield();
			}
		}
		producer.join();

		check("concurrent: consumer received exactly N elements", received == kN);
		check("concurrent: elements arrived strictly in order (no loss/dup)", order_ok);
		check("concurrent: ring drained empty at the end", ring.empty_approx());
		(void)producer_ok;
	}

	std::printf(g_fail == 0 ? "\nALL SPSC RING TESTS PASSED\n"
	                        : "\n%d SPSC RING TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}

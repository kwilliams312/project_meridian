// Project Meridian — deterministic REPLAY HARNESS core (issue #106).
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores", R5 "custom
// netcode: every desync bug is ours → golden fixtures shared with server track;
// network replay repro"). This header + its .cpp contain NO Godot types: they
// drive the SAME engine-free prediction/reconciliation path (#102/#103,
// movement_controller.*) so a recorded (input + server-correction) sequence can
// be replayed BIT-IDENTICALLY. That is the desync regression net: prediction and
// reconciliation are our own code, so the only guard against silent nondeterminism
// (float evaluation order, uninitialised state, constant drift) is a harness that
// records a run and proves a re-run reproduces it byte-for-byte.
//
// The harness records a `Recording` — an initial state + a stream of per-tick
// `ReplayEvent`s (the input sampled that tick and an optional authoritative server
// correction) — and `run_recording()` drives a fresh `PredictionReconciler`
// through it, capturing a `StateFrame` per tick (the predicted SIM state, the
// visible/render state, and the error-offset smoothing observables). Two runs of
// the same Recording MUST produce bit-identical `ReplayResult`s. A `Fixture`
// bundles a Recording with its GOLDEN result + a hash so a recorded sequence can
// be checked in (test/fixtures/) as a regression artifact: if the sim ever changes
// numerically, the checked-in golden stops matching a fresh replay and the test
// fails loudly.
//
// DETERMINISM CONTRACT: bit-identical means IEEE-754 bit patterns are equal, NOT
// "within epsilon". Floats serialise as their raw 32-bit pattern (see the .cpp)
// so a fixture round-trips exactly and the comparison is unambiguous — an epsilon
// net would hide the sub-bit drift that accumulates into a real desync over a
// match. This is a SAME-BINARY guarantee (record on a machine, replay on the same
// build reproduces); cross-compiler bit-parity is the server-track golden-fixture
// job (movement_constants.h §4), not this harness.

#ifndef MERIDIAN_REPLAY_HARNESS_H
#define MERIDIAN_REPLAY_HARNESS_H

#include "movement_controller.h"

#include <cstdint>
#include <string>
#include <vector>

namespace meridian::replay {

using movement::MovementInput;
using movement::MovementSnapshot;
using movement::MovementStateIn;
using movement::Vec3;

// ===========================================================================
// Recording — the serialisable input side of a replay.
// ===========================================================================

// One recorded tick: the input sampled this tick (fed to predict()), an OPTIONAL
// authoritative server correction that arrives this tick (applied via reconcile()
// AFTER the predict), and an optional render-time advance of the error-offset
// decay (advance_smoothing()). A stream of these is exactly the (seq, input,
// server_correction) sequence the desync net replays.
struct ReplayEvent {
	uint64_t      client_time_ms = 0;      // timestamp passed to predict()
	MovementInput input;                   // input sampled this tick
	bool          has_correction = false;  // a server MovementState arrives this tick
	MovementStateIn correction;            // valid iff has_correction
	uint64_t      advance_ms = 0;          // advance_smoothing(dt) after reconcile (render time)
};

// A full recording: the flat-world plane, the seed state, and the event stream.
// FlatWorldQuery(plane_y) is the M0 ground (D-19 flat bootstrap map); the seam is
// swappable at M1 but the harness pins the M0 contract the sim ships against.
struct Recording {
	float                    world_plane_y = 0.0f;
	MovementSnapshot         start;
	std::vector<ReplayEvent> events;
};

// ===========================================================================
// ReplayResult — the observable per-tick trace (the bit-identical target).
// ===========================================================================

// Everything observable after one tick's predict (+ optional reconcile + decay).
// The full state the desync net compares: the authoritative-correct SIM state, the
// visible/render state, and the smoothing observables. Bit-identical comparison of
// these across two runs is the determinism assertion.
struct StateFrame {
	uint32_t         seq = 0;              // seq predict() assigned this tick
	MovementSnapshot predicted;            // SIM state (drives the next prediction)
	MovementSnapshot visible;              // render state (sim + decaying error offset)
	Vec3             error_offset;         // current render offset
	float            last_error_mag = 0.0f;// |error| the reconciler last computed
	bool             reconciled = false;   // a server correction was applied this tick
	bool             snapped = false;      // that correction snapped (else smoothed/none)
};

// The per-tick trace of a run — one StateFrame per event, in order.
struct ReplayResult {
	std::vector<StateFrame> frames;

	// Convenience: the final SIM / render states (empty-safe: default-constructed
	// snapshot when there are no frames).
	MovementSnapshot final_predicted() const;
	MovementSnapshot final_visible() const;
};

// Drive a fresh PredictionReconciler through the recording, capturing a StateFrame
// per event. PURE w.r.t. `rec` — no global/static state — so calling it twice on
// the same Recording yields bit-identical ReplayResults. This is the single code
// path both "record" and "replay" go through (same predict()/reconcile()/
// integrate_tick() the live client runs).
ReplayResult run_recording(const Recording& rec);

// ===========================================================================
// Bit-identical comparison (the determinism assertion — EXACT, not epsilon).
// ===========================================================================

// True iff `a` and `b` have identical IEEE-754 bit patterns (so -0.0f != +0.0f and
// two NaNs are equal iff their payloads match). This is the desync-net primitive:
// approximate equality would mask the drift the harness exists to catch.
bool bit_equal(float a, float b);
bool bit_equal(const Vec3& a, const Vec3& b);
bool bit_equal(const MovementSnapshot& a, const MovementSnapshot& b);
bool bit_equal(const StateFrame& a, const StateFrame& b);
bool bit_equal(const ReplayResult& a, const ReplayResult& b);

// If `a` and `b` differ, returns the index of the FIRST diverging frame (for a
// pinpoint "desync at tick N" report); returns -1 when they are bit-identical.
// A size mismatch reports the first index past the shorter trace.
long first_divergent_frame(const ReplayResult& a, const ReplayResult& b);

// FNV-1a hash over the raw bytes of every frame's floats/ints — a compact integrity
// digest of a whole trace. Equal traces hash equal; a single flipped bit almost
// always changes it. Used as the fixture's golden digest + a fast tamper check.
uint64_t trace_hash(const ReplayResult& result);

// ===========================================================================
// Serialisation — check-in-able regression fixtures.
// ===========================================================================

// A Recording plus its GOLDEN trace + digest: the checked-in regression artifact.
// `make_fixture` runs the recording to fill the golden; `parse_fixture`/
// `serialize_fixture` round-trip it EXACTLY (floats as raw bit patterns).
struct Fixture {
	std::string  name;
	Recording    recording;
	ReplayResult golden;
	uint64_t     golden_hash = 0;
};

// Run `recording` and bundle it with the resulting golden trace + hash.
Fixture make_fixture(const std::string& name, const Recording& recording);

// Text serialisation. Floats are written as their exact 32-bit hex pattern so a
// parsed fixture reproduces the original bit-for-bit (a decimal round-trip could
// lose the low bit — fatal for a bit-identical net). Line-oriented + token-keyed
// so a fixture diff is legible in review.
std::string serialize_fixture(const Fixture& fx);
std::string serialize_recording(const Recording& rec);
std::string serialize_result(const ReplayResult& result);

// Parse the matching serialised forms. Return false (leaving `out` unspecified) on
// any malformed input — a truncated/garbled fixture must fail, never silently parse
// to a wrong-but-plausible state.
bool parse_fixture(const std::string& text, Fixture& out);
bool parse_recording(const std::string& text, Recording& out);
bool parse_result(const std::string& text, ReplayResult& out);

} // namespace meridian::replay

#endif // MERIDIAN_REPLAY_HARNESS_H

// Project Meridian — deterministic REPLAY HARNESS core (issue #106).
// Engine-free implementation (no Godot) — see replay_harness.h header note.
//
// TRACE: Client SAD §9.2 (engine-agnostic cores), R5 (custom netcode / golden
// fixtures / network replay repro). Drives movement_controller.* (#102/#103) —
// the SAME predict()/reconcile()/integrate_tick() the live client runs — so a
// recorded (input + correction) sequence replays bit-identically.

#include "replay_harness.h"

#include "movement_query.h"

#include <cstring>
#include <sstream>

namespace meridian::replay {

// ---------------------------------------------------------------------------
// IEEE-754 bit reinterpretation. std::memcpy is the only well-defined way to
// read a float's object representation (a reinterpret_cast<uint32_t*> would be
// a strict-aliasing violation). The compiler folds these to a register move.
// ---------------------------------------------------------------------------
static uint32_t float_bits(float f) {
	uint32_t bits;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static float bits_to_float(uint32_t bits) {
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

// ---------------------------------------------------------------------------
// run_recording — the single record/replay code path.
// ---------------------------------------------------------------------------
ReplayResult run_recording(const Recording& rec) {
	movement::FlatWorldQuery world(rec.world_plane_y);
	movement::PredictionReconciler reconciler(world, rec.start);

	ReplayResult out;
	out.frames.reserve(rec.events.size());

	for (const ReplayEvent& ev : rec.events) {
		// 1. Sample-and-predict this tick (assigns seq, integrates locally).
		const movement::MovementIntentOut intent =
		    reconciler.predict(ev.input, ev.client_time_ms);

		StateFrame frame;
		frame.seq = intent.seq;

		// 2. Optional authoritative server correction (rewind + re-simulate).
		if (ev.has_correction) {
			reconciler.reconcile(ev.correction);
			frame.reconciled = true;
			frame.snapped    = reconciler.last_reconcile_snapped();
		}

		// 3. Optional render-time decay of the error offset (#103 smoothing).
		if (ev.advance_ms != 0) {
			reconciler.advance_smoothing(ev.advance_ms);
		}

		// 4. Capture the full observable state AFTER this tick.
		frame.predicted      = reconciler.predicted_state();
		frame.visible        = reconciler.visible_state();
		frame.error_offset   = reconciler.error_offset();
		frame.last_error_mag = reconciler.last_error_magnitude();
		out.frames.push_back(frame);
	}

	return out;
}

MovementSnapshot ReplayResult::final_predicted() const {
	return frames.empty() ? MovementSnapshot{} : frames.back().predicted;
}

MovementSnapshot ReplayResult::final_visible() const {
	return frames.empty() ? MovementSnapshot{} : frames.back().visible;
}

// ---------------------------------------------------------------------------
// Bit-identical comparison.
// ---------------------------------------------------------------------------
bool bit_equal(float a, float b) { return float_bits(a) == float_bits(b); }

bool bit_equal(const Vec3& a, const Vec3& b) {
	return bit_equal(a.x, b.x) && bit_equal(a.y, b.y) && bit_equal(a.z, b.z);
}

bool bit_equal(const MovementSnapshot& a, const MovementSnapshot& b) {
	return bit_equal(a.position, b.position) && bit_equal(a.velocity, b.velocity) &&
	       a.grounded == b.grounded && a.mode == b.mode &&
	       bit_equal(a.orientation, b.orientation);
}

bool bit_equal(const StateFrame& a, const StateFrame& b) {
	return a.seq == b.seq && bit_equal(a.predicted, b.predicted) &&
	       bit_equal(a.visible, b.visible) && bit_equal(a.error_offset, b.error_offset) &&
	       bit_equal(a.last_error_mag, b.last_error_mag) &&
	       a.reconciled == b.reconciled && a.snapped == b.snapped;
}

bool bit_equal(const ReplayResult& a, const ReplayResult& b) {
	if (a.frames.size() != b.frames.size()) return false;
	for (std::size_t i = 0; i < a.frames.size(); ++i) {
		if (!bit_equal(a.frames[i], b.frames[i])) return false;
	}
	return true;
}

long first_divergent_frame(const ReplayResult& a, const ReplayResult& b) {
	const std::size_t n = a.frames.size() < b.frames.size() ? a.frames.size()
	                                                        : b.frames.size();
	for (std::size_t i = 0; i < n; ++i) {
		if (!bit_equal(a.frames[i], b.frames[i])) return static_cast<long>(i);
	}
	if (a.frames.size() != b.frames.size()) return static_cast<long>(n);
	return -1;
}

// ---------------------------------------------------------------------------
// trace_hash — FNV-1a over every frame's raw bytes.
// ---------------------------------------------------------------------------
static void fnv_mix(uint64_t& h, uint32_t word) {
	// Feed 4 bytes, little-end first (order is fixed, so the digest is stable).
	for (int i = 0; i < 4; ++i) {
		h ^= static_cast<uint8_t>(word >> (8 * i));
		h *= 0x100000001b3ull;   // FNV-1a 64-bit prime
	}
}

uint64_t trace_hash(const ReplayResult& result) {
	uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a 64-bit offset basis
	auto mix_vec = [&](const Vec3& v) {
		fnv_mix(h, float_bits(v.x));
		fnv_mix(h, float_bits(v.y));
		fnv_mix(h, float_bits(v.z));
	};
	auto mix_snap = [&](const MovementSnapshot& s) {
		mix_vec(s.position);
		mix_vec(s.velocity);
		fnv_mix(h, s.grounded ? 1u : 0u);
		fnv_mix(h, static_cast<uint32_t>(s.mode));
		fnv_mix(h, float_bits(s.orientation));
	};
	for (const StateFrame& f : result.frames) {
		fnv_mix(h, f.seq);
		mix_snap(f.predicted);
		mix_snap(f.visible);
		mix_vec(f.error_offset);
		fnv_mix(h, float_bits(f.last_error_mag));
		fnv_mix(h, f.reconciled ? 1u : 0u);
		fnv_mix(h, f.snapped ? 1u : 0u);
	}
	return h;
}

// ===========================================================================
// Serialisation. Line-oriented, token-keyed. Floats as their exact 32-bit hex
// pattern (bit-for-bit round-trip); ints decimal; the digest as 16-hex.
// ===========================================================================
namespace {

// --- Writer helpers: append space-separated tokens to a stream. ---
void wf(std::ostream& os, float f) {
	// 8 hex chars = the exact IEEE-754 bit pattern.
	std::ostringstream t;
	t.width(8);
	t.fill('0');
	t << std::hex << float_bits(f);
	os << t.str() << ' ';
}
void wu(std::ostream& os, uint64_t v) { os << v << ' '; }
void wb(std::ostream& os, bool b) { os << (b ? 1 : 0) << ' '; }

void write_snapshot(std::ostream& os, const MovementSnapshot& s) {
	wf(os, s.position.x); wf(os, s.position.y); wf(os, s.position.z);
	wf(os, s.velocity.x); wf(os, s.velocity.y); wf(os, s.velocity.z);
	wb(os, s.grounded);
	wu(os, static_cast<uint64_t>(s.mode));
	wf(os, s.orientation);
}

void write_input(std::ostream& os, const MovementInput& in) {
	wf(os, in.move_x); wf(os, in.move_z);
	wb(os, in.jump); wb(os, in.walk);
	wf(os, in.orientation);
}

void write_correction(std::ostream& os, const MovementStateIn& c) {
	wu(os, c.ack_seq);
	wu(os, c.state_flags);
	wf(os, c.position.x); wf(os, c.position.y); wf(os, c.position.z);
	wf(os, c.orientation);
	wu(os, c.server_time_ms);
}

// --- Reader helpers: pull typed tokens from an istream; set ok=false on failure. ---
float rf(std::istream& is, bool& ok) {
	std::string tok;
	if (!(is >> tok)) { ok = false; return 0.0f; }
	uint32_t bits = 0;
	std::istringstream t(tok);
	if (!(t >> std::hex >> bits)) { ok = false; return 0.0f; }
	return bits_to_float(bits);
}
uint64_t ru(std::istream& is, bool& ok) {
	uint64_t v = 0;
	if (!(is >> v)) { ok = false; return 0; }
	return v;
}
bool rb(std::istream& is, bool& ok) { return ru(is, ok) != 0; }

MovementSnapshot read_snapshot(std::istream& is, bool& ok) {
	MovementSnapshot s;
	s.position.x = rf(is, ok); s.position.y = rf(is, ok); s.position.z = rf(is, ok);
	s.velocity.x = rf(is, ok); s.velocity.y = rf(is, ok); s.velocity.z = rf(is, ok);
	s.grounded = rb(is, ok);
	s.mode = static_cast<movement::MoveMode>(ru(is, ok));
	s.orientation = rf(is, ok);
	return s;
}

MovementInput read_input(std::istream& is, bool& ok) {
	MovementInput in;
	in.move_x = rf(is, ok); in.move_z = rf(is, ok);
	in.jump = rb(is, ok); in.walk = rb(is, ok);
	in.orientation = rf(is, ok);
	return in;
}

MovementStateIn read_correction(std::istream& is, bool& ok) {
	MovementStateIn c;
	c.ack_seq = static_cast<uint32_t>(ru(is, ok));
	c.state_flags = static_cast<uint32_t>(ru(is, ok));
	c.position.x = rf(is, ok); c.position.y = rf(is, ok); c.position.z = rf(is, ok);
	c.orientation = rf(is, ok);
	c.server_time_ms = ru(is, ok);
	return c;
}

// Require that the next token equals `kw` (structural keyword). Fails cleanly on a
// malformed/truncated fixture rather than mis-parsing.
bool expect_kw(std::istream& is, const char* kw) {
	std::string tok;
	if (!(is >> tok)) return false;
	return tok == kw;
}

} // namespace

// --- Recording ---
static void write_recording_body(std::ostream& os, const Recording& rec) {
	os << "plane_y "; wf(os, rec.world_plane_y); os << '\n';
	os << "start "; write_snapshot(os, rec.start); os << '\n';
	os << "events " << rec.events.size() << '\n';
	for (const ReplayEvent& ev : rec.events) {
		os << "ev ";
		wu(os, ev.client_time_ms);
		write_input(os, ev.input);
		wb(os, ev.has_correction);
		write_correction(os, ev.correction);
		wu(os, ev.advance_ms);
		os << '\n';
	}
}

static bool read_recording_body(std::istream& is, Recording& out) {
	bool ok = true;
	if (!expect_kw(is, "plane_y")) return false;
	out.world_plane_y = rf(is, ok);
	if (!expect_kw(is, "start")) return false;
	out.start = read_snapshot(is, ok);
	if (!expect_kw(is, "events")) return false;
	const uint64_t n = ru(is, ok);
	if (!ok) return false;
	out.events.clear();
	out.events.reserve(static_cast<std::size_t>(n));
	for (uint64_t i = 0; i < n; ++i) {
		if (!expect_kw(is, "ev")) return false;
		ReplayEvent ev;
		ev.client_time_ms = ru(is, ok);
		ev.input = read_input(is, ok);
		ev.has_correction = rb(is, ok);
		ev.correction = read_correction(is, ok);
		ev.advance_ms = ru(is, ok);
		if (!ok) return false;
		out.events.push_back(ev);
	}
	return ok;
}

std::string serialize_recording(const Recording& rec) {
	std::ostringstream os;
	os << "meridian-replay-recording v1\n";
	write_recording_body(os, rec);
	return os.str();
}

bool parse_recording(const std::string& text, Recording& out) {
	std::istringstream is(text);
	if (!expect_kw(is, "meridian-replay-recording")) return false;
	if (!expect_kw(is, "v1")) return false;
	return read_recording_body(is, out);
}

// --- Result (golden trace) ---
static void write_result_body(std::ostream& os, const ReplayResult& result) {
	os << "frames " << result.frames.size() << '\n';
	for (const StateFrame& f : result.frames) {
		os << "fr ";
		wu(os, f.seq);
		write_snapshot(os, f.predicted);
		write_snapshot(os, f.visible);
		wf(os, f.error_offset.x); wf(os, f.error_offset.y); wf(os, f.error_offset.z);
		wf(os, f.last_error_mag);
		wb(os, f.reconciled);
		wb(os, f.snapped);
		os << '\n';
	}
}

static bool read_result_body(std::istream& is, ReplayResult& out) {
	bool ok = true;
	if (!expect_kw(is, "frames")) return false;
	const uint64_t n = ru(is, ok);
	if (!ok) return false;
	out.frames.clear();
	out.frames.reserve(static_cast<std::size_t>(n));
	for (uint64_t i = 0; i < n; ++i) {
		if (!expect_kw(is, "fr")) return false;
		StateFrame f;
		f.seq = static_cast<uint32_t>(ru(is, ok));
		f.predicted = read_snapshot(is, ok);
		f.visible = read_snapshot(is, ok);
		f.error_offset.x = rf(is, ok);
		f.error_offset.y = rf(is, ok);
		f.error_offset.z = rf(is, ok);
		f.last_error_mag = rf(is, ok);
		f.reconciled = rb(is, ok);
		f.snapped = rb(is, ok);
		if (!ok) return false;
		out.frames.push_back(f);
	}
	return ok;
}

std::string serialize_result(const ReplayResult& result) {
	std::ostringstream os;
	os << "meridian-replay-result v1\n";
	write_result_body(os, result);
	return os.str();
}

bool parse_result(const std::string& text, ReplayResult& out) {
	std::istringstream is(text);
	if (!expect_kw(is, "meridian-replay-result")) return false;
	if (!expect_kw(is, "v1")) return false;
	return read_result_body(is, out);
}

// --- Fixture (recording + golden + digest) ---
Fixture make_fixture(const std::string& name, const Recording& recording) {
	Fixture fx;
	fx.name = name;
	fx.recording = recording;
	fx.golden = run_recording(recording);
	fx.golden_hash = trace_hash(fx.golden);
	return fx;
}

std::string serialize_fixture(const Fixture& fx) {
	std::ostringstream os;
	os << "meridian-replay-fixture v1\n";
	os << "name " << (fx.name.empty() ? "unnamed" : fx.name) << '\n';
	os << "golden_hash " << std::hex << fx.golden_hash << std::dec << '\n';
	write_recording_body(os, fx.recording);
	write_result_body(os, fx.golden);
	return os.str();
}

bool parse_fixture(const std::string& text, Fixture& out) {
	std::istringstream is(text);
	bool ok = true;
	if (!expect_kw(is, "meridian-replay-fixture")) return false;
	if (!expect_kw(is, "v1")) return false;
	if (!expect_kw(is, "name")) return false;
	if (!(is >> out.name)) return false;
	if (!expect_kw(is, "golden_hash")) return false;
	{
		std::string tok;
		if (!(is >> tok)) return false;
		std::istringstream t(tok);
		if (!(t >> std::hex >> out.golden_hash)) return false;
	}
	if (!read_recording_body(is, out.recording)) return false;
	if (!read_result_body(is, out.golden)) return false;
	return ok;
}

} // namespace meridian::replay

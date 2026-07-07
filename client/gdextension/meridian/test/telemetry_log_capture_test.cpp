// Project Meridian — engine-free unit test for the client telemetry log capture
// core (issue #168). NO Godot: compiles against the plain-C++ core
// (telemetry_log_core.* + telemetry_log_capture.*) and the header-only transport
// seam, so it runs in any C++17 toolchain (Client SAD §9.2 engine-agnostic
// cores). Plain-main style, mirroring the movement controller test.
//
// Proves the #168 test matrix (issue "TEST" list), each a hard rule of the
// telemetry-privacy contract:
//   (a) ERROR/CRITICAL captured, TRACE/DEBUG/INFO/WARN dropped     (privacy §2a)
//   (b) context attached, NO PII fields present in the envelope    (privacy §3)
//   (c) batching: flush at N events / at T ms                       (D-29 rule 2)
//   (d) rate-limit caps a storm                                     (D-29 rule 2)
//   (e) opt-out → nothing captured / nothing shipped               (privacy §5)
//   (f) envelope is well-formed + parseable, shipped through a MOCK transport
//       (assert the mock received the expected batch)              (privacy §2a)

#include "telemetry_log_capture.h"
#include "telemetry_log_core.h"
#include "telemetry_transport.h"

#include <cstdio>
#include <cstdint>
#include <string>

namespace tel = meridian::telemetry;

static int g_fail = 0;
static void check(const char *name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

// True if `haystack` contains `needle` (substring). Used both to confirm
// expected context tags ARE present and to confirm PII substrings are NOT.
static bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// ── A minimal JSON well-formedness check for the newline-delimited envelope ──
// Not a full parser — validates each non-empty line is a balanced-brace,
// balanced-quote JSON object (enough to prove serialize_envelope() emits
// parseable Sentry envelope lines; the real ingest uses a full parser).
static bool json_object_well_formed(const std::string &line) {
	if (line.empty()) return true;               // blank separator lines are fine
	if (line.front() != '{' || line.back() != '}') return false;
	int depth = 0;
	bool in_str = false, esc = false;
	for (char c : line) {
		if (esc) { esc = false; continue; }
		if (in_str) {
			if (c == '\\') esc = true;
			else if (c == '"') in_str = false;
			continue;
		}
		if (c == '"') in_str = true;
		else if (c == '{') ++depth;
		else if (c == '}') { --depth; if (depth < 0) return false; }
	}
	return depth == 0 && !in_str;
}

static bool envelope_well_formed(const std::string &env) {
	std::size_t start = 0;
	while (start <= env.size()) {
		std::size_t nl = env.find('\n', start);
		std::string line = env.substr(start, nl == std::string::npos ? std::string::npos
		                                                             : nl - start);
		if (!json_object_well_formed(line)) return false;
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	return true;
}

static tel::SessionContext test_context() {
	tel::SessionContext ctx;
	ctx.session_id = "ephemeral-8f2a-run-token";   // opaque, NOT an account id
	ctx.build      = "meridian-client 0.1.0+abcd";
	ctx.platform   = "macos-arm64";
	return ctx;
}

static tel::CaptureConfig fast_config() {
	tel::CaptureConfig cfg;
	cfg.enabled                 = true;
	cfg.batch_max_events        = 5;
	cfg.batch_flush_interval_ms = 1000;
	cfg.rate_limit_max_events   = 10;
	cfg.rate_limit_window_ms    = 1000;
	return cfg;
}

int main() {
	std::printf("meridian telemetry log capture core test (#168)\n");

	// =======================================================================
	// (a) SEVERITY GATE — ERROR/CRITICAL captured; TRACE/DEBUG/INFO/WARN dropped
	// =======================================================================
	std::printf("[a] severity gate: only ERROR/CRITICAL leave the device\n");
	{
		tel::LogCapture cap(fast_config(), test_context());
		uint64_t t = 1000;
		check("TRACE dropped",    !cap.capture(tel::Severity::Trace,    "t", "x", t));
		check("DEBUG dropped",    !cap.capture(tel::Severity::Debug,    "d", "x", t));
		check("INFO dropped",     !cap.capture(tel::Severity::Info,     "i", "x", t));
		check("WARN dropped",     !cap.capture(tel::Severity::Warn,     "w", "x", t));
		check("ERROR captured",    cap.capture(tel::Severity::Error,    "e", "net", t));
		check("CRITICAL captured", cap.capture(tel::Severity::Critical, "c", "sim", t));
		check("only 2 shippable events buffered", cap.buffered_count() == 2);
		check("is_shippable(Error) true",    tel::is_shippable(tel::Severity::Error));
		check("is_shippable(Warn) false",   !tel::is_shippable(tel::Severity::Warn));
	}

	// =======================================================================
	// (b) CONTEXT ATTACHED + NO PII — the structural privacy guarantee
	// =======================================================================
	std::printf("[b] context attached; NO PII fields serialized\n");
	{
		tel::LogCapture cap(fast_config(), test_context());
		cap.capture(tel::Severity::Error, "disconnect: opcode 0x12 decode failed", "net", 2000);
		cap.capture(tel::Severity::Critical, "assertion failed in sim tick", "sim", 2001);
		tel::LogBatch batch = cap.flush();
		std::string env = tel::serialize_envelope(batch);

		// Expected no-PII context IS present.
		check("session_id tag present",  contains(env, "\"session_id\":\"ephemeral-8f2a-run-token\""));
		check("build tag present",       contains(env, "\"build\":\"meridian-client 0.1.0+abcd\""));
		check("platform tag present",    contains(env, "\"platform\":\"macos-arm64\""));
		check("error level mapped",      contains(env, "\"level\":\"error\""));
		check("critical -> fatal mapped", contains(env, "\"level\":\"fatal\""));

		// NO PII substrings — the type has no such fields, so none can serialize.
		// Case-sensitive JSON keys are lowercase; check the key forms that would
		// appear if someone regressed and added a PII field.
		check("no username field",  !contains(env, "username"));
		check("no \"user\" field",  !contains(env, "\"user\""));
		check("no email field",     !contains(env, "email"));
		check("no ip_address field",!contains(env, "ip_address"));
		check("no \"ip\" field",    !contains(env, "\"ip\""));
		check("no hardware id",     !contains(env, "hardware"));
		check("no machine id",      !contains(env, "machine_id"));
		check("no account id",      !contains(env, "account"));
	}

	// =======================================================================
	// (c) BATCHING — flush at N events, and flush at T ms.
	// =======================================================================
	std::printf("[c] batching: flush at N events / at T ms\n");
	{
		// N trigger: batch_max_events = 5.
		tel::LogCapture cap(fast_config(), test_context());
		uint64_t t = 5000;
		for (int i = 0; i < 4; ++i) cap.capture(tel::Severity::Error, "e", "net", t + i);
		check("not due before N reached", !cap.flush_due(t + 4));
		cap.capture(tel::Severity::Error, "e5", "net", t + 5);   // 5th event
		check("due at N events", cap.flush_due(t + 5));
		tel::LogBatch b = cap.flush();
		check("batch has N events", b.events.size() == 5);
		check("buffer cleared after flush", cap.buffered_count() == 0);
	}
	{
		// T trigger: one event, then time passes past batch_flush_interval_ms.
		tel::LogCapture cap(fast_config(), test_context());
		cap.capture(tel::Severity::Error, "lonely", "net", 10000);
		check("not due immediately", !cap.flush_due(10500));            // 500 ms < 1000
		check("due after T ms elapsed", cap.flush_due(11000));          // 1000 ms == T
		tel::LogBatch b = cap.flush();
		check("time-flushed batch has the event", b.events.size() == 1);
	}

	// =======================================================================
	// (d) RATE LIMIT — a storm is capped; drops are counted + reported.
	// =======================================================================
	std::printf("[d] rate limit caps a log storm\n");
	{
		tel::CaptureConfig cfg = fast_config();
		cfg.rate_limit_max_events = 10;
		cfg.rate_limit_window_ms  = 1000;
		cfg.batch_max_events      = 10000;   // don't let batching interfere
		tel::LogCapture cap(cfg, test_context());

		// Fire 100 ERRORs within one 1000 ms window (all at t=20000).
		int captured = 0;
		for (int i = 0; i < 100; ++i) {
			if (cap.capture(tel::Severity::Error, "storm", "net", 20000)) ++captured;
		}
		check("storm capped to window max", captured == 10);
		check("buffered == window max", cap.buffered_count() == 10);
		check("dropped counted", cap.pending_dropped() == 90);

		// After the window advances, capture is allowed again.
		check("post-window capture allowed", cap.capture(tel::Severity::Error, "next", "net", 21000));

		// The drop count surfaces on the batch + its envelope.
		tel::LogBatch b = cap.flush();
		check("batch reports rate-limit drops", b.dropped_by_rate_limit == 90);
		std::string env = tel::serialize_envelope(b);
		check("envelope carries drop summary", contains(env, "\"rate_limited_dropped\":90"));
	}

	// =======================================================================
	// (e) OPT-OUT — off ⇒ nothing captured, nothing shipped.
	// =======================================================================
	std::printf("[e] opt-out: capture nothing / ship nothing\n");
	{
		tel::CaptureConfig cfg = fast_config();
		cfg.enabled = false;   // opted OUT
		tel::LogCapture cap(cfg, test_context());
		check("ERROR not captured when opted out",    !cap.capture(tel::Severity::Error, "e", "net", 30000));
		check("CRITICAL not captured when opted out", !cap.capture(tel::Severity::Critical, "c", "sim", 30000));
		check("nothing buffered when opted out", cap.buffered_count() == 0);

		// And even a hand-built non-empty batch does not ship through transport.
		tel::MockTransport mock(/*endpoint_configured=*/true);
		tel::LogBatch forced;
		forced.context = test_context();
		forced.events.push_back(tel::LogEvent{tel::Severity::Error, "e", "net", 30000});
		tel::ShipResult r = cap.ship(forced, mock);
		check("opted-out ship is a no-op (NoSink)", r == tel::ShipResult::NoSink);
		check("mock received nothing while opted out", mock.shipped.empty());

		// Toggling back ON resumes capture (the setting drives the flag).
		cap.set_enabled(true);
		check("re-enabled resumes capture", cap.capture(tel::Severity::Error, "e", "net", 31000));
	}

	// =======================================================================
	// (f) SHIPPING through a MOCK transport — envelope well-formed + received.
	// =======================================================================
	std::printf("[f] shipping: well-formed envelope through a mock transport\n");
	{
		tel::LogCapture cap(fast_config(), test_context());
		cap.capture(tel::Severity::Error,    "boom", "net", 40000);
		cap.capture(tel::Severity::Critical, "fatal boom", "sim", 40001);
		tel::LogBatch batch = cap.flush();

		tel::MockTransport mock(/*endpoint_configured=*/true);
		tel::ShipResult r = cap.ship(batch, mock);
		check("ship returns Ok", r == tel::ShipResult::Ok);
		check("mock received exactly one envelope", mock.shipped.size() == 1);
		check("shipped envelope well-formed (parseable lines)",
		      envelope_well_formed(mock.shipped.front()));
		check("shipped envelope has the two messages",
		      contains(mock.shipped.front(), "boom") &&
		          contains(mock.shipped.front(), "fatal boom"));
		check("shipped envelope has sdk header",
		      contains(mock.shipped.front(), "meridian.client.telemetry"));
		check("shipped envelope has NO PII",
		      !contains(mock.shipped.front(), "username") &&
		          !contains(mock.shipped.front(), "email") &&
		          !contains(mock.shipped.front(), "\"user\""));

		// Back-off/retry: a Failed transport re-queues the batch for a retry.
		tel::LogCapture cap2(fast_config(), test_context());
		cap2.capture(tel::Severity::Error, "retry-me", "net", 41000);
		tel::LogBatch b2 = cap2.flush();
		tel::MockTransport failing(/*endpoint_configured=*/true);
		failing.script_results({tel::ShipResult::Failed});
		tel::ShipResult r2 = cap2.ship(b2, failing);
		check("failed ship returns Failed", r2 == tel::ShipResult::Failed);
		check("failed batch re-queued for retry", cap2.buffered_count() == 1);

		// A NullTransport (no endpoint configured) swallows the batch: NoSink.
		tel::LogCapture cap3(fast_config(), test_context());
		cap3.capture(tel::Severity::Error, "no-endpoint", "net", 42000);
		tel::LogBatch b3 = cap3.flush();
		tel::NullTransport null_sink;
		check("null sink reports no endpoint", !null_sink.has_endpoint());
		check("null-sink ship is NoSink", cap3.ship(b3, null_sink) == tel::ShipResult::NoSink);
	}

	std::printf(g_fail == 0 ? "\nALL TELEMETRY LOG CAPTURE TESTS PASSED\n"
	                        : "\n%d TELEMETRY LOG CAPTURE TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}

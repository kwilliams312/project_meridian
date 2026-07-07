// Project Meridian — engine-free unit test for the client CRASH report core
// (issue #109). NO Godot: compiles against the plain-C++ cores
// (crash_report_core.* + crash_upload_queue.*) and the header-only transport
// seam, so it runs in any C++17 toolchain (Client SAD §9.2). Plain-main style,
// mirroring the #168 telemetry test.
//
// Proves the crash-channel matrix:
//   (a) report FILE serialize→parse round-trips (the handler's on-disk format)
//   (b) a malformed / non-magic file is rejected (parse returns false)
//   (c) the Sentry envelope is well-formed, carries level=fatal + event_kind=crash
//       + the no-PII context tags, and contains NO PII substrings   (privacy §3)
//   (d) the upload queue ships every pending report through a MOCK transport,
//       deletes the delivered files, and tallies found/shipped
//   (e) a transport Failure KEEPS the report file (retry on a later launch)
//   (f) a malformed file in the queue is dropped (can never succeed)

#include "crash_report_core.h"
#include "crash_upload_queue.h"
#include "telemetry_transport.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace cr = meridian::crash;
namespace tel = meridian::telemetry;
namespace fs = std::filesystem;

static int g_fail = 0;
static void check(const char *name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

static bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// Minimal per-line JSON well-formedness check (balanced braces/quotes), same as
// the #168 test — enough to prove serialize_crash_envelope emits parseable lines.
static bool json_object_well_formed(const std::string &line) {
	if (line.empty()) return true;
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

static bool all_lines_well_formed(const std::string &env) {
	std::size_t pos = 0;
	while (pos < env.size()) {
		std::size_t nl = env.find('\n', pos);
		std::string line = (nl == std::string::npos) ? env.substr(pos) : env.substr(pos, nl - pos);
		pos = (nl == std::string::npos) ? env.size() : nl + 1;
		if (!json_object_well_formed(line)) return false;
	}
	return true;
}

static cr::CrashReport make_report() {
	cr::CrashReport r;
	r.signal_number = 11;   // SIGSEGV
	r.fault_address = 0xdeadbeef;
	r.timestamp_ms = 1720000000123ull;
	r.context.session_id = "ephemeral-abc123";
	r.context.build = "meridian-client 0.1.0+abcd";
	r.context.platform = "macos-arm64";
	r.frames = {0x1000, 0x2000, 0x3abc};
	return r;
}

// Unique temp dir for the queue tests.
static std::string make_temp_dir() {
	static int seq = 0;
	fs::path base = fs::temp_directory_path() /
	    ("meridian-crash-test-" + std::to_string(reinterpret_cast<uintptr_t>(&seq)) + "-" +
	     std::to_string(seq++));
	std::error_code ec;
	fs::create_directories(base, ec);
	return base.string();
}

static void write_file(const std::string &path, const std::string &content) {
	std::ofstream f(path, std::ios::binary);
	f << content;
}

int main() {
	std::printf("crash-report-core-test\n");

	// (a) report file round-trip.
	{
		cr::CrashReport r = make_report();
		std::string text = cr::serialize_report_file(r);
		cr::CrashReport back;
		bool ok = cr::parse_report_file(text, back);
		check("report file parses", ok);
		check("round-trip signal", back.signal_number == 11);
		check("round-trip signal_name", back.signal_name == std::string("SIGSEGV"));
		check("round-trip addr", back.fault_address == 0xdeadbeef);
		check("round-trip time", back.timestamp_ms == 1720000000123ull);
		check("round-trip session", back.context.session_id == "ephemeral-abc123");
		check("round-trip build", back.context.build == "meridian-client 0.1.0+abcd");
		check("round-trip platform", back.context.platform == "macos-arm64");
		check("round-trip frame count", back.frames.size() == 3);
		check("round-trip frame[2]", back.frames.size() == 3 && back.frames[2] == 0x3abc);
	}

	// (b) malformed / non-magic file rejected.
	{
		cr::CrashReport out;
		check("empty rejected", !cr::parse_report_file("", out));
		check("non-magic rejected", !cr::parse_report_file("hello\nsig 11\n", out));
		check("wrong-version rejected", !cr::parse_report_file("MERIDIAN-CRASH v2\n", out));
	}

	// (c) envelope well-formed + fatal + event_kind crash + no-PII context.
	{
		cr::CrashReport r = make_report();
		std::string env = cr::serialize_crash_envelope(r);
		check("envelope lines well-formed", all_lines_well_formed(env));
		check("envelope sdk header", contains(env, "\"meridian.client.telemetry\""));
		check("envelope level fatal", contains(env, "\"level\":\"fatal\""));
		check("envelope event_kind crash", contains(env, "\"event_kind\":\"crash\""));
		check("envelope logger crash", contains(env, "\"logger\":\"crash\""));
		check("envelope has session tag", contains(env, "\"session_id\":\"ephemeral-abc123\""));
		check("envelope has build tag", contains(env, "\"build\":\"meridian-client 0.1.0+abcd\""));
		check("envelope has platform tag", contains(env, "\"platform\":\"macos-arm64\""));
		check("envelope has frames", contains(env, "\"frames\":") && contains(env, "0x1000"));
		check("envelope has summary", contains(env, "SIGSEGV"));
		// NO PII substrings anywhere (privacy §3) — the structural guarantee.
		for (const char *pii : {"email", "username", "password", "ip_addr", "account", "hwid"}) {
			check((std::string("envelope no PII: ") + pii).c_str(), !contains(env, pii));
		}
	}

	// (d) upload queue: ships pending reports through the mock, deletes delivered.
	{
		std::string dir = make_temp_dir();
		write_file(dir + "/crash-1-11-0.mcrash", cr::serialize_report_file(make_report()));
		cr::CrashReport r2 = make_report();
		r2.signal_number = 6;   // SIGABRT
		write_file(dir + "/crash-1-6-1.mcrash", cr::serialize_report_file(r2));

		cr::CrashReportQueue queue(dir);
		check("queue finds 2 pending", queue.pending_count() == 2);

		tel::MockTransport mock(true);   // endpoint configured → Ok
		cr::UploadSweepResult res = cr::upload_pending(queue, mock);
		check("sweep found 2", res.found == 2);
		check("sweep shipped 2", res.shipped == 2);
		check("mock received 2 envelopes", mock.shipped.size() == 2);
		check("delivered files deleted", queue.pending_count() == 0);
		// The mock actually received crash envelopes.
		check("mock envelope is crash",
		      !mock.shipped.empty() && contains(mock.shipped[0], "\"event_kind\":\"crash\""));

		std::error_code ec; fs::remove_all(dir, ec);
	}

	// (e) a transport Failure KEEPS the file for a later launch.
	{
		std::string dir = make_temp_dir();
		write_file(dir + "/crash-1-11-0.mcrash", cr::serialize_report_file(make_report()));
		cr::CrashReportQueue queue(dir);

		tel::MockTransport mock(true);
		mock.script_results({tel::ShipResult::Failed});
		cr::UploadSweepResult res = cr::upload_pending(queue, mock);
		check("failed sweep found 1", res.found == 1);
		check("failed sweep shipped 0", res.shipped == 0);
		check("failed sweep failed 1", res.failed == 1);
		check("failed report retained", queue.pending_count() == 1);

		std::error_code ec; fs::remove_all(dir, ec);
	}

	// (f) a malformed queued file is dropped (can never succeed).
	{
		std::string dir = make_temp_dir();
		write_file(dir + "/crash-bad.mcrash", "not a crash report\n");
		cr::CrashReportQueue queue(dir);

		tel::MockTransport mock(true);
		cr::UploadSweepResult res = cr::upload_pending(queue, mock);
		check("malformed found 1", res.found == 1);
		check("malformed tallied", res.malformed == 1);
		check("malformed not shipped", mock.shipped.empty());
		check("malformed dropped from queue", queue.pending_count() == 0);

		std::error_code ec; fs::remove_all(dir, ec);
	}

	if (g_fail == 0) {
		std::printf("crash-report-core-test: ALL PASS\n");
		return 0;
	}
	std::printf("crash-report-core-test: %d FAILURE(S)\n", g_fail);
	return 1;
}

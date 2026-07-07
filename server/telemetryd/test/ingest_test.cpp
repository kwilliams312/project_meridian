// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — PURE ingest-contract test (OPS-05 / D-29; issue #167).
//
// No socket, no DB, no network: drives parse_and_validate() / RateLimiter /
// forward_event_json() directly. Proves the whole D-29 privacy contract as pure
// functions, so it runs in the plain `server` job's ctest.
//
// The envelopes below are byte-for-byte what the #168 client
// serialize_envelope() (client/gdextension/meridian/src/telemetry_log_core.cpp)
// produces — the ingest MUST parse exactly this shape. Test matrix (issue "TEST"):
//   (a) a valid #168 batch parses + validates + forwards (sink got it),
//   (b) malformed / oversized → rejected with the right reason, no crash,
//   (c) a PII-shaped field / unknown tag → rejected,
//   (d) rate-limit: a flood → refused after the cap.

#include "ingest.h"

#include <cstdio>
#include <string>

using namespace meridian::telemetryd;

namespace {

int g_fail = 0;
void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ── Build a #168 envelope EXACTLY as client serialize_envelope() does ────────
// Header line, then per event: item-header line + payload line. Kept identical
// to telemetry_log_core.cpp so this test is a faithful cross-check of the wire
// contract (if #168 changes the shape, this test must change with it).
const char* kSdkHeader = "{\"sdk\":{\"name\":\"meridian.client.telemetry\",\"version\":\"1\"}}\n";
const char* kItemHeader = "{\"type\":\"event\",\"content_type\":\"application/json\"}\n";

std::string log_payload(const char* level, const std::string& message, std::uint64_t ts,
                        const char* logger, const std::string& session,
                        const std::string& build, const std::string& platform) {
    std::string p = "{";
    p += std::string("\"level\":\"") + level + "\",";
    p += "\"message\":\"" + message + "\",";
    p += "\"timestamp\":" + std::to_string(ts) + ",";
    p += std::string("\"logger\":\"") + logger + "\",";
    p += "\"tags\":{\"session_id\":\"" + session + "\",\"build\":\"" + build +
         "\",\"platform\":\"" + platform + "\"}";
    p += "}\n";
    return p;
}

// A full valid batch: two events (one error, one fatal) with the drop-summary
// synthetic event appended — the exact structure #168 emits when events were
// dropped by the client rate limiter.
std::string valid_envelope() {
    std::string env = kSdkHeader;
    env += kItemHeader;
    env += log_payload("error", "connect failed: timeout", 1720000000000ULL, "net",
                       "sess-abc123", "meridian-client 0.1.0+abcd", "macos-arm64");
    env += kItemHeader;
    env += log_payload("fatal", "renderer lost device", 1720000000500ULL, "gfx",
                       "sess-abc123", "meridian-client 0.1.0+abcd", "macos-arm64");
    // Drop-summary synthetic event (level "warning" + rate_limited_dropped).
    env += kItemHeader;
    env += "{\"level\":\"warning\",\"message\":\"meridian.telemetry: 7 ERROR/CRITICAL events "
           "dropped by client rate limit\",\"rate_limited_dropped\":7,"
           "\"tags\":{\"session_id\":\"sess-abc123\",\"build\":\"meridian-client 0.1.0+abcd\","
           "\"platform\":\"macos-arm64\"}}\n";
    return env;
}

// A crash envelope EXACTLY as the client serialize_crash_envelope() (#109)
// produces: one "event" item, level "fatal", logger "crash", event_kind "crash",
// a space-joined frames STRING, and the no-PII context tags.
std::string crash_payload(const std::string& session, const std::string& build,
                          const std::string& platform) {
    std::string p = "{";
    p += "\"level\":\"fatal\",";
    p += "\"message\":\"SIGSEGV (signal 11) at 0x0 — 3 frames\",";
    p += "\"timestamp\":1720000000123,";
    p += "\"logger\":\"crash\",";
    p += "\"event_kind\":\"crash\",";
    p += "\"frames\":\"0x1000 0x2000 0x3abc\",";
    p += "\"tags\":{\"session_id\":\"" + session + "\",\"build\":\"" + build +
         "\",\"platform\":\"" + platform + "\"}";
    p += "}\n";
    return p;
}

std::string crash_envelope() {
    std::string env = kSdkHeader;
    env += kItemHeader;
    env += crash_payload("sess-crash", "meridian-client 0.1.0+abcd", "macos-arm64");
    return env;
}

}  // namespace

int main() {
    // ── (a) A valid #168 batch parses, validates, and forwards ───────────────
    std::printf("[a] valid #168 envelope → accepted + parsed + forwardable\n");
    {
        IngestResult r = parse_and_validate(valid_envelope());
        check("accepted", r.accepted());
        check("reason is kNone", r.reason == RejectReason::kNone);
        if (r.batch) {
            check("three events parsed", r.batch->events.size() == 3);
            check("context session_id", r.batch->context.session_id == "sess-abc123");
            check("context build", r.batch->context.build == "meridian-client 0.1.0+abcd");
            check("context platform", r.batch->context.platform == "macos-arm64");
            check("event 0 is error", r.batch->events[0].severity == IngestSeverity::kError);
            check("event 0 message", r.batch->events[0].message == "connect failed: timeout");
            check("event 0 logger", r.batch->events[0].logger == "net");
            check("event 0 timestamp", r.batch->events[0].timestamp_ms == 1720000000000ULL);
            check("event 1 is fatal", r.batch->events[1].severity == IngestSeverity::kFatal);
            check("event 2 is drop-summary",
                  r.batch->events[2].kind == EventKind::kRateLimitDrop);
            check("event 2 drop count", r.batch->events[2].rate_limited_dropped == 7);

            // Forward to the sink (Loki-compatible JSON line) — assert the shape.
            std::string line =
                forward_event_json("reference", r.batch->context, r.batch->events[0]);
            check("sink line has realm", contains(line, "\"realm\":\"reference\""));
            check("sink line has process", contains(line, "\"process\":\"telemetryd\""));
            check("sink line has event name", contains(line, "\"event\":\"client_log_ingest\""));
            check("sink line has severity", contains(line, "\"severity\":\"error\""));
            check("sink line has build", contains(line, "\"build\":\"meridian-client 0.1.0+abcd\""));
            check("sink line has session_id", contains(line, "\"session_id\":\"sess-abc123\""));
            check("sink line has message", contains(line, "connect failed: timeout"));
            // No-PII structural check: the forwarded line carries only the three
            // permitted context fields — none of these ever appear.
            check("sink line has NO email", !contains(line, "email"));
            check("sink line has NO username", !contains(line, "username"));
        }
    }

    // ── (a2) Exact-envelope faithfulness: parse a hand-crafted single event ──
    // that mirrors serialize_envelope() with an escaped message (quotes/newline).
    std::printf("[a2] escaped message round-trips through the JSON reader\n");
    {
        std::string env = kSdkHeader;
        env += kItemHeader;
        // message contains an escaped quote + newline, exactly as #168 escapes.
        env += "{\"level\":\"error\",\"message\":\"bad \\\"value\\\"\\nline2\",\"timestamp\":1,"
               "\"logger\":\"sim\",\"tags\":{\"session_id\":\"s\",\"build\":\"b\","
               "\"platform\":\"p\"}}\n";
        IngestResult r = parse_and_validate(env);
        check("escaped envelope accepted", r.accepted());
        if (r.batch && !r.batch->events.empty()) {
            check("escaped message decoded",
                  r.batch->events[0].message == "bad \"value\"\nline2");
        }
    }

    // ── (b) Malformed / oversized rejected (no crash) ────────────────────────
    std::printf("[b] malformed / oversized → rejected with the right reason\n");
    {
        // Empty body.
        check("empty body → malformed",
              parse_and_validate("").reason == RejectReason::kMalformed);
        // Garbage (not JSON).
        check("garbage → malformed",
              parse_and_validate("this is not json\n").reason == RejectReason::kMalformed);
        // Good header, but a truncated payload line (unbalanced braces).
        check("truncated payload → malformed",
              parse_and_validate(std::string(kSdkHeader) + kItemHeader + "{\"level\":\"error\"\n")
                      .reason == RejectReason::kMalformed);
        // Header only, no items.
        check("header only → empty",
              parse_and_validate(kSdkHeader).reason == RejectReason::kEmpty);
        // Wrong sdk header.
        check("wrong sdk header → bad sdk header",
              parse_and_validate("{\"sdk\":{\"name\":\"evil\"}}\n" + std::string(kItemHeader) +
                                 log_payload("error", "m", 1, "l", "s", "b", "p"))
                      .reason == RejectReason::kBadSdkHeader);
        // Oversized: a body just over the cap, WITHOUT allocating a real batch.
        std::string big(kMaxEnvelopeBytes + 1, 'x');
        check("oversized → too large",
              parse_and_validate(big).reason == RejectReason::kTooLarge);
        // Bad event type in the item header.
        check("bad item type → bad event type",
              parse_and_validate(std::string(kSdkHeader) +
                                 "{\"type\":\"transaction\"}\n" +
                                 log_payload("error", "m", 1, "l", "s", "b", "p"))
                      .reason == RejectReason::kBadEventType);
        // Bad level (info is not shippable — the client never sends it).
        check("info level → bad level",
              parse_and_validate(std::string(kSdkHeader) + kItemHeader +
                                 log_payload("info", "m", 1, "l", "s", "b", "p"))
                      .reason == RejectReason::kBadLevel);
    }

    // ── (c) PII-shaped field / unknown tag rejected ──────────────────────────
    std::printf("[c] a PII-shaped field / unknown tag → rejected\n");
    {
        // A top-level "email" field on the payload.
        std::string pii_top = std::string(kSdkHeader) + kItemHeader +
                              "{\"level\":\"error\",\"message\":\"m\",\"timestamp\":1,"
                              "\"email\":\"a@b.com\",\"tags\":{\"session_id\":\"s\","
                              "\"build\":\"b\",\"platform\":\"p\"}}\n";
        check("payload email field → PII rejected",
              parse_and_validate(pii_top).reason == RejectReason::kPiiField);

        // A PII key inside the tags object (ip_address).
        std::string pii_tag = std::string(kSdkHeader) + kItemHeader +
                             "{\"level\":\"error\",\"message\":\"m\",\"timestamp\":1,"
                             "\"tags\":{\"session_id\":\"s\",\"build\":\"b\",\"platform\":\"p\","
                             "\"ip_address\":\"1.2.3.4\"}}\n";
        check("tags ip_address → PII rejected",
              parse_and_validate(pii_tag).reason == RejectReason::kPiiField);

        // An account_id field (the account is NOT a permitted identifier).
        std::string pii_acct = std::string(kSdkHeader) + kItemHeader +
                              "{\"level\":\"error\",\"message\":\"m\",\"timestamp\":1,"
                              "\"tags\":{\"session_id\":\"s\",\"build\":\"b\",\"platform\":\"p\","
                              "\"account_id\":\"42\"}}\n";
        check("tags account_id → PII rejected",
              parse_and_validate(pii_acct).reason == RejectReason::kPiiField);

        // An unknown (non-PII) tag key → rejected as unknown (allow-list only).
        std::string unknown = std::string(kSdkHeader) + kItemHeader +
                             "{\"level\":\"error\",\"message\":\"m\",\"timestamp\":1,"
                             "\"tags\":{\"session_id\":\"s\",\"build\":\"b\",\"platform\":\"p\","
                             "\"zone\":\"orgrimmar\"}}\n";
        check("unknown tag key → rejected",
              parse_and_validate(unknown).reason == RejectReason::kUnknownTag);

        // Sanity: the permitted context (session_id/build/platform) is NOT PII.
        check("permitted context is accepted",
              parse_and_validate(std::string(kSdkHeader) + kItemHeader +
                                 log_payload("error", "m", 1, "l", "s", "b", "p"))
                      .accepted());
    }

    // ── (d) Rate-limit: a flood is refused after the cap ─────────────────────
    std::printf("[d] rate-limit: a flood → refused after the cap\n");
    {
        RateLimiter::Config rc;
        rc.max_requests = 5;
        rc.window_ms = 1000;
        RateLimiter rl(rc);
        const std::string build = "meridian-client 0.1.0+abcd";
        const std::string ip = "10.0.0.7";

        int allowed = 0, refused = 0;
        for (int i = 0; i < 20; ++i) {
            if (rl.allow(build, ip, /*now_ms=*/100)) ++allowed;  // same window
            else ++refused;
        }
        check("exactly max_requests allowed in a window", allowed == 5);
        check("the rest refused", refused == 15);

        // A different IP is a different key — not throttled by the first IP's flood.
        check("different IP is independent", rl.allow(build, "10.0.0.8", 100));
        // A different build (same IP) is a different key too.
        check("different build is independent", rl.allow("other-build", ip, 100));

        // After the window elapses, the same key is allowed again.
        check("window reset re-allows", rl.allow(build, ip, /*now_ms=*/1200));
        check("one key per (build,ip); 3 keys tracked", rl.tracked_keys() == 3);
    }

    // ── (e) Crash report (#109): classified kCrash, frames forwarded ─────────
    std::printf("[e] #109 crash envelope → accepted + kCrash + frames forwarded\n");
    {
        IngestResult r = parse_and_validate(crash_envelope());
        check("crash envelope accepted", r.accepted());
        if (r.batch) {
            check("one crash event parsed", r.batch->events.size() == 1);
            check("crash event kind is kCrash",
                  r.batch->events[0].kind == EventKind::kCrash);
            check("crash event is fatal", r.batch->events[0].severity == IngestSeverity::kFatal);
            check("crash frames extracted",
                  r.batch->events[0].frames == "0x1000 0x2000 0x3abc");
            check("crash context session", r.batch->context.session_id == "sess-crash");

            // Forwarded sink line names the crash stream + carries the backtrace.
            std::string line =
                forward_event_json("reference", r.batch->context, r.batch->events[0]);
            check("crash sink line event name",
                  contains(line, "\"event\":\"client_crash_ingest\""));
            check("crash sink line has frames",
                  contains(line, "\"frames\":\"0x1000 0x2000 0x3abc\""));
            check("crash sink line has build",
                  contains(line, "\"build\":\"meridian-client 0.1.0+abcd\""));
            // Structural no-PII: only the permitted context fields.
            check("crash sink line NO email", !contains(line, "email"));
        }
    }

    if (g_fail == 0) {
        std::printf("telemetryd-ingest-test: OK (all checks passed)\n");
        return 0;
    }
    std::printf("telemetryd-ingest-test: FAILED (%d checks)\n", g_fail);
    return 1;
}

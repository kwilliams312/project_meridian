// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — client telemetry ingest core implementation (OPS-05 / D-29; #167).
// See ingest.h for the design, the exact #168 envelope shape, and the D-29
// privacy contract this file enforces. Clean-room; no third-party JSON library.

#include "ingest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace meridian::telemetryd {

const char* severity_str(IngestSeverity s) {
    switch (s) {
        case IngestSeverity::kError:   return "error";
        case IngestSeverity::kFatal:   return "fatal";
        case IngestSeverity::kWarning: return "warning";
    }
    return "error";
}

const char* reject_reason_str(RejectReason r) {
    switch (r) {
        case RejectReason::kNone:           return "accepted";
        case RejectReason::kTooLarge:       return "payload too large";
        case RejectReason::kMalformed:      return "malformed envelope";
        case RejectReason::kEmpty:          return "empty envelope";
        case RejectReason::kTooManyEvents:  return "too many events";
        case RejectReason::kBadSdkHeader:   return "bad or missing sdk header";
        case RejectReason::kBadEventType:   return "unsupported event type";
        case RejectReason::kBadLevel:       return "unsupported log level";
        case RejectReason::kPiiField:       return "PII-shaped field rejected";
        case RejectReason::kUnknownTag:     return "unknown tag key rejected";
        case RejectReason::kMissingContext: return "missing session context";
    }
    return "malformed envelope";
}

// ===========================================================================
// A minimal RFC-8259 JSON value scanner (clean-room, hand-rolled).
// ===========================================================================
// Only what the ingest needs: parse ONE JSON object per line into a small tree of
// {string, number, object} — the #168 payload has no arrays and only string /
// integer / nested-object values, so the scanner stays tiny. It is strict: a
// malformed line fails the parse (→ kMalformed), never a partial accept.
namespace {

// A parsed JSON value: either a string, an integer (timestamp / drop count), or
// an object (map of key → value). #168 payloads use no arrays / floats / bools /
// null, so those are not modelled — an unexpected token fails the parse.
struct JsonValue {
    enum class Type { kString, kNumber, kObject } type = Type::kString;
    std::string str;                                  // kString
    std::uint64_t num = 0;                            // kNumber (unsigned int)
    std::vector<std::pair<std::string, JsonValue>> obj;  // kObject (ordered)

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    // Parse a single JSON value; on success `ok_` stays true and pos_ is left at
    // the first non-whitespace char after the value (must be end-of-line for a
    // valid envelope line).
    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        return v;
    }

    bool ok() const { return ok_; }
    bool at_end() const { return pos_ >= s_.size(); }

private:
    void fail() { ok_ = false; }

    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    JsonValue parse_value() {
        if (!ok_) return {};
        char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail();
        return {};
    }

    JsonValue parse_string() {
        JsonValue v;
        v.type = JsonValue::Type::kString;
        v.str = parse_raw_string();
        return v;
    }

    // Parse a JSON string literal, decoding the escapes #168 emits (\" \\ \b \f
    // \n \r \t \uXXXX). Leaves pos_ after the closing quote.
    std::string parse_raw_string() {
        std::string out;
        if (peek() != '"') { fail(); return out; }
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= s_.size()) { fail(); return out; }
                char e = s_[pos_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) { fail(); return out; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else { fail(); return out; }
                        }
                        // Encode the code point as UTF-8 (BMP only — #168 escapes
                        // only control chars < 0x20, so this path stays simple).
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: fail(); return out;
                }
            } else {
                out.push_back(c);
            }
        }
        fail();  // unterminated string
        return out;
    }

    JsonValue parse_number() {
        JsonValue v;
        v.type = JsonValue::Type::kNumber;
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;  // sign tolerated but value is stored unsigned
        std::uint64_t n = 0;
        bool any = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            n = n * 10 + static_cast<std::uint64_t>(s_[pos_] - '0');
            ++pos_;
            any = true;
        }
        // #168 emits only unsigned integer numbers (timestamps, drop counts). A
        // fractional / exponent form is not expected; reject it as malformed.
        if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            fail();
            return v;
        }
        if (!any || s_[start] == '-') {
            // A leading '-' means a negative timestamp/count — not something #168
            // ever emits; treat as malformed rather than silently wrapping.
            if (s_[start] == '-') { fail(); return v; }
        }
        v.num = n;
        return v;
    }

    JsonValue parse_object() {
        JsonValue v;
        v.type = JsonValue::Type::kObject;
        if (peek() != '{') { fail(); return v; }
        ++pos_;  // '{'
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        for (;;) {
            skip_ws();
            if (peek() != '"') { fail(); return v; }
            std::string key = parse_raw_string();
            if (!ok_) return v;
            skip_ws();
            if (peek() != ':') { fail(); return v; }
            ++pos_;  // ':'
            skip_ws();
            JsonValue val = parse_value();
            if (!ok_) return v;
            v.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return v; }
            fail();
            return v;
        }
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// Parse exactly one JSON object from a full line. Fails unless the line is a
// single object consuming the whole line (no trailing garbage).
bool parse_line_object(const std::string& line, JsonValue& out) {
    JsonParser p(line);
    out = p.parse();
    if (!p.ok() || !p.at_end()) return false;
    return out.type == JsonValue::Type::kObject;
}

// ── PII-shaped key detection (defense-in-depth, privacy §3) ──────────────────
// The allow-list of tag keys the client is permitted to send (privacy §2a). A
// key outside this set inside `tags` is rejected as unknown; a key matching a
// PII shape ANYWHERE in a payload object is rejected outright.
bool is_allowed_tag_key(const std::string& k) {
    return k == "session_id" || k == "build" || k == "platform";
}

// A lowercase copy for case-insensitive PII matching.
std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

// True if a key looks like a personal identifier. This is a REJECT trigger, not
// a scrub — the server refuses a payload that carries one (the client must never
// send it; privacy §3). Substring match so "user_email"/"ipAddress"/"account_id"
// etc. are all caught. Deliberately conservative: the permitted tag keys
// (session_id/build/platform) contain none of these substrings.
bool is_pii_key(const std::string& key) {
    static const char* const kPii[] = {
        "email",  "username", "user_name", "realname", "real_name",
        "ip",     "ipaddr",   "ip_addr",   "account",  "userid",
        "user_id", "hwid",    "hardware",  "fingerprint", "macaddr",
        "mac_addr", "phone",  "address",   "geo",      "location",
        "firstname", "lastname", "first_name", "last_name",
    };
    const std::string lk = lower(key);
    for (const char* p : kPii) {
        if (lk.find(p) != std::string::npos) return true;
    }
    return false;
}

// Scan every key in a payload object (and its nested `tags` object) for a
// PII-shaped key. Returns the reject reason (kNone if clean). `tags` keys are
// additionally held to the allow-list.
RejectReason scan_payload_keys(const JsonValue& payload) {
    for (const auto& kv : payload.obj) {
        if (is_pii_key(kv.first)) return RejectReason::kPiiField;
        if (kv.first == "tags" && kv.second.type == JsonValue::Type::kObject) {
            for (const auto& tag : kv.second.obj) {
                if (is_pii_key(tag.first)) return RejectReason::kPiiField;
                if (!is_allowed_tag_key(tag.first)) return RejectReason::kUnknownTag;
            }
        }
    }
    return RejectReason::kNone;
}

// Map a Sentry `level` string to IngestSeverity. Returns false for anything the
// D-29 contract does not permit on the wire (only error/fatal/warning).
bool level_from_str(const std::string& level, IngestSeverity& out) {
    if (level == "error")   { out = IngestSeverity::kError;   return true; }
    if (level == "fatal")   { out = IngestSeverity::kFatal;   return true; }
    if (level == "warning") { out = IngestSeverity::kWarning; return true; }
    return false;
}

// Split the envelope body into lines (on '\n'), dropping a trailing empty line.
// The #168 writer terminates every line with '\n'.
std::vector<std::string> split_lines(const std::string& body) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t nl = body.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(body.substr(start));
            break;
        }
        lines.push_back(body.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

}  // namespace

// ===========================================================================
// parse_and_validate — the whole pure contract.
// ===========================================================================
IngestResult parse_and_validate(const std::string& body) {
    IngestResult res;

    // (1) Size cap BEFORE any per-line work (exhaustion guard, #173).
    if (body.size() > kMaxEnvelopeBytes) {
        res.reason = RejectReason::kTooLarge;
        return res;
    }

    std::vector<std::string> lines = split_lines(body);
    // Strip blank lines (a trailing '\n' produces one; blanks are benign).
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](const std::string& l) {
                                   return l.find_first_not_of(" \t\r") == std::string::npos;
                               }),
                lines.end());

    if (lines.empty()) {
        res.reason = RejectReason::kMalformed;
        return res;
    }

    // (2) Envelope header line: EXACTLY the #168 sdk header. We parse it and
    // require sdk.name == "meridian.client.telemetry".
    JsonValue header;
    if (!parse_line_object(lines[0], header)) {
        res.reason = RejectReason::kMalformed;
        return res;
    }
    const JsonValue* sdk = header.find("sdk");
    if (sdk == nullptr || sdk->type != JsonValue::Type::kObject) {
        res.reason = RejectReason::kBadSdkHeader;
        return res;
    }
    const JsonValue* sdk_name = sdk->find("name");
    if (sdk_name == nullptr || sdk_name->type != JsonValue::Type::kString ||
        sdk_name->str != "meridian.client.telemetry") {
        res.reason = RejectReason::kBadSdkHeader;
        return res;
    }

    // (3) Remaining lines are (item-header, payload) PAIRS.
    ParsedBatch batch;
    bool have_context = false;

    std::size_t i = 1;
    while (i < lines.size()) {
        // Item header.
        JsonValue item;
        if (!parse_line_object(lines[i], item)) {
            res.reason = RejectReason::kMalformed;
            return res;
        }
        const JsonValue* type = item.find("type");
        if (type == nullptr || type->type != JsonValue::Type::kString) {
            res.reason = RejectReason::kMalformed;
            return res;
        }
        // Only "event" items are part of the D-29 client-triple envelope #168
        // emits. (Crash minidumps ship as Sentry "attachment" items when the
        // client emits them; accept that type too so the endpoint is ready, but
        // reject anything else.)
        const bool is_event = (type->str == "event");
        const bool is_attachment = (type->str == "attachment");
        if (!is_event && !is_attachment) {
            res.reason = RejectReason::kBadEventType;
            return res;
        }

        // Payload MUST follow the item header.
        if (i + 1 >= lines.size()) {
            res.reason = RejectReason::kMalformed;
            return res;
        }
        JsonValue payload;
        if (!parse_line_object(lines[i + 1], payload)) {
            res.reason = RejectReason::kMalformed;
            return res;
        }

        // (4) PII / unknown-tag scan on the payload (defense-in-depth).
        RejectReason pii = scan_payload_keys(payload);
        if (pii != RejectReason::kNone) {
            res.reason = pii;
            return res;
        }

        // Cap event count (bounded independent of byte size).
        if (batch.events.size() >= kMaxEventsPerBatch) {
            res.reason = RejectReason::kTooManyEvents;
            return res;
        }

        ParsedEvent ev;

        // level → severity (accepted set only).
        const JsonValue* level = payload.find("level");
        if (level == nullptr || level->type != JsonValue::Type::kString) {
            res.reason = RejectReason::kMalformed;
            return res;
        }
        if (!level_from_str(level->str, ev.severity)) {
            res.reason = RejectReason::kBadLevel;
            return res;
        }

        // Message (required, string).
        const JsonValue* msg = payload.find("message");
        if (msg == nullptr || msg->type != JsonValue::Type::kString) {
            res.reason = RejectReason::kMalformed;
            return res;
        }
        ev.message = msg->str;

        // Optional logger + timestamp.
        if (const JsonValue* lg = payload.find("logger");
            lg != nullptr && lg->type == JsonValue::Type::kString) {
            ev.logger = lg->str;
        }
        if (const JsonValue* ts = payload.find("timestamp");
            ts != nullptr && ts->type == JsonValue::Type::kNumber) {
            ev.timestamp_ms = ts->num;
        }

        // Classify: a payload with rate_limited_dropped is the synthetic drop
        // summary; otherwise a normal log/crash event.
        if (const JsonValue* dropped = payload.find("rate_limited_dropped");
            dropped != nullptr && dropped->type == JsonValue::Type::kNumber) {
            ev.kind = EventKind::kRateLimitDrop;
            ev.rate_limited_dropped = static_cast<std::uint32_t>(dropped->num);
        } else {
            ev.kind = EventKind::kLog;
        }

        // (5) Context from tags (session_id/build/platform). Required on a log
        // event; the tags object is validated PII-clean above.
        const JsonValue* tags = payload.find("tags");
        if (tags == nullptr || tags->type != JsonValue::Type::kObject) {
            res.reason = RejectReason::kMissingContext;
            return res;
        }
        IngestContext ctx;
        if (const JsonValue* sid = tags->find("session_id");
            sid != nullptr && sid->type == JsonValue::Type::kString) {
            ctx.session_id = sid->str;
        }
        if (const JsonValue* bld = tags->find("build");
            bld != nullptr && bld->type == JsonValue::Type::kString) {
            ctx.build = bld->str;
        }
        if (const JsonValue* plat = tags->find("platform");
            plat != nullptr && plat->type == JsonValue::Type::kString) {
            ctx.platform = plat->str;
        }
        if (ctx.session_id.empty() && ctx.build.empty() && ctx.platform.empty()) {
            res.reason = RejectReason::kMissingContext;
            return res;
        }

        // The batch context is taken from the first event; #168 emits the same
        // context on every event of a batch (structural — one SessionContext).
        if (!have_context) {
            batch.context = ctx;
            have_context = true;
        }

        batch.events.push_back(std::move(ev));
        i += 2;  // consumed item-header + payload
    }

    if (batch.events.empty()) {
        res.reason = RejectReason::kEmpty;
        return res;
    }

    res.reason = RejectReason::kNone;
    res.batch = std::move(batch);
    return res;
}

// ===========================================================================
// RateLimiter — fixed-window per (build, IP).
// ===========================================================================
bool RateLimiter::allow(const std::string& build, const std::string& ip,
                        std::uint64_t now_ms) {
    const std::string key = build + '\x1f' + ip;
    std::lock_guard<std::mutex> lk(mtx_);
    Window& w = windows_[key];
    if (now_ms - w.window_start_ms >= cfg_.window_ms || w.count == 0) {
        // New window (or first request for this key): reset.
        if (now_ms - w.window_start_ms >= cfg_.window_ms) {
            w.window_start_ms = now_ms;
            w.count = 0;
        } else if (w.count == 0) {
            w.window_start_ms = now_ms;
        }
    }
    if (w.count >= cfg_.max_requests) {
        return false;  // over the cap → caller answers 429
    }
    ++w.count;
    return true;
}

std::size_t RateLimiter::tracked_keys() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return windows_.size();
}

// ===========================================================================
// Sink — Loki-compatible JSON-lines formatting (M0 stdout sink).
// ===========================================================================
namespace {

void json_escape(std::string& out, const std::string& in) {
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void kv(std::string& out, const char* key, const std::string& val, bool comma) {
    json_escape(out, key);
    out.push_back(':');
    json_escape(out, val);
    if (comma) out.push_back(',');
}

}  // namespace

std::string forward_event_json(const std::string& realm, const IngestContext& ctx,
                               const ParsedEvent& ev) {
    // Loki body shape (telemetry-architecture.md §5.2): realm/process/level are
    // the low-cardinality labels; the rest is body. `event` names the stream.
    std::string out;
    out.reserve(256 + ev.message.size());
    out.push_back('{');
    kv(out, "realm", realm, true);
    kv(out, "process", "telemetryd", true);
    kv(out, "level", severity_str(ev.severity), true);
    kv(out, "event",
       ev.kind == EventKind::kRateLimitDrop ? "client_rate_limit_drop" : "client_log_ingest",
       true);
    kv(out, "severity", severity_str(ev.severity), true);
    kv(out, "build", ctx.build, true);
    kv(out, "platform", ctx.platform, true);
    kv(out, "session_id", ctx.session_id, true);
    kv(out, "logger", ev.logger, true);
    kv(out, "message", ev.message, true);
    json_escape(out, "timestamp_ms");
    out.push_back(':');
    out += std::to_string(ev.timestamp_ms);
    if (ev.kind == EventKind::kRateLimitDrop) {
        out.push_back(',');
        json_escape(out, "rate_limited_dropped");
        out.push_back(':');
        out += std::to_string(ev.rate_limited_dropped);
    }
    out.push_back('}');
    return out;
}

}  // namespace meridian::telemetryd

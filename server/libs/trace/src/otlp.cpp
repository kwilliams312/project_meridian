// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — OTLP/HTTP-JSON span serialization.
// See include/meridian/trace/otlp.h for the wire shape + protojson rules.

#include "meridian/trace/otlp.h"

#include <cmath>
#include <cstdio>

namespace meridian::trace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
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
    return out;
}

namespace {

// A JSON string value: "escaped".
std::string jstr(const std::string& s) { return "\"" + json_escape(s) + "\""; }

// Render a double as a JSON number (protojson: doubleValue is a JSON number;
// non-finite values are not representable, so clamp to 0 defensively).
std::string jnum(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// One OTLP KeyValue: {"key":"k","value":{"<type>Value":...}}.
std::string attr_json(const Attribute& a) {
    std::string v;
    switch (a.type) {
        case Attribute::Type::kString:
            v = "{\"stringValue\":" + jstr(a.str_value) + "}";
            break;
        case Attribute::Type::kInt:
            // protojson: int64 is a STRING (JSON numbers cannot hold full 2^63).
            v = "{\"intValue\":\"" + std::to_string(a.int_value) + "\"}";
            break;
        case Attribute::Type::kBool:
            v = std::string("{\"boolValue\":") + (a.bool_value ? "true" : "false") + "}";
            break;
        case Attribute::Type::kDouble:
            v = "{\"doubleValue\":" + jnum(a.double_value) + "}";
            break;
    }
    return "{\"key\":" + jstr(a.key) + ",\"value\":" + v + "}";
}

std::string attrs_json(const std::vector<Attribute>& attrs) {
    std::string out = "[";
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        if (i) out += ",";
        out += attr_json(attrs[i]);
    }
    out += "]";
    return out;
}

}  // namespace

std::string span_to_otlp_json(const Span& span) {
    std::string out = "{";
    out += "\"traceId\":" + jstr(to_hex(span.trace_id));
    out += ",\"spanId\":" + jstr(to_hex(span.span_id));
    // Root span: all-zero parent is OMITTED (protojson: an empty bytes default is
    // not emitted; the collector treats an absent parentSpanId as a root).
    if (!is_zero(span.parent_span_id)) {
        out += ",\"parentSpanId\":" + jstr(to_hex(span.parent_span_id));
    }
    out += ",\"name\":" + jstr(span.name);
    out += ",\"kind\":" + std::to_string(static_cast<int>(span.kind));
    // uint64 nano timestamps are STRINGS on the wire (protojson uint64 rule).
    out += ",\"startTimeUnixNano\":\"" + std::to_string(span.start_unix_nano) + "\"";
    out += ",\"endTimeUnixNano\":\"" + std::to_string(span.end_unix_nano) + "\"";
    if (!span.attributes.empty()) {
        out += ",\"attributes\":" + attrs_json(span.attributes);
    }
    // Status: emit only when set (an Unset status is the default and omitted).
    if (span.status != StatusCode::kUnset) {
        out += ",\"status\":{\"code\":" + std::to_string(static_cast<int>(span.status));
        if (!span.status_message.empty()) {
            out += ",\"message\":" + jstr(span.status_message);
        }
        out += "}";
    }
    out += "}";
    return out;
}

std::string spans_to_otlp_json(const std::vector<Span>& spans,
                               const std::string& service_name,
                               const std::string& realm) {
    // resource.attributes: service.name + deployment realm.
    std::string resource_attrs =
        "[{\"key\":\"service.name\",\"value\":{\"stringValue\":" + jstr(service_name) + "}}"
        ",{\"key\":\"deployment.environment\",\"value\":{\"stringValue\":" + jstr(realm) + "}}]";

    std::string spans_arr = "[";
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (i) spans_arr += ",";
        spans_arr += span_to_otlp_json(spans[i]);
    }
    spans_arr += "]";

    std::string out = "{\"resourceSpans\":[{";
    out += "\"resource\":{\"attributes\":" + resource_attrs + "}";
    out += ",\"scopeSpans\":[{";
    out += "\"scope\":{\"name\":" + jstr(kScopeName) +
           ",\"version\":" + jstr(kScopeVersion) + "}";
    out += ",\"spans\":" + spans_arr;
    out += "}]";  // scopeSpans[0]
    out += "}]}";  // resourceSpans[0] + root
    return out;
}

}  // namespace meridian::trace

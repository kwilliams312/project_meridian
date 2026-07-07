// SPDX-License-Identifier: Apache-2.0
//
// meridian client SCHEMA/PROTOCOL VERSION COMPAT core implementation (issue #98).
// The engine-free realization of the client SAD §5.1 / server SAD §5.2 compat rule.
// See version_compat.h for the full rule + rationale.

#include "version_compat.h"

namespace meridian::compat {

const char* to_string(CompatVerdict v) {
    switch (v) {
        case CompatVerdict::kCompatible:      return "compatible";
        case CompatVerdict::kClientOutOfDate: return "client-out-of-date";
        case CompatVerdict::kContentMismatch: return "content-mismatch";
        case CompatVerdict::kMalformed:       return "malformed";
    }
    return "?";
}

CompatResult check_compat(const SchemaVersion& local, const SchemaVersion& remote,
                          const CompatPolicy& policy) {
    CompatResult r;

    // ── proto_ver — EXACT match, ALWAYS blocking on any difference (either direction).
    // Majors are incompatible and there is no min-supported floor on the wire at M0, so
    // a newer client and an older client are handled identically: update required.
    if (local.proto_ver != remote.proto_ver) {
        r.verdict = CompatVerdict::kClientOutOfDate;
        r.blocking = true;
        r.detail = (local.proto_ver < remote.proto_ver)
                       ? "client proto_ver older than server — client out of date"
                       : "client proto_ver newer than server — client out of date";
        return r;
    }

    // ── content_hash — compared only when BOTH sides advertise one. An absent hash on
    // either side means the content check is simply not performed (compatible).
    if (local.content_hash.empty() || remote.content_hash.empty()) {
        r.verdict = CompatVerdict::kCompatible;
        r.blocking = false;
        r.detail = "proto match; no content hash advertised (content check skipped)";
        return r;
    }

    // A present hash of the wrong width is ill-formed. Handle it the SAME safe way as a
    // mismatch: blocking only under a hard-fail policy; at M0 warn + proceed (never UB).
    if (policy.content_hash_len != 0 &&
        (local.content_hash.size() != policy.content_hash_len ||
         remote.content_hash.size() != policy.content_hash_len)) {
        r.verdict = CompatVerdict::kMalformed;
        r.blocking = policy.content_hash_hard_fail;
        r.detail = policy.content_hash_hard_fail
                       ? "content hash malformed (bad width) — rejected"
                       : "content hash malformed (bad width) — warning, proceeding (M0)";
        return r;
    }

    // Well-formed on both sides: a byte-for-byte difference is a content mismatch —
    // blocking only under a hard-fail policy (M1+ test realm); a warning at M0–M1.
    if (local.content_hash != remote.content_hash) {
        r.verdict = CompatVerdict::kContentMismatch;
        r.blocking = policy.content_hash_hard_fail;
        r.detail = policy.content_hash_hard_fail
                       ? "content hash mismatch — realm rejected the client's content"
                       : "content hash mismatch — warning, proceeding (M0–M1)";
        return r;
    }

    // Proto matches and content hashes agree: fully compatible.
    r.verdict = CompatVerdict::kCompatible;
    r.blocking = false;
    r.detail = "proto + content hash match";
    return r;
}

}  // namespace meridian::compat

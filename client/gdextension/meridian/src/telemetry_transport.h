// Project Meridian — client telemetry transport seam (issue #168).
//
// The SHIPPING side of the D-29 client log channel. The capture core
// (telemetry_log_core.*) produces a serialized Sentry-compatible envelope; a
// transport POSTs it to the project-hosted ingest endpoint (privacy §2a). This
// header is the SEAM — engine-free, so the doctest ships a MockTransport and the
// GDExtension binding can inject an HTTP transport, exactly like the movement
// controller's IWorldQuery seam (movement_query.h): one interface, a test double
// and a real implementation behind it.
//
// Design rules from the issue + D-29 rule 2:
//   • The endpoint URL is CONFIG-INJECTED — #167 (the ingest) may not exist yet,
//     so the URL is never hard-coded; it is supplied by the client setting.
//   • When no endpoint is configured, a NULL/local sink swallows batches (the
//     "degrade gracefully … no collector configured" posture, privacy §6 /
//     telemetry-architecture §1 principle 5) — telemetry is never a hard
//     dependency of the game running.
//   • The transport MUST NOT block the game loop (issue: "never block the game
//     loop (async/queued)") — real HTTP shipping runs off-thread; this seam
//     only defines the hand-off contract. Back-off on failure is the transport's
//     responsibility; the core reports failures back for retry accounting.

#ifndef MERIDIAN_TELEMETRY_TRANSPORT_H
#define MERIDIAN_TELEMETRY_TRANSPORT_H

#include <string>
#include <vector>

namespace meridian::telemetry {

// Result of one ship attempt. Kept minimal: the pipeline only needs to know
// success vs. retryable failure to drive back-off / retry accounting.
enum class ShipResult : uint8_t {
	Ok       = 0,   // endpoint accepted the batch
	Failed   = 1,   // transient/permanent failure — caller may back off + retry
	NoSink   = 2,   // no endpoint configured — batch dropped locally (not an error)
};

// The shipping seam. Implementations receive a fully serialized envelope
// (already JSON, already the Sentry shape) and are responsible ONLY for getting
// the bytes to the endpoint (or swallowing them if there is no endpoint).
class ITransport {
public:
	virtual ~ITransport() = default;

	// Ship one serialized envelope. `envelope` is the output of
	// serialize_envelope(). Returns the outcome. MUST NOT block the game loop in
	// a real implementation (queue + off-thread); the interface itself is
	// synchronous so the test double is trivial and deterministic.
	virtual ShipResult ship(const std::string &envelope) = 0;

	// True if a real endpoint is configured. When false the pipeline may skip
	// serialization entirely (a micro-optimisation + a clear "no-op sink" signal).
	virtual bool has_endpoint() const = 0;
};

// ---------------------------------------------------------------------------
// NullTransport — the no-op / local sink used when no endpoint is configured.
// ---------------------------------------------------------------------------
// Swallows every batch and reports NoSink. This is the DEFAULT transport so a
// client with no configured ingest URL (or a community realm pointing at none,
// privacy §6) simply never ships — and never crashes for lack of a sink.
class NullTransport final : public ITransport {
public:
	ShipResult ship(const std::string & /*envelope*/) override { return ShipResult::NoSink; }
	bool has_endpoint() const override { return false; }
};

// ---------------------------------------------------------------------------
// MockTransport — the test double the #168 engine-free tests ship batches
// through (issue test (f): "shipped through a MOCK transport (assert the mock
// received the expected batch)").
// ---------------------------------------------------------------------------
// Records every envelope it is handed and lets a test program a scripted result
// sequence (to exercise back-off / retry). NOT compiled into the GDExtension —
// it lives here so both the test and any future integration harness can use it.
class MockTransport final : public ITransport {
public:
	explicit MockTransport(bool endpoint_configured = true)
		: endpoint_configured_(endpoint_configured) {}

	ShipResult ship(const std::string &envelope) override {
		shipped.push_back(envelope);
		if (!scripted_results_.empty()) {
			ShipResult r = scripted_results_[next_result_ % scripted_results_.size()];
			++next_result_;
			return r;
		}
		return endpoint_configured_ ? ShipResult::Ok : ShipResult::NoSink;
	}

	bool has_endpoint() const override { return endpoint_configured_; }

	// Program the result of the next ship() calls (cycled). Used to test back-off.
	void script_results(std::vector<ShipResult> results) {
		scripted_results_ = std::move(results);
		next_result_ = 0;
	}

	// Every serialized envelope handed to ship(), in order — the test inspects
	// these to assert the expected batch (and NO PII) was shipped.
	std::vector<std::string> shipped;

private:
	bool                    endpoint_configured_;
	std::vector<ShipResult> scripted_results_;
	std::size_t             next_result_ = 0;
};

} // namespace meridian::telemetry

#endif // MERIDIAN_TELEMETRY_TRANSPORT_H

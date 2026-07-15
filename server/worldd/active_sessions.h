// SPDX-License-Identifier: Apache-2.0
//
// worldd — single active session per account (issue #326).
//
// CLEAN-ROOM: designed from the server SAD + issues #326/#84 only. §5.3 defines
// the IF-3 grant handoff (authd issues a single-use grant; worldd consumes it on
// enter-world), but grant single-use alone does NOT bound how many sessions an
// account can hold in-world at once: authd can issue a fresh grant while an older
// session is still live (the two grants are different rows), so two sessions can
// both consume a valid grant and both be in-world. No GPL source consulted.
//
// WHAT THIS IS: the account-scoped active-session registry that enforces "at most
// ONE in-world session per account". Enforcement lives HERE, in worldd, at the
// point an account actually goes in-world (right after a grant is consumed), NOT
// in authd — authd is realm-agnostic and does not know which sessions are live.
//
// RESOLUTION POLICY (issue #326, option (b) — KICK-OLD): a second login for an
// account that already has a live session KICKS the existing session and admits
// the new one (WoW-style). This lets a crashed/zombied client reconnect without
// waiting for its stale session to time out — the common M0.5 case. The tradeoff
// vs. option (a) reject-new is documented in the PR.
//
// SINGLE-SESSION GUARANTEE (the CAS/token design): admit() hands each admitted
// session a UNIQUE SessionToken plus a monotonic SessionGeneration and stores both
// as the account's current holder. release() is a COMPARE-AND-REMOVE: it drops the
// account's entry ONLY if the stored token still equals the caller's token. So a
// session that was already kicked (its token has been overwritten by the session
// that replaced it) can NEVER, on its own later teardown, evict the session that
// replaced it — a classic ABA-safe generation guard. This is what makes the
// registry correct under interleaved enter/leave from many IO-worker threads.
//
// THREADING: fully internally synchronized (one mutex). admit()/release() are
// called from IO-worker threads (one per connection). The displaced session's
// KickFn is invoked by admit() OUTSIDE the lock (see admit()), so a kick that
// writes to a socket / leaves the AoI grid never runs under the registry mutex
// and cannot deadlock against a concurrent admit()/release().

#ifndef MERIDIAN_WORLDD_ACTIVE_SESSIONS_H
#define MERIDIAN_WORLDD_ACTIVE_SESSIONS_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace meridian::worldd {

// A per-admission opaque identity. Unique for the process lifetime; release() uses
// equality only. Async consumers that must ORDER admissions use SessionGeneration
// instead of inferring order from this token.
using SessionToken = std::uint64_t;
// Authoritative monotonic admission order, allocated under the same registry lock
// that installs the current holder. World-thread ingress keeps a per-player
// high-water mark so delayed older enters cannot replace a newer admitted session.
using SessionGeneration = std::uint64_t;

// Invoked by admit() (OUTSIDE the registry lock) to evict the account's currently
// live session when a newer login displaces it. Implemented by the serve loop as
// "signal THIS session to disconnect" — set its kicked flag, send Disconnect
// {KICKED}, drop it from the AoI world. Best-effort transport teardown; the
// registry replacement is the authoritative single-session guarantee (the old
// session is no longer the account's holder the instant the new one is admitted).
using KickFn = std::function<void()>;

// Outcome of an admit().
struct AdmitResult {
    SessionToken token = 0;         // this session's holder identity (for release)
    SessionGeneration generation = 0;  // monotonic admission order for async consumers
    bool kicked_previous = false;   // true iff a live session for the account was evicted
};

// The account-keyed single-active-session registry. One per WorldServer, shared
// (thread-safe) across every serve_connection.
class ActiveSessionRegistry {
public:
    ActiveSessionRegistry() = default;

    ActiveSessionRegistry(const ActiveSessionRegistry&) = delete;
    ActiveSessionRegistry& operator=(const ActiveSessionRegistry&) = delete;

    // Admit `account_id` as its single live in-world session. If the account
    // already holds a live session, its `kick` (the one registered by THAT
    // session) is invoked to evict it and the new session replaces it as the
    // holder. `kick` is the NEW-nothing/OLD-eviction callback the CALLER of this
    // admit registers for ITS OWN future eviction — i.e. the callback stored under
    // this admission and fired when a LATER login displaces this session.
    //
    // Returns this admission's token (pass it to release() on teardown) and
    // whether a previous session was kicked. The displaced session's kick runs
    // AFTER the registry lock is released, so it may safely write to a socket /
    // touch the AoI world without risking a deadlock against the registry.
    AdmitResult admit(std::uint64_t account_id, KickFn kick);

    // Compare-and-remove: drop `account_id`'s entry IFF it is still held by
    // `token`. Returns true if this call removed the entry (the caller was still
    // the holder), false if the account was already re-held by a newer session
    // (this caller had been kicked) or was not present. A kicked-old session's
    // teardown therefore NEVER evicts the session that replaced it.
    bool release(std::uint64_t account_id, SessionToken token);

    // Authoritative current-holder check for asynchronous simulation ingress.
    // Both opaque identity and monotonic generation must still match the account.
    bool is_current(std::uint64_t account_id, SessionToken token,
                    SessionGeneration generation) const;

    // Test/diagnostic: number of accounts with a live session.
    std::size_t active_count() const;

    // Test/diagnostic: is `account_id` currently holding a live session?
    bool is_active(std::uint64_t account_id) const;

private:
    struct Entry {
        SessionToken token = 0;
        SessionGeneration generation = 0;
        KickFn kick;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::uint64_t, Entry> by_account_;
    SessionToken next_token_ = 1;
    SessionGeneration next_generation_ = 1;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_ACTIVE_SESSIONS_H

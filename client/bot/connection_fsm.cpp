// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — CLIENT connection state machine implementation (#96). Pure
// FSM: no I/O, no clock. See connection_fsm.h for the state graph + the honest M0
// reconnect reality (worldd has no server-side session resume; the working path is
// a full re-login for a fresh grant).

#include "connection_fsm.h"

#include <algorithm>
#include <cmath>

namespace meridian::bot {

const char* to_string(ConnState s) {
    switch (s) {
        case ConnState::kDisconnected:   return "Disconnected";
        case ConnState::kConnecting:     return "Connecting";
        case ConnState::kAuthenticating: return "Authenticating";
        case ConnState::kEnteringWorld:  return "EnteringWorld";
        case ConnState::kInWorld:        return "InWorld";
        case ConnState::kReconnecting:   return "Reconnecting";
        case ConnState::kFailed:         return "Failed";
    }
    return "?";
}

const char* to_string(ConnEvent e) {
    switch (e) {
        case ConnEvent::kConnect:          return "Connect";
        case ConnEvent::kConnected:        return "Connected";
        case ConnEvent::kAuthOk:           return "AuthOk";
        case ConnEvent::kEntered:          return "Entered";
        case ConnEvent::kDropped:          return "Dropped";
        case ConnEvent::kReconnectAttempt: return "ReconnectAttempt";
        case ConnEvent::kReconnectFailed:  return "ReconnectFailed";
        case ConnEvent::kGaveUp:           return "GaveUp";
        case ConnEvent::kFatal:            return "Fatal";
        case ConnEvent::kReset:            return "Reset";
    }
    return "?";
}

const char* to_string(ReconnectStrategy s) {
    switch (s) {
        case ReconnectStrategy::kResumeWithGrant: return "ResumeWithGrant";
        case ReconnectStrategy::kFullRelogin:     return "FullRelogin";
    }
    return "?";
}

std::uint32_t BackoffPolicy::delay_for_attempt(std::uint32_t n) const {
    // base * factor^n, clamped to [0, max_delay_ms]. Computed in double then
    // clamped so a large n cannot overflow the exponent before the cap applies.
    double d = static_cast<double>(base_delay_ms) * std::pow(factor, static_cast<double>(n));
    if (!(d >= 0.0)) d = 0.0;  // NaN / negative guard
    if (d > static_cast<double>(max_delay_ms)) d = static_cast<double>(max_delay_ms);
    return static_cast<std::uint32_t>(d);
}

bool ConnectionFsm::should_give_up(std::uint64_t elapsed_reconnect_ms) const {
    if (policy_.max_attempts != 0 && attempts_ >= policy_.max_attempts) return true;
    if (policy_.window_ms != 0 && elapsed_reconnect_ms >= policy_.window_ms) return true;
    return false;
}

void ConnectionFsm::schedule_next_backoff() {
    next_backoff_ms_ = policy_.delay_for_attempt(attempts_);
}

void ConnectionFsm::begin_reconnect_episode() {
    state_ = ConnState::kReconnecting;
    attempts_ = 0;
    schedule_next_backoff();
}

bool ConnectionFsm::dispatch(ConnEvent ev, std::uint64_t elapsed_reconnect_ms) {
    // A Reset from ANY state returns to a clean Disconnected (new session / teardown).
    if (ev == ConnEvent::kReset) {
        state_ = ConnState::kDisconnected;
        attempts_ = 0;
        next_backoff_ms_ = 0;
        // ever_in_world_ is intentionally sticky across a Reset only if the caller
        // wants a fresh machine — but Reset means "new session", so clear it.
        ever_in_world_ = false;
        return true;
    }

    // A Fatal from any live/handshaking/reconnecting state is terminal. (Ignored if
    // already Failed or cleanly Disconnected — nothing to fail.)
    if (ev == ConnEvent::kFatal) {
        if (state_ == ConnState::kFailed || state_ == ConnState::kDisconnected) return false;
        state_ = ConnState::kFailed;
        next_backoff_ms_ = 0;
        return true;
    }

    switch (state_) {
        case ConnState::kDisconnected:
            if (ev == ConnEvent::kConnect) {
                state_ = ConnState::kConnecting;
                return true;
            }
            return false;

        case ConnState::kConnecting:
            if (ev == ConnEvent::kConnected) {
                state_ = ConnState::kAuthenticating;
                return true;
            }
            if (ev == ConnEvent::kDropped) {
                // Lost the connection before authenticating. This is NOT an
                // in-world drop → a plain connect failure, not a resume case.
                state_ = ConnState::kDisconnected;
                return true;
            }
            return false;

        case ConnState::kAuthenticating:
            if (ev == ConnEvent::kAuthOk) {
                state_ = ConnState::kEnteringWorld;
                return true;
            }
            if (ev == ConnEvent::kDropped) {
                state_ = ConnState::kDisconnected;
                return true;
            }
            return false;

        case ConnState::kEnteringWorld:
            if (ev == ConnEvent::kEntered) {
                state_ = ConnState::kInWorld;
                ever_in_world_ = true;
                return true;
            }
            if (ev == ConnEvent::kDropped) {
                // Dropped mid-handshake. If we had ever been in-world this is a
                // reconnect episode; if not (first entry failing), fall back to
                // Disconnected so the driver can decide (grant reject is Fatal).
                if (ever_in_world_) {
                    begin_reconnect_episode();
                } else {
                    state_ = ConnState::kDisconnected;
                }
                return true;
            }
            return false;

        case ConnState::kInWorld:
            if (ev == ConnEvent::kDropped) {
                begin_reconnect_episode();
                return true;
            }
            return false;

        case ConnState::kReconnecting:
            if (ev == ConnEvent::kReconnectAttempt) {
                // A backoff timer fired; a resume/relogin attempt is beginning.
                // First check the budget — if spent, give up now rather than
                // starting a doomed attempt.
                if (should_give_up(elapsed_reconnect_ms)) {
                    state_ = ConnState::kFailed;
                    next_backoff_ms_ = 0;
                    return true;
                }
                ++attempts_;
                next_backoff_ms_ = 0;  // an attempt is in flight; no delay pending
                return true;
            }
            if (ev == ConnEvent::kEntered) {
                // The reconnect attempt succeeded — resumed / re-entered the world.
                state_ = ConnState::kInWorld;
                ever_in_world_ = true;
                next_backoff_ms_ = 0;
                return true;
            }
            if (ev == ConnEvent::kReconnectFailed) {
                // The attempt failed. Give up if the budget is now spent; else
                // schedule the next backoff and stay Reconnecting.
                if (should_give_up(elapsed_reconnect_ms)) {
                    state_ = ConnState::kFailed;
                    next_backoff_ms_ = 0;
                } else {
                    schedule_next_backoff();
                }
                return true;
            }
            if (ev == ConnEvent::kGaveUp) {
                state_ = ConnState::kFailed;
                next_backoff_ms_ = 0;
                return true;
            }
            return false;

        case ConnState::kFailed:
            // Terminal. Only Reset (handled above) leaves it.
            return false;
    }
    return false;
}

}  // namespace meridian::bot

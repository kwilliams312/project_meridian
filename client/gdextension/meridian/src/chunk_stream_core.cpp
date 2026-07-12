// Project Meridian — engine-free CHUNK STREAMER core (issue #555 Story B +
// issue #556 Story C). See chunk_stream_core.h for the full contract. This file
// holds the pure streaming policy: world→cell floor division, the Chebyshev
// desired bands (full-detail ring + proxy far-ring), the per-tick state machine
// (reconcile → poll → enter → instance), priority-ordered load dispatch, the
// gapless proxy↔full swap, and the time-sliced instancing budget shared across
// proxy and full. NO Godot — every engine primitive is a call through
// IStreamBackend.

#include "chunk_stream_core.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace meridian::stream {

const char *chunk_state_name(ChunkState s) {
	switch (s) {
		case ChunkState::Unloaded:  return "unloaded";
		case ChunkState::Loading:   return "loading";
		case ChunkState::Ready:     return "ready";
		case ChunkState::Instanced: return "instanced";
		case ChunkState::Failed:    return "failed";
	}
	return "?";
}

const char *rep_name(Rep r) {
	switch (r) {
		case Rep::None:  return "none";
		case Rep::Proxy: return "proxy";
		case Rep::Full:  return "full";
	}
	return "?";
}

void ChunkStreamer::configure(const StreamZone &zone) {
	zone_ = zone;
	chunks_.clear();
	chunks_.reserve(zone.chunks.size());
	for (const StreamChunk &c : zone.chunks) {
		Slot s;
		s.chunk = c;
		s.shown = Rep::None;
		s.loading = Rep::None;
		s.load_state = ChunkState::Unloaded;
		chunks_.push_back(std::move(s));
	}
	// A fresh zone invalidates any prior player cell; the next set_player_position
	// re-establishes it.
	has_player_ = false;
	last_instanced_ = 0;
}

void ChunkStreamer::world_to_cell(double world_x, double world_z,
                                  int &out_cx, int &out_cz) const {
	// Cell (0,0)'s minimum corner is the zone origin; each cell is chunk_size_m
	// wide. floor division handles NEGATIVE cells (world left/below the origin)
	// correctly — e.g. origin -384, size 128: world -450 → floor(-66/128) = -1.
	const int size = zone_.chunk_size_m > 0 ? zone_.chunk_size_m : 1;
	const double fx = (world_x - zone_.origin_x) / static_cast<double>(size);
	const double fz = (world_z - zone_.origin_z) / static_cast<double>(size);
	out_cx = static_cast<int>(std::floor(fx));
	out_cz = static_cast<int>(std::floor(fz));
}

void ChunkStreamer::set_player_position(double world_x, double world_z) {
	world_to_cell(world_x, world_z, pcx_, pcz_);
	has_player_ = true;
}

int ChunkStreamer::chebyshev(int cx, int cz) const {
	const int dx = cx - pcx_;
	const int dz = cz - pcz_;
	return std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz);
}

bool ChunkStreamer::within_ring(int cx, int cz) const {
	return chebyshev(cx, cz) <= active_radius();
}

Rep ChunkStreamer::desired_rep(const StreamChunk &c) const {
	if (!has_player_) return Rep::None;
	const int d = chebyshev(c.cx, c.cz);
	if (d <= active_radius()) return Rep::Full;                  // full-detail ring
	if (d <= active_far_ring()) return c.has_proxy ? Rep::Proxy  // proxy far-ring band …
	                                               : Rep::None;  // … unless proxy:null (C3)
	return Rep::None;                                            // beyond the far-ring
}

const std::string &ChunkStreamer::path_for(const StreamChunk &c, Rep rep) {
	return rep == Rep::Proxy ? c.proxy_path : c.scene_path;
}

ChunkState ChunkStreamer::derived_state(const Slot &s) {
	if (s.shown != Rep::None) return ChunkState::Instanced;   // anything visible wins
	if (s.loading != Rep::None) return s.load_state;          // Loading/Ready/Failed
	return ChunkState::Unloaded;
}

int ChunkStreamer::chunk_id_at(int cx, int cz) const {
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		if (chunks_[i].chunk.cx == cx && chunks_[i].chunk.cz == cz) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

ChunkStreamer::ChunkView ChunkStreamer::view(std::size_t id) const {
	ChunkView v;
	if (id < chunks_.size()) {
		const Slot &s = chunks_[id];
		v.cx = s.chunk.cx;
		v.cz = s.chunk.cz;
		v.priority = s.chunk.priority;
		v.state = derived_state(s);
		v.shown = s.shown;
		v.loading = s.loading;
	}
	return v;
}

ChunkState ChunkStreamer::state_at(int cx, int cz) const {
	const int id = chunk_id_at(cx, cz);
	return id < 0 ? ChunkState::Unloaded
	              : derived_state(chunks_[static_cast<std::size_t>(id)]);
}

Rep ChunkStreamer::shown_rep_at(int cx, int cz) const {
	const int id = chunk_id_at(cx, cz);
	return id < 0 ? Rep::None : chunks_[static_cast<std::size_t>(id)].shown;
}

Rep ChunkStreamer::desired_rep_at(int cx, int cz) const {
	const int id = chunk_id_at(cx, cz);
	return id < 0 ? Rep::None : desired_rep(chunks_[static_cast<std::size_t>(id)].chunk);
}

bool ChunkStreamer::is_desired(int cx, int cz) const {
	if (!has_player_) return false;
	if (chunk_id_at(cx, cz) < 0) return false;
	return within_ring(cx, cz);
}

bool ChunkStreamer::is_proxy_desired(int cx, int cz) const {
	const int id = chunk_id_at(cx, cz);
	return id >= 0 &&
	       desired_rep(chunks_[static_cast<std::size_t>(id)].chunk) == Rep::Proxy;
}

std::size_t ChunkStreamer::count_state(ChunkState s) const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (derived_state(slot) == s) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::loading_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.loading != Rep::None && slot.load_state == ChunkState::Loading) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::ready_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.loading != Rep::None && slot.load_state == ChunkState::Ready) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::instanced_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.shown != Rep::None) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::proxy_instanced_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.shown == Rep::Proxy) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::full_instanced_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.shown == Rep::Full) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::resident_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.shown != Rep::None || slot.loading != Rep::None) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::desired_count() const {
	if (!has_player_) return 0;
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (within_ring(slot.chunk.cx, slot.chunk.cz)) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::proxy_desired_count() const {
	if (!has_player_) return 0;
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (desired_rep(slot.chunk) == Rep::Proxy) ++n;
	}
	return n;
}

int ChunkStreamer::tick(int instancing_budget) {
	last_instanced_ = 0;
	if (!backend_ || !has_player_) {
		return 0;
	}

	// A small ordering key: most urgent first — lower priority number, then closer
	// to the player (Chebyshev, then Manhattan), then a stable coord tiebreak.
	auto order_key = [this](const Slot &slot) {
		const int dx = slot.chunk.cx - pcx_;
		const int dz = slot.chunk.cz - pcz_;
		const int adx = dx < 0 ? -dx : dx;
		const int adz = dz < 0 ? -dz : dz;
		const int cheb = std::max(adx, adz);
		const int manh = adx + adz;
		// (priority, chebyshev, manhattan, cx, cz)
		return std::make_tuple(slot.chunk.priority, cheb, manh, slot.chunk.cx, slot.chunk.cz);
	};

	// ── 1. RECONCILE — abandon wrong-target loads; recycle reps no longer wanted ──
	// A load whose target no longer matches the chunk's wanted representation (the
	// player moved, or a swap direction reversed) is released. A SHOWN representation
	// is recycled here ONLY when nothing is wanted at the cell — a swap keeps the old
	// rep visible until the new one instances (phase 4), so there is no visible gap.
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		const Rep want = desired_rep(slot.chunk);

		if (slot.loading != Rep::None && slot.loading != want) {
			// Only Loading/Ready loads hold an engine handle to drop; a Failed load
			// already resolved — just clear it so a later ENTER can retry the new want.
			if (slot.load_state == ChunkState::Loading || slot.load_state == ChunkState::Ready) {
				backend_->release_load(static_cast<int>(i), slot.loading,
				                       path_for(slot.chunk, slot.loading));
			}
			slot.loading = Rep::None;
			slot.load_state = ChunkState::Unloaded;
		}

		if (slot.shown != Rep::None && want == Rep::None) {
			backend_->recycle(static_cast<int>(i), slot.shown);   // pooled deferred unload
			slot.shown = Rep::None;
			++total_recycled_;
		}
	}

	// ── 2. POLL — advance in-flight loads requested on PRIOR ticks ──────────────
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		if (slot.loading == Rep::None || slot.load_state != ChunkState::Loading) continue;
		const LoadPoll p = backend_->poll_load(static_cast<int>(i), slot.loading,
		                                        path_for(slot.chunk, slot.loading));
		if (p == LoadPoll::Ready) {
			slot.load_state = ChunkState::Ready;
		} else if (p == LoadPoll::Failed) {
			slot.load_state = ChunkState::Failed;
			++total_failures_;
		}
	}

	// ── 3. ENTER — begin async loads for a wanted rep not yet shown or loading ──
	// Covers both bands: a full mesh for a chunk entering the radius AND a proxy for
	// a chunk entering the far-ring band. PRIORITY-ordered so the centre streams first.
	std::vector<std::size_t> to_load;
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		const Rep want = desired_rep(slot.chunk);
		if (want == Rep::None) continue;
		if (slot.shown == want) continue;         // already showing what we want
		if (slot.loading != Rep::None) continue;  // a load toward `want` is already under way
		to_load.push_back(i);
	}
	std::sort(to_load.begin(), to_load.end(),
	          [&](std::size_t a, std::size_t b) {
		          return order_key(chunks_[a]) < order_key(chunks_[b]);
	          });
	for (std::size_t idx : to_load) {
		Slot &slot = chunks_[idx];
		const Rep want = desired_rep(slot.chunk);
		backend_->request_load(static_cast<int>(idx), want, path_for(slot.chunk, want));
		slot.loading = want;
		slot.load_state = ChunkState::Loading;
		++total_loads_;
	}

	// ── 4. INSTANCE — time-slice: instance up to `instancing_budget` ready reps ──
	// Proxy AND full instancings share the ONE budget (the ≤ 50 ms hitch gate covers
	// all streaming instancing). Instancing the swap target and recycling the old rep
	// happen together so the swap is gapless.
	if (instancing_budget <= 0) {
		return 0;   // no instancing slots this tick
	}
	std::vector<std::size_t> ready;
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		if (slot.loading == Rep::None || slot.load_state != ChunkState::Ready) continue;
		if (slot.loading != desired_rep(slot.chunk)) continue;   // no longer wanted — guard
		ready.push_back(i);
	}
	std::sort(ready.begin(), ready.end(),
	          [&](std::size_t a, std::size_t b) {
		          return order_key(chunks_[a]) < order_key(chunks_[b]);
	          });
	int instanced = 0;
	for (std::size_t idx : ready) {
		if (instanced >= instancing_budget) break;
		Slot &slot = chunks_[idx];
		const Rep newrep = slot.loading;
		backend_->instantiate(static_cast<int>(idx), newrep, path_for(slot.chunk, newrep));
		// Gapless swap: the new representation is in — recycle the one it replaces.
		if (slot.shown != Rep::None && slot.shown != newrep) {
			backend_->recycle(static_cast<int>(idx), slot.shown);
			++total_recycled_;
			++total_swaps_;
		}
		slot.shown = newrep;
		slot.loading = Rep::None;
		slot.load_state = ChunkState::Unloaded;
		++instanced;
		++total_instanced_;
	}
	last_instanced_ = instanced;
	return instanced;
}

}  // namespace meridian::stream

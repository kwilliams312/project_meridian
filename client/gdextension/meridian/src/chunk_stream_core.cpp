// Project Meridian — engine-free CHUNK STREAMER core (issue #555, Epic #22
// Story B). See chunk_stream_core.h for the full contract. This file holds the
// pure streaming policy: world→cell floor division, the Chebyshev desired-ring,
// the per-tick state machine (leave → poll → enter → instance), priority-ordered
// load dispatch, and the time-sliced instancing budget. NO Godot — every engine
// primitive is a call through IStreamBackend.

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

void ChunkStreamer::configure(const StreamZone &zone) {
	zone_ = zone;
	chunks_.clear();
	chunks_.reserve(zone.chunks.size());
	for (const StreamChunk &c : zone.chunks) {
		Slot s;
		s.chunk = c;
		s.state = ChunkState::Unloaded;
		chunks_.push_back(std::move(s));
	}
	// A fresh zone invalidates any prior player cell; the next set_player_position
	// re-establishes it. (Keep has_player_ so a configure-then-tick without a new
	// position still uses the last known cell — but coords may be meaningless, so
	// reset to be safe.)
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

bool ChunkStreamer::within_ring(int cx, int cz) const {
	const int r = active_radius();
	const int dx = cx - pcx_;
	const int dz = cz - pcz_;
	const int cheb = std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz);
	return cheb <= r;
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
		v.state = s.state;
	}
	return v;
}

ChunkState ChunkStreamer::state_at(int cx, int cz) const {
	const int id = chunk_id_at(cx, cz);
	return id < 0 ? ChunkState::Unloaded : chunks_[static_cast<std::size_t>(id)].state;
}

bool ChunkStreamer::is_desired(int cx, int cz) const {
	if (!has_player_) return false;
	if (chunk_id_at(cx, cz) < 0) return false;
	return within_ring(cx, cz);
}

std::size_t ChunkStreamer::count_state(ChunkState s) const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.state == s) ++n;
	}
	return n;
}

std::size_t ChunkStreamer::resident_count() const {
	std::size_t n = 0;
	for (const Slot &slot : chunks_) {
		if (slot.state == ChunkState::Loading || slot.state == ChunkState::Ready ||
		    slot.state == ChunkState::Instanced) {
			++n;
		}
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

	// ── 1. LEAVE — chunks no longer in the ring release/recycle their resources ──
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		const bool desired = within_ring(slot.chunk.cx, slot.chunk.cz);
		if (desired) continue;
		switch (slot.state) {
			case ChunkState::Instanced:
				backend_->recycle(static_cast<int>(i));   // pooled deferred unload
				slot.state = ChunkState::Unloaded;
				++total_recycled_;
				break;
			case ChunkState::Loading:
			case ChunkState::Ready:
				backend_->release_load(static_cast<int>(i), slot.chunk.scene_path);
				slot.state = ChunkState::Unloaded;
				break;
			case ChunkState::Failed:
				slot.state = ChunkState::Unloaded;   // clear so a later re-entry retries
				break;
			case ChunkState::Unloaded:
				break;
		}
	}

	// ── 2. POLL — advance in-flight loads requested on PRIOR ticks ──────────────
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		if (slot.state != ChunkState::Loading) continue;
		const LoadPoll p = backend_->poll_load(static_cast<int>(i), slot.chunk.scene_path);
		if (p == LoadPoll::Ready) {
			slot.state = ChunkState::Ready;
		} else if (p == LoadPoll::Failed) {
			slot.state = ChunkState::Failed;
			++total_failures_;
		}
	}

	// ── 3. ENTER — begin async loads for newly-desired chunks, PRIORITY-ordered ──
	std::vector<std::size_t> to_load;
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		if (slot.state != ChunkState::Unloaded) continue;
		if (!within_ring(slot.chunk.cx, slot.chunk.cz)) continue;
		to_load.push_back(i);
	}
	std::sort(to_load.begin(), to_load.end(),
	          [&](std::size_t a, std::size_t b) {
		          return order_key(chunks_[a]) < order_key(chunks_[b]);
	          });
	for (std::size_t idx : to_load) {
		Slot &slot = chunks_[idx];
		backend_->request_load(static_cast<int>(idx), slot.chunk.scene_path);
		slot.state = ChunkState::Loading;
		++total_loads_;
	}

	// ── 4. INSTANCE — time-slice: instance up to `instancing_budget` ready chunks ─
	if (instancing_budget <= 0) {
		return 0;   // no instancing slots this tick
	}
	std::vector<std::size_t> ready;
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		Slot &slot = chunks_[i];
		if (slot.state != ChunkState::Ready) continue;
		if (!within_ring(slot.chunk.cx, slot.chunk.cz)) continue;   // left while ready — leave handled it, but guard
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
		backend_->instantiate(static_cast<int>(idx), slot.chunk.scene_path);
		slot.state = ChunkState::Instanced;
		++instanced;
		++total_instanced_;
	}
	last_instanced_ = instanced;
	return instanced;
}

}  // namespace meridian::stream

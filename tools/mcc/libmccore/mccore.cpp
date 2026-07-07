// tools/mcc/libmccore/mccore.cpp — the libmccore C ABI implementation (#125).
//
// This is the ABI EDGE: a thin C-linkage wrapper that reuses the existing mcc
// pipeline stages (mcc::stages::check / index_content / pickable_content /
// refs_content and the #127 IdIndex) and translates C++ <-> C at the boundary.
//
// Two hard rules of the edge, enforced by MCCORE_ABI_GUARD below:
//   1. NO C++ exception may escape into C. Every entry point wraps its body in a
//      try/catch that turns any throw into a status code + NULL/no-op return.
//   2. NO C++ type crosses the boundary. std::string results are copied into
//      malloc'd char* buffers (freed by mccore_free); the workspace is an opaque
//      heap object the caller only ever sees as an MccWorkspace*.
//
// The queries mirror the CLI's `--json` output byte-for-byte because they call
// the SAME stage functions the CLI's cmd_* handlers call (SAD §6.4: the editor
// and CI can never disagree).

#include "mccore.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <sstream>
#include <string>

#include "stages/check.h"
#include "stages/discover.h"
#include "stages/diagnostics.h"
#include "stages/index.h"
#include "stages/link.h"
#include "stages/model.h"
#include "stages/parse.h"
#include "stages/validate.h"

#ifndef MCC_VERSION
#define MCC_VERSION "0.0.0"
#endif

namespace {

// The opaque workspace: just the content ROOT path. Queries re-run the relevant
// read-only stage over it, so the object is trivially cheap to hold open and a
// query always reflects the content on disk (an editor re-opens after a build).
// (A future revision can cache a parsed ContentModel here for incremental
// validation — SAD §6.3 — behind the same ABI; callers never see the change.)
struct Workspace {
    std::string content_root;
};

// Copy a std::string into a malloc'd, NUL-terminated C buffer the caller frees
// with mccore_free(). Returns nullptr only on allocation failure. Using malloc
// (not new[]) so mccore_free()'s free() matches the allocator — a P/Invoke
// caller who accidentally frees with the C runtime's free() still matches.
char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

void set_status(int* out_status, MccStatus s) {
    if (out_status) *out_status = s;
}

// Map an mcc stage exit code (0 ok / 1 content-error / 2 not-found) to MccStatus.
MccStatus status_from_rc(int rc) {
    switch (rc) {
        case 0:  return MCCORE_OK;
        case 1:  return MCCORE_ERR_CONTENT;
        case 2:  return MCCORE_ERR_NOT_FOUND;
        default: return MCCORE_ERR_INTERNAL;
    }
}

// Build the read-only ID index over a content root (mirrors index.cpp's
// build_index_ro, which is file-local there). Returns 0 on success (out_index
// filled), 1 if the corpus has errors, 2 if the dir can't be scanned. No disk
// writes: link runs with allocate=false, so idmap.lock is never touched.
int build_index_ro(const std::string& content_root, mcc::stages::IdIndex& out_index) {
    mcc::model::ContentModel model;
    if (!mcc::stages::discover(content_root, model)) return 2;
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    const mcc::stages::LinkResult linked = mcc::stages::link(
        model, content_root, /*allocate=*/false, diags, /*emit_dangling=*/false);
    if (!diags.ok()) return 1;
    out_index = mcc::stages::IdIndex::build(model, linked);
    return 0;
}

// Run one of the JSON-producing stage functions over the workspace root, capture
// its output, and translate the C++ result into the C ABI (a malloc'd JSON string
// + a status). This is the wall exceptions do not cross (rule 1): any throw is
// caught here and turned into MCCORE_ERR_INTERNAL + a NULL return. `run` is the
// stage call `(root, out, err) -> int rc` (0 ok / 1 content-error / 2 not-found);
// `err_content_returns_json` says whether an rc-1 result still carries a useful
// document to return (validate: yes; index/pickable/backlinks: no).
template <typename Fn>
char* run_json_query(Workspace* ws, int* out_status, bool err_content_returns_json,
                     Fn&& run) {
    try {
        std::ostringstream out, err;
        const int rc = run(ws->content_root, out, err);
        if (rc == 2) { set_status(out_status, MCCORE_ERR_NOT_FOUND); return nullptr; }
        if (rc == 1 && !err_content_returns_json) {
            set_status(out_status, MCCORE_ERR_CONTENT);
            return nullptr;
        }
        set_status(out_status, status_from_rc(rc));
        return dup_cstr(out.str());
    } catch (...) {
        set_status(out_status, MCCORE_ERR_INTERNAL);
        return nullptr;
    }
}

}  // namespace

extern "C" {

int mccore_abi_version(void) { return MCCORE_ABI_VERSION; }

char* mccore_version_string(void) {
    // noexcept edge: dup_cstr can only fail by returning nullptr (no throw).
    try {
        return dup_cstr("mcc " MCC_VERSION);
    } catch (...) {
        return nullptr;
    }
}

MccWorkspace* mccore_workspace_open(const char* content_root, int* out_status) {
    if (!content_root || content_root[0] == '\0') {
        set_status(out_status, MCCORE_ERR_USAGE);
        return nullptr;
    }
    try {
        // Verify the root is scannable up front so open() fails fast with a clear
        // status, matching the CLI's rc-2 "content directory not found".
        mcc::model::ContentModel probe;
        if (!mcc::stages::discover(content_root, probe)) {
            set_status(out_status, MCCORE_ERR_NOT_FOUND);
            return nullptr;
        }
        Workspace* ws = new Workspace{std::string(content_root)};
        set_status(out_status, MCCORE_OK);
        return reinterpret_cast<MccWorkspace*>(ws);
    } catch (...) {
        set_status(out_status, MCCORE_ERR_INTERNAL);
        return nullptr;
    }
}

void mccore_workspace_close(MccWorkspace* ws) {
    // No status; deletion cannot throw for our POD-ish Workspace, but stay safe.
    try {
        delete reinterpret_cast<Workspace*>(ws);
    } catch (...) {
        // Swallow — a destructor throwing across the C boundary is never allowed.
    }
}

char* mccore_validate(MccWorkspace* ws, int* out_status) {
    if (!ws) { set_status(out_status, MCCORE_ERR_USAGE); return nullptr; }
    // validate's rc-1 (content has errors) STILL yields a useful diagnostics
    // document that lists them — return it, and report the error via the status.
    return run_json_query(
        reinterpret_cast<Workspace*>(ws), out_status, /*err_content_returns_json=*/true,
        [](const std::string& root, std::ostream& out, std::ostream& err) {
            return mcc::stages::check(root, mcc::stages::DiagFormat::Json, out, err);
        });
}

char* mccore_index_json(MccWorkspace* ws, int* out_status) {
    if (!ws) { set_status(out_status, MCCORE_ERR_USAGE); return nullptr; }
    // An errored corpus makes the index untrustworthy → rc-1 returns NULL.
    return run_json_query(
        reinterpret_cast<Workspace*>(ws), out_status, /*err_content_returns_json=*/false,
        [](const std::string& root, std::ostream& out, std::ostream& err) {
            return mcc::stages::index_content(root, /*as_json=*/true, out, err);
        });
}

char* mccore_pickable_json(MccWorkspace* ws, const char* ref_type, int* out_status) {
    if (!ws || !ref_type) { set_status(out_status, MCCORE_ERR_USAGE); return nullptr; }
    const std::string rt(ref_type);
    return run_json_query(
        reinterpret_cast<Workspace*>(ws), out_status, /*err_content_returns_json=*/false,
        [&rt](const std::string& root, std::ostream& out, std::ostream& err) {
            return mcc::stages::pickable_content(root, rt, /*as_json=*/true, out, err);
        });
}

char* mccore_backlinks_json(MccWorkspace* ws, const char* id, int* out_status) {
    if (!ws || !id) { set_status(out_status, MCCORE_ERR_USAGE); return nullptr; }
    const std::string qid(id);
    return run_json_query(
        reinterpret_cast<Workspace*>(ws), out_status, /*err_content_returns_json=*/false,
        [&qid](const std::string& root, std::ostream& out, std::ostream& err) {
            return mcc::stages::refs_content(root, qid, /*as_json=*/true, out, err);
        });
}

int mccore_resolve(MccWorkspace* ws, const char* id, uint32_t* out_numeric_id,
                   char** out_type) {
    if (!ws || !id) return MCCORE_ERR_USAGE;
    // out_status is not a param here; the return value carries the status.
    try {
        const std::string& root = reinterpret_cast<Workspace*>(ws)->content_root;
        mcc::stages::IdIndex index;
        const int rc = build_index_ro(root, index);
        if (rc == 2) return MCCORE_ERR_NOT_FOUND;
        if (rc == 1) return MCCORE_ERR_CONTENT;
        const auto entry = index.resolve(std::string(id));
        if (!entry) return 0;  // unknown id — a valid, non-error answer
        if (out_numeric_id) *out_numeric_id = entry->numeric_id;
        if (out_type) {
            char* t = dup_cstr(entry->type);
            if (!t) return MCCORE_ERR_INTERNAL;
            *out_type = t;
        }
        return 1;  // resolved
    } catch (...) {
        return MCCORE_ERR_INTERNAL;
    }
}

void mccore_free(char* s) {
    std::free(s);  // free(nullptr) is a safe no-op (C standard)
}

}  // extern "C"

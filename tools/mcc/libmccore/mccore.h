/*
 * tools/mcc/libmccore/mccore.h — the libmccore C ABI (Tools SAD §2 "libmccore",
 * §6 Codex, TLS-03/TLS-04, issue #125).
 *
 * WHAT THIS IS
 * ------------
 * A stable, C-linkage (`extern "C"`) surface over the mcc pipeline's core
 * query/validation stages, built as a shared library (libmccore.{dylib,so,dll}).
 * It lets the Codex editor (.NET 8 / Avalonia, #124) drive parse+validate+lint
 * and query the ID index / typed pickers / backlinks (#127) IN-PROCESS via
 * P/Invoke — without shelling out to the `mcc` CLI (SAD §6: "in-process
 * `libmccore` (C ABI) for anything interactive, subprocess `mcc` for builds").
 *
 * It REUSES the exact stage code the CLI uses (mcc::stages::check /
 * index_content / pickable_content / refs_content and the #127 IdIndex) — there
 * is no reimplementation, so the editor and CI can never disagree (SAD §6.4).
 *
 * ABI CONTRACT (the rules that make this callable across the C boundary)
 * ---------------------------------------------------------------------
 *  1. C linkage, C types only. No C++ types, no exceptions, no RTTI cross the
 *     boundary. Every entry point is `noexcept` at the edge: C++ exceptions are
 *     caught inside and turned into MCCORE_* error codes (see mccore_status).
 *  2. Opaque handles. `MccWorkspace*` is an opaque pointer; callers never see
 *     its layout. Open with mccore_workspace_open(), release with
 *     mccore_workspace_close(). A handle is NOT thread-safe for concurrent use;
 *     use one handle per thread or serialize access (SAD §7: all libmccore
 *     calls off the UI thread).
 *  3. Strings are UTF-8, NUL-terminated `const char*` in, heap `char*` out.
 *     OWNERSHIP: every non-null `char*` RETURNED by a query function is owned by
 *     the caller and MUST be released with mccore_free(). Passing a pointer NOT
 *     obtained from a libmccore query to mccore_free() is undefined. Input
 *     strings are borrowed for the duration of the call only (libmccore copies
 *     what it needs).
 *  4. Determinism. The JSON documents returned mirror the CLI's `--json` output
 *     byte-for-byte (same stage code), so they are stable editor contracts:
 *       - validate  -> schema "mcc-diagnostics@1"
 *       - index     -> schema "meridian/id-index@1"
 *       - pickable  -> schema "meridian/pickable@1"
 *       - backlinks -> schema "meridian/backlinks@1"
 *  5. Versioning (SAD §8 TS-4: "ABI version handshake; Codex refuses mismatched
 *     libmccore"). mccore_abi_version() returns a monotonic integer. Codex
 *     compares it against MCCORE_ABI_VERSION (this header's value at build time)
 *     and refuses to load a library whose runtime version differs, prompting an
 *     update instead of risking a layout/semantics mismatch.
 *
 * P/INVOKE MAPPING (.NET 8, C# — no full project shipped, this is the contract)
 * ----------------------------------------------------------------------------
 * `mccore.h` maps directly onto .NET source-generated P/Invoke. Suggested
 * signatures for Meridian.Codex/Services/LibMcCore (SAD §6 file map):
 *
 *   internal static partial class LibMcCore
 *   {
 *       private const string Dll = "mccore"; // resolves libmccore.dylib/.so/.dll
 *
 *       [LibraryImport(Dll)]
 *       internal static partial int mccore_abi_version();
 *
 *       [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
 *       internal static partial string? mccore_version_string();
 *
 *       // Open a content ROOT (the dir that CONTAINS pack dirs, e.g. "content").
 *       // Returns an opaque handle (IntPtr.Zero on failure — check out_status).
 *       [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
 *       internal static partial IntPtr mccore_workspace_open(
 *           string content_root, out int out_status);
 *
 *       [LibraryImport(Dll)]
 *       internal static partial void mccore_workspace_close(IntPtr ws);
 *
 *       // Queries: each returns a heap UTF-8 JSON string the caller must free.
 *       // Marshal manually (IntPtr) so we control the lifetime, then call
 *       // mccore_free — a `string` return would leak (the marshaller frees with
 *       // the WRONG allocator). See MccString helper note below.
 *       [LibraryImport(Dll)]
 *       internal static partial IntPtr mccore_validate(IntPtr ws, out int out_status);
 *
 *       [LibraryImport(Dll)]
 *       internal static partial IntPtr mccore_index_json(IntPtr ws, out int out_status);
 *
 *       [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
 *       internal static partial IntPtr mccore_pickable_json(
 *           IntPtr ws, string ref_type, out int out_status);
 *
 *       [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
 *       internal static partial IntPtr mccore_backlinks_json(
 *           IntPtr ws, string id, out int out_status);
 *
 *       // Resolve an id -> its numeric IF-9 id + grammar type. Returns 1 when
 *       // resolved (out params filled), 0 when unknown, negative on error.
 *       [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
 *       internal static partial int mccore_resolve(
 *           IntPtr ws, string id, out uint out_numeric_id, IntPtr out_type_json);
 *
 *       [LibraryImport(Dll)]
 *       internal static partial void mccore_free(IntPtr s);
 *   }
 *
 *   // Turn a returned IntPtr into a managed string, then free the native buffer:
 *   //   static string? Take(IntPtr p) {
 *   //       if (p == IntPtr.Zero) return null;
 *   //       try { return Marshal.PtrToStringUTF8(p); }
 *   //       finally { LibMcCore.mccore_free(p); }
 *   //   }
 *
 * Every function is `extern "C"` with a C-compatible signature (primitives,
 * pointers, out-params via pointer) precisely so this generator-friendly mapping
 * holds with no shims.
 */

#ifndef MCCORE_H
#define MCCORE_H

#include <stdint.h>   /* uint32_t */
#include <stddef.h>   /* size_t  */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Export/visibility ---------------------------------------------------- */
#if defined(_WIN32)
#  if defined(MCCORE_BUILDING_DLL)
#    define MCCORE_API __declspec(dllexport)
#  else
#    define MCCORE_API __declspec(dllimport)
#  endif
#else
#  define MCCORE_API __attribute__((visibility("default")))
#endif

/* ---- ABI version handshake (SAD §8 TS-4) ---------------------------------- *
 * Bump on ANY breaking change to a signature, the ownership model, or the JSON
 * contract. Codex compares mccore_abi_version() (runtime) against
 * MCCORE_ABI_VERSION (the value it was compiled against) and refuses a mismatch.
 */
#define MCCORE_ABI_VERSION 1

/* ---- Status / error codes (no exceptions cross the boundary) --------------- *
 * Every entry point reports success/failure through one of these. The query
 * functions also write it into their `out_status` out-parameter so a P/Invoke
 * caller gets a NULL return AND a reason in a single call.
 */
typedef enum MccStatus {
    MCCORE_OK            =  0,  /* success */
    MCCORE_ERR_USAGE     = -1,  /* bad argument (null handle, null path, …) */
    MCCORE_ERR_NOT_FOUND = -2,  /* content root cannot be scanned (CLI rc 2) */
    MCCORE_ERR_CONTENT   = -3,  /* content has validation errors (CLI rc 1);
                                   the returned JSON still describes them */
    MCCORE_ERR_INTERNAL  = -4   /* an exception was caught at the ABI edge */
} MccStatus;

/* Opaque workspace handle: an opened, parsed content root. Never dereferenced
 * by the caller. Created by mccore_workspace_open, destroyed by
 * mccore_workspace_close. */
typedef struct MccWorkspace MccWorkspace;

/* --------------------------------------------------------------------------- *
 * Versioning
 * --------------------------------------------------------------------------- */

/* The runtime ABI version of this shared library. Compare against
 * MCCORE_ABI_VERSION at load time; refuse a mismatch (SAD §8 TS-4). */
MCCORE_API int mccore_abi_version(void);

/* A human-readable "mcc <version>" build string, heap-allocated — free with
 * mccore_free(). Never null (returns a static-ish copy). For an About box /
 * diagnostics; NOT the handshake — use mccore_abi_version() for that. */
MCCORE_API char* mccore_version_string(void);

/* --------------------------------------------------------------------------- *
 * Workspace lifecycle
 * --------------------------------------------------------------------------- */

/* Open a content ROOT — the directory that CONTAINS pack directories (e.g.
 * "content", which holds "content/core"). Runs discover+parse over the tree so
 * later queries are answered from the in-memory model. Returns an opaque handle,
 * or NULL on failure; when non-null, `*out_status` (if out_status != NULL) is
 * MCCORE_OK. On NULL it is MCCORE_ERR_USAGE (null/empty path) or
 * MCCORE_ERR_NOT_FOUND (dir cannot be scanned). The handle is valid until
 * mccore_workspace_close(). */
MCCORE_API MccWorkspace* mccore_workspace_open(const char* content_root,
                                               int* out_status);

/* Release a workspace handle and all resources it owns. NULL is a safe no-op.
 * After this call the handle is dangling — do not reuse it. */
MCCORE_API void mccore_workspace_close(MccWorkspace* ws);

/* --------------------------------------------------------------------------- *
 * Queries — each returns a heap UTF-8 JSON string the CALLER must mccore_free().
 * On error they return NULL and set `*out_status` (when out_status != NULL).
 * --------------------------------------------------------------------------- */

/* Validate the whole content root: discover+parse+validate diagnostics as the
 * "mcc-diagnostics@1" JSON envelope (identical to `mcc check --diag-format=json`).
 * A clean corpus yields {"ok":true,...}. `*out_status` is MCCORE_OK when the
 * content validates, MCCORE_ERR_CONTENT when there are validation errors (the
 * returned JSON still lists them — a valid, useful answer), or a negative code
 * for a usage/internal fault (NULL return). */
MCCORE_API char* mccore_validate(MccWorkspace* ws, int* out_status);

/* The full ID index as "meridian/id-index@1" JSON (identical to `mcc index
 * --json`): every id with its grammar type, namespace, IF-9 numeric id, source
 * file, and asset flag, grouped by type. `*out_status` MCCORE_OK on success;
 * MCCORE_ERR_CONTENT when the corpus has errors (the index would be
 * untrustworthy → NULL). */
MCCORE_API char* mccore_index_json(MccWorkspace* ws, int* out_status);

/* Typed reference picker for `ref_type` (npc|item|quest|ability|loot|vendor|
 * zone or art|mus|sfx|amb; the schema `*Ref` $defs — "itemRef" or "item" both
 * accepted) as "meridian/pickable@1" JSON (identical to `mcc pickable <t>
 * --json`): the VALID target ids a picker UI would offer. An unknown ref type is
 * a valid answer (ok:false, empty candidates), NOT an error. */
MCCORE_API char* mccore_pickable_json(MccWorkspace* ws, const char* ref_type,
                                      int* out_status);

/* Backlinks / find-usages for `id` as "meridian/backlinks@1" JSON (identical to
 * `mcc refs <id> --json`): who references it, each with the referencing field's
 * json-path. An unreferenced or unknown id is a valid answer (referrer_count:0),
 * NOT an error — the `resolved` field distinguishes "exists but unreferenced"
 * from "unknown id". */
MCCORE_API char* mccore_backlinks_json(MccWorkspace* ws, const char* id,
                                       int* out_status);

/* Resolve an id (bare or fully-qualified) to its IF-9 numeric id + grammar type.
 * Returns 1 and fills the out-params when resolved, 0 when the id is unknown
 * (out-params untouched), or a negative MccStatus on a usage/internal fault.
 *  - out_numeric_id (may be NULL): the IF-9 runtime numeric id (0 = unmapped).
 *  - out_type       (may be NULL): receives a heap UTF-8 copy of the grammar
 *                   type token (e.g. "item"); the CALLER must mccore_free() it.
 *                   Only written (non-null) on a return value of 1. */
MCCORE_API int mccore_resolve(MccWorkspace* ws, const char* id,
                              uint32_t* out_numeric_id, char** out_type);

/* --------------------------------------------------------------------------- *
 * Memory
 * --------------------------------------------------------------------------- */

/* Free a string returned by any libmccore query (mccore_version_string,
 * mccore_validate, mccore_index_json, mccore_pickable_json,
 * mccore_backlinks_json, or mccore_resolve's out_type). NULL is a safe no-op.
 * Do NOT pass a pointer libmccore did not return. */
MCCORE_API void mccore_free(char* s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* MCCORE_H */

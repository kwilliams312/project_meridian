/*
 * tools/mcc/libmccore/tests/smoke.c — the libmccore C ABI smoke test (#125).
 *
 * Compiled as PURE C (C11) and linked against libmccore, this proves the ABI is
 * genuinely callable across the C boundary: no C++ headers, no C++ types, no
 * name-mangling — just <mccore.h> and the shared library's exported symbols. If
 * the surface were accidentally C++-only this translation unit would not link.
 *
 * It exercises the whole surface against the real `content/` root:
 *   - version handshake (mccore_abi_version, mccore_version_string)
 *   - open/close lifecycle
 *   - validate  -> asserts {"ok": true}
 *   - index     -> asserts the meridian/id-index@1 schema; id_count is checked
 *                  DYNAMICALLY against the `mcc` CLI (ABI↔CLI parity), not a
 *                  frozen literal — see the [index] section for why (#772)
 *   - pickable  -> asserts itemRef yields ok:true; its candidate count is
 *                  likewise cross-checked against the CLI, not a frozen literal
 *   - backlinks -> asserts core:item.kobold_ear has referrer_count:2
 *   - resolve   -> asserts kobold_ear -> numeric_id 17, type "item"
 *   - alloc/free discipline: every returned string is mccore_free()'d; a bulk
 *     alloc/free loop shakes out an obvious leak/double-free.
 * The stable numeric anchors (kobold_ear #17, its 2 referrers) match the
 * committed idmap.lock, and the full-corpus id_count is cross-checked live
 * against the `mcc` CLI, so a drift between the ABI and the CLI fails this test.
 *
 * Returns 0 on success; prints a FAIL line and returns 1 on the first mismatch.
 */

/* popen()/pclose() are POSIX, not ISO C — request the POSIX surface before any
 * header is pulled in so the declarations are visible under -std=c11 -Wpedantic. */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mccore.h"

#ifndef MCCORE_TEST_CONTENT_ROOT
#error "MCCORE_TEST_CONTENT_ROOT must be defined by the build (the content/ root)"
#endif

#ifndef MCC_CLI_EXE
#error "MCC_CLI_EXE must be defined by the build (path to the mcc CLI, for ABI↔CLI parity)"
#endif

/* Windows spells the pipe-open primitives with a leading underscore. */
#ifdef _WIN32
#  define MCC_POPEN  _popen
#  define MCC_PCLOSE _pclose
#else
#  define MCC_POPEN  popen
#  define MCC_PCLOSE pclose
#endif

static int g_failures = 0;

/* A tiny assertion harness: report + count, but keep going so one run shows
 * every failure, then return non-zero at the end. */
static void check(int cond, const char* what) {
    if (cond) {
        printf("  ok   : %s\n", what);
    } else {
        printf("  FAIL : %s\n", what);
        ++g_failures;
    }
}

/* Substring presence — the JSON is a stable, deterministic contract (same stage
 * code as the CLI), so a literal substring check is a sufficient, robust assert
 * without pulling a JSON parser into a C smoke test. */
static int contains(const char* hay, const char* needle) {
    return hay && strstr(hay, needle) != NULL;
}

/* Parse the integer value of a named JSON field (e.g. "id_count" in an index
 * document, "count" in a pickable document). `key` is the quoted key, like
 * "\"id_count\"". Returns the value, or -1 if the field is absent/malformed.
 * Each doc carries exactly one such field, so no JSON parser is needed (same
 * dependency-free discipline as contains()). */
static long json_uint_field(const char* json, const char* key) {
    if (!json) return -1;
    const char* p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p && *p != ':') ++p;           /* skip to the key's colon */
    if (*p != ':') return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;    /* skip whitespace before the number */
    if (*p < '0' || *p > '9') return -1;
    return strtol(p, NULL, 10);
}

/* Count the id entries actually enumerated in an index JSON's "ids" array. Each
 * entry carries exactly one "numeric_id" key, and that key appears nowhere else
 * in the index document, so counting the key counts the ids. Lets us assert the
 * index is self-consistent (id_count == ids actually listed). */
static long count_id_entries(const char* json) {
    if (!json) return -1;
    long n = 0;
    const char* needle = "\"numeric_id\"";
    const size_t nlen = strlen(needle);
    for (const char* p = strstr(json, needle); p != NULL; p = strstr(p + nlen, needle)) {
        ++n;
    }
    return n;
}

/* Run a read-only index-family `mcc` subcommand against the same content root
 * and capture its stdout. `subcmd` is "index" or "pickable"; `arg` is the
 * subcommand's positional argument (the ref-type for pickable) or NULL (index
 * takes none). This is the AUTHORITATIVE source the ABI is checked against: the
 * standalone CLI and libmccore read the SAME content root through the SAME stage
 * code, so their answers must agree — a live parity check that needs no
 * hand-maintained literal. Returns a malloc'd, NUL-terminated buffer (caller
 * frees) or NULL on failure (spawn error or non-zero CLI exit). */
static char* run_cli(const char* subcmd, const char* arg) {
    char cmd[8192];
    /* mcc <subcmd> [arg] <root> --json  (index takes no arg; pickable takes the
     * ref-type first, then the dir). Quote paths so spaces are tolerated. */
    int n;
    if (arg) {
        n = snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\" \"%s\" --json",
                     MCC_CLI_EXE, subcmd, arg, MCCORE_TEST_CONTENT_ROOT);
    } else {
        n = snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\" --json",
                     MCC_CLI_EXE, subcmd, MCCORE_TEST_CONTENT_ROOT);
    }
    if (n < 0 || (size_t)n >= sizeof cmd) return NULL;

    FILE* fp = MCC_POPEN(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 1u << 16, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { MCC_PCLOSE(fp); return NULL; }

    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (len + got + 1 > cap) {
            while (len + got + 1 > cap) cap <<= 1;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); MCC_PCLOSE(fp); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    buf[len] = '\0';

    int rc = MCC_PCLOSE(fp);
    if (rc != 0) { free(buf); return NULL; }  /* CLI must exit 0 for a valid corpus */
    return buf;
}

int main(void) {
    printf("libmccore C ABI smoke test\n");
    printf("content root: %s\n", MCCORE_TEST_CONTENT_ROOT);

    /* ---- Version handshake (SAD §8 TS-4) --------------------------------- */
    printf("\n[version]\n");
    check(mccore_abi_version() == MCCORE_ABI_VERSION,
          "mccore_abi_version() matches header MCCORE_ABI_VERSION");
    char* ver = mccore_version_string();
    check(ver != NULL && contains(ver, "mcc "),
          "mccore_version_string() returns an 'mcc <ver>' string");
    if (ver) printf("         version string: %s\n", ver);
    mccore_free(ver);  /* ownership: caller frees */

    /* ---- Open the content root ------------------------------------------- */
    printf("\n[open]\n");
    int st = 12345;  /* sentinel: must be overwritten */
    MccWorkspace* ws = mccore_workspace_open(MCCORE_TEST_CONTENT_ROOT, &st);
    check(ws != NULL, "mccore_workspace_open() returns a handle");
    check(st == MCCORE_OK, "open status is MCCORE_OK");

    /* Negative: a bogus path must fail cleanly (no crash, clear status). */
    int bad_st = 0;
    MccWorkspace* bad = mccore_workspace_open("/no/such/meridian/content", &bad_st);
    check(bad == NULL && bad_st == MCCORE_ERR_NOT_FOUND,
          "open() of a missing dir returns NULL + MCCORE_ERR_NOT_FOUND");
    mccore_workspace_close(bad);  /* NULL close is a safe no-op */

    /* Negative: null path is a usage error. */
    int null_st = 0;
    check(mccore_workspace_open(NULL, &null_st) == NULL && null_st == MCCORE_ERR_USAGE,
          "open(NULL) returns NULL + MCCORE_ERR_USAGE");

    if (!ws) {  /* nothing more to test without a handle */
        printf("\nno workspace handle — aborting.\n");
        return 1;
    }

    /* ---- validate -> {"ok": true} ---------------------------------------- */
    printf("\n[validate]\n");
    st = 12345;
    char* diag = mccore_validate(ws, &st);
    check(diag != NULL, "mccore_validate() returns a diagnostics document");
    check(st == MCCORE_OK, "validate status is MCCORE_OK (clean corpus)");
    check(contains(diag, "\"schema\": \"mcc-diagnostics@1\""),
          "diagnostics carry the mcc-diagnostics@1 schema");
    check(contains(diag, "\"ok\": true"), "content validates: \"ok\": true");
    check(contains(diag, "\"error_count\": 0"), "error_count is 0");
    mccore_free(diag);

    /* ---- index -> meridian/id-index@1 ------------------------------------ */
    printf("\n[index]\n");
    st = 12345;
    char* idx = mccore_index_json(ws, &st);
    check(idx != NULL && st == MCCORE_OK, "mccore_index_json() returns the index");
    check(contains(idx, "\"schema\": \"meridian/id-index@1\""),
          "index carries the meridian/id-index@1 schema");

    /* id_count is verified DYNAMICALLY, never against a frozen literal (#772): a
     * hardcoded full-corpus count went stale on every content addition and every
     * idmap-reallocating rebase, forcing brittle manual bumps. Instead we assert
     * the invariants that actually matter and let the number float with content:
     *   1. non-empty corpus                    (id_count > 0),
     *   2. the ABI index is self-consistent    (id_count == ids it enumerates),
     *   3. ABI↔CLI PARITY: the count equals what the `mcc` CLI reports for the
     *      SAME content root — the authoritative cross-check this smoke test
     *      exists to guard. Content grows freely; a genuine drift between the
     *      shared library and the CLI still fails the test. */
    long abi_id_count = json_uint_field(idx, "\"id_count\"");
    if (abi_id_count >= 0) printf("         ABI id_count: %ld\n", abi_id_count);
    check(abi_id_count > 0, "index reports a positive id_count");
    check(abi_id_count == count_id_entries(idx),
          "id_count matches the number of ids the ABI enumerates (self-consistent)");
    check(contains(idx, "core:item.kobold_ear"), "index contains core:item.kobold_ear");

    char* cli_idx = run_cli("index", NULL);
    check(cli_idx != NULL, "mcc CLI `index --json` runs (exit 0) and returns output");
    long cli_id_count = json_uint_field(cli_idx, "\"id_count\"");
    if (cli_id_count >= 0) printf("         CLI id_count: %ld\n", cli_id_count);
    check(cli_id_count > 0 && cli_id_count == abi_id_count,
          "ABI id_count matches the mcc CLI id_count for the same content (ABI<->CLI parity)");
    free(cli_idx);
    mccore_free(idx);

    /* ---- pickable(itemRef) -> 28 candidates ------------------------------ */
    printf("\n[pickable]\n");
    st = 12345;
    char* pick = mccore_pickable_json(ws, "itemRef", &st);
    check(pick != NULL && st == MCCORE_OK, "mccore_pickable_json(itemRef) returns candidates");
    check(contains(pick, "\"schema\": \"meridian/pickable@1\""),
          "pickable carries the meridian/pickable@1 schema");
    check(contains(pick, "\"type\": \"item\""), "pickable normalized itemRef -> type item");
    check(contains(pick, "\"ok\": true"), "pickable ok:true");

    /* Same #772 treatment as id_count: the itemRef candidate count is a
     * full-subset count that grows with every item added, so it is checked
     * against the live CLI (ABI↔CLI parity), not a frozen literal. */
    long abi_pick_count = json_uint_field(pick, "\"count\"");
    if (abi_pick_count >= 0) printf("         ABI itemRef count: %ld\n", abi_pick_count);
    check(abi_pick_count > 0, "pickable itemRef reports a positive count");
    char* cli_pick = run_cli("pickable", "itemRef");
    check(cli_pick != NULL, "mcc CLI `pickable itemRef --json` runs (exit 0) and returns output");
    long cli_pick_count = json_uint_field(cli_pick, "\"count\"");
    if (cli_pick_count >= 0) printf("         CLI itemRef count: %ld\n", cli_pick_count);
    check(cli_pick_count > 0 && cli_pick_count == abi_pick_count,
          "ABI itemRef count matches the mcc CLI count for the same content (ABI<->CLI parity)");
    free(cli_pick);
    mccore_free(pick);

    /* An unknown ref type is a valid answer (ok:false), NOT an error. */
    st = 12345;
    char* pick_bad = mccore_pickable_json(ws, "bogusRef", &st);
    check(pick_bad != NULL && st == MCCORE_OK,
          "pickable(unknown type) is a valid answer, not an error");
    check(contains(pick_bad, "\"ok\": false") && contains(pick_bad, "\"count\": 0"),
          "pickable(unknown type) is ok:false / count:0");
    mccore_free(pick_bad);

    /* ---- backlinks(core:item.kobold_ear) -> 2 referrers ------------------ */
    printf("\n[backlinks]\n");
    st = 12345;
    char* back = mccore_backlinks_json(ws, "core:item.kobold_ear", &st);
    check(back != NULL && st == MCCORE_OK, "mccore_backlinks_json() returns backlinks");
    check(contains(back, "\"schema\": \"meridian/backlinks@1\""),
          "backlinks carry the meridian/backlinks@1 schema");
    check(contains(back, "\"resolved\": true"), "kobold_ear resolves");
    check(contains(back, "\"referrer_count\": 2"),
          "kobold_ear has referrer_count 2 (matches mcc refs)");
    check(contains(back, "core:loot.kobold_miner") &&
          contains(back, "core:quest.ears_for_evidence"),
          "backlinks name both referrers (loot + quest)");
    mccore_free(back);

    /* ---- resolve(core:item.kobold_ear) -> #17, type item ----------------- */
    printf("\n[resolve]\n");
    uint32_t numeric = 0;
    char* type = NULL;
    int r = mccore_resolve(ws, "core:item.kobold_ear", &numeric, &type);
    check(r == 1, "mccore_resolve(kobold_ear) returns 1 (resolved)");
    check(numeric == 17, "resolve numeric_id is 17 (matches mcc index)");
    check(type != NULL && strcmp(type, "item") == 0, "resolve type is \"item\"");
    if (type) printf("         resolved -> numeric=%u type=%s\n", numeric, type);
    mccore_free(type);  /* ownership: caller frees the out_type buffer */

    /* Unknown id -> 0 (not an error), out-params untouched. */
    uint32_t un = 999;
    char* utype = (char*)0x1;  /* sentinel: must NOT be written on a 0 return */
    int ur = mccore_resolve(ws, "core:item.does_not_exist", &un, &utype);
    check(ur == 0, "resolve(unknown id) returns 0");
    check(un == 999 && utype == (char*)0x1,
          "resolve(unknown id) leaves out-params untouched");

    /* Null-out-param tolerance: resolve must not require the out-params. */
    check(mccore_resolve(ws, "core:item.kobold_ear", NULL, NULL) == 1,
          "resolve tolerates NULL out-params");

    /* ---- alloc/free discipline (shake out leaks/double-frees) ------------ */
    printf("\n[alloc/free]\n");
    int churn_ok = 1;
    for (int i = 0; i < 1000; ++i) {
        int cs = 0;
        char* s = mccore_index_json(ws, &cs);
        if (!s || cs != MCCORE_OK) { churn_ok = 0; }
        mccore_free(s);  /* free every allocation; a leak would show under ASan */
    }
    check(churn_ok, "1000x index alloc/free round-trips succeed");
    mccore_free(NULL);  /* NULL free is a safe no-op */

    /* Using a workspace after close is not tested (it is UB by contract); we do
     * test that close() of a valid handle is clean and NULL-close is a no-op. */
    mccore_workspace_close(ws);
    printf("\n[close] workspace closed\n");

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

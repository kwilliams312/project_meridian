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
 *   - index     -> asserts the meridian/id-index@1 schema + expected ids
 *   - pickable  -> asserts itemRef yields count:20 / ok:true
 *   - backlinks -> asserts core:item.kobold_ear has referrer_count:2
 *   - resolve   -> asserts kobold_ear -> numeric_id 17, type "item"
 *   - alloc/free discipline: every returned string is mccore_free()'d; a bulk
 *     alloc/free loop shakes out an obvious leak/double-free.
 * The numeric expectations match the committed idmap.lock (`mcc index/pickable/
 * refs`), so a drift between the ABI and the CLI fails this test.
 *
 * Returns 0 on success; prints a FAIL line and returns 1 on the first mismatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mccore.h"

#ifndef MCCORE_TEST_CONTENT_ROOT
#error "MCCORE_TEST_CONTENT_ROOT must be defined by the build (the content/ root)"
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
    check(contains(idx, "\"id_count\": 132"), "index reports id_count 132 (matches mcc index)");
    check(contains(idx, "core:item.kobold_ear"), "index contains core:item.kobold_ear");
    mccore_free(idx);

    /* ---- pickable(itemRef) -> 20 candidates ------------------------------ */
    printf("\n[pickable]\n");
    st = 12345;
    char* pick = mccore_pickable_json(ws, "itemRef", &st);
    check(pick != NULL && st == MCCORE_OK, "mccore_pickable_json(itemRef) returns candidates");
    check(contains(pick, "\"schema\": \"meridian/pickable@1\""),
          "pickable carries the meridian/pickable@1 schema");
    check(contains(pick, "\"type\": \"item\""), "pickable normalized itemRef -> type item");
    check(contains(pick, "\"ok\": true"), "pickable ok:true");
    check(contains(pick, "\"count\": 20"), "pickable itemRef count is 20 (matches mcc pickable)");
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

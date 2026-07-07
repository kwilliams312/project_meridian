// tools/mcc/tests/test_check_file.cpp — unit/integration tests for the
// single-file / validation-as-you-type path (`mcc check --file`, SAD §6.3).
// Self-contained (no GoogleTest), matching test_format.cpp's style.
//
// Covers the task's required scenarios:
//   (a) a valid file (no cross-file refs) -> no errors, exit 0
//   (a2) a valid file WITH cross-file refs -> no errors, refs deferred as info
//   (b) a schema/structure-invalid file -> a JSON diagnostic with the right rule
//       id + "error" severity, exit 1
//   (c) single-file mode on a cross-file ref -> graceful (deferred info, NOT a
//       false L011 error)
//   (d) the JSON envelope is stable/parseable (well-formed, expected keys)
//   + watch_file re-validates on change (bounded via a keep_running predicate)
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "stages/check.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool cond, const std::string& name, const std::string& detail) {
    ++g_checks;
    if (cond) {
        std::cout << "  ok   " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL " << name << "\n";
        if (!detail.empty()) std::cout << "       " << detail << "\n";
    }
}

// Write `content` to `path`, creating parent dirs.
void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

// ---- A tiny, dependency-free JSON reader for assertions --------------------
// We only need to (1) confirm the buffer parses as balanced JSON and (2) pull a
// few flat scalar fields out of the top-level object and the diagnostics array.
// This is deliberately minimal — enough to prove the shape is stable/parseable
// without pulling in a JSON library (the repo's minimal-dep ethos).

// Return the value text for top-level `"key":` (string or bare token), or "".
std::string json_field(const std::string& js, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    std::size_t p = js.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    while (p < js.size() && (js[p] == ' ' || js[p] == '\n')) ++p;
    if (p >= js.size()) return "";
    if (js[p] == '"') {  // string value
        std::size_t q = js.find('"', p + 1);
        return q == std::string::npos ? "" : js.substr(p + 1, q - p - 1);
    }
    std::size_t q = p;  // bare token: number / true / false, up to a delimiter
    while (q < js.size() && js[q] != ',' && js[q] != '\n' && js[q] != '}') ++q;
    std::string v = js.substr(p, q - p);
    while (!v.empty() && (v.back() == ' ')) v.pop_back();
    return v;
}

// True if every '{' '[' is balanced by a matching close (quotes/escapes aware).
bool json_balanced(const std::string& js) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : js) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && !in_str;
}

// Count occurrences of a substring (used to count diagnostics of a rule).
int count_substr(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

using mcc::stages::DiagFormat;

}  // namespace

int main() {
    std::cout << "mcc check --file unit/integration tests\n";

    fs::path tmp = fs::temp_directory_path() / "mcc_check_file_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // A pack root so paths look like a real content tree (single-file mode does
    // NOT read it — that is the point — but it makes rel-path context realistic).
    const fs::path content_root = tmp / "content";
    write_file(content_root / "pack.yaml",
               "schema: meridian/pack@1\nnamespace: core\n");

    // --- (a) valid file, no refs: no errors, exit 0 -------------------------
    std::cout << "[a] valid file -> no diagnostics, exit 0\n";
    {
        const fs::path f = content_root / "widget.item.yaml";
        write_file(f, "schema: meridian/item@1\nid: core:item.widget\nname: Widget\n");
        std::ostringstream out, err;
        int rc = mcc::stages::check_file(f.string(), content_root.string(),
                                         DiagFormat::Json, out, err);
        const std::string js = out.str();
        report(rc == 0, "valid file: exit 0", "rc=" + std::to_string(rc));
        report(json_field(js, "ok") == "true", "valid file: ok=true", js);
        report(json_field(js, "error_count") == "0", "valid file: 0 errors", js);
    }

    // --- (b) structure-invalid file -> right rule id + error severity -------
    std::cout << "[b] invalid file -> JSON diagnostic, right rule + severity, exit 1\n";
    {
        // L001: filename says npc, envelope declares item.
        const fs::path f = content_root / "bad.npc.yaml";
        write_file(f, "schema: meridian/item@1\nid: core:npc.bad\nname: Bad\n");
        std::ostringstream out, err;
        int rc = mcc::stages::check_file(f.string(), content_root.string(),
                                         DiagFormat::Json, out, err);
        const std::string js = out.str();
        report(rc == 1, "invalid file: exit 1", "rc=" + std::to_string(rc));
        report(json_field(js, "ok") == "false", "invalid file: ok=false", js);
        bool has_l001 = js.find("\"rule\": \"L001\"") != std::string::npos;
        bool is_error = js.find("\"severity\": \"error\"") != std::string::npos;
        report(has_l001 && is_error, "invalid file: L001 error diagnostic present", js);
    }
    {
        // PARSE: malformed YAML carries a 1-based line and error severity.
        const fs::path f = content_root / "broken.npc.yaml";
        write_file(f, "schema: meridian/npc@1\nid: : : broken\n  x\n");
        std::ostringstream out, err;
        int rc = mcc::stages::check_file(f.string(), content_root.string(),
                                         DiagFormat::Json, out, err);
        const std::string js = out.str();
        report(rc == 1 && js.find("\"rule\": \"PARSE\"") != std::string::npos,
               "broken YAML: PARSE error, exit 1", "rc=" + std::to_string(rc) + " " + js);
        // line is a positive integer (not 0) — an editor can place a squiggle.
        report(json_field(js, "line") != "0" && !json_field(js, "line").empty(),
               "broken YAML: line reported (>0)", js);
    }

    // --- (c) cross-file ref -> graceful (deferred info, NOT a false error) --
    std::cout << "[c] cross-file ref -> graceful degradation (info, not error)\n";
    {
        const fs::path f = content_root / "with_ref.quest.yaml";
        write_file(f,
                   "schema: meridian/quest@1\n"
                   "id: core:quest.chain\n"
                   "name: Chain\n"
                   "giver: npc.some_npc_in_another_file\n"
                   "objectives:\n"
                   "  - type: kill\n"
                   "    target: npc.also_elsewhere\n"
                   "    count: 3\n");
        std::ostringstream out, err;
        int rc = mcc::stages::check_file(f.string(), content_root.string(),
                                         DiagFormat::Json, out, err);
        const std::string js = out.str();
        // The two npc refs must NOT be errors; they are deferred info notes.
        report(rc == 0, "cross-file ref: exit 0 (no false error)",
               "rc=" + std::to_string(rc) + " " + js);
        report(json_field(js, "error_count") == "0",
               "cross-file ref: 0 errors", js);
        int l011_info = count_substr(js, "\"rule\": \"L011\", \"severity\": \"info\"");
        report(l011_info == 2, "cross-file ref: both refs are L011 info notes",
               "L011-info=" + std::to_string(l011_info) + " " + js);
        // The json-path (`where`) points at the offending field (editor mapping).
        report(js.find("\"where\": \"$.giver\"") != std::string::npos,
               "cross-file ref: json-path 'where' present for editor mapping", js);
    }

    // --- (d) JSON envelope is stable / parseable ----------------------------
    std::cout << "[d] JSON envelope stable + parseable\n";
    {
        const fs::path f = content_root / "widget.item.yaml";  // reuse valid file
        std::ostringstream out, err;
        mcc::stages::check_file(f.string(), content_root.string(),
                                DiagFormat::Json, out, err);
        const std::string js = out.str();
        report(json_balanced(js), "envelope: balanced JSON", js);
        report(json_field(js, "schema") == "mcc-diagnostics@1",
               "envelope: schema tag 'mcc-diagnostics@1'", js);
        report(json_field(js, "mode") == "file", "envelope: mode 'file'", js);
        // All four count/flag envelope fields present.
        report(!json_field(js, "ok").empty() &&
                   !json_field(js, "error_count").empty() &&
                   !json_field(js, "warning_count").empty() &&
                   !json_field(js, "info_count").empty(),
               "envelope: ok/error_count/warning_count/info_count all present", js);
    }

    // --- unknown filename -> L001 (graceful, not a crash) -------------------
    std::cout << "[e] unknown filename -> L001\n";
    {
        const fs::path f = content_root / "notcontent.txt.yaml";
        write_file(f, "schema: whatever\n");
        std::ostringstream out, err;
        int rc = mcc::stages::check_file(f.string(), content_root.string(),
                                         DiagFormat::Text, out, err);
        report(rc == 1 && out.str().find("L001") != std::string::npos,
               "unknown filename: L001 error", "rc=" + std::to_string(rc) + " " + out.str());
    }

    // --- missing file -> usage error (exit 2) -------------------------------
    std::cout << "[f] missing file -> exit 2\n";
    {
        std::ostringstream out, err;
        int rc = mcc::stages::check_file((content_root / "nope.item.yaml").string(),
                                         content_root.string(), DiagFormat::Json, out, err);
        report(rc == 2 && !err.str().empty(), "missing file: exit 2 + stderr msg",
               "rc=" + std::to_string(rc));
    }

    // --- watch_file re-validates on change (bounded loop) -------------------
    std::cout << "[g] watch_file re-validates on change\n";
    {
        const fs::path f = content_root / "watched.npc.yaml";
        write_file(f, "schema: meridian/npc@1\nid: core:npc.w\nname: W\n");
        std::ostringstream out, err;
        std::atomic<int> ticks{0};
        // Stop after we have observed enough loop iterations to catch the edit.
        auto keep = [&ticks]() -> bool { return ticks.fetch_add(1) < 40; };

        std::thread mutator([&f]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            // Introduce an L001 error on the second validation.
            write_file(f, "schema: meridian/item@1\nid: core:npc.w\nname: W\n");
        });
        int rc = mcc::stages::watch_file(f.string(), content_root.string(),
                                         DiagFormat::Json, out, err, keep, /*poll_ms=*/20);
        mutator.join();
        const std::string streamed = out.str();
        // Two renders: the initial OK and the post-edit L001 error.
        int renders = count_substr(streamed, "\"schema\": \"mcc-diagnostics@1\"");
        report(rc == 0, "watch: returns 0", "rc=" + std::to_string(rc));
        report(renders >= 2, "watch: re-validated after change (>=2 renders)",
               "renders=" + std::to_string(renders));
        report(streamed.find("\"rule\": \"L001\"") != std::string::npos,
               "watch: streamed the post-edit L001 error", streamed);
    }

    fs::remove_all(tmp, ec);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed";
    if (g_failures) {
        std::cout << " — " << g_failures << " FAILED\n";
        return 1;
    }
    std::cout << " — ALL PASS\n";
    return 0;
}

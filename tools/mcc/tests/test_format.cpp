// tools/mcc/tests/test_format.cpp — unit tests for the `mcc fmt` canonical
// emitter. Self-contained (no GoogleTest) to keep the hermetic build's
// dependency surface at just yaml-cpp, matching the repo's minimal-dep ethos.
//
// Proves the four contract properties from format.h / FORMAT.md:
//   (a) idempotency         — format(format(x)) == format(x)
//   (b) semantic preservation — parse(format(x)) is node-identical to parse(x)
//   (c) --check drift        — format_file(check) returns 1 on non-canonical input
//   (d) ugly->canonical      — known-ugly fixtures format to a known-canonical form
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "stages/format.h"

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

std::string quote_vis(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// ---- Semantic comparison: are two YAML node trees identical in meaning? -----
// Compares structure, keys, resolved scalar values AND resolved types (so a
// string "4.6" is NOT equal to the float 4.6). We normalize each scalar to a
// (type, text) pair using yaml-cpp's resolution.

std::string scalar_signature(const YAML::Node& n) {
    // tag "!" = explicit string; "?"/other = plain (type-inferred); "" = null.
    const std::string tag = n.Tag();
    if (n.IsNull()) return "null";
    if (tag == "!") return "str:" + n.Scalar();  // definitely a string
    // Plain scalar: classify by what it resolves to, mirroring YAML core.
    const std::string& s = n.Scalar();
    // null-ish
    if (s == "null" || s == "Null" || s == "NULL" || s == "~" || s.empty())
        return "null";
    // bool-ish (yaml-cpp resolves these)
    for (const char* b : {"true", "True", "TRUE", "false", "False", "FALSE"})
        if (s == b) return std::string("bool:") + (s[0] == 't' || s[0] == 'T' ? "1" : "0");
    // number-ish: normalize via double so 340 == 340.0 in text-form differences
    // do not matter, but 340 (int) vs "340" (str) DO (str goes through tag "!").
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size()) {
        std::ostringstream os;
        os << "num:" << d;
        return os.str();
    }
    return "str:" + s;
}

bool nodes_equal(const YAML::Node& a, const YAML::Node& b, std::string& why) {
    if (a.Type() != b.Type()) {
        // Null vs plain-null-string edge: treat both as null already handled.
        why = "type mismatch";
        return false;
    }
    switch (a.Type()) {
        case YAML::NodeType::Null:
            return true;
        case YAML::NodeType::Scalar: {
            const std::string sa = scalar_signature(a), sb = scalar_signature(b);
            if (sa != sb) {
                why = "scalar mismatch: [" + sa + "] vs [" + sb + "]";
                return false;
            }
            return true;
        }
        case YAML::NodeType::Sequence: {
            if (a.size() != b.size()) {
                why = "sequence size mismatch";
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!nodes_equal(a[i], b[i], why)) return false;
            return true;
        }
        case YAML::NodeType::Map: {
            if (a.size() != b.size()) {
                why = "map size mismatch";
                return false;
            }
            for (auto it = a.begin(); it != a.end(); ++it) {
                const std::string key = it->first.Scalar();
                if (!b[key]) {
                    why = "missing key: " + key;
                    return false;
                }
                if (!nodes_equal(it->second, b[key], why)) {
                    why = "under key '" + key + "': " + why;
                    return false;
                }
            }
            return true;
        }
        default:
            return true;
    }
}

bool semantically_equal(const std::string& a, const std::string& b, std::string& why) {
    YAML::Node na = YAML::Load(a);
    YAML::Node nb = YAML::Load(b);
    return nodes_equal(na, nb, why);
}

// ---- Property helpers ------------------------------------------------------

// Assert format succeeds, is idempotent, and preserves semantics vs `src`.
void check_roundtrip(const std::string& name, const std::string& src) {
    mcc::stages::FormatResult r1 = mcc::stages::format_yaml(src);
    report(r1.ok, name + ": formats ok", r1.error);
    if (!r1.ok) return;

    // (a) idempotency
    mcc::stages::FormatResult r2 = mcc::stages::format_yaml(r1.output);
    report(r2.ok && r2.output == r1.output, name + ": idempotent",
           r2.ok ? ("second pass differs:\n--- pass1 ---\n" + quote_vis(r1.output) +
                    "\n--- pass2 ---\n" + quote_vis(r2.output))
                 : r2.error);

    // (b) semantic preservation
    std::string why;
    report(semantically_equal(src, r1.output, why),
           name + ": semantics preserved", why + "\n--- output ---\n" + r1.output);

    // canonical form ends in exactly one newline, no trailing spaces.
    bool clean_ws = !r1.output.empty() && r1.output.back() == '\n';
    if (clean_ws) {
        std::istringstream is(r1.output);
        std::string line;
        while (std::getline(is, line)) {
            if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                clean_ws = false;
                break;
            }
        }
    }
    report(clean_ws, name + ": no trailing whitespace, final newline", "");
}

}  // namespace

int main() {
    std::cout << "mcc fmt unit tests\n";

    // --- (d) known-ugly -> known-canonical golden fixtures -------------------
    std::cout << "[golden] ugly -> canonical\n";
    {
        // Messy indentation, tight flow map, trailing whitespace, no final
        // newline, keys out of envelope order, extra blank lines. Canonical
        // form: envelope first, flow leaf-map kept but spaced "{ ... }", one
        // trailing newline, no trailing whitespace.
        const std::string ugly =
            "id:    core:npc.test   \n"
            "\n"
            "schema: meridian/npc@2\n"
            "level: {min: 3, max: 4}\n"
            "name: Test Guy";
        const std::string want =
            "schema: meridian/npc@2\n"
            "id: core:npc.test\n"
            "level: { min: 3, max: 4 }\n"
            "name: Test Guy\n";
        mcc::stages::FormatResult r = mcc::stages::format_yaml(ugly);
        report(r.ok && r.output == want, "golden: envelope reorder + flow spacing + ws",
               r.ok ? ("got:\n" + quote_vis(r.output) + "\nwant:\n" + quote_vis(want))
                    : r.error);
    }
    {
        // Comment preservation: leading + trailing comments survive.
        const std::string ugly =
            "# top note\n"
            "schema: meridian/quest@1\n"
            "id: core:quest.x\n"
            "level: 4  # inline note\n";
        mcc::stages::FormatResult r = mcc::stages::format_yaml(ugly);
        bool has_top = r.output.find("# top note") != std::string::npos;
        bool has_inline = r.output.find("# inline note") != std::string::npos;
        report(r.ok && has_top && has_inline, "golden: comments preserved",
               r.ok ? ("got:\n" + quote_vis(r.output)) : r.error);
    }
    {
        // Quoted-string type preservation: "4.6" must stay quoted (string),
        // bare 4.6 must stay bare (number). null normalizes to `null`.
        const std::string src =
            "schema: meridian/pack@1\n"
            "namespace: core\n"
            "engine:\n"
            "  godot: \"4.6\"\n"
            "ratio: 4.6\n"
            "script: null\n";
        mcc::stages::FormatResult r = mcc::stages::format_yaml(src);
        bool quoted_kept = r.output.find("godot: \"4.6\"") != std::string::npos;
        bool bare_kept = r.output.find("ratio: 4.6\n") != std::string::npos;
        bool null_norm = r.output.find("script: null") != std::string::npos;
        report(r.ok && quoted_kept && bare_kept && null_norm,
               "golden: scalar type/quote preservation",
               r.ok ? ("got:\n" + quote_vis(r.output)) : r.error);
    }
    {
        // Collection style: a block leaf map STAYS block (author intent kept);
        // a flow leaf seq stays flow with canonical spacing.
        const std::string src =
            "schema: meridian/item@2\n"
            "id: core:item.x\n"
            "price:\n"
            "  buy: 10\n"
            "  sell: 5\n"
            "tags: [a,b,c]\n";
        const std::string want =
            "schema: meridian/item@2\n"
            "id: core:item.x\n"
            "price:\n"
            "  buy: 10\n"
            "  sell: 5\n"
            "tags: [a, b, c]\n";
        mcc::stages::FormatResult r = mcc::stages::format_yaml(src);
        report(r.ok && r.output == want, "golden: block leaf stays block, flow seq spaced",
               r.ok ? ("got:\n" + quote_vis(r.output) + "\nwant:\n" + quote_vis(want)) : r.error);
    }
    {
        // A flow map whose child is itself a collection MUST normalize to block
        // (flow is only for leaf collections — never squeeze structure inline).
        const std::string src =
            "schema: meridian/npc@2\n"
            "id: core:npc.x\n"
            "stats: { damage: { min: 1, max: 2 }, health: 5 }\n";
        mcc::stages::FormatResult r = mcc::stages::format_yaml(src);
        bool block_outer = r.output.find("stats:\n  ") != std::string::npos;
        bool flow_inner = r.output.find("damage: { min: 1, max: 2 }") != std::string::npos;
        report(r.ok && block_outer && flow_inner,
               "golden: non-leaf flow map normalized to block",
               r.ok ? ("got:\n" + quote_vis(r.output)) : r.error);
    }

    // --- (a)+(b) idempotency + semantic preservation on varied inputs --------
    std::cout << "[property] idempotency + semantic preservation\n";
    check_roundtrip("flow-map-npc",
        "schema: meridian/npc@2\n"
        "id: core:npc.kobold_miner\n"
        "level: { min: 3, max: 4 }\n"
        "faction: hostile\n"
        "stats:\n"
        "  health: 120\n"
        "  damage: { min: 6, max: 9 }\n");
    check_roundtrip("nested-seq-spawn",
        "schema: meridian/spawn@1\n"
        "id: core:spawn.x\n"
        "spawns:\n"
        "  - npc: npc.a\n"
        "    position: { x: -295.0, y: 21.0, z: 88.0 }\n"
        "  - npc: npc.b\n"
        "    patrol:\n"
        "      loop: true\n"
        "      waypoints:\n"
        "        - position: { x: 1.0, y: 2.0, z: 3.0 }\n"
        "          wait_seconds: 5\n"
        "        - position: { x: 4.0, y: 5.0, z: 6.0 }\n");
    check_roundtrip("block-scalar-quest",
        "schema: meridian/quest@1\n"
        "id: core:quest.x\n"
        "offer_text: >-\n"
        "  line one\n"
        "  line two\n"
        "level: 4\n"
        "script: null\n");
    check_roundtrip("flow-seq-asset",
        "schema: meridian/asset@1\n"
        "id: core:art.x\n"
        "provenance:\n"
        "  authors: [a, b, c]\n"
        "  source_tier: original\n");
    check_roundtrip("empty-collections",
        "schema: meridian/vendor@1\n"
        "id: core:vendor.x\n"
        "items: []\n"
        "meta: {}\n");
    check_roundtrip("string-that-looks-numeric",
        "schema: meridian/pack@1\n"
        "namespace: core\n"
        "version: \"0.1.0\"\n"
        "engine:\n"
        "  godot: \"4.6\"\n");

    // --- (c) --check drift detection + in-place write (via format_file) ------
    std::cout << "[integration] --check drift + in-place write\n";
    {
        fs::path tmp = fs::temp_directory_path() / "mcc_fmt_test";
        std::error_code ec;
        fs::create_directories(tmp, ec);

        // Already-canonical file: --check returns 0.
        {
            const std::string canonical =
                "schema: meridian/npc@2\n"
                "id: core:npc.a\n"
                "name: A\n";
            fs::path f = tmp / "clean.npc.yaml";
            { std::ofstream(f) << canonical; }
            std::ostringstream out, err;
            int rc = mcc::stages::format_file(f.string(), /*check_only=*/true, out, err);
            report(rc == 0, "check: canonical file passes", "rc=" + std::to_string(rc) +
                   " out=" + out.str());
        }

        // Non-canonical file: --check returns 1 and does NOT modify the file.
        {
            const std::string ugly = "id: core:npc.b\nschema: meridian/npc@2\nname: B\n";
            fs::path f = tmp / "dirty.npc.yaml";
            { std::ofstream(f) << ugly; }
            std::ostringstream out, err;
            int rc = mcc::stages::format_file(f.string(), /*check_only=*/true, out, err);
            std::ifstream in(f);
            std::stringstream buf;
            buf << in.rdbuf();
            report(rc == 1, "check: non-canonical file flagged", "rc=" + std::to_string(rc));
            report(buf.str() == ugly, "check: --check does not modify file", "");
        }

        // In-place write: rewrites the dirty file to canonical, second run is a no-op.
        {
            const std::string ugly = "id: core:npc.c\nschema: meridian/npc@2\nname: C\n";
            const std::string want = "schema: meridian/npc@2\nid: core:npc.c\nname: C\n";
            fs::path f = tmp / "write.npc.yaml";
            { std::ofstream(f) << ugly; }
            std::ostringstream out, err;
            int rc1 = mcc::stages::format_file(f.string(), /*check_only=*/false, out, err);
            std::string after;
            { std::ifstream in(f); std::stringstream b; b << in.rdbuf(); after = b.str(); }
            report(rc1 == 0 && after == want, "write: rewrites to canonical",
                   "rc=" + std::to_string(rc1) + " got:\n" + quote_vis(after));
            // second pass: no change, rc 0
            std::ostringstream out2, err2;
            int rc2 = mcc::stages::format_file(f.string(), /*check_only=*/true, out2, err2);
            report(rc2 == 0, "write: result is stable under --check", "rc=" + std::to_string(rc2));
        }

        fs::remove_all(tmp, ec);
    }

    // --- malformed input is a clean error, never a crash --------------------
    std::cout << "[robustness] malformed input\n";
    {
        mcc::stages::FormatResult r = mcc::stages::format_yaml("[ not, a, mapping ]\n");
        report(!r.ok && !r.error.empty(), "non-mapping root -> error", "");
        mcc::stages::FormatResult r2 = mcc::stages::format_yaml("key: : : broken\n  bad\n");
        report(!r2.ok, "broken YAML -> error (no crash)", "");
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed";
    if (g_failures) {
        std::cout << " — " << g_failures << " FAILED\n";
        return 1;
    }
    std::cout << " — ALL PASS\n";
    return 0;
}

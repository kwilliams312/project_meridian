// tools/mcc/src/stages/model.h — the in-memory content model (Tools SAD §2.1).
//
// A pared-down version of the SAD's `ContentModel`: enough structure for the
// discover/parse and structural-validate stages implemented this round. Full
// typed per-schema structs (generated from /schema/content) and the asset
// registry / pack manifests arrive with schema-generated decoding in a later
// M0 task; here a parsed file keeps its raw yaml-cpp node plus the envelope.

#ifndef MCC_STAGES_MODEL_H
#define MCC_STAGES_MODEL_H

#include <optional>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace mcc::model {

// A file's classification by name (Tools SAD §2.1 / schema README file rules).
// `Pack` is the pack manifest; `Content` covers the eight content types; `Asset`
// is an IF-8 sidecar; `Unknown` fails L001 outright.
enum class FileKind { Pack, Content, Asset, Unknown };

// One discovered YAML file under /content, with its name-based classification.
struct DiscoveredFile {
    std::string abs_path;   // absolute path on disk
    std::string rel_path;   // repo-root-relative, forward slashes (for diagnostics)
    FileKind kind = FileKind::Unknown;
    // The declared type token from the filename: "pack", one of the eight
    // content types, or "asset". Empty when kind == Unknown.
    std::string file_type;
};

// A file after parsing: the discovered metadata plus its YAML root and the
// parsed envelope. `parsed` is false when the YAML was malformed or not a
// mapping (a PARSE diagnostic was emitted); such files carry no envelope.
struct ParsedFile {
    DiscoveredFile file;
    bool parsed = false;
    YAML::Node root;            // valid only when parsed
    std::string schema;         // envelope `schema:` (e.g. "meridian/npc@1")
    std::string id;             // envelope `id:` (empty for pack manifests)
    std::string namespace_;     // pack `namespace:` (only for pack manifests)
};

// The whole model for one `mcc check`/`build` invocation over a content root.
struct ContentModel {
    std::string content_dir;   // absolute path to the scanned /content
    std::string root_dir;      // repo root = parent of content_dir (for rel paths)
    std::vector<ParsedFile> files;

    // Stats surfaced in the summary line (parity with validate_content.py).
    std::size_t file_count = 0;     // total *.yaml discovered
    std::size_t entity_count = 0;   // content ids (L011-resolvable)
    std::size_t asset_count = 0;    // asset sidecar ids
    std::size_t content_ref_count = 0;  // content references seen
};

}  // namespace mcc::model

#endif  // MCC_STAGES_MODEL_H

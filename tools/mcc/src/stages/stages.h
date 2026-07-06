// tools/mcc/src/stages/stages.h — pipeline stage stubs (v0 skeleton).
//
// The compiler is a DAG of pure stages over an immutable in-memory content
// model (Tools SAD §2):
//
//   discover/parse -> validate -> link -> bake -> emit-sql
//                                          |  \-> emit-pck
//                                          \----> emit-sql (bake feeds sql too)
//
// Each stage is separately testable and cacheable. In this skeleton every stage
// is a no-op that reports its role; real logic (typed content model, JSON
// Schema + lint engine, IF-9 idmap, Recast bake, IF-4 SQL / IF-5 .pck emit)
// lands in later M0 tasks. Kept header-only and dependency-free on purpose.

#ifndef MCC_STAGES_STAGES_H
#define MCC_STAGES_STAGES_H

#include <ostream>
#include <string_view>

namespace mcc::stages {

// One entry per stage in the DAG. Ordered as the pipeline runs them.
enum class Stage {
    DiscoverParse,  // §2.1 walk /content trees, parse YAML -> typed model
    Validate,       // §2.2 Content Schema v1 + lint engine (L001..L081)
    Link,           // §2.3 reference graph, backlinks, IF-9 id allocation
    Bake,           // §2.5 navmesh (Recast), audio (Vorbis), texture hooks
    EmitSql,        // §2.6 IF-4 world.sql + world_manifest (deterministic dump)
    EmitPck,        // §2.7 IF-5 client .pck packs + pack.manifest.json
};

// Human-readable stage name used in stub diagnostics.
constexpr std::string_view name(Stage s) {
    switch (s) {
        case Stage::DiscoverParse: return "discover/parse";
        case Stage::Validate:      return "validate";
        case Stage::Link:          return "link";
        case Stage::Bake:          return "bake";
        case Stage::EmitSql:       return "emit-sql";
        case Stage::EmitPck:       return "emit-pck";
    }
    return "unknown";
}

// One-line role summary per stage (Tools SAD §2.x), for stub output.
constexpr std::string_view role(Stage s) {
    switch (s) {
        case Stage::DiscoverParse:
            return "walk /content/<namespace> trees, parse YAML into a typed content model";
        case Stage::Validate:
            return "JSON Schema 2020-12 validation + lint engine (L001..L081)";
        case Stage::Link:
            return "resolve *Ref graph, build backlinks, allocate IF-9 numeric ids";
        case Stage::Bake:
            return "content-addressed navmesh (Recast), audio (Vorbis), texture hooks";
        case Stage::EmitSql:
            return "deterministic IF-4 world.sql dump + world_manifest row";
        case Stage::EmitPck:
            return "assemble IF-5 client .pck packs + pack.manifest.json";
    }
    return "unknown stage";
}

// Skeleton stage runner: reports the stage's role and returns success. Replaced
// by real per-stage implementations in later M0 tasks.
inline bool run(Stage s, std::ostream& out) {
    out << "  stage " << name(s) << ": stub — " << role(s) << '\n';
    return true;
}

}  // namespace mcc::stages

#endif  // MCC_STAGES_STAGES_H

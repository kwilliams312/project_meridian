// Project Meridian — golden replay-fixture GENERATOR (issue #106).
//
// Writes each recording in replay_fixture_defs.h to a checked-in fixture file
// (recording + golden trace + digest) under test/fixtures/. Run ONCE to (re)mint
// the fixtures, then commit them; the replay-harness test loads and verifies them.
// NOT a ctest — it is a maintenance tool. Deliberately kept separate so a normal
// `ctest` run never rewrites the golden it is meant to guard.
//
// Usage:  replay-fixture-gen <output-dir>
//         (CMake passes the source-tree test/fixtures dir as the default target.)

#include "replay_fixture_defs.h"
#include "replay_harness.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace rp = meridian::replay;

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
		return 2;
	}
	const std::string out_dir = argv[1];

	int written = 0;
	for (const auto& def : rp::fixtures::all_fixtures()) {
		const rp::Fixture fx = rp::make_fixture(def.name, def.recording);
		const std::string text = rp::serialize_fixture(fx);
		const std::string path = out_dir + "/" + def.filename;

		std::ofstream os(path, std::ios::binary);
		if (!os) {
			std::fprintf(stderr, "ERROR: cannot open %s for writing\n", path.c_str());
			return 1;
		}
		os << text;
		os.close();
		std::printf("wrote %s (%zu frames, hash %016llx)\n", path.c_str(),
		            fx.golden.frames.size(),
		            static_cast<unsigned long long>(fx.golden_hash));
		++written;
	}
	std::printf("generated %d replay fixture(s)\n", written);
	return 0;
}

/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// The probe behind docs/PLATFORM_FINDINGS.md ("Fewer flushes per commit") and
// docs/mechanisms/commit-barriers.md: one commit shape per pattern, a set of 200 KB data
// files and a manifest temporary published by rename, made durable four ways. The commit
// (from the first sync to the directory barrier after the rename) is timed, the writes
// are not. Every round runs each pattern once, rotating the start, on fresh files.
//
//   completion_commit_probe [DIRECTORY [ROUNDS [FILES...]]]     (defaults: $TMPDIR, 30, 3 9)
//
// Run it on the device under test, otherwise idle. Prints the median commit time in
// milliseconds per pattern and file count, with the flushes each pattern issues on macOS
// (on Linux and FreeBSD the ordering calls run the same full syncs).
#include "probe.h"
#include <span>

namespace {
using namespace probe;

constexpr std::uint64_t kDataBytes = 200 * 1024;
constexpr std::uint64_t kManifestBytes = 4 * 1024;

struct Pattern {
	const char* name;
	const char* flushes;
	void (*commit)(j::IO&, Batch& data, j::File& temporary);
};

void publish(j::IO& io) {
	io.replace("MANIFEST.tmp", "MANIFEST");
	io.sync_directory();
}

// The data batch, the directory, the temporary, and the directory after the rename.
void before(j::IO& io, Batch& data, j::File& temporary) {
	io.sync_files(data.pointers());
	io.sync_directory();
	temporary.sync();
	publish(io);
}

// The directory's fsync joins the files' before the one flush.
void directory_folded(j::IO& io, Batch& data, j::File& temporary) {
	io.sync_files_and_directory(data.pointers());
	temporary.sync();
	publish(io);
}

// The temporary is ordered by a barrier instead of flushed.
void temporary_ordered(j::IO& io, Batch& data, j::File& temporary) {
	io.sync_files_and_directory(data.pointers());
	j::File* files[] = {&temporary};
	io.order_files_and_directory(files);
	publish(io);
}

// One ordering point for the data files, the directory and the temporary.
void one_ordering_point(j::IO& io, Batch& data, j::File& temporary) {
	auto files = data.pointers();
	files.push_back(&temporary);
	io.order_files_and_directory(files);
	publish(io);
}

const Pattern kPatterns[] = {
	{"Before", "4 full", before},
	{"Directory folded in", "3 full", directory_folded},
	{"Temporary ordered", "2 full, 1 barrier", temporary_ordered},
	{"One ordering point", "1 full, 1 barrier", one_ordering_point},
};
constexpr std::size_t kPatternCount = sizeof kPatterns / sizeof *kPatterns;
} // namespace

int main(int argc, char** argv) {
	try {
		const auto parent = directory_argument(argc, argv, 1);
		const int rounds = static_cast<int>(integer_argument(argc, argv, 2, 30));
		std::vector<int> counts;
		for (int i = 3; i < argc; ++i) { counts.push_back(std::atoi(argv[i])); }
		if (counts.empty()) { counts = {3, 9}; }
		if (rounds < 1) { std::cerr << "usage: completion_commit_probe [DIRECTORY [ROUNDS [FILES...]]]\n"; return 2; }
		ScratchDirectory scratch(parent);
		j::PosixIO io(scratch.path());

		std::vector<std::vector<std::vector<double>>> times(counts.size(), std::vector<std::vector<double>>(kPatternCount));
		std::cout << "data files of " << kDataBytes / 1024 << " KB and a manifest temporary in "
			<< scratch.path().parent_path().string() << ", " << rounds << " rounds\n";
		for (int round = 0; round < rounds; ++round) {
			for (std::size_t c = 0; c < counts.size(); ++c) {
				for (std::size_t p = 0; p < kPatternCount; ++p) {
					const std::size_t index = (p + static_cast<std::size_t>(round)) % kPatternCount;
					auto data = make_batch(io, "data", counts[c], kDataBytes);
					auto temporary = io.create_exclusive("MANIFEST.tmp");
					fill(*temporary, kManifestBytes);
					const auto start = Clock::now();
					kPatterns[index].commit(io, data, *temporary);
					times[c][index].push_back(since_ms(start));
					temporary.reset();
					discard(io, data);
				}
			}
		}

		std::printf("%-22s %-20s", "pattern", "device flushes");
		for (int count : counts) { std::printf(" %7d files", count); }
		std::printf("\n");
		for (std::size_t p = 0; p < kPatternCount; ++p) {
			std::printf("%-22s %-20s", kPatterns[p].name, kPatterns[p].flushes);
			for (std::size_t c = 0; c < counts.size(); ++c) { std::printf(" %8.1f ms   ", quantile(times[c][p], 0.5)); }
			std::printf("\n");
		}
	} catch (const std::exception& error) {
		std::cerr << "completion_commit_probe: " << error.what() << '\n';
		return 1;
	}
}

/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// The probe behind docs/PLATFORM_FINDINGS.md section 7: what a flush through a directory
// descriptor costs after a staged write, against the same flush through a regular file.
// Each round stages a write of 200 KB (written, not synced) and times one call:
//
//   F_FULLFSYNC on the directory descriptor    (macOS)
//   F_FULLFSYNC on a regular file descriptor   (macOS)
//   fsync on the directory descriptor
//   fsync on the regular file descriptor
//   IO::sync_directory
//
//   completion_dirflush_probe [DIRECTORY [ROUNDS]]     (defaults: $TMPDIR, 50)
//
// Run it on the device under test, otherwise idle. Prints the median, p90 and maximum of
// each call in milliseconds.
#include "probe.h"
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace probe;

struct Call {
	std::string name;
	// Times one call after a write was staged through `file`.
	int (*invoke)(j::IO&, int directory, int file);
	std::vector<double> milliseconds;
};

#ifdef F_FULLFSYNC
int full_directory(j::IO&, int directory, int) { return ::fcntl(directory, F_FULLFSYNC); }
int full_file(j::IO&, int, int file) { return ::fcntl(file, F_FULLFSYNC); }
#endif
int fsync_directory(j::IO&, int directory, int) { return ::fsync(directory); }
int fsync_file(j::IO&, int, int file) { return ::fsync(file); }
int io_sync_directory(j::IO& io, int, int) { io.sync_directory(); return 0; }
} // namespace

int main(int argc, char** argv) {
	try {
		const auto parent = directory_argument(argc, argv, 1);
		const int rounds = static_cast<int>(integer_argument(argc, argv, 2, 50));
		if (rounds < 1) { std::cerr << "usage: completion_dirflush_probe [DIRECTORY [ROUNDS]]\n"; return 2; }
		ScratchDirectory scratch(parent);
		j::PosixIO io(scratch.path());
		const int directory = ::open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY);
		if (directory < 0) { throw std::runtime_error("open directory"); }

		std::vector<Call> calls;
#ifdef F_FULLFSYNC
		calls.push_back({"F_FULLFSYNC, directory descriptor", full_directory, {}});
		calls.push_back({"F_FULLFSYNC, regular file", full_file, {}});
#endif
		calls.push_back({"fsync, directory descriptor", fsync_directory, {}});
		calls.push_back({"fsync, regular file", fsync_file, {}});
		calls.push_back({"IO::sync_directory", io_sync_directory, {}});

		std::cout << "one flush after a staged 200 KB write, " << rounds << " rounds, in " << parent.string() << '\n';
		for (int round = 0; round < rounds; ++round) {
			for (std::size_t c = 0; c < calls.size(); ++c) {
				auto& call = calls[(c + static_cast<std::size_t>(round)) % calls.size()];
				auto batch = make_batch(io, "staged", 1, 200 * 1024);
				const auto start = Clock::now();
				const int result = call.invoke(io, directory, native_fd(*batch.files[0]));
				call.milliseconds.push_back(since_ms(start));
				if (result != 0) { std::perror(call.name.c_str()); }
				discard(io, batch);
			}
		}
		::close(directory);

		std::printf("%-36s %10s %10s %10s\n", "call", "median ms", "p90 ms", "max ms");
		for (auto& call : calls) {
			const double median = quantile(call.milliseconds, 0.5), p90 = quantile(call.milliseconds, 0.9);
			std::printf("%-36s %10.3f %10.3f %10.3f\n", call.name.c_str(), median, p90, call.milliseconds.back());
		}
	} catch (const std::exception& error) {
		std::cerr << "completion_dirflush_probe: " << error.what() << '\n';
		return 1;
	}
}

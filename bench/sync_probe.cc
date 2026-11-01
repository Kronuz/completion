/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// The probe behind docs/PLATFORM_FINDINGS.md section 1: N new files of 200 KB are written,
// made durable one way, and then a directory barrier is taken; the commit part is timed,
// the writes are not. Every round runs each variant once, in turn, on fresh files.
//
//   completion_sync_probe [DIRECTORY [FILES [ROUNDS]]]     (defaults: $TMPDIR, 9, 15)
//
// Run it on the device under test (DIRECTORY decides which), otherwise idle. Prints the
// median, minimum and maximum of each variant in milliseconds.
#include "probe.h"
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#ifdef __FreeBSD__
#include <aio.h>
#endif

namespace {
using namespace probe;

constexpr std::uint64_t kFileBytes = 200 * 1024;

struct Variant {
	std::string name;
	// Makes `batch` durable and takes the directory barrier.
	void (*commit)(j::IO&, Batch&, int directory);
	std::vector<double> milliseconds;
};

void each_full_sync(j::IO& io, Batch& batch, int) {
	for (auto& file : batch.files) { file->sync(); }
	io.sync_directory();
}

void batched(j::IO& io, Batch& batch, int) {
	io.sync_files(batch.pointers());
	io.sync_directory();
}

void fsync_threads(j::IO& io, Batch& batch, int) {
	std::vector<std::thread> threads;
	for (int fd : batch.descriptors()) { threads.emplace_back([fd] { if (::fsync(fd) != 0) { std::perror("fsync"); } }); }
	for (auto& thread : threads) { thread.join(); }
	io.sync_directory();
}

void fdatasync_each(j::IO& io, Batch& batch, int) {
	for (int fd : batch.descriptors()) {
#ifdef __APPLE__
		if (::fsync(fd) != 0) { std::perror("fsync"); }
#else
		if (::fdatasync(fd) != 0) { std::perror("fdatasync"); }
#endif
	}
	io.sync_directory();
}

#ifdef __linux__
// One syncfs flushes every dirty file on the filesystem, not only these.
void syncfs_once(j::IO& io, Batch&, int directory) {
	if (::syncfs(directory) != 0) { std::perror("syncfs"); }
	io.sync_directory();
}
#endif

#ifdef __FreeBSD__
void aio_fsync_all(j::IO& io, Batch& batch, int) {
	auto descriptors = batch.descriptors();
	std::vector<struct aiocb> requests(descriptors.size());
	for (std::size_t i = 0; i < descriptors.size(); ++i) {
		requests[i] = {};
		requests[i].aio_fildes = descriptors[i];
		if (::aio_fsync(O_SYNC, &requests[i]) != 0) { std::perror("aio_fsync"); }
	}
	for (auto& request : requests) {
		while (::aio_error(&request) == EINPROGRESS) { ::usleep(50); }
		(void)::aio_return(&request);
	}
	io.sync_directory();
}
#endif
} // namespace

int main(int argc, char** argv) {
	try {
		const auto parent = directory_argument(argc, argv, 1);
		const int files = static_cast<int>(integer_argument(argc, argv, 2, 9));
		const int rounds = static_cast<int>(integer_argument(argc, argv, 3, 15));
		if (files < 1 || rounds < 1) { std::cerr << "usage: completion_sync_probe [DIRECTORY [FILES [ROUNDS]]]\n"; return 2; }
		ScratchDirectory scratch(parent);
		j::PosixIO io(scratch.path());
		const int directory = ::open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY);
		if (directory < 0) { throw std::runtime_error("open directory"); }

		std::vector<Variant> variants;
		variants.push_back({"one full sync each (File::sync)", each_full_sync, {}});
		variants.push_back({"batched (IO::sync_files)", batched, {}});
		variants.push_back({"concurrent fsync threads", fsync_threads, {}});
		variants.push_back({"fdatasync each (fsync on macOS)", fdatasync_each, {}});
#ifdef __linux__
		variants.push_back({"one syncfs", syncfs_once, {}});
#endif
#ifdef __FreeBSD__
		variants.push_back({"aio_fsync all", aio_fsync_all, {}});
#endif

		std::cout << files << " files of " << kFileBytes / 1024 << " KB in " << scratch.path().parent_path().string()
			<< ", " << rounds << " rounds\n";
		for (int round = 0; round < rounds; ++round) {
			for (std::size_t v = 0; v < variants.size(); ++v) {
				// Rotate the start so no variant always runs right after the same one.
				auto& variant = variants[(v + static_cast<std::size_t>(round)) % variants.size()];
				auto batch = make_batch(io, "data-" + std::to_string(round), files, kFileBytes);
				const auto start = Clock::now();
				variant.commit(io, batch, directory);
				variant.milliseconds.push_back(since_ms(start));
				discard(io, batch);
			}
		}
		::close(directory);

		std::printf("%-36s %10s %10s %10s\n", "variant", "median ms", "min ms", "max ms");
		for (auto& variant : variants) {
			auto sorted = variant.milliseconds;
			const double median = quantile(sorted, 0.5);
			std::printf("%-36s %10.1f %10.1f %10.1f\n", variant.name.c_str(), median, sorted.front(), sorted.back());
		}
	} catch (const std::exception& error) {
		std::cerr << "completion_sync_probe: " << error.what() << '\n';
		return 1;
	}
}

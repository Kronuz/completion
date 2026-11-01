/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// The probe behind docs/PLATFORM_FINDINGS.md section 2: what freeing a large file costs,
// and what it does to other files' reads.
//
//   completion_unlink_probe [DIRECTORY [MIB [STEP_MIB]]]     (defaults: $TMPDIR, 2048, 16)
//
// 1. Time: a synced file of MIB MiB (after sync(2)) is removed with IO::remove (three times), and the time
//    of the call is printed.
// 2. Latency: a reader thread issues random 4 KiB reads over a set of other files
//    (uncached: O_DIRECT on Linux, F_NOCACHE on macOS) while the main thread (a) waits,
//    (b) removes a MIB-MiB file at once, and (c) truncates a same-size file from the end in
//    STEP_MIB steps and then removes it. Read latencies are printed as quantiles per phase,
//    with the main thread's total time.
//
// It writes MIB MiB several times: run it on the device under test, otherwise idle, with
// enough free space. Reads on FreeBSD go through the page cache, so its tail numbers say
// less there.
#include "probe.h"
#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace probe;

constexpr std::uint64_t kMiB = 1024 * 1024;
constexpr std::size_t kReadBytes = 4096;
constexpr int kVictimFiles = 8;
constexpr std::uint64_t kVictimBytes = 16 * kMiB;

// Reads random 4 KiB blocks of the victim files until stopped, recording each latency in
// microseconds into the current phase's vector.
class Reader {
public:
	explicit Reader(const std::filesystem::path& directory) {
		for (int i = 0; i < kVictimFiles; ++i) {
			const auto path = (directory / ("victim-" + std::to_string(i))).string();
			int fd = -1;
#ifdef __linux__
			// Bypass the page cache; a filesystem without O_DIRECT (tmpfs) reads through it.
			fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
#endif
			if (fd < 0) { fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }
			if (fd < 0) { throw std::runtime_error("open victim file"); }
#ifdef __APPLE__
			(void)::fcntl(fd, F_NOCACHE, 1);
#endif
			descriptors_.push_back(fd);
		}
		thread_ = std::thread([this] { run(); });
	}
	~Reader() {
		stop_.store(true);
		thread_.join();
		for (int fd : descriptors_) { ::close(fd); }
	}
	// Starts a new phase and returns the previous one's latencies.
	std::vector<double> next_phase() {
		std::lock_guard<std::mutex> lock(mutex_);
		return std::exchange(latencies_, {});
	}
private:
	void run() {
		void* raw = nullptr;
		if (::posix_memalign(&raw, 4096, kReadBytes) != 0) { return; }
		std::unique_ptr<void, decltype(&std::free)> buffer(raw, &std::free);
		std::mt19937_64 random(12345);
		const std::uint64_t blocks = kVictimBytes / kReadBytes;
		std::vector<double> local;
		while (!stop_.load(std::memory_order_relaxed)) {
			const int fd = descriptors_[random() % descriptors_.size()];
			const off_t offset = static_cast<off_t>((random() % blocks) * kReadBytes);
			const auto start = Clock::now();
			const ssize_t count = ::pread(fd, buffer.get(), kReadBytes, offset);
			const double microseconds = since_ms(start) * 1000;
			if (count != static_cast<ssize_t>(kReadBytes)) { continue; }
			local.push_back(microseconds);
			if (local.size() >= 256) {
				std::lock_guard<std::mutex> lock(mutex_);
				latencies_.insert(latencies_.end(), local.begin(), local.end());
				local.clear();
			}
		}
	}
	std::vector<int> descriptors_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::mutex mutex_;
	std::vector<double> latencies_;
};

// A synced file of `bytes`, written in 8 MiB steps.
void make_big(j::IO& io, const std::string& name, std::uint64_t bytes) {
	auto file = io.create_exclusive(name);
	for (std::uint64_t offset = 0; offset < bytes; offset += 8 * kMiB) {
		const std::uint64_t piece = std::min<std::uint64_t>(8 * kMiB, bytes - offset);
		static std::string block(8 * kMiB, 'x');
		std::size_t done = 0;
		while (done < piece) {
			const auto count = file->write_at(offset + done, std::string_view(block.data(), piece - done));
			if (!count) { throw std::runtime_error("write made no progress"); }
			done += count;
		}
	}
	file->sync();
	io.sync_directory();
	// Let the writeback settle: right after an fsync, ext4 frees a file in microseconds, and
	// the cost this probe is after appears once the blocks are committed.
	::sync();
}

void report(const char* phase, std::vector<double> latencies, double milliseconds) {
	const double p50 = quantile(latencies, 0.5), p99 = quantile(latencies, 0.99), p999 = quantile(latencies, 0.999);
	const double worst = latencies.empty() ? 0.0 : latencies.back();
	std::printf("%-34s %9.1f ms  %8zu reads  p50 %7.0f  p99 %8.0f  p99.9 %8.0f  max %9.0f us\n", phase, milliseconds,
		latencies.size(), p50, p99, p999, worst);
}
} // namespace

int main(int argc, char** argv) {
	try {
		const auto parent = directory_argument(argc, argv, 1);
		const std::uint64_t mib = static_cast<std::uint64_t>(integer_argument(argc, argv, 2, 2048));
		const std::uint64_t step = static_cast<std::uint64_t>(integer_argument(argc, argv, 3, 16)) * kMiB;
		if (mib < 1 || step < kMiB) { std::cerr << "usage: completion_unlink_probe [DIRECTORY [MIB [STEP_MIB]]]\n"; return 2; }
		const std::uint64_t bytes = mib * kMiB;
		ScratchDirectory scratch(parent);
		j::PosixIO io(scratch.path());

		std::printf("unlink of a synced %llu MiB file in %s\n", static_cast<unsigned long long>(mib), parent.string().c_str());
		for (int run = 0; run < 3; ++run) {
			make_big(io, "big", bytes);
			const auto start = Clock::now();
			io.remove("big");
			const double milliseconds = since_ms(start);
			io.sync_directory();
			std::printf("  IO::remove, run %d: %.1f ms\n", run + 1, milliseconds);
		}

		for (int i = 0; i < kVictimFiles; ++i) { make_big(io, "victim-" + std::to_string(i), kVictimBytes); }
		Reader reader(scratch.path());
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		(void)reader.next_phase();

		std::printf("random 4 KiB reads of other files while a %llu MiB file is freed:\n", static_cast<unsigned long long>(mib));
		auto start = Clock::now();
		std::this_thread::sleep_for(std::chrono::seconds(2));
		report("no removal (2 s)", reader.next_phase(), since_ms(start));

		make_big(io, "big", bytes);
		(void)reader.next_phase();
		start = Clock::now();
		io.remove("big");
		double milliseconds = since_ms(start);
		report("remove at once", reader.next_phase(), milliseconds);
		io.sync_directory();

		make_big(io, "big", bytes);
		(void)reader.next_phase();
		start = Clock::now();
		{
			auto file = io.open_existing("big");
			for (std::uint64_t size = bytes; size > 0;) {
				size = size > step ? size - step : 0;
				file->truncate(size);
			}
		}
		io.remove("big");
		milliseconds = since_ms(start);
		report("truncate in steps, then remove", reader.next_phase(), milliseconds);
	} catch (const std::exception& error) {
		std::cerr << "completion_unlink_probe: " << error.what() << '\n';
		return 1;
	}
}

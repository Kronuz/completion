/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// The probe behind docs/MEASUREMENTS.md ("Turn lane"): how long a turn's primitives (a 4 KiB write,
// then SyncData) wait for the queue's one slot while Bulk tasks keep it busy with 64 KiB chunk
// writes, the way snapshot and artifact IO shares a loop with turns. One loop, one native queue,
// one AsioCompletionDriver. Each turn is timed from its issue to its completion (total) and
// from each readiness to the driver's submission, summed over its primitives (admission wait,
// the part the lane changes).
//
//   completion_lane_probe [DIRECTORY [BULK_TASKS [TURNS [CHUNK_KIB]]]]   (defaults: $TMPDIR, 8, 100, 64)
//
// Prints one line: the arm's settings, then the turn's admission wait and total latency in
// milliseconds (p50, p99, max), the Bulk writes completed during the turns and their MiB/s.
// Run it on the device under test, otherwise idle.
#include "completion/asio_completion.h"
#include "completion/native_completion.h"
#include "completion/posix.h"
#include "probe.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace j = kronuz::journal;
namespace c = kronuz::journal;
using probe::Clock;

class Chain final : public j::IOOperation {
public:
	Chain(c::IO& io, std::shared_ptr<c::File> file, std::vector<j::PrimitiveKind> steps, std::string_view bytes,
	      std::uint64_t offset = 0)
		: io_(io), file_(std::move(file)), steps_(std::move(steps)), bytes_(bytes), offset_(offset) {
		prepare();
	}
	const j::MutationRequest& request() const override { return request_; }
	void submitted() override {
		submitted_ = true;
		const auto now = Clock::now();
		wait_ms += std::chrono::duration<double, std::milli>(now - ready_).count();
	}
	bool complete(j::MutationCompletion result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token) { return false; }
		submitted_ = false;
		if (result.error) { error = result.error; done_ = true; return true; }
		ready_ = Clock::now();
		if (++step_ == steps_.size()) { done_ = true; } else { prepare(); }
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_; }
	j::IO& io() const noexcept override { return io_; }
	void start() { ready_ = started = Clock::now(); }
	j::IOLane lane() const noexcept override { return lane_; }
	void set_lane(j::IOLane lane) noexcept { lane_ = lane; }
	Clock::time_point started;
	double wait_ms = 0;
	std::exception_ptr error;

private:
	void prepare() {
		request_ = {};
		request_.token.operation = 1;
		request_.token.step = step_;
		request_.kind = steps_[step_];
		request_.file = file_;
		request_.offset = offset_;
		request_.bytes = bytes_;
	}
	c::IO& io_;
	std::shared_ptr<c::File> file_;
	std::vector<j::PrimitiveKind> steps_;
	std::string_view bytes_;
	std::uint64_t offset_;
	j::MutationRequest request_;
	Clock::time_point ready_;
	std::size_t step_ = 0;
	bool submitted_ = false, done_ = false;
	j::IOLane lane_ = j::IOLane::Bulk;
};

struct Run {
	asio::io_context context;
	task::asio_scheduler scheduler{context.get_executor()};
	std::shared_ptr<j::NativeCompletionQueue> queue = std::make_shared<j::NativeCompletionQueue>();
	std::shared_ptr<j::AsioCompletionDriver<j::NativeCompletionQueue>> driver =
		std::make_shared<j::AsioCompletionDriver<j::NativeCompletionQueue>>(scheduler, queue);
	std::unique_ptr<j::PosixIO> io;
	std::string chunk, turn;
	bool stop = false;
	std::uint64_t bulk_writes = 0;
	std::vector<double> waits, totals;
};

task::task<void> bulk(Run& run, std::shared_ptr<c::File> file, std::size_t index) {
	std::uint64_t offset = 0;
	while (!run.stop) {
		auto op = std::make_shared<Chain>(*run.io, file, std::vector{j::PrimitiveKind::Write}, run.chunk, offset);
		op->start();
		co_await run.driver->run(op);
		if (op->error) { std::rethrow_exception(op->error); }
		++run.bulk_writes;
		offset = (offset + run.chunk.size()) % (64ull << 20);  // wrap inside 64 MiB
	}
	(void)index;
}

task::task<void> turns(Run& run, std::shared_ptr<c::File> file, int count, bool lane_aware) {
	asio::steady_timer timer(run.context);
	for (int i = 0; i < count; ++i) {
		timer.expires_after(std::chrono::milliseconds(2));
		co_await task::await_asio<void(asio::error_code)>([&](auto handler) { timer.async_wait(std::move(handler)); });
		auto op = std::make_shared<Chain>(*run.io, file, std::vector{j::PrimitiveKind::Write, j::PrimitiveKind::SyncData},
		                                  run.turn, static_cast<std::uint64_t>(i) * run.turn.size());
		if (lane_aware) { op->set_lane(j::IOLane::Turn); }
		op->start();
		co_await run.driver->run(op);
		if (op->error) { std::rethrow_exception(op->error); }
		run.waits.push_back(op->wait_ms);
		run.totals.push_back(std::chrono::duration<double, std::milli>(Clock::now() - op->started).count());
	}
	run.stop = true;
}

double quantile(std::vector<double> values, double q) {
	std::sort(values.begin(), values.end());
	return values[std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<double>(values.size())))];
}
}  // namespace

int main(int argc, char** argv) {
	try {
		const std::filesystem::path parent = argc > 1 ? argv[1] : std::filesystem::temp_directory_path();
		const int bulk_tasks = argc > 2 ? std::atoi(argv[2]) : 8;
		const int turn_count = argc > 3 ? std::atoi(argv[3]) : 100;
		const int chunk_kib = argc > 4 ? std::atoi(argv[4]) : 64;
		probe::ScratchDirectory scratch(parent);
		Run run;
		run.io = std::make_unique<j::PosixIO>(scratch.path(), {.flush_mode = j::FlushMode::full});
		run.chunk.assign(static_cast<std::size_t>(chunk_kib) * 1024, 'b');
		run.turn.assign(4096, 't');
		// The turn lane is the arm's setting: a driver that honors it admits a turn's primitive ahead of waiting Bulk ones.
		const bool lane_aware = std::getenv("LANE_PROBE_FLAT") == nullptr;
		std::vector<std::shared_ptr<c::File>> files;
		for (int i = 0; i <= bulk_tasks; ++i) { files.push_back(run.io->create_exclusive("f" + std::to_string(i))); }
		std::exception_ptr failure;
		int running = bulk_tasks + 1;
		auto finished = [&](std::exception_ptr error) { if (error && !failure) { failure = error; run.stop = true; } --running; };
		const auto begin = Clock::now();
		for (int i = 0; i < bulk_tasks; ++i) { task::spawn(task::scheduler_ref::of(run.scheduler), bulk(run, files[i], i), finished); }
		task::spawn(task::scheduler_ref::of(run.scheduler), turns(run, files[bulk_tasks], turn_count, lane_aware), finished);
		run.context.run();
		const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
		if (failure) { std::rethrow_exception(failure); }
		const double mib = static_cast<double>(run.bulk_writes) * chunk_kib / 1024.0;
		std::printf("bulk=%d turns=%d chunk_kib=%d lane=%s | admission wait ms p50 %.3f p99 %.3f max %.3f | total ms p50 %.3f p99 %.3f max %.3f | bulk writes %llu, %.1f MiB/s\n",
		            bulk_tasks, turn_count, chunk_kib, lane_aware ? "on" : "flat",
		            quantile(run.waits, 0.5), quantile(run.waits, 0.99), *std::max_element(run.waits.begin(), run.waits.end()),
		            quantile(run.totals, 0.5), quantile(run.totals, 0.99), *std::max_element(run.totals.begin(), run.totals.end()),
		            static_cast<unsigned long long>(run.bulk_writes), mib / seconds);
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "lane_probe: %s\n", error.what());
		return 1;
	}
}

/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The Asio driver behind the seam: a completion_ref over it, SyncData with and without a fault,
// several operations sharing the one queue slot a primitive at a time, the token read before
// each primitive is issued, a request while an operation waits for the slot releasing it at once,
// and one Turn source per driver (asserted in Debug).
#include "completion/asio_completion.h"
#include "completion/native_completion.h"
#include "completion/seam.h"
#include "seam_support.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <memory>
#include <random>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace c = kronuz::io::completion;
namespace j = kronuz::journal;
using namespace seam_support;

namespace {
using Driver = j::AsioCompletionDriver<c::NativeQueue>;
static_assert(j::completion_driver<Driver>);

struct Result {
	bool finished = false;
	std::exception_ptr error;
	bool cancelled() const {
		try { if (error) { std::rethrow_exception(error); } } catch (const task::cancelled &) { return true; } catch (...) {}
		return false;
	}
};

struct Rig {
	asio::io_context context;
	task::asio_scheduler scheduler{context.get_executor()};
	std::shared_ptr<c::NativeQueue> queue = std::make_shared<c::NativeQueue>();
	std::shared_ptr<Driver> driver;
	explicit Rig(bool metrics = false) : driver(std::make_shared<Driver>(scheduler, queue, metrics)) {}
	void start(task::task<void> work, Result &result) {
		task::spawn(task::scheduler_ref::of(scheduler), std::move(work), [&result](std::exception_ptr error) {
			result.finished = true;
			result.error = error;
		});
	}
	// Runs the loop until nothing is left to do.
	void run() {
		context.restart();
		context.run();
	}
	j::completion_ref ref() { return j::completion_ref::of(*driver); }
};

void through_the_reference() {
	Rig rig(true);
	Log log;
	auto op = std::make_shared<Steps>("a", log, 4);
	Result result;
	rig.start(rig.ref().run(op), result);
	rig.run();
	check(result.finished && !result.error && op->done() && !rig.queue->busy(), "a completion_ref runs an operation on the Asio driver");
	expect(log, {"a:write", "a:sync_data", "a:write", "a:sync_data"}, "the primitives, in order");
	auto stats = rig.driver->stats();
	check(stats.extension_submitted_to_reaped[static_cast<std::size_t>(j::PrimitiveKind::SyncData) - j::completion_primitive_count].count == 2,
		  "SyncData is counted in its own latency slot");
}

void sync_data_fault() {
	Rig rig(true);
	Log log;
	auto op = std::make_shared<Steps>("a", log, 2);
	op->file()->before_sync_data = [] { throw std::system_error(EIO, std::generic_category(), "injected"); };
	Result result;
	rig.start(rig.ref().run(op), result);
	rig.run();
	check(result.finished && !result.error && op->done() && op->error && !op->in_flight() && !rig.queue->busy(),
		  "a failed SyncData settles the operation and frees the queue");
	try { std::rethrow_exception(op->error); } catch (const std::system_error &e) {
		check(e.code() == std::error_code(EIO, std::generic_category()), "with its errno");
	}
	expect(log, {"a:write"}, "the write before it stays applied");
	check(rig.driver->stats().extension_failed_submitted_to_reaped[static_cast<std::size_t>(j::PrimitiveKind::SyncData) - j::completion_primitive_count].count == 1,
		  "the failure is counted");
	// The queue serves the next operation.
	Log again;
	auto next = std::make_shared<Steps>("b", again, 2);
	Result second;
	rig.start(rig.ref().run(next), second);
	rig.run();
	check(second.finished && !second.error && next->done() && !next->error, "the driver runs on after a fault");
}

// Operations that overlap take the slot one primitive at a time, in the order they asked.
void shared_slot() {
	Rig rig;
	Log log;
	auto a = std::make_shared<Steps>("a", log, 3), b = std::make_shared<Steps>("b", log, 3);
	// a's first write waits until b has asked for the slot, so both are queued before either moves.
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	a->file()->before_write = [opened] { opened.wait(); };
	Result ra, rb;
	rig.start(rig.ref().run(a), ra);
	rig.start(rig.ref().run(b), rb);
	asio::post(rig.context, [&] { gate.set_value(); });
	rig.run();
	check(ra.finished && rb.finished && !ra.error && !rb.error && a->done() && b->done() && !rig.queue->busy(), "both finish");
	expect(log, {"a:write", "b:write", "a:sync_data", "b:sync_data", "a:write", "b:write"}, "primitives alternate in arrival order");
}

// A token requested while an operation waits for the slot releases it: its primitive is not
// issued, and the operations behind it still run.
void token_while_waiting() {
	Rig rig;
	Log log;
	auto a = std::make_shared<Steps>("a", log, 1), b = std::make_shared<Steps>("b", log, 1), c = std::make_shared<Steps>("c", log, 1);
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	a->file()->before_write = [opened] { opened.wait(); };
	task::cancel_source source;
	Result ra, rb, rc;
	rig.start(rig.ref().run(a), ra);
	rig.start(rig.ref().run(b, source.token()), rb);
	rig.start(rig.ref().run(c), rc);
	asio::post(rig.context, [&] {
		source.request();
		gate.set_value();
	});
	rig.run();
	check(ra.finished && !ra.error && a->done(), "the holder of the slot finishes");
	check(rb.cancelled() && !b->in_flight() && !b->done() && b->completed == 0, "the waiter whose token was requested is released unissued");
	check(rc.finished && !rc.error && c->done() && !rig.queue->busy(), "the waiter behind it still runs");
	expect(log, {"a:write", "c:write"}, "only the issued primitives ran");
}

// A Turn's primitive is admitted ahead of every waiting Bulk one, in arrival order within a lane;
// the primitive in flight is never preempted, and a Turn operation keeps the slot between its own
// primitives when only Bulk operations wait.
class Lane final : public j::IOOperation {
  public:
	Lane(std::shared_ptr<Steps> inner, j::IOLane lane) : inner_(std::move(inner)), lane_(lane) {}
	const j::MutationRequest &request() const override { return inner_->request(); }
	void submitted() override { inner_->submitted(); }
	bool complete(j::MutationCompletion result) noexcept override { return inner_->complete(std::move(result)); }
	bool done() const noexcept override { return inner_->done(); }
	bool in_flight() const noexcept override { return inner_->in_flight(); }
	j::IO &io() const noexcept override { return inner_->io(); }
	j::IOLane lane() const noexcept override { return lane_; }

  private:
	std::shared_ptr<Steps> inner_;
	j::IOLane lane_;
};

// A token requested from another thread while an operation waits for the slot releases the caller
// within a hop, while the holder's primitive is still blocked; the waiter behind it still runs.
void cancel_while_waiting() {
	using Clock = std::chrono::steady_clock;
	Rig rig;
	Log log;
	auto a = std::make_shared<Steps>("a", log, 1), b = std::make_shared<Steps>("b", log, 1), c = std::make_shared<Steps>("c", log, 1);
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	// Opened once b is released; bounded so a missing release fails instead of hanging.
	a->file()->before_write = [opened] { opened.wait_for(std::chrono::seconds(2)); };
	task::cancel_source source;
	Clock::time_point requested_at, released_at;
	bool holder_still_blocked = false;
	auto waiter = [&]() -> task::task<void> {
		try {
			co_await rig.ref().run(b, source.token());
		} catch (const task::cancelled &) {
			released_at = Clock::now();
			holder_still_blocked = a->in_flight() && a->completed == 0;
			gate.set_value();
			throw;
		}
	};
	Result ra, rb, rc;
	rig.start(rig.ref().run(a), ra);
	rig.start(waiter(), rb);
	rig.start(rig.ref().run(c), rc);
	std::thread requester;
	asio::post(rig.context, [&] {
		requester = std::thread([&] {
			requested_at = Clock::now();
			source.request();
		});
	});
	rig.run();
	requester.join();
	auto latency = std::chrono::duration_cast<std::chrono::microseconds>(released_at - requested_at).count();
	std::cout << "asio_seam: cancelled waiter released in " << latency << " us\n";
	check(rb.cancelled() && !b->in_flight() && b->completed == 0, "the cancelled waiter is released unissued");
	check(holder_still_blocked, "it is released while the holder's primitive is still blocked");
	check(latency < 100'000, "within a hop, not the slot (under 100 ms)");
	check(ra.finished && !ra.error && a->done(), "the holder finishes");
	check(rc.finished && !rc.error && c->done() && !rig.queue->busy(), "the waiter behind it still runs");
	expect(log, {"a:write", "c:write"}, "only the issued primitives ran");
}

// Tokens requested from another thread at random moments while operations queue for, take and
// pass the slot: every run ends, cancelled or done, nothing is issued twice, the slot ends free.
void cancel_races_admission(unsigned rounds) {
	std::mt19937 random(20261116);
	unsigned cancelled = 0, finished = 0;
	for (unsigned round = 0; round < rounds; ++round) {
		Rig rig;
		Log log;
		constexpr unsigned count = 5;
		std::vector<std::shared_ptr<j::IOOperation>> ops;
		std::vector<std::shared_ptr<Steps>> steps;
		std::vector<task::cancel_source> sources(count);
		std::vector<Result> results(count);
		for (unsigned i = 0; i < count; ++i) {
			steps.push_back(std::make_shared<Steps>("s" + std::to_string(i), log, 1 + random() % 4));
			// One Turn source: operation 0 only.
			ops.push_back(std::make_shared<Lane>(steps.back(), i == 0 ? j::IOLane::Turn : j::IOLane::Bulk));
			rig.start(rig.ref().run(ops.back(), sources[i].token()), results[i]);
		}
		std::vector<unsigned> delays(count);
		for (auto &delay : delays) { delay = random() % 400; }
		std::thread requester([&] {
			for (unsigned i = 1; i < count; ++i) {
				std::this_thread::sleep_for(std::chrono::microseconds(delays[i]));
				if (delays[i] % 3) { sources[i].request(); }
			}
		});
		rig.run();
		requester.join();
		rig.run();
		for (unsigned i = 0; i < count; ++i) {
			check(results[i].finished, "every run ends");
			check(!steps[i]->in_flight(), "nothing is left in flight");
			if (results[i].error) {
				check(results[i].cancelled() && !steps[i]->done(), "a run fails only by cancellation, unfinished");
				++cancelled;
			} else {
				check(steps[i]->done() && !steps[i]->error, "an uncancelled run completes");
				++finished;
			}
		}
		check(!rig.queue->busy(), "the queue is idle");
		Log last;
		auto next = std::make_shared<Steps>("n", last, 1);
		Result done;
		rig.start(rig.ref().run(next), done);
		rig.run();
		check(done.finished && !done.error && next->done(), "the slot is free after the round");
	}
	std::cout << "asio_seam: " << rounds << " racing rounds, " << cancelled << " cancelled, " << finished << " finished\n";
}

void turn_lane() {
	Rig rig;
	Log log;
	auto holder = std::make_shared<Steps>("h", log, 1);
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	holder->file()->before_write = [opened] { opened.wait(); };
	auto op = [&](const char *name, unsigned count, j::IOLane lane) {
		return std::make_shared<Lane>(std::make_shared<Steps>(name, log, count), lane);
	};
	// h holds the slot. Queued behind it, in this order: b1 (Bulk, 2 primitives), b2 (Bulk, 1), t (Turn, 1).
	auto b1 = op("b1", 2, j::IOLane::Bulk), b2 = op("b2", 1, j::IOLane::Bulk), t = op("t", 1, j::IOLane::Turn);
	Result rh, r1, r2, r3;
	rig.start(rig.ref().run(holder), rh);
	rig.start(rig.ref().run(b1), r1);
	rig.start(rig.ref().run(b2), r2);
	rig.start(rig.ref().run(t), r3);
	asio::post(rig.context, [&] { gate.set_value(); });
	rig.run();
	check(rh.finished && r1.finished && r2.finished && r3.finished && !rig.queue->busy(), "all finish");
	check(!rh.error && !r1.error && !r2.error && !r3.error, "none fails");
	// h; then the Turn ahead of both Bulks; b1's second primitive queues behind b2's first.
	expect(log, {"h:write", "t:write", "b1:write", "b2:write", "b1:sync_data"},
		   "a Turn's primitive is admitted ahead of waiting Bulk ones, Bulk in arrival order");
}

// A Turn operation's second primitive does not queue behind a waiting Bulk one: the Bulk waits for
// the whole turn, a write and its SyncData.
void turn_keeps_the_slot() {
	Rig rig;
	Log log;
	auto holder = std::make_shared<Steps>("h", log, 1);
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	holder->file()->before_write = [opened] { opened.wait(); };
	auto bulk = std::make_shared<Lane>(std::make_shared<Steps>("b", log, 1), j::IOLane::Bulk);
	auto turn = std::make_shared<Lane>(std::make_shared<Steps>("t", log, 2), j::IOLane::Turn);
	Result rh, rb, rt;
	rig.start(rig.ref().run(holder), rh);
	rig.start(rig.ref().run(bulk), rb);
	rig.start(rig.ref().run(turn), rt);
	asio::post(rig.context, [&] { gate.set_value(); });
	rig.run();
	check(rh.finished && rb.finished && rt.finished && !rb.error && !rt.error, "all finish");
	expect(log, {"h:write", "t:write", "t:sync_data", "b:write"}, "the Turn's two primitives run before the waiting Bulk one");
}

void token_between_primitives() {
	Rig rig;
	Log log;
	task::cancel_source source;
	auto op = std::make_shared<Steps>("a", log, 3);
	op->on_completed = [&](unsigned step) { if (step == 1) { source.request(); } };
	Result result;
	rig.start(rig.ref().run(op, source.token()), result);
	rig.run();
	check(result.cancelled() && !op->done() && !op->in_flight() && !rig.queue->busy(), "a request between primitives stops the operation");
	expect(log, {"a:write"}, "the second primitive was not issued");
	// Requested before the call: nothing is issued, nothing waits.
	Log none;
	auto idle = std::make_shared<Steps>("b", none, 1);
	task::cancel_source early;
	early.request();
	Result refused;
	rig.start(rig.ref().run(idle, early.token()), refused);
	rig.run();
	check(refused.cancelled() && none.empty() && !idle->in_flight(), "a token requested first issues nothing");
	// The slot is free after both.
	Log last;
	auto next = std::make_shared<Steps>("c", last, 1);
	Result done;
	rig.start(rig.ref().run(next), done);
	rig.run();
	check(done.finished && !done.error && next->done(), "the slot is free after a cancelled operation");
}

// Two Turn operations at once on one driver abort in Debug; the control, one Turn with a Bulk
// beside it and a second Turn after the first, runs. Each runs in a child process.
#ifndef NDEBUG
int in_child(void (*scenario)()) {
	std::cout.flush();
	std::cerr.flush();
	pid_t pid = ::fork();
	check(pid >= 0, "fork");
	if (pid == 0) {
		scenario();
		std::_Exit(0);
	}
	int status = 0;
	check(::waitpid(pid, &status, 0) == pid, "waitpid");
	return status;
}
void two_turn_sources() {
	Rig rig;
	Log log;
	auto first = std::make_shared<Steps>("t1", log, 2);
	auto t1 = std::make_shared<Lane>(first, j::IOLane::Turn);
	auto t2 = std::make_shared<Lane>(std::make_shared<Steps>("t2", log, 2), j::IOLane::Turn);
	// t1's first write waits until t2 has started, so the two overlap.
	std::promise<void> gate;
	auto opened = gate.get_future().share();
	first->file()->before_write = [opened] { opened.wait(); };
	Result r1, r2;
	rig.start(rig.ref().run(t1), r1);
	rig.start(rig.ref().run(t2), r2);
	asio::post(rig.context, [&] { gate.set_value(); });
	rig.run();
}
void one_turn_source() {
	Rig rig;
	Log log;
	auto t1 = std::make_shared<Lane>(std::make_shared<Steps>("t1", log, 2), j::IOLane::Turn);
	auto bulk = std::make_shared<Lane>(std::make_shared<Steps>("b", log, 2), j::IOLane::Bulk);
	auto t2 = std::make_shared<Lane>(std::make_shared<Steps>("t2", log, 2), j::IOLane::Turn);
	Result r1, rb, r2;
	rig.start(rig.ref().run(t1), r1);
	rig.start(rig.ref().run(bulk), rb);
	rig.run();
	rig.start(rig.ref().run(t2), r2);
	rig.run();
	check(r1.finished && rb.finished && r2.finished && !r1.error && !rb.error && !r2.error, "one Turn source runs");
}
void turn_source_assertion() {
	int control = in_child(one_turn_source);
	check(WIFEXITED(control) && WEXITSTATUS(control) == 0, "one Turn source at a time passes the assertion");
	int two = in_child(two_turn_sources);
	check(WIFSIGNALED(two) && WTERMSIG(two) == SIGABRT, "two Turn operations at once abort");
}
#endif
} // namespace

int main(int argc, char **argv) {
	try {
#ifndef NDEBUG
		turn_source_assertion();
#else
		std::cout << "asio_seam: NDEBUG, one-Turn-source assertion not checked\n";
#endif
		through_the_reference();
		sync_data_fault();
		shared_slot();
		token_while_waiting();
		cancel_while_waiting();
		cancel_races_admission(argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 200);
		turn_lane();
		turn_keeps_the_slot();
		token_between_primitives();
	} catch (const std::exception &error) {
		std::cerr << "failed: " << error.what() << '\n';
		return 1;
	}
	std::cout << "asio_seam: ok\n";
	return 0;
}

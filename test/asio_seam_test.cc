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
// several operations sharing the one queue slot a primitive at a time, and the token read before
// each primitive is issued, while an operation waits for the slot included.
#include "completion/asio_completion.h"
#include "completion/native_completion.h"
#include "completion/seam.h"
#include "seam_support.h"
#include <future>
#include <memory>
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

// A token requested while an operation waits for the slot is read when the slot reaches it: its
// primitive is not issued, the slot passes on, and the operations behind it still run.
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
} // namespace

int main() {
	try {
		through_the_reference();
		sync_data_fault();
		shared_slot();
		token_while_waiting();
		token_between_primitives();
	} catch (const std::exception &error) {
		std::cerr << "failed: " << error.what() << '\n';
		return 1;
	}
	std::cout << "asio_seam: ok\n";
	return 0;
}

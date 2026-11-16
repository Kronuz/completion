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

// The seam: completion_ref over InlineCompletion and HeldCompletion on a task scheduler. Inline
// runs in the call, with the token read before each primitive. Held holds every primitive until
// the test executes, delivers, fails or drops it, in any order, one step at a time. Each case
// observes its effect through a log of what the backend ran.
#include "completion/seam.h"
#include "seam_support.h"
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace j = kronuz::journal;
using namespace seam_support;

namespace {
// A scheduler that owns its roots: a task left suspended (a dropped primitive) dies with it.
class Loop {
  public:
	void post(std::coroutine_handle<> handle) { inner_.post(handle); }
	std::uint64_t now() const noexcept { return inner_.now(); }
	task::root_list *roots() noexcept { return &roots_; }
	std::size_t run() { return inner_.run(); }
	~Loop() {
		roots_.close();
		roots_.destroy_all();
	}

  private:
	task::inline_scheduler inner_;
	task::root_list roots_;
};

struct Outcome {
	bool finished = false;
	std::exception_ptr error;
	bool cancelled() const {
		try { if (error) { std::rethrow_exception(error); } } catch (const task::cancelled &) { return true; } catch (...) {}
		return false;
	}
};
void start(Loop &loop, task::task<void> work, Outcome &outcome) {
	task::spawn(task::scheduler_ref::of(loop), std::move(work), [&outcome](std::exception_ptr error) {
		outcome.finished = true;
		outcome.error = error;
	});
}
std::shared_ptr<Steps> steps(const char *name, Log &log, unsigned count) {
	return std::make_shared<Steps>(name, log, count);
}
static_assert(j::completion_driver<j::InlineCompletion>);
static_assert(j::completion_driver<j::HeldCompletion>);
static_assert(!j::completion_driver<int>);

void reference() {
	j::InlineCompletion inline_io;
	j::HeldCompletion held;
	auto a = j::completion_ref::of(inline_io), b = j::completion_ref::of(held), none = j::completion_ref{};
	check(a && b && !none && a == a && !(a == b) && !(none == a), "a reference names its driver");
	bool refused = false;
	try { static_cast<void>(none.run(nullptr)); } catch (const std::logic_error &) { refused = true; }
	check(refused, "an empty reference refuses a run");
}

// The loop InlineCompletion runs is drive_synchronously: one operation, the same primitives.
void inline_is_the_loop() {
	Log direct, through_ref;
	auto a = steps("f", direct, 4);
	j::drive_synchronously(*a);
	check(a->done() && !a->in_flight(), "drive_synchronously finishes the operation");
	Loop loop;
	j::InlineCompletion inline_io;
	auto b = steps("f", through_ref, 4);
	Outcome outcome;
	start(loop, j::completion_ref::of(inline_io).run(b), outcome);
	loop.run();
	check(outcome.finished && !outcome.error && b->done(), "InlineCompletion finishes the operation");
	expect(through_ref, direct, "InlineCompletion issues the primitives drive_synchronously does");
	expect(direct, {"f:write", "f:sync_data", "f:write", "f:sync_data"}, "the primitives, in order");
	// The proceed form: stopping before a primitive leaves the operation idle and unfinished.
	Log stopped;
	auto c = steps("f", stopped, 3);
	unsigned asked = 0;
	check(!j::drive_synchronously(*c, [&] { return ++asked <= 2; }) && !c->done() && !c->in_flight() && stopped.size() == 2,
		  "a refused primitive is not issued");
}

void inline_token() {
	Loop loop;
	j::InlineCompletion inline_io;
	auto ref = j::completion_ref::of(inline_io);
	{
		// Requested before the call: nothing is issued.
		Log log;
		task::cancel_source source;
		source.request();
		auto op = steps("f", log, 3);
		Outcome outcome;
		start(loop, ref.run(op, source.token()), outcome);
		loop.run();
		check(outcome.cancelled() && log.empty() && !op->in_flight() && !op->done(), "a token requested first issues nothing");
	}
	{
		// Requested while the first primitive completes: the second is not issued.
		Log log;
		task::cancel_source source;
		auto op = steps("f", log, 3);
		op->on_completed = [&](unsigned step) { if (step == 1) { source.request(); } };
		Outcome outcome;
		start(loop, ref.run(op, source.token()), outcome);
		loop.run();
		check(outcome.cancelled() && !op->done() && !op->in_flight(), "a request between primitives stops the operation");
		expect(log, {"f:write"}, "only the primitive already issued ran");
	}
	{
		// Requested by the last primitive: the work is done, so the call returns normally.
		Log log;
		task::cancel_source source;
		auto op = steps("f", log, 2);
		op->on_completed = [&](unsigned step) { if (step == 2) { source.request(); } };
		Outcome outcome;
		start(loop, ref.run(op, source.token()), outcome);
		loop.run();
		check(outcome.finished && !outcome.error && op->done(), "a request after the last primitive does not fail the call");
	}
	{
		// A busy operation is refused.
		Log log;
		auto op = steps("f", log, 1);
		op->submitted();
		Outcome outcome;
		start(loop, ref.run(op), outcome);
		loop.run();
		check(outcome.finished && outcome.error && !outcome.cancelled(), "an operation in flight is refused");
	}
}

// Two operations hold a primitive each; the test releases them out of order, one step at a time.
void held_reorder() {
	Loop loop;
	Log log;
	j::HeldCompletion held;
	auto ref = j::completion_ref::of(held);
	auto a = steps("a", log, 2), b = steps("b", log, 2);
	Outcome done_a, done_b;
	start(loop, ref.run(a), done_a);
	start(loop, ref.run(b), done_b);
	loop.run();
	check(held.held() == 2 && a->in_flight() && b->in_flight() && log.empty(), "every primitive is held, none run");
	auto tickets = held.tickets();
	check(tickets.size() == 2 && held.request(tickets[0]).kind == j::PrimitiveKind::Write, "a held primitive's request is readable");
	check(held.holds(tickets[0]) && !held.executed(tickets[0]), "a held primitive is not executed");

	held.complete(tickets[1]);  // b first
	check(b->completed == 0 && log.size() == 1, "complete runs the primitive; the operation hears of it on its scheduler");
	loop.run();
	check(b->completed == 1 && held.held() == 2, "b's next primitive is held behind a's first");
	auto next = held.tickets();
	check(next.size() == 2 && next[0] == tickets[0] && next[1] != tickets[1], "a keeps its ticket; b's next is new");
	held.complete(next[1]);  // b again, before a's first
	loop.run();
	check(done_b.finished && !done_b.error && b->done() && !done_a.finished, "b finishes while a is still held");
	expect(log, {"b:write", "b:sync_data"}, "b's primitives ran in b's order, before a's");
	held.complete(tickets[0]);
	loop.run();
	held.complete(held.tickets().front());
	loop.run();
	check(done_a.finished && !done_a.error && a->done() && held.held() == 0, "a finishes last");
	expect(log, {"b:write", "b:sync_data", "a:write", "a:sync_data"}, "the order the test chose");
}

// execute applies the primitive and keeps its completion; deliver acknowledges it.
void held_execute_deliver() {
	Loop loop;
	Log log;
	j::HeldCompletion held;
	auto op = steps("a", log, 1);
	Outcome outcome;
	start(loop, j::completion_ref::of(held).run(op), outcome);
	loop.run();
	auto ticket = held.tickets().front();
	bool refused = false;
	try { held.deliver(ticket); } catch (const std::logic_error &) { refused = true; }
	check(refused, "an unexecuted primitive cannot be delivered");
	held.execute(ticket);
	expect(log, {"a:write"}, "execute runs the primitive");
	check(held.executed(ticket) && held.holds(ticket) && op->in_flight() && op->completed == 0, "executed, not acknowledged");
	refused = false;
	try { held.execute(ticket); } catch (const std::logic_error &) { refused = true; }
	check(refused, "a primitive runs once");
	loop.run();
	check(!outcome.finished && op->in_flight(), "nothing resumes before delivery");
	held.deliver(ticket);
	check(!held.holds(ticket) && op->completed == 0, "delivery posts, never calls the operation");
	loop.run();
	check(outcome.finished && !outcome.error && op->done() && log.size() == 1, "delivery completes the operation");
	refused = false;
	try { held.request(ticket); } catch (const std::out_of_range &) { refused = true; }
	check(refused, "a delivered ticket is gone");
}

void held_fail() {
	Loop loop;
	Log log;
	j::HeldCompletion held;
	auto op = steps("a", log, 2);
	Outcome outcome;
	start(loop, j::completion_ref::of(held).run(op), outcome);
	loop.run();
	held.fail(held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "injected")));
	loop.run();
	check(outcome.finished && !outcome.error && op->done() && op->error, "a failed primitive reaches the operation as its error");
	try { std::rethrow_exception(op->error); } catch (const std::system_error &e) {
		check(e.code() == std::error_code(EIO, std::generic_category()), "with the injected errno");
	}
	check(log.empty() && held.held() == 0, "the backend never ran it");
	// Applied, then reported failed: the bytes are there, the operation hears an error.
	Log applied;
	auto other = steps("b", applied, 1);
	Outcome second;
	start(loop, j::completion_ref::of(held).run(other), second);
	loop.run();
	auto ticket = held.tickets().front();
	held.execute(ticket);
	held.fail(ticket, std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "after the write")));
	loop.run();
	check(other->error && applied.size() == 1, "an executed primitive can still be failed");
}

// A crash: the primitive is forgotten and its operation stays in flight, forever.
void held_drop() {
	Log log;
	j::HeldCompletion held;
	std::shared_ptr<Steps> op;
	{
		Loop loop;
		op = steps("a", log, 2);
		Outcome outcome;
		start(loop, j::completion_ref::of(held).run(op), outcome);
		loop.run();
		auto ticket = held.tickets().front();
		held.execute(ticket);
		held.drop(ticket);
		check(held.held() == 0 && !held.holds(ticket), "a dropped primitive leaves the queue");
		loop.run();
		check(!outcome.finished && op->in_flight() && !op->done() && op->completed == 0, "the operation never hears of it");
		expect(log, {"a:write"}, "a primitive executed before the crash stays applied");
	}
	check(op.use_count() == 1, "tearing the scheduler down releases the frame's hold on the operation");
}

void held_token() {
	Loop loop;
	Log log;
	j::HeldCompletion held;
	auto ref = j::completion_ref::of(held);
	{
		task::cancel_source source;
		source.request();
		auto op = steps("a", log, 2);
		Outcome outcome;
		start(loop, ref.run(op, source.token()), outcome);
		loop.run();
		check(outcome.cancelled() && held.held() == 0 && !op->in_flight() && log.empty(), "a token requested first holds nothing");
	}
	{
		// Requested while a primitive is held: it still completes into the operation; the next is not issued.
		task::cancel_source source;
		auto op = steps("b", log, 3);
		Outcome outcome;
		start(loop, ref.run(op, source.token()), outcome);
		loop.run();
		check(held.held() == 1, "the first primitive is held");
		source.request();
		loop.run();
		check(held.held() == 1 && !outcome.finished, "a request does not release a held primitive");
		held.complete(held.tickets().front());
		loop.run();
		check(outcome.cancelled() && held.held() == 0 && op->completed == 1 && !op->done() && !op->in_flight(),
			  "the held primitive completes, the next is not issued, the caller is released");
		expect(log, {"b:write"}, "the issued primitive was applied once");
	}
}

// A frame destroyed while its primitive is held leaves the queue; a queue destroyed first does
// not reach the frame.
void held_teardown() {
	Log log;
	{
		j::HeldCompletion held;
		{
			Loop loop;
			Outcome outcome;
			auto op = steps("a", log, 1);
			start(loop, j::completion_ref::of(held).run(op), outcome);
			loop.run();
			check(held.held() == 1, "held");
		}
		check(held.held() == 0, "a destroyed frame leaves the queue");
	}
	{
		Loop loop;
		Outcome outcome;
		auto op = steps("a", log, 1);
		auto held = std::make_unique<j::HeldCompletion>();
		start(loop, j::completion_ref::of(*held).run(op), outcome);
		loop.run();
		held.reset();
	}
	check(log.empty(), "nothing ran");
}

// The queue needs a scheduler to resume on.
void held_needs_scheduler() {
	Log log;
	j::HeldCompletion held;
	auto op = steps("a", log, 1);
	bool refused = false;
	try { task::sync_wait(held.run(op)); } catch (const std::logic_error &) { refused = true; }
	check(refused && !op->in_flight(), "a held run outside any scheduler is refused before issuing");
}
} // namespace

int main() {
	reference();
	inline_is_the_loop();
	inline_token();
	held_reorder();
	held_execute_deliver();
	held_fail();
	held_drop();
	held_token();
	held_teardown();
	held_needs_scheduler();
	std::cout << "seam: ok\n";
	return 0;
}

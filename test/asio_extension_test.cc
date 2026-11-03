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

#include "completion/api.h"
#include "completion/asio_completion.h"
#include "completion/native_completion.h"
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <thread>

namespace c = kronuz::io::completion;
namespace j = kronuz::journal;
namespace {
void check(bool value) {
	if (!value)
		throw std::runtime_error("Asio extension invariant failed");
}
struct State {
	std::shared_ptr<c::IO> io;
	std::unique_ptr<j::DirectoryCursor> cursor;
	std::unique_ptr<c::File> file;
	std::unique_ptr<c::File> reusable_file;
	std::unique_ptr<c::DirectoryCursor> reusable_cursor;
	std::array<char, 255> name{};
	std::optional<c::EntryInspection> named;
	std::exception_ptr error;
	unsigned completed = 0;
	std::function<void()> on_completed;
};
class Operation final : public c::Operation {
  public:
	explicit Operation(std::shared_ptr<State> state, std::optional<c::Kind> single = {})
		: state_(std::move(state)), single_(single) {
		prepare();
	}
	const c::Request &request() const override { return request_; }
	void submitted() override {
		if (done_ || submitted_)
			throw std::logic_error("duplicate coroutine original");
		submitted_ = true;
	}
	bool complete(c::Result result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token)
			return false;
		submitted_ = false;
		try {
			if (result.error)
				std::rethrow_exception(result.error);
			if (single_) {
				done_ = true;
				return true;
			}
			switch (request_.kind) {
			case c::Kind::Scan:
				check(static_cast<bool>(result.cursor));
				state_->cursor = std::move(result.cursor);
				break;
			case c::Kind::NextInto:
				check(result.count == (step_ == 1 || step_ == 17 ? 6 : 0));
				if (step_ == 1 || step_ == 17)
					check(std::string_view(state_->name.data(), result.count) == "anchor");
				break;
			case c::Kind::InspectEntry:
				check(result.inspection && result.inspection->footprint.logical_bytes == 5);
				state_->named = result.inspection;
				break;
			case c::Kind::Open:
				check(static_cast<bool>(result.file));
				state_->file = std::move(result.file);
				break;
			case c::Kind::InspectFile:
				check(result.inspection == state_->named);
				break;
			case c::Kind::OpenCandidateInto:
			case c::Kind::SharedLease:
			case c::Kind::ExclusiveLease:
				check(result.count == 1);
				break;
			default:
				break;
			}
			++state_->completed;
			if (state_->on_completed)
				state_->on_completed();
			if (++step_ == kinds_.size()) {
				done_ = true;
				return true;
			}
			prepare();
		} catch (...) {
			state_->error = std::current_exception();
			done_ = true;
		}
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_; }
	c::IO &io() const noexcept override { return *state_->io; }

  private:
	void prepare() {
		request_ = {};
		request_.token.operation = 1;
		request_.token.step = step_;
		request_.kind = single_.value_or(kinds_[step_]);
		request_.source = single_ ? "../invalid" : "anchor";
		if (!single_ && request_.kind == c::Kind::CreateInto)
			request_.source = "created";
		request_.destination_bytes = state_->name;
		if (step_ >= 12 && !single_) {
			request_.cursor = std::shared_ptr<c::DirectoryCursor>(state_, state_->reusable_cursor.get());
			request_.file = std::shared_ptr<c::File>(state_, state_->reusable_file.get());
			return;
		}
		if (state_->cursor)
			request_.cursor = std::shared_ptr<j::DirectoryCursor>(state_, state_->cursor.get());
		if (state_->file)
			request_.file = std::shared_ptr<c::File>(state_, state_->file.get());
	}
	inline static constexpr std::array kinds_{
		c::Kind::Scan,	   c::Kind::NextInto,		c::Kind::NextInto,			c::Kind::InspectEntry,
		c::Kind::Open,	   c::Kind::InspectFile,	c::Kind::SharedLease,		c::Kind::CloseFile,
		c::Kind::Open,	   c::Kind::ExclusiveLease, c::Kind::CloseFile,			c::Kind::CloseCursor,
		c::Kind::OpenInto, c::Kind::CloseFile,		c::Kind::OpenCandidateInto, c::Kind::CloseFile,
		c::Kind::ScanInto, c::Kind::NextInto,		c::Kind::CloseCursor,		c::Kind::CreateInto,
		c::Kind::CloseFile};
	std::shared_ptr<State> state_;
	std::optional<c::Kind> single_;
	c::Request request_;
	std::size_t step_ = 0;
	bool submitted_ = false, done_ = false;
};
task::task<void> single_then_remaining(std::shared_ptr<j::AsioCompletionDriver<c::NativeQueue>> driver,
									   std::shared_ptr<Operation> operation, std::shared_ptr<State> state,
									   std::shared_ptr<c::NativeQueue> queue) {
	co_await driver->run_one(operation);
	check(state->completed == 1 && !operation->done() && !operation->in_flight() && !queue->busy());
	co_await driver->run(operation);
}
// A queue whose poll() never reports "not yet": the drive reaches every step without awaiting.
class SpinQueue {
  public:
	int notification_descriptor() const noexcept { return queue_.notification_descriptor(); }
	bool busy() const noexcept { return queue_.busy(); }
	bool submit(const std::shared_ptr<j::IOOperation> &operation) { return queue_.submit(operation); }
	std::optional<j::OwnedCompletion> poll() {
		auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;) {
			if (auto result = queue_.poll())
				return result;
			check(std::chrono::steady_clock::now() < limit);
			std::this_thread::yield();
		}
	}

  private:
	c::NativeQueue queue_;
};
// A queue that holds the first reaped completion back for `hold`, so the drive waits on it.
class HeldQueue {
  public:
	int notification_descriptor() const noexcept { return queue_.notification_descriptor(); }
	bool busy() const noexcept { return queue_.busy() || held_; }
	bool submit(const std::shared_ptr<j::IOOperation> &operation) { return !held_ && queue_.submit(operation); }
	std::optional<j::OwnedCompletion> poll() {
		if (held_) {
			if (std::chrono::steady_clock::now() < release_)
				return std::nullopt;
			auto result = std::move(held_);
			held_.reset();
			return result;
		}
		auto result = queue_.poll();
		if (result && !held_once_) {
			held_once_ = true;
			held_ = std::move(result);
			release_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
			return std::nullopt;
		}
		return result;
	}

  private:
	c::NativeQueue queue_;
	std::optional<j::OwnedCompletion> held_;
	std::chrono::steady_clock::time_point release_;
	bool held_once_ = false;
};
std::shared_ptr<State> make_state(const std::shared_ptr<c::PosixIO> &backend) {
	backend->remove("created");
	auto state = std::make_shared<State>();
	state->io = backend;
	state->reusable_file = backend->make_closed_file();
	state->reusable_cursor = backend->make_closed_cursor();
	return state;
}
void run_to_end(asio::io_context &context, task::asio_scheduler &scheduler, task::task<void> work) {
	std::exception_ptr failure;
	task::spawn(task::scheduler_ref::of(scheduler), std::move(work),
				[&](std::exception_ptr error) { failure = error; });
	context.run();
	if (failure)
		std::rethrow_exception(failure);
}

// Two schedulers on two threads. A run() on the driver's own scheduler returns there, and the
// task that awaited it through another scheduler is posted back home; a task on any other
// scheduler is refused.
struct Homes {
	task::scheduler_ref owner, other;
	std::thread::id owner_thread, other_thread;
	std::shared_ptr<j::AsioCompletionDriver<c::NativeQueue>> driver;
};
bool on(const task::scheduler_ref &where, std::thread::id thread) {
	return task::current() == where && std::this_thread::get_id() == thread;
}
task::task<void> run_through_owner(Homes &homes, std::shared_ptr<Operation> operation, bool single) {
	co_await task::schedule_on{homes.owner};
	check(on(homes.owner, homes.owner_thread));
	if (single)
		co_await homes.driver->run_one(operation);
	else
		co_await homes.driver->run(operation);
	check(on(homes.owner, homes.owner_thread));
}
task::task<void> from_other(Homes &homes, std::shared_ptr<Operation> refused, std::shared_ptr<Operation> operation,
							std::shared_ptr<c::NativeQueue> queue) {
	check(on(homes.other, homes.other_thread));
	bool thrown = false;
	try {
		co_await homes.driver->run(refused);
	} catch (const std::logic_error &) {
		thrown = true;
	}
	check(thrown && !refused->in_flight() && !queue->busy());
	check(on(homes.other, homes.other_thread));
	co_await run_through_owner(homes, operation, true);
	check(on(homes.other, homes.other_thread));
	co_await run_through_owner(homes, operation, false);
	check(on(homes.other, homes.other_thread) && operation->done());
}
void scheduler_homes(const std::shared_ptr<c::PosixIO> &backend) {
	asio::io_context owner_context, other_context;
	task::asio_scheduler owner(owner_context.get_executor()), other(other_context.get_executor());
	auto owner_guard = asio::make_work_guard(owner_context);
	auto other_guard = asio::make_work_guard(other_context);
	std::thread owner_thread([&] { owner_context.run(); }), other_thread([&] { other_context.run(); });
	auto queue = std::make_shared<c::NativeQueue>();
	Homes homes{task::scheduler_ref::of(owner), task::scheduler_ref::of(other), owner_thread.get_id(),
				other_thread.get_id(),
				std::make_shared<j::AsioCompletionDriver<c::NativeQueue>>(owner, queue)};
	auto state = make_state(backend);
	auto refused = std::make_shared<Operation>(state, c::Kind::InspectEntry);
	auto operation = std::make_shared<Operation>(state);
	std::promise<void> finished;
	task::spawn(homes.other, from_other(homes, refused, operation, queue),
				[&](std::exception_ptr error) { error ? finished.set_exception(error) : finished.set_value(); });
	auto result = finished.get_future();
	std::exception_ptr failure;
	try {
		result.get();
	} catch (...) {
		failure = std::current_exception();
	}
	owner_guard.reset();
	other_guard.reset();
	owner_thread.join();
	other_thread.join();
	if (failure)
		std::rethrow_exception(failure);
	check(state->completed == 21 && !queue->busy());
}

// A fast backend yields to the owner's queue every 16 steps: work posted at step 3 runs after
// step 15 and before step 16.
void yields_to_owner(const std::shared_ptr<c::PosixIO> &backend) {
	asio::io_context context;
	task::asio_scheduler scheduler(context.get_executor());
	auto queue = std::make_shared<SpinQueue>();
	auto driver = std::make_shared<j::AsioCompletionDriver<SpinQueue>>(scheduler, queue);
	auto state = make_state(backend);
	unsigned seen = 0;
	bool posted = false, ran = false;
	state->on_completed = [&] {
		if (state->completed == 3 && !posted) {
			posted = true;
			asio::post(context, [&] {
				ran = true;
				seen = state->completed;
			});
		}
	};
	run_to_end(context, scheduler, driver->run(std::make_shared<Operation>(state)));
	check(ran && seen == 15 && state->completed == 21);
}

// Smoke test: run() takes no cancel token, so a request made while the drive waits cannot reach
// it (structural, not proven here). The original completes every step and leaves nothing in flight.
task::task<void> uncancelled_drive(std::shared_ptr<j::AsioCompletionDriver<HeldQueue>> driver,
								   std::shared_ptr<Operation> operation, std::shared_ptr<State> state,
								   std::shared_ptr<HeldQueue> queue, task::cancel_source &source,
								   asio::steady_timer &timer, unsigned &completed_at_request) {
	co_await driver->run(operation);
	check(completed_at_request < 21 && source.token().requested());
	check(operation->done() && !operation->in_flight() && !queue->busy() && state->completed == 21);
	auto error = co_await task::await_asio<void(asio::error_code)>(
		[&](auto handler) { timer.async_wait(std::move(handler)); }, source.token());
	check(error == asio::error::operation_aborted);
}
void cancel_mid_drive(const std::shared_ptr<c::PosixIO> &backend) {
	asio::io_context context;
	task::asio_scheduler scheduler(context.get_executor());
	auto queue = std::make_shared<HeldQueue>();
	auto driver = std::make_shared<j::AsioCompletionDriver<HeldQueue>>(scheduler, queue);
	auto state = make_state(backend);
	auto operation = std::make_shared<Operation>(state);
	task::cancel_source source;
	unsigned completed_at_request = 99;
	asio::steady_timer request(context, std::chrono::milliseconds(20)), timer(context);
	request.async_wait([&](asio::error_code) {
		completed_at_request = state->completed;
		check(!operation->done());
		source.request();
	});
	run_to_end(context, scheduler,
			   uncancelled_drive(driver, operation, state, queue, source, timer, completed_at_request));
	check(completed_at_request < 21 && state->completed == 21);
}
} // namespace
int main(int argc, char **argv) {
	try {
		if (argc != 2 && argc != 3)
			throw std::invalid_argument("scratch directory required, then optionally one section");
		std::string_view only = argc == 3 ? argv[2] : "";
		auto wanted = [&](std::string_view section) { return only.empty() || only == section; };
		std::filesystem::path directory(argv[1]);
		check(std::filesystem::create_directory(directory));
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		auto backend = std::make_shared<c::PosixIO>(directory);
		{
			auto file = backend->create_exclusive("anchor");
			check(file->write_at(0, "proof") == 5);
		}
		for (bool metrics : {false, true}) {
			if (!wanted("core"))
				break;
			backend->remove("created");
			asio::io_context context;
			task::asio_scheduler scheduler(context.get_executor());
			auto queue = std::make_shared<c::NativeQueue>();
			auto driver = std::make_shared<j::AsioCompletionDriver<c::NativeQueue>>(scheduler, queue, metrics);
			auto state = std::make_shared<State>();
			state->io = backend;
			state->reusable_file = backend->make_closed_file();
			state->reusable_cursor = backend->make_closed_cursor();
			auto operation = std::make_shared<Operation>(state);
			std::weak_ptr<Operation> weak = operation;
			std::exception_ptr failure;
			bool done = false;
			auto work =
				metrics ? driver->run(operation) : single_then_remaining(driver, operation, state, queue);
			task::spawn(task::scheduler_ref::of(scheduler), std::move(work), [&](std::exception_ptr error) {
				failure = error;
				done = true;
			});
			operation.reset();
			check(!weak.expired());
			context.run();
			if (failure)
				std::rethrow_exception(failure);
			if (state->error)
				std::rethrow_exception(state->error);
			check(done && weak.expired() && state->completed == 21 && !queue->busy());
			check(driver->stats().submitted_to_reaped.size() == 12);
			context.restart();
			auto invalid = std::make_shared<Operation>(state, static_cast<c::Kind>(999));
			task::spawn(task::scheduler_ref::of(scheduler), driver->run(invalid),
						[&](std::exception_ptr error) { failure = error; });
			context.run();
			check(static_cast<bool>(failure) && !invalid->in_flight() && !queue->busy());
			context.restart();
			state->error = {};
			auto failed = std::make_shared<Operation>(state, c::Kind::InspectEntry);
			task::spawn(task::scheduler_ref::of(scheduler), driver->run(failed),
						[&](std::exception_ptr error) { failure = error; });
			context.run();
			check(!failure && static_cast<bool>(state->error) && failed->done());
			check(driver->stats().original_drive.count == (metrics ? 2 : 0));
			auto stats = driver->stats();
			for (std::size_t at = 0; at < j::completion_extension_count; ++at)
				check(stats.extension_submitted_to_reaped[at].count == (metrics ? (at == 0	 ? 3
																				   : at == 5 ? 5
																				   : at == 6 ? 2
																							 : 1)
																				: 0));
			check(stats.extension_failed_submitted_to_reaped[1].count == (metrics ? 1 : 0));
		}
		if (wanted("homes"))
			scheduler_homes(backend);
		if (wanted("yield"))
			yields_to_owner(backend);
		if (wanted("cancel"))
			cancel_mid_drive(backend);
		std::filesystem::remove_all(directory);
		std::cout << "task driver dispatch, original retention, metrics, scheduler homes, owner yield and "
					 "uncancelled drive passed\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}

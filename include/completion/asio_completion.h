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

#pragma once

#include "completion.h"
#include "metrics.hh"
#include "task_asio.hh"
#include <array>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

namespace kronuz::journal {

inline constexpr std::size_t completion_primitive_count = static_cast<std::size_t>(PrimitiveKind::Remove) + 1;
inline constexpr std::size_t completion_extension_count =
	std::tuple_size_v<decltype(CompletionStats::native_extensions)>;
struct CompletionDriverStats {
	std::array<kronuz::metrics::DurationSnapshot, completion_primitive_count> submitted_to_reaped{},
		failed_submitted_to_reaped{};
	std::array<kronuz::metrics::DurationSnapshot, completion_extension_count> extension_submitted_to_reaped{},
		extension_failed_submitted_to_reaped{};
	kronuz::metrics::DurationSnapshot original_drive;
};
struct CompletionDriverMetrics {
	std::array<kronuz::metrics::DurationHistogram<>, completion_primitive_count> submitted_to_reaped{},
		failed_submitted_to_reaped{};
	std::array<kronuz::metrics::DurationHistogram<>, completion_extension_count>
		extension_submitted_to_reaped{}, extension_failed_submitted_to_reaped{};
	kronuz::metrics::DurationHistogram<> original_drive;
	CompletionDriverStats snapshot() const noexcept {
		CompletionDriverStats result;
		for (std::size_t i = 0; i < completion_primitive_count; ++i) {
			result.submitted_to_reaped[i] = submitted_to_reaped[i].snapshot();
			result.failed_submitted_to_reaped[i] = failed_submitted_to_reaped[i].snapshot();
		}
		for (std::size_t i = 0; i < completion_extension_count; ++i) {
			result.extension_submitted_to_reaped[i] = extension_submitted_to_reaped[i].snapshot();
			result.extension_failed_submitted_to_reaped[i] =
				extension_failed_submitted_to_reaped[i].snapshot();
		}
		result.original_drive = original_drive.snapshot();
		return result;
	}
};

// Optional task adapter over Asio. The generic journal and completion backend depend on
// neither. The driver belongs to one task::asio_scheduler; only tasks running on it may call
// run(). The queue has one slot: any number of operations may run at once, and each primitive
// takes the slot in the order its caller asked (a Turn's ahead of a Bulk's, see IOLane). One Turn
// source per driver: at most one Turn operation runs at a time (asserted in Debug), so the Bulk
// head gets the slot between turns.
template <class Queue>
class AsioCompletionDriver : public std::enable_shared_from_this<AsioCompletionDriver<Queue>> {
  public:
	AsioCompletionDriver(task::asio_scheduler &scheduler, std::shared_ptr<Queue> queue,
						 bool collect_metrics = false)
		: owner_(task::scheduler_ref::of(scheduler)), queue_(std::move(queue)),
		  notification_(scheduler.executor()), timer_(scheduler.executor()) {
		if (!queue_) {
			throw std::invalid_argument("null completion queue");
		}
		if (collect_metrics) {
			metrics_ = std::make_unique<CompletionDriverMetrics>();
		}
		int fd = ::dup(queue_->notification_descriptor());
		if (fd < 0) {
			throw std::system_error(errno, std::generic_category(), "duplicate IO notification");
		}
		if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
			auto error = errno;
			::close(fd);
			throw std::system_error(error, std::generic_category(), "protect IO notification");
		}
		asio::error_code error;
		notification_.assign(fd, error);
		if (error) {
			::close(fd);
			throw std::system_error(error);
		}
	}
	CompletionDriverStats stats() const noexcept {
		return metrics_ ? metrics_->snapshot() : CompletionDriverStats{};
	}
	// The token is read before each primitive is issued, before and after its wait for the
	// slot, and a request while waiting releases the caller at once: the primitive is not
	// issued and the call throws task::cancelled. A primitive already issued completes into
	// the operation first, and the caller is released then.
	task::task<void> run(std::shared_ptr<IOOperation> operation, task::cancel_token token = {}) {
		return drive(std::move(operation), false, std::move(token));
	}
	// Reap exactly one original primitive, then return control to the owner.
	// This supports operations with explicit paused scheduling boundaries.
	task::task<void> run_one(std::shared_ptr<IOOperation> operation, task::cancel_token token = {}) {
		return drive(std::move(operation), true, std::move(token));
	}

  private:
	// The queue's one slot. A primitive holds it from its submission to its reaping; the next
	// waiter inherits it when it passes, posted to the owner, so the slot is never free while
	// anyone waits. Everything here runs on the owner scheduler.
	struct Waiter {
		enum : std::uint8_t { waiting, granted, cancelled };
		std::coroutine_handle<> handle;
		Waiter *prev = nullptr, *next = nullptr;
		bool linked = false;
		// pass_slot grants it, or its token cancels it from any thread: the first claim wins.
		std::atomic<std::uint8_t> state{waiting};
		bool claim(std::uint8_t to) noexcept {
			std::uint8_t expected = waiting;
			return state.compare_exchange_strong(expected, to, std::memory_order_acq_rel);
		}
	};
	struct Waiting {
		Waiter *head = nullptr, *tail = nullptr;
		void push(Waiter *waiter) noexcept {
			waiter->prev = tail;
			waiter->next = nullptr;
			(tail ? tail->next : head) = waiter;
			tail = waiter;
			waiter->linked = true;
		}
		void erase(Waiter *waiter) noexcept {
			(waiter->prev ? waiter->prev->next : head) = waiter->next;
			(waiter->next ? waiter->next->prev : tail) = waiter->prev;
			waiter->linked = false;
		}
		Waiter *pop() noexcept {
			auto *waiter = head;
			if (waiter) { erase(waiter); }
			return waiter;
		}
	};
	// Awaited before each primitive. A caller holding the slot from its previous primitive keeps
	// it unless a waiter goes ahead of it; otherwise it hands the slot over and queues. Waiters
	// are in two queues by lane, first come first served in each. A token requested while it
	// waits claims the waiter and posts its resume; it then leaves the queue and throws.
	struct Admit {
		struct Release {
			Admit *admit;
			void operator()() const {
				auto handle = admit->waiter.handle;
				auto owner = admit->driver.owner_;
				if (admit->waiter.claim(Waiter::cancelled)) { owner.post(handle); }
			}
		};
		AsioCompletionDriver &driver;
		bool &holding;
		IOLane lane;
		const task::cancel_token &token;
		Waiter waiter{};
		std::optional<task::cancel_callback<Release>> release{};
		bool await_ready() noexcept {
			if (holding) { return !driver.waiter_ahead(lane); }
			if (driver.slot_taken_) { return false; }
			driver.slot_taken_ = true;
			return true;
		}
		void await_suspend(std::coroutine_handle<> handle) noexcept {
			waiter.handle = handle;
			driver.waiting_[static_cast<std::size_t>(lane)].push(&waiter);
			if (holding) {
				holding = false;
				driver.pass_slot();
			}
			release.emplace(token, Release{this});
		}
		void await_resume() {
			if (waiter.state.load(std::memory_order_acquire) == Waiter::cancelled) {
				leave();
				throw task::cancelled{};
			}
			holding = true;
		}
		~Admit() {
			release.reset(); // waits for a Release running on another thread
			leave();
		}
		void leave() noexcept {
			if (waiter.linked) { driver.waiting_[static_cast<std::size_t>(lane)].erase(&waiter); }
		}
	};
	// A Turn waits behind earlier Turns only; a Bulk behind every Turn and every earlier Bulk.
	bool waiter_ahead(IOLane lane) const noexcept {
		return waiting_[static_cast<std::size_t>(IOLane::Turn)].head != nullptr ||
			   (lane == IOLane::Bulk && waiting_[static_cast<std::size_t>(IOLane::Bulk)].head != nullptr);
	}
	// A waiter its token already claimed is skipped: it resumes by its own post.
	void pass_slot() {
		for (;;) {
			auto *next = waiting_[static_cast<std::size_t>(IOLane::Turn)].pop();
			if (!next) { next = waiting_[static_cast<std::size_t>(IOLane::Bulk)].pop(); }
			if (!next) {
				slot_taken_ = false;
				return;
			}
			if (next->claim(Waiter::granted)) {
				owner_.post(next->handle); // The slot passes to it, still taken.
				return;
			}
		}
	}
	// One Turn source per driver: turn logs are named by (generation, loop), and strict priority
	// would starve Bulk under two.
	struct TurnSource {
		AsioCompletionDriver &driver;
		bool turn;
		TurnSource(AsioCompletionDriver &d, bool t) : driver(d), turn(t) {
			if (turn) {
				assert(!driver.turn_running_ && "one Turn operation at a time per AsioCompletionDriver");
				driver.turn_running_ = true;
			}
		}
		~TurnSource() {
			if (turn) { driver.turn_running_ = false; }
		}
	};
	bool turn_running_ = false;
	// Releases the slot the drive still holds when it ends, however it ends.
	struct Hold {
		AsioCompletionDriver &driver;
		bool holding = false;
		~Hold() {
			if (holding) { driver.pass_slot(); }
		}
	};

	task::task<void> drive(std::shared_ptr<IOOperation> operation, bool single, task::cancel_token token) {
		auto lifetime = this->shared_from_this();
		if (task::current() != owner_) {
			throw std::logic_error("completion driver requires its owner scheduler");
		}
		if (!operation || operation->in_flight()) {
			throw std::logic_error("completion driver is not available");
		}
		auto started = metrics_ ? Clock::now() : Clock::time_point{};
		TurnSource turn{*this, operation->lane() == IOLane::Turn};
		Hold hold{*this};
		std::size_t immediate_steps = 0;
		while (!operation->done()) {
			// A fast backend still yields bounded owner turns to sockets and timers.
			if (++immediate_steps == 16) {
				immediate_steps = 0;
				co_await task::schedule_on{owner_};
			}
			auto primitive = operation->request().kind;
			auto kind = static_cast<std::size_t>(primitive);
			if (!CompletionStats::supported(primitive)) {
				throw std::invalid_argument("invalid completion primitive kind");
			}
			if (token.requested()) {
				throw task::cancelled{};
			}
			co_await Admit{*this, hold.holding, operation->lane(), token};
			if (token.requested()) {
				throw task::cancelled{};
			}
			auto submitted = metrics_ ? Clock::now() : Clock::time_point{};
			if (!queue_->submit(operation)) {
				throw std::logic_error("reserved completion queue is busy");
			}
			for (;;) {
				if (auto result = queue_->poll()) {
					auto latency = metrics_ ? elapsed_ns(submitted) : 0;
					bool failed = static_cast<bool>(result->completion.error);
					if (result->operation != operation ||
						!operation->complete(std::move(result->completion))) {
						throw std::logic_error("completion ownership mismatch");
					}
					if (metrics_) {
						auto &histogram =
							kind < completion_primitive_count
								? (failed ? metrics_->failed_submitted_to_reaped[kind]
										  : metrics_->submitted_to_reaped[kind])
								: (failed
									   ? metrics_->extension_failed_submitted_to_reaped
											 [kind - completion_primitive_count]
									   : metrics_->extension_submitted_to_reaped[kind -
																				 completion_primitive_count]);
						histogram.observe(latency);
					}
					break;
				}
				// Bounded polling also protects against a lost fallback wakeup.
				// The descriptor provides prompt readiness where supported.
				timer_.expires_after(std::chrono::milliseconds(1));
				if (notification_failed_) {
					auto error = co_await task::await_asio<void(asio::error_code)>(
						[&](auto handler) { timer_.async_wait(std::move(handler)); });
					if (error) {
						throw std::system_error(error);
					}
				} else {
					// The first of the two ends the wait; the other is cancelled and reaped.
					auto [order, notification_error, timer_error] = co_await task::await_asio<void(
						std::array<std::size_t, 2>, asio::error_code, asio::error_code)>([&](auto handler) {
						asio::experimental::make_parallel_group(
							notification_.async_wait(asio::posix::stream_descriptor::wait_read, asio::deferred),
							timer_.async_wait(asio::deferred))
							.async_wait(asio::experimental::wait_for_one(), std::move(handler));
					});
					if (notification_error && notification_error != asio::error::operation_aborted) {
						notification_failed_ = true;
					}
					if (order[0] == 1 && timer_error) {
						throw std::system_error(timer_error);
					}
				}
			}
			if (single)
				break;
		}
		if (metrics_) {
			metrics_->original_drive.observe(elapsed_ns(started));
		}
	}

  private:
	using Clock = std::chrono::steady_clock;
	static std::uint64_t elapsed_ns(Clock::time_point start) noexcept {
		auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
	}
	std::unique_ptr<CompletionDriverMetrics> metrics_;
	task::scheduler_ref owner_;
	std::shared_ptr<Queue> queue_;
	asio::posix::stream_descriptor notification_;
	asio::steady_timer timer_;
	std::array<Waiting, 2> waiting_;  // by IOLane
	bool slot_taken_ = false, notification_failed_ = false;
};
} // namespace kronuz::journal

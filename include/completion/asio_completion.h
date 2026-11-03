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
#include <chrono>
#include <fcntl.h>
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
// neither. The driver belongs to one task::asio_scheduler; only tasks running on it may
// call run(), and one at a time.
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
	task::task<void> run(std::shared_ptr<IOOperation> operation) {
		return drive(std::move(operation), false);
	}
	// Reap exactly one original primitive, then return control to the owner.
	// This supports operations with explicit paused scheduling boundaries.
	task::task<void> run_one(std::shared_ptr<IOOperation> operation) {
		return drive(std::move(operation), true);
	}

  private:
	task::task<void> drive(std::shared_ptr<IOOperation> operation, bool single) {
		auto lifetime = this->shared_from_this();
		if (task::current() != owner_) {
			throw std::logic_error("completion driver requires its owner scheduler");
		}
		if (active_ || !operation || operation->in_flight()) {
			throw std::logic_error("completion driver is not available");
		}
		// The drive takes no cancel token and awaits no operation with one. A request above
		// it releases interest only: it cannot destroy accepted IO buffers or skip the
		// remaining durable barriers.
		active_ = true;
		auto started = metrics_ ? Clock::now() : Clock::time_point{};
		struct Reset {
			bool &active;
			~Reset() { active = false; }
		} reset{active_};
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
	bool active_ = false, notification_failed_ = false;
};
} // namespace kronuz::journal

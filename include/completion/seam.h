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

// The IO seam the coroutine drivers run through: completion_ref, and the two drivers that need
// no event loop, InlineCompletion and HeldCompletion. Needs task.hh only; the Asio driver
// (asio_completion.h) is the third driver behind the same reference. docs/mechanisms/completion-seam.md.
#include "operation.h"
#include "task.hh"
#include <algorithm>
#include <concepts>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace kronuz::journal {

// A driver runs an operation to its end: one awaited call, one primitive at a time. The token is
// read before each primitive is issued: requested, the next primitive is not issued and the call
// throws task::cancelled. A primitive already issued completes into the operation first; only
// the caller is released, never the operation's buffers.
template <class D>
concept completion_driver = requires(D &driver, std::shared_ptr<IOOperation> operation, task::cancel_token token) {
	{ driver.run(std::move(operation), std::move(token)) } -> std::same_as<task::task<void>>;
};

// A non-owning reference to a completion driver, as task::scheduler_ref is to a scheduler. The
// driver outlives every call through the reference.
class completion_ref {
  public:
	completion_ref() = default;
	template <completion_driver D>
	static completion_ref of(D &driver) noexcept {
		completion_ref ref;
		ref.self_ = &driver;
		ref.run_ = [](void *self, std::shared_ptr<IOOperation> operation, task::cancel_token token) {
			return static_cast<D *>(self)->run(std::move(operation), std::move(token));
		};
		return ref;
	}
	task::task<void> run(std::shared_ptr<IOOperation> operation, task::cancel_token token = {}) const {
		if (!self_) { throw std::logic_error("empty completion reference"); }
		return run_(self_, std::move(operation), std::move(token));
	}
	explicit operator bool() const noexcept { return self_ != nullptr; }
	friend bool operator==(const completion_ref &a, const completion_ref &b) noexcept { return a.self_ == b.self_; }

  private:
	void *self_ = nullptr;
	task::task<void> (*run_)(void *, std::shared_ptr<IOOperation>, task::cancel_token) = nullptr;
};

// Runs every primitive on the caller's thread, inside the call: nothing suspends, and the
// operation is done when the call returns. The loop is drive_synchronously.
class InlineCompletion {
  public:
	task::task<void> run(std::shared_ptr<IOOperation> operation, task::cancel_token token = {}) {
		if (!operation || operation->in_flight()) { throw std::logic_error("inline completion needs an idle operation"); }
		if (!drive_synchronously(*operation, [&] { return !token.requested(); })) { throw task::cancelled{}; }
		co_return;
	}
};

// Holds every primitive it is given until the owner completes it, in any order. For a simulator
// and for tests, on one thread: the one that runs the scheduler the awaiting tasks run on. A held
// primitive is named by its ticket, issued in order. The owner chooses each one's fate:
//   execute   the primitive runs on its backend now; its completion stays held (applied, not yet acknowledged)
//   deliver   the held completion reaches the operation, and the task resumes on its scheduler
//   complete  execute (if not yet) and deliver
//   fail      deliver an error in place of the completion; the backend is untouched unless already executed
//   drop      the primitive is forgotten, a crash: never delivered, the operation stays in flight
class HeldCompletion {
  public:
	using Ticket = std::uint64_t;

	HeldCompletion() = default;
	HeldCompletion(const HeldCompletion &) = delete;
	HeldCompletion &operator=(const HeldCompletion &) = delete;
	~HeldCompletion() {
		for (auto *wait : waits_) { wait->queue = nullptr; }
	}

	task::task<void> run(std::shared_ptr<IOOperation> operation, task::cancel_token token = {}) {
		if (!operation || operation->in_flight()) { throw std::logic_error("held completion needs an idle operation"); }
		if (!task::current()) { throw std::logic_error("held completion needs a running scheduler"); }
		while (!operation->done()) {
			if (token.requested()) { throw task::cancelled{}; }
			operation->submitted();
			auto result = co_await Wait{*this, operation, next_++};
			if (!operation->complete(std::move(result))) { throw std::logic_error("completion ownership mismatch"); }
		}
	}

	std::size_t held() const noexcept { return waits_.size(); }
	std::vector<Ticket> tickets() const {
		std::vector<Ticket> result;
		result.reserve(waits_.size());
		for (auto *wait : waits_) { result.push_back(wait->ticket); }
		return result;
	}
	bool holds(Ticket ticket) const noexcept { return find(ticket) != waits_.end(); }
	const MutationRequest &request(Ticket ticket) const { return (*at(ticket))->operation->request(); }
	bool executed(Ticket ticket) const { return (*at(ticket))->result.has_value(); }

	void execute(Ticket ticket) {
		auto *wait = *at(ticket);
		if (wait->result) { throw std::logic_error("primitive already executed"); }
		wait->result = detail::execute_primitive(wait->operation->io(), wait->operation->request());
	}
	void deliver(Ticket ticket) {
		auto *wait = *at(ticket);
		if (!wait->result) { throw std::logic_error("primitive not executed"); }
		release(wait);
	}
	void complete(Ticket ticket) {
		if (!executed(ticket)) { execute(ticket); }
		deliver(ticket);
	}
	void fail(Ticket ticket, std::exception_ptr error) {
		auto *wait = *at(ticket);
		MutationCompletion failure;
		failure.token = wait->operation->request().token;
		failure.error = std::move(error);
		wait->result = std::move(failure);
		release(wait);
	}
	void drop(Ticket ticket) {
		auto it = at(ticket);
		(*it)->queue = nullptr;
		waits_.erase(it);
	}

  private:
	// The awaiter, in the awaiting task's frame: its destructor leaves the queue, so a frame
	// destroyed while held (teardown) is never reached.
	struct Wait {
		Wait(HeldCompletion &held, std::shared_ptr<IOOperation> op, Ticket id)
			: queue(&held), operation(std::move(op)), ticket(id) {}
		Wait(Wait &&) = delete;
		~Wait() {
			if (queue) { queue->forget(this); }
		}
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> handle) {
			waiter = handle;
			home = task::current();
			queue->waits_.push_back(this);
		}
		MutationCompletion await_resume() { return std::move(*result); }

		HeldCompletion *queue;
		std::shared_ptr<IOOperation> operation;
		Ticket ticket;
		std::coroutine_handle<> waiter;
		task::scheduler_ref home;
		std::optional<MutationCompletion> result;
	};

	using Waits = std::vector<Wait *>;
	Waits::const_iterator find(Ticket ticket) const noexcept {
		for (auto it = waits_.begin(); it != waits_.end(); ++it) {
			if ((*it)->ticket == ticket) { return it; }
		}
		return waits_.end();
	}
	Waits::iterator at(Ticket ticket) {
		auto it = waits_.begin() + (find(ticket) - waits_.cbegin());
		if (it == waits_.end()) { throw std::out_of_range("no such held primitive"); }
		return it;
	}
	Waits::const_iterator at(Ticket ticket) const {
		auto it = find(ticket);
		if (it == waits_.end()) { throw std::out_of_range("no such held primitive"); }
		return it;
	}
	void release(Wait *wait) {
		waits_.erase(find_wait(wait));
		wait->queue = nullptr;
		wait->home.post(wait->waiter);
	}
	Waits::iterator find_wait(Wait *wait) { return std::find(waits_.begin(), waits_.end(), wait); }
	void forget(Wait *wait) {
		if (auto it = find_wait(wait); it != waits_.end()) { waits_.erase(it); }
	}

	Waits waits_;
	Ticket next_ = 1;
};

} // namespace kronuz::journal

namespace kronuz::io::completion {
using kronuz::journal::completion_driver;
using kronuz::journal::completion_ref;
using kronuz::journal::HeldCompletion;
using kronuz::journal::InlineCompletion;
} // namespace kronuz::io::completion

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

// Shared by the seam tests: a backend that logs what it ran, an IO with no directory, and an
// operation of alternating Write and SyncData primitives.
#pragma once
#include "completion/operation.h"
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace seam_support {
namespace j = kronuz::journal;

inline void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}

// What the backend ran, in order.
using Log = std::vector<std::string>;

class LogFile final : public j::File {
  public:
	LogFile(std::string name, Log &log) : name_(std::move(name)), log_(&log) {}
	std::uint64_t size() override { return 0; }
	std::size_t read_at(std::uint64_t, std::span<char>) override { return 0; }
	std::size_t write_at(std::uint64_t, std::string_view bytes) override {
		if (before_write) { before_write(); }
		log_->push_back(name_ + ":write");
		return bytes.size();
	}
	void truncate(std::uint64_t) override {}
	void sync() override { log_->push_back(name_ + ":sync"); }
	void sync_data() override {
		if (before_sync_data) { before_sync_data(); }
		log_->push_back(name_ + ":sync_data");
	}

	// Run on the thread that executes the primitive; they may block or throw.
	std::function<void()> before_write, before_sync_data;

  private:
	std::string name_;
	Log *log_;
};

// No directory: the primitives these operations issue touch only their file.
class NoIO final : public j::IO {
  public:
	std::unique_ptr<j::OwnerLock> acquire_owner(bool) override { throw std::logic_error("unused"); }
	void open_existing_read_only_into(std::string_view, j::File &) override { throw std::logic_error("unused"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, j::File &) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> open_existing(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> open_existing_read_only(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> create_exclusive(std::string_view) override { throw std::logic_error("unused"); }
	void replace(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
	void remove(std::string_view) override { throw std::logic_error("unused"); }
	void sync_directory() override { throw std::logic_error("unused"); }
};
inline NoIO no_io;

// Write, SyncData, Write ... : `steps` primitives alternating, stopping at the first error.
class Steps final : public j::IOOperation {
  public:
	Steps(std::string name, Log &log, unsigned steps)
		: file_(std::make_shared<LogFile>(std::move(name), log)), steps_(steps) {
		prepare();
	}
	const j::MutationRequest &request() const override { return request_; }
	void submitted() override {
		if (submitted_ || done_) { throw std::logic_error("duplicate submission"); }
		submitted_ = true;
	}
	bool complete(j::MutationCompletion result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token) { return false; }
		submitted_ = false;
		++completed;
		if (result.error) {
			error = result.error;
			done_ = true;
		} else if (++step_ == steps_) {
			done_ = true;
		} else {
			prepare();
		}
		if (on_completed) { on_completed(step_); }
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_; }
	j::IO &io() const noexcept override { return no_io; }

	std::shared_ptr<LogFile> file() const { return file_; }
	std::exception_ptr error;
	unsigned completed = 0;
	std::function<void(unsigned)> on_completed;

  private:
	void prepare() {
		request_ = {};
		request_.token.operation = 1;
		request_.token.step = step_;
		request_.kind = step_ % 2 ? j::PrimitiveKind::SyncData : j::PrimitiveKind::Write;
		request_.file = file_;
		request_.bytes = "x";
	}
	std::shared_ptr<LogFile> file_;
	unsigned steps_, step_ = 0;
	j::MutationRequest request_;
	bool submitted_ = false, done_ = false;
};

inline void expect(const Log &got, const Log &want, const char *what) {
	if (got != want) {
		std::cerr << "failed: " << what << "\n  got:";
		for (auto &entry : got) { std::cerr << ' ' << entry; }
		std::cerr << "\n  want:";
		for (auto &entry : want) { std::cerr << ' ' << entry; }
		std::cerr << '\n';
		std::exit(1);
	}
}

} // namespace seam_support

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

// IO::sync_data_files and the seam: the default runs File::sync_data on each file, in order, and
// the executor's Sync and SyncData primitives reach the IO's sync_files and sync_data_files (the
// files themselves are not touched), through drive_synchronously and through the native queue's
// worker. A counting IO records each call with the files it was given, as a test host does.
#include "completion/api.h"
#include "completion/native_completion.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace c = kronuz::io::completion;

namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}

using Log = std::vector<std::string>;

// Records which of sync() and sync_data() ran, and fails sync_data on demand.
class NamedFile final : public c::File {
  public:
	NamedFile(std::string name, Log &log) : name_(std::move(name)), log_(&log) {}
	std::uint64_t size() override { return 0; }
	std::size_t read_at(std::uint64_t, std::span<char>) override { return 0; }
	std::size_t write_at(std::uint64_t, std::string_view bytes) override { return bytes.size(); }
	void truncate(std::uint64_t) override {}
	void sync() override { log_->push_back(name_ + ":sync"); }
	void sync_data() override {
		log_->push_back(name_ + ":sync_data");
		if (fail_with) { throw std::system_error(fail_with, std::generic_category(), "data sync"); }
	}
	int fail_with = 0;

  private:
	std::string name_;
	Log *log_;
};

// No directory. `overridden` makes it a host that counts: sync_files and sync_data_files record
// the names they were given and run no File call; otherwise the defaults run.
class SeamIO final : public c::IO {
  public:
	SeamIO(Log &log, bool overridden) : log_(&log), overridden_(overridden) {}
	std::unique_ptr<c::OwnerLock> acquire_owner(bool) override { throw std::logic_error("unused"); }
	void open_existing_read_only_into(std::string_view, c::File &) override { throw std::logic_error("unused"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, c::File &) override { throw std::logic_error("unused"); }
	std::unique_ptr<c::File> open_reclaim_candidate_read_only(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<c::File> open_existing(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<c::File> open_existing_read_only(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<c::File> create_exclusive(std::string_view) override { throw std::logic_error("unused"); }
	void replace(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
	void remove(std::string_view) override { throw std::logic_error("unused"); }
	void sync_directory() override { throw std::logic_error("unused"); }
	void sync_files(std::span<c::File *const> files) override {
		if (!overridden_) { c::IO::sync_files(files); return; }
		log_->push_back("seam:sync_files:" + std::to_string(files.size()) + ":" + names(files));
	}
	void sync_data_files(std::span<c::File *const> files) override {
		if (!overridden_) { c::IO::sync_data_files(files); return; }
		log_->push_back("seam:sync_data_files:" + std::to_string(files.size()) + ":" + names(files));
	}

  private:
	static std::string names(std::span<c::File *const> files) {
		std::string out;
		for (auto *file : files) { out += dynamic_cast<NamedFile *>(file) ? "named," : "foreign,"; }
		return out;
	}
	Log *log_;
	bool overridden_;
};

// The listed primitives on one file, one after another; stops at the first error.
class Script final : public c::Operation {
  public:
	Script(c::IO &io, std::shared_ptr<c::File> file, std::vector<c::Kind> kinds) : io_(io), file_(std::move(file)), kinds_(std::move(kinds)) {
		prepare();
	}
	const c::Request &request() const override { return request_; }
	void submitted() override { submitted_ = true; }
	bool complete(c::Result result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token) { return false; }
		submitted_ = false;
		if (result.error) { error = result.error; done_ = true; }
		else if (++step_ == kinds_.size()) { done_ = true; }
		else { prepare(); }
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_; }
	c::IO &io() const noexcept override { return io_; }
	std::exception_ptr error;

  private:
	void prepare() {
		request_ = {};
		request_.token.operation = 1;
		request_.token.step = step_;
		request_.kind = kinds_[step_];
		request_.file = file_;
	}
	c::IO &io_;
	std::shared_ptr<c::File> file_;
	std::vector<c::Kind> kinds_;
	std::size_t step_ = 0;
	c::Request request_;
	bool submitted_ = false, done_ = false;
};

using Route = void (*)(c::Operation &);
void inline_route(c::Operation &operation) { c::drive_synchronously(operation); }
void queue_route(c::Operation &operation) {
	c::NativeQueue queue;
	auto shared = std::shared_ptr<c::Operation>(std::shared_ptr<c::Operation>{}, &operation);
	while (!operation.done()) {
		check(queue.submit(shared), "the queue admits the primitive");
		for (;;) {
			if (auto completion = queue.poll()) {
				check(completion->operation->complete(std::move(completion->completion)), "the original completes");
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

void expect(const Log &got, const Log &want, const std::string &what) {
	if (got == want) { return; }
	std::cerr << "failed: " << what << "\n  got:";
	for (const auto &line : got) { std::cerr << ' ' << line; }
	std::cerr << "\n  want:";
	for (const auto &line : want) { std::cerr << ' ' << line; }
	std::cerr << '\n';
	std::exit(1);
}

// The default: File::sync_data on each file in order, none for an empty batch, the first error
// stops the batch; sync_files stays File::sync.
void defaults() {
	Log log;
	SeamIO io(log, false);
	NamedFile a("a", log), b("b", log), z("z", log);
	std::vector<c::File *> all{&a, &b, &z};
	io.sync_data_files(all);
	expect(log, {"a:sync_data", "b:sync_data", "z:sync_data"}, "sync_data_files runs sync_data on each file, in order");
	log.clear();
	io.sync_data_files({});
	expect(log, {}, "an empty batch syncs nothing");
	io.sync_files(all);
	expect(log, {"a:sync", "b:sync", "z:sync"}, "sync_files still runs sync");
	log.clear();
	b.fail_with = EIO;
	try { io.sync_data_files(all); check(false, "a failed sync_data throws"); } catch (const std::system_error &e) {
		check(e.code() == std::error_code(EIO, std::generic_category()), "the failure is the file's errno");
	}
	expect(log, {"a:sync_data", "b:sync_data"}, "the failing file stops the batch");
}

// PosixIO keeps the default: a real file is data-synced and keeps its bytes, a foreign one runs
// its own sync_data, not sync.
void posix(const std::filesystem::path &base) {
	auto directory = base / "posix";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	c::PosixIO io(directory, {.flush_mode = c::FlushMode::full});
	auto file = io.create_exclusive("data");
	check(file->write_at(0, "durable") == 7, "write before sync_data_files");
	Log log;
	NamedFile foreign("f", log);
	std::vector<c::File *> both{file.get(), &foreign};
	io.sync_data_files(both);
	expect(log, {"f:sync_data"}, "a foreign file under PosixIO runs sync_data, not sync");
	char back[8]{};
	check(io.open_existing("data")->read_at(0, back) == 7 && std::string_view(back, 7) == "durable", "sync_data_files keeps the bytes");
}

// The executor goes through the seam: each Sync is one sync_files of that one file, each SyncData
// one sync_data_files, in order, and neither calls the file.
void executor(const char *route_name, Route route) {
	const std::string at = std::string(" through the ") + route_name;
	Log log;
	SeamIO counting(log, true);
	auto file = std::make_shared<NamedFile>("n", log);
	Script script(counting, file, {c::Kind::Sync, c::Kind::SyncData, c::Kind::SyncData, c::Kind::Sync});
	route(script);
	check(script.done() && !script.error, ("the primitives complete" + at).c_str());
	expect(log, {"seam:sync_files:1:named,", "seam:sync_data_files:1:named,", "seam:sync_data_files:1:named,", "seam:sync_files:1:named,"},
		   "each async sync reaches the seam, once, with the file" + at);

	// Without the override, the defaults reach the file: Sync is sync, SyncData is sync_data.
	Log plain;
	SeamIO defaulted(plain, false);
	auto other = std::make_shared<NamedFile>("d", plain);
	Script both(defaulted, other, {c::Kind::Sync, c::Kind::SyncData});
	route(both);
	check(both.done() && !both.error, ("the default seam completes" + at).c_str());
	expect(plain, {"d:sync", "d:sync_data"}, "the default seam syncs the file and data-syncs the file" + at);

	// A fault from the seam settles the operation with that error.
	Log failing;
	SeamIO failer(failing, false);
	auto bad = std::make_shared<NamedFile>("x", failing);
	bad->fail_with = EIO;
	Script fails(failer, bad, {c::Kind::SyncData, c::Kind::Sync});
	route(fails);
	check(fails.done() && fails.error, ("a failed data sync settles the operation" + at).c_str());
	try { std::rethrow_exception(fails.error); } catch (const std::system_error &e) {
		check(e.code() == std::error_code(EIO, std::generic_category()), ("the errno surfaces" + at).c_str());
	}
	expect(failing, {"x:sync_data"}, "the step after the failure does not run" + at);

	// A file-less Sync or SyncData is refused, not dereferenced.
	for (auto kind : {c::Kind::Sync, c::Kind::SyncData}) {
		Log none;
		SeamIO io(none, true);
		Script missing(io, nullptr, {kind});
		route(missing);
		check(missing.done() && missing.error, ("a sync without a file fails" + at).c_str());
		try { std::rethrow_exception(missing.error); } catch (const std::invalid_argument &) {}
		expect(none, {}, "no seam call without a file" + at);
	}
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_seam_test DIRECTORY\n";
		return 2;
	}
	defaults();
	posix(argv[1]);
	executor("inline loop", inline_route);
	executor("native queue", queue_route);
	std::cout << "sync_seam: ok\n";
	return 0;
}

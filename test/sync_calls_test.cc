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

// PosixIO's real sync call sequence on a real directory, recorded through replace_sync_calls:
// which files and the directory are fsync'd, what the one ordering point issues, and which
// barrier errors fail the commit.
#include "completion/posix.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <system_error>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace j = kronuz::journal;

#ifdef __APPLE__
namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}
using Log = std::vector<std::string>;
Log calls;
int barrier_error = 0;   // fails the next barriers with this errno
int barrier_failures = 0;
int full_error = 0;      // fails the next full flush with this errno

std::string name_of(int fd) {
	char path[1024] = {};
	check(::fcntl(fd, F_GETPATH, path) == 0, "fd has a path");
	return std::filesystem::path(path).filename().string();
}
int record_fsync(int fd) {
	calls.push_back("fsync:" + name_of(fd));
	return ::fsync(fd);
}
int record_fcntl(int fd, int command) {
	if (command == F_BARRIERFSYNC) {
		calls.push_back("barrier");
		if (barrier_error && barrier_failures > 0) { --barrier_failures; errno = barrier_error; return -1; }
		return ::fcntl(fd, command);
	}
	check(command == F_FULLFSYNC, "only F_BARRIERFSYNC and F_FULLFSYNC are issued");
	calls.push_back("full");
	if (full_error) { errno = full_error; return -1; }
	return ::fcntl(fd, command);
}
Log run(const std::function<void()>& body) {
	calls.clear();
	body();
	return calls;
}
void expect(const Log& got, const Log& want, const char *what) {
	if (got != want) {
		std::cerr << "failed: " << what << "\n  got:";
		for (auto& c : got) { std::cerr << ' ' << c; }
		std::cerr << "\n  want:";
		for (auto& c : want) { std::cerr << ' ' << c; }
		std::cerr << '\n';
		std::exit(1);
	}
}
// A file of another backend, which PosixIO cannot batch.
class Foreign final : public j::File {
  public:
	Foreign(std::unique_ptr<j::File> inner, Log& log) : inner_(std::move(inner)), log_(&log) {}
	std::uint64_t size() override { return inner_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner_->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner_->truncate(size); }
	void sync() override { log_->push_back("foreign"); inner_->sync(); }
  private:
	std::unique_ptr<j::File> inner_;
	Log* log_;
};
struct Fixture {
	std::filesystem::path directory;
	std::unique_ptr<j::PosixIO> io;
	std::unique_ptr<j::OwnerLock> owner;
	std::vector<std::unique_ptr<j::File>> files;
	std::vector<j::File*> all;
	Fixture(const std::filesystem::path& where, j::PosixIOOptions options) : directory(where) {
		std::filesystem::remove_all(directory);
		std::filesystem::create_directory(directory);
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		io = std::make_unique<j::PosixIO>(directory, 0600, options);
		owner = io->acquire_owner(true);
		for (const char *name : {"a", "b", "c"}) {
			auto file = io->create_exclusive(name);
			check(file->write_at(0, "bytes") == 5, "write");
			all.push_back(file.get());
			files.push_back(std::move(file));
		}
		io->replace_sync_calls(&record_fsync, &record_fcntl);
	}
	std::string dir() const { return directory.filename().string(); }
};
Log cat(Log a, const Log& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

void ordering_protocol(const std::filesystem::path& base) {
	Fixture f(base / "ordered", {});
	const Log stage{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir()};
	// One ordering point: every file and the directory fsync'd, then one barrier and no flush.
	expect(run([&] { f.io->order_files_and_directory(f.all); }), cat(stage, {"barrier"}),
		"order_files_and_directory fsyncs each file and the directory, then issues one barrier");
	// The directory alone is still ordered.
	expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "barrier"}, "a directory-only ordering point");
	// The publication's sync_directory is the first full flush.
	expect(run([&] { f.io->sync_directory(); }), {"fsync:" + f.dir(), "full"}, "sync_directory fsyncs the directory then flushes in full");
	// Durable batches flush in full and never barrier.
	expect(run([&] { f.io->sync_files_and_directory(f.all); }), cat(stage, {"full"}), "sync_files_and_directory is one full flush");
	expect(run([&] { f.io->sync_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "an empty durable batch syncs the directory");
	expect(run([&] { f.io->sync_files(f.all); }), {"fsync:a", "fsync:b", "fsync:c", "full"}, "sync_files is one full flush");
	// A foreign file syncs itself; the directory is still ordered.
	Log foreign_log;
	Foreign foreign(f.io->open_existing("a"), foreign_log);
	std::vector<j::File*> only_foreign{&foreign};
	expect(run([&] { f.io->order_files_and_directory(only_foreign); }), {"fsync:" + f.dir(), "barrier"},
		"a foreign file leaves the directory ordered");
	check(foreign_log == Log{"foreign"}, "a foreign file syncs itself once");
}

void errno_rule(const std::filesystem::path& base) {
	Fixture f(base / "errno-rule", {});
	for (int error : {EIO, ENOSPC, EDQUOT, EINVAL, EBADF}) {
		barrier_error = error; barrier_failures = 1000;
		bool threw = false;
		calls.clear();
		try { f.io->order_files_and_directory(f.all); }
		catch (const std::system_error& e) { threw = e.code() == std::error_code(error, std::generic_category()); }
		check(threw, "a barrier error other than unsupported fails the commit with its errno");
		check(std::count(calls.begin(), calls.end(), "barrier") == 1 && std::count(calls.begin(), calls.end(), "full") == 0,
			"a failed barrier is not retried as a full flush");
	}
	for (int error : {ENOTSUP, EOPNOTSUPP, ENOTTY}) {
		barrier_error = error; barrier_failures = 1000;
		expect(run([&] { f.io->order_files_and_directory(f.all); }),
			{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "barrier", "full"}, "an unsupported barrier falls back to a full flush");
		full_error = EIO;
		bool threw = false;
		try { f.io->order_files_and_directory(f.all); }
		catch (const std::system_error& e) { threw = e.code() == std::error_code(EIO, std::generic_category()); }
		check(threw, "a failing fallback flush fails the commit");
		full_error = 0;
	}
	barrier_error = EINTR; barrier_failures = 1;
	expect(run([&] { f.io->order_files_and_directory(f.all); }),
		{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "barrier", "barrier"}, "an interrupted barrier retries the barrier");
	barrier_error = 0; barrier_failures = 0;
}

void full_flush_option(const std::filesystem::path& base) {
	Fixture f(base / "full-flush", {.full_flush_ordering = true});
	expect(run([&] { f.io->order_files_and_directory(f.all); }), {"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "full"},
		"full_flush_ordering replaces the barrier with one full flush");
	expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "full_flush_ordering orders a directory alone in full");
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_calls_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path base(argv[1]);
	std::filesystem::remove_all(base);
	std::filesystem::create_directory(base);
	ordering_protocol(base);
	errno_rule(base);
	full_flush_option(base);
	std::cout << "sync_calls: ok\n";
	return 0;
}
#else
int main() {
	std::cout << "sync_calls: skipped (macOS only)\n";
	return 0;
}
#endif

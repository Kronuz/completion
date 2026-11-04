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

// The SyncFiles and OrderFiles primitives: the exact system-call sequence PosixIO issues on this
// platform for each, recorded through replace_sync_calls on a real directory, the same through the
// synchronous IO methods, drive_synchronously and the native queue's worker; read-only descriptors;
// and what a failed call leaves undone. The expected sequences below are the committed values.
#include "completion/api.h"
#include "completion/native_completion.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
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
Log calls;
std::map<std::pair<dev_t, ino_t>, std::string> names;  // the files and the directory under test, by identity
int fail_name_errno = 0;                               // fails the fsync of failing_name with this errno
std::string failing_name;

std::string name_of(int fd) {
	struct stat status{};
	check(::fstat(fd, &status) == 0, "fd has a status");
	auto found = names.find({status.st_dev, status.st_ino});
	check(found != names.end(), "fd is one of the files or the directory under test");
	return found->second;
}
int record_fsync(int fd) {
	auto name = name_of(fd);
	calls.push_back("fsync:" + name);
	if (fail_name_errno && name == failing_name) { errno = fail_name_errno; return -1; }
	return ::fsync(fd);
}
#ifdef __APPLE__
int record_fcntl(int fd, int command) {
	check(command == F_FULLFSYNC || command == F_BARRIERFSYNC, "only F_BARRIERFSYNC and F_FULLFSYNC are issued");
	calls.push_back(std::string(command == F_FULLFSYNC ? "full:" : "barrier:") + name_of(fd));
	return ::fcntl(fd, command);
}
void install(c::PosixIO &io) { io.replace_sync_calls(&record_fsync, &record_fcntl); }
#elif defined(__linux__)
int record_writeback(int fd) {
	calls.push_back("writeback:" + name_of(fd));
	return kronuz::journal::detail::default_writeback(fd);
}
void install(c::PosixIO &io) { io.replace_sync_calls(&record_fsync, &record_writeback); }
#else
void install(c::PosixIO &io) { io.replace_sync_calls(&record_fsync); }
#endif

// The sequences, per platform. Group is a (read-write), d (read-only), b (read-write), then the directory.
// macOS: fsync each file and the directory, then one device flush: F_FULLFSYNC for SyncFiles, F_BARRIERFSYNC
// for OrderFiles, through the first file on the device. Linux: writeback hints, then fsync in order. FreeBSD: fsync in order.
#ifdef __APPLE__
const Log sync_group{"fsync:a", "fsync:d", "fsync:b", "fsync:dir", "full:a"};
const Log order_group{"fsync:a", "fsync:d", "fsync:b", "fsync:dir", "barrier:a"};
const Log sync_read_only{"fsync:d", "fsync:dir", "full:d"};
const Log order_read_only{"fsync:d", "fsync:dir", "barrier:d"};
const Log sync_empty{"fsync:dir", "full:dir"};
const Log order_empty{"fsync:dir", "barrier:dir"};
// d fails: a is staged, b and the directory are not attempted, and no device is flushed.
const Log sync_failed{"fsync:a", "fsync:d"};
const Log order_failed{"fsync:a", "fsync:d"};
#elif defined(__linux__)
const Log sync_group{"writeback:a", "writeback:d", "writeback:b", "fsync:a", "fsync:d", "fsync:b", "fsync:dir"};
const Log order_group = sync_group;
const Log sync_read_only{"writeback:d", "fsync:d", "fsync:dir"};
const Log order_read_only = sync_read_only;
const Log sync_empty{"fsync:dir"};
const Log order_empty = sync_empty;
const Log sync_failed{"writeback:a", "writeback:d", "writeback:b", "fsync:a", "fsync:d"};
const Log order_failed = sync_failed;
#else
const Log sync_group{"fsync:a", "fsync:d", "fsync:b", "fsync:dir"};
const Log order_group = sync_group;
const Log sync_read_only{"fsync:d", "fsync:dir"};
const Log order_read_only = sync_read_only;
const Log sync_empty{"fsync:dir"};
const Log order_empty = sync_empty;
const Log sync_failed{"fsync:a", "fsync:d"};
const Log order_failed = sync_failed;
#endif

void expect(const Log &got, const Log &want, const std::string &what) {
	if (got != want) {
		std::cerr << "failed: " << what << "\n  got:";
		for (auto &entry : got) { std::cerr << ' ' << entry; }
		std::cerr << "\n  want:";
		for (auto &entry : want) { std::cerr << ' ' << entry; }
		std::cerr << '\n';
		std::exit(1);
	}
}

class OnePrimitive final : public c::Operation {
  public:
	OnePrimitive(c::IO &io, c::Request request) : io_(io), request_(request) {}
	const c::Request &request() const override { return request_; }
	void submitted() override { submitted_ = true; }
	bool complete(c::Result result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token) { return false; }
		result_ = std::move(result);
		done_ = true;
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_ && !done_; }
	c::IO &io() const noexcept override { return io_; }
	c::Result result_;

  private:
	c::IO &io_;
	c::Request request_;
	bool submitted_ = false, done_ = false;
};
using Group = std::vector<std::shared_ptr<c::File>>;

struct Fixture {
	std::filesystem::path directory;
	std::shared_ptr<c::PosixIO> io;
	Group everything, read_only, empty;
	explicit Fixture(const std::filesystem::path &where) : directory(where) {
		names.clear();
		std::filesystem::remove_all(directory);
		std::filesystem::create_directory(directory);
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		io = std::make_shared<c::PosixIO>(directory);
		for (const char *name : {"a", "d", "b"}) {
			auto file = io->create_exclusive(name);
			check(file->write_at(0, "bytes") == 5, "write");
		}
		auto reopen = [&](const char *name, bool read_only_open) {
			std::shared_ptr<c::File> file = read_only_open ? io->open_existing_read_only(name) : io->open_existing(name);
			auto handle = dynamic_cast<kronuz::journal::detail::PosixFile &>(*file).native_handle();
			struct stat status{};
			check(::fstat(handle, &status) == 0, "stat");
			names[{status.st_dev, status.st_ino}] = name;
			return file;
		};
		everything = {reopen("a", false), reopen("d", true), reopen("b", false)};
		read_only = {everything[1]};
		struct stat status{};
		check(::stat(directory.c_str(), &status) == 0, "stat directory");
		names[{status.st_dev, status.st_ino}] = "dir";
		install(*io);
	}
	std::vector<c::File *> raw(const Group &group) const {
		std::vector<c::File *> result;
		for (auto &file : group) { result.push_back(file.get()); }
		return result;
	}
};

using Route = std::function<void(Fixture &, c::Kind, const Group &)>;
c::Request request_for(c::Kind kind, const Group &group) {
	c::Request request;
	request.token.operation = 1;
	request.kind = kind;
	request.files = group;
	return request;
}
// The IO method the primitive stands for.
void direct(Fixture &f, c::Kind kind, const Group &group) {
	auto files = f.raw(group);
	if (kind == c::Kind::SyncFiles) { f.io->sync_files_and_directory(files); } else { f.io->order_files_and_directory(files); }
}
void synchronous(Fixture &f, c::Kind kind, const Group &group) {
	OnePrimitive operation(*f.io, request_for(kind, group));
	c::drive_synchronously(operation);
	if (operation.result_.error) { std::rethrow_exception(operation.result_.error); }
}
// The native queue runs these on its fallback worker.
void queued(Fixture &f, c::Kind kind, const Group &group) {
	auto operation = std::make_shared<OnePrimitive>(*f.io, request_for(kind, group));
	c::NativeQueue queue;
	check(queue.submit(operation), "the queue admits the primitive");
	for (;;) {
		if (auto completion = queue.poll()) {
			check(completion->operation->complete(std::move(completion->completion)), "the original completes");
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	auto stats = queue.stats();
	const auto at = static_cast<std::size_t>(kind) - stats.native_by_primitive.size();
	check(stats.fallback_extensions.at(at) == 1 && stats.native_extensions.at(at) == 0, "SyncFiles and OrderFiles run on the fallback worker");
	if (operation->result_.error) { std::rethrow_exception(operation->result_.error); }
}
Log run(const Route &route, Fixture &f, c::Kind kind, const Group &group, int error = 0) {
	calls.clear();
	fail_name_errno = error;
	failing_name = "d";
	try { route(f, kind, group); } catch (const std::system_error &e) {
		fail_name_errno = 0;
		check(error && e.code() == std::error_code(error, std::generic_category()), "the failure surfaces with its errno");
		return calls;
	}
	fail_name_errno = 0;
	check(!error, "an injected failure fails the call");
	return calls;
}

void sequences(const std::filesystem::path &base) {
	Fixture f(base / "sequences");
	const struct { const char *route; Route run; } routes[] = {{"IO method", direct}, {"drive_synchronously", synchronous}, {"native queue", queued}};
	for (auto &route : routes) {
		const std::string at = std::string(" through the ") + route.route;
		expect(run(route.run, f, c::Kind::SyncFiles, f.everything), sync_group, "SyncFiles of a, d (read-only), b" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.everything), order_group, "OrderFiles of a, d (read-only), b" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.read_only), sync_read_only, "SyncFiles of a read-only file alone" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.read_only), order_read_only, "OrderFiles of a read-only file alone" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.empty), sync_empty, "SyncFiles of no files syncs the directory" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.empty), order_empty, "OrderFiles of no files orders the directory" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.everything, EIO), sync_failed, "a failed SyncFiles stops at the failing file" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.everything, EIO), order_failed, "a failed OrderFiles stops at the failing file" + at);
	}
}

void read_only_descriptor(const std::filesystem::path &base) {
	Fixture f(base / "read-only");
	bool refused = false;
	try { f.everything[1]->write_at(0, "x"); } catch (const std::system_error &) { refused = true; }
	check(refused, "a read-only descriptor refuses writes");
	char buffer[8]{};
	check(f.everything[1]->read_at(0, buffer) == 5, "a read-only descriptor reads");
}

void invalid_group(const std::filesystem::path &base) {
	Fixture f(base / "invalid");
	Group with_null{f.everything[0], nullptr};
	bool refused = false;
	calls.clear();
	try { synchronous(f, c::Kind::SyncFiles, with_null); } catch (const std::invalid_argument &) { refused = true; }
	check(refused && calls.empty(), "a null file in the group is refused before any sync");
}

// Every open primitive has a read-only mode: the descriptor reads and syncs, and refuses writes.
void read_only_opens(const std::filesystem::path &base) {
	Fixture f(base / "read-only-opens");
	auto writes_refused = [](c::File &file) {
		try { file.write_at(0, "x"); } catch (const std::system_error &) { return true; }
		return false;
	};
	auto refuses_and_reads = [&](c::File &file, const char *what) {
		char buffer[8]{};
		check(writes_refused(file), what);
		check(file.read_at(0, buffer) == 5, what);
	};
	for (bool read_only : {false, true}) {
		auto open = [&](c::Kind kind, c::File *into) {
			c::Request request;
			request.token.operation = 1;
			request.kind = kind;
			request.source = "a";
			request.read_only = read_only;
			if (into) { request.file = std::shared_ptr<c::File>(std::shared_ptr<c::File>{}, into); }
			OnePrimitive operation(*f.io, request);
			c::drive_synchronously(operation);
			if (operation.result_.error) { std::rethrow_exception(operation.result_.error); }
			return std::move(operation.result_);
		};
		for (auto kind : {c::Kind::Open, c::Kind::OpenCandidate}) {
			auto result = open(kind, nullptr);
			check(result.file, "the open returns a file");
			if (read_only) {
				refuses_and_reads(*result.file, "a read-only Open or OpenCandidate refuses writes");
				check(result.file->try_lease(c::LeaseMode::Exclusive), "a read-only descriptor takes a lease");
			}
			else { check(result.file->write_at(0, "b") == 1, "a read-write open writes"); }
		}
		for (auto kind : {c::Kind::OpenInto, c::Kind::OpenCandidateInto}) {
			auto closed = f.io->make_closed_file();
			auto result = open(kind, closed.get());
			if (kind == c::Kind::OpenCandidateInto) { check(result.count == 1, "the candidate opens"); }
			if (read_only) { refuses_and_reads(*closed, "a read-only OpenInto or OpenCandidateInto refuses writes"); }
			else { check(closed->write_at(0, "b") == 1, "a read-write open writes"); }
		}
	}
}

// The queue retains the operation, and so the group it names, until the original completes.
void retention(const std::filesystem::path &base) {
	Fixture f(base / "retention");
	auto group = std::make_shared<Group>(f.everything);
	std::weak_ptr<Group> retained = group;
	// The request views the group; the operation owns the group it views.
	struct Holder final : c::Operation {
		Holder(std::shared_ptr<Group> held, c::IO &io) : group(std::move(held)), inner(io, request_for(c::Kind::SyncFiles, *group)) {}
		const c::Request &request() const override { return inner.request(); }
		void submitted() override { inner.submitted(); }
		bool complete(c::Result result) noexcept override { return inner.complete(std::move(result)); }
		bool done() const noexcept override { return inner.done(); }
		bool in_flight() const noexcept override { return inner.in_flight(); }
		c::IO &io() const noexcept override { return inner.io(); }
		std::shared_ptr<Group> group;
		OnePrimitive inner;
	};
	auto holder = std::make_shared<Holder>(group, *f.io);
	group.reset();
	c::NativeQueue queue;
	calls.clear();
	check(queue.submit(holder), "the queue admits the primitive");
	holder.reset();
	check(!retained.expired(), "an accepted original retains its group");
	std::shared_ptr<c::Operation> original;
	for (;;) {
		if (auto completion = queue.poll()) {
			original = std::move(completion->operation);
			check(original->complete(std::move(completion->completion)), "the original completes after the caller let go");
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	expect(calls, sync_group, "the retained original syncs the group");
	original.reset();
	check(retained.expired(), "completion releases the group");
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_primitives_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path base(argv[1]);
	std::filesystem::remove_all(base);
	std::filesystem::create_directory(base);
	sequences(base);
	read_only_descriptor(base);
	invalid_group(base);
	read_only_opens(base);
	retention(base);
	std::cout << "sync_primitives: ok\n";
	return 0;
}

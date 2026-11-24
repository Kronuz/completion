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

// Every sync PosixIO issues, in each flush mode: the exact system-call sequence on this platform
// for SyncFiles and OrderFiles, Sync, SyncData (the turn barrier), DirectorySync, File::sync,
// File::sync_data and the owner lock file's creation, recorded through replace_sync_calls on a
// real directory, the same through the synchronous IO methods, drive_synchronously and the native
// queue's worker; read-only descriptors; and what a failed call leaves undone. The expected
// sequences below are the committed values.
#include "completion/api.h"
#include "completion/native_completion.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace c = kronuz::io::completion;

// The flush mode has no default: options without one, and a PosixIO without options, do not compile.
template <class T> concept braced_default = requires { T{}; };
static_assert(!braced_default<c::PosixIOOptions> && !std::is_default_constructible_v<c::PosixIOOptions>);
static_assert(!std::is_constructible_v<c::PosixIO, std::filesystem::path>);
static_assert(!std::is_constructible_v<c::PosixIO, std::filesystem::path, unsigned>);
static_assert(std::is_constructible_v<c::PosixIO, std::filesystem::path, c::PosixIOOptions, unsigned>);


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
std::filesystem::path under_test;                      // the directory whose entries name the files
int fail_name_errno = 0;                               // fails the fsync of failing_name with this errno
std::string failing_name;

std::string name_of(int fd) {
	struct stat status{};
	check(::fstat(fd, &status) == 0, "fd has a status");
	auto found = names.find({status.st_dev, status.st_ino});
	if (found != names.end()) { return found->second; }
	for (const auto &entry : std::filesystem::directory_iterator(under_test)) {
		struct stat entry_status{};
		if (!::lstat(entry.path().c_str(), &entry_status) && entry_status.st_dev == status.st_dev && entry_status.st_ino == status.st_ino) {
			return names[{status.st_dev, status.st_ino}] = entry.path().filename().string();
		}
	}
	check(false, "fd is one of the files or the directory under test");
	return {};
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
int record_fdatasync(int fd) {
	calls.push_back("fdatasync:" + name_of(fd));
	return ::fdatasync(fd);
}
int record_writeback(int fd) {
	calls.push_back("writeback:" + name_of(fd));
	return kronuz::journal::detail::default_writeback(fd);
}
void install(c::PosixIO &io) { io.replace_sync_calls(&record_fsync, &record_fdatasync, &record_writeback); }
#else
void install(c::PosixIO &io) { io.replace_sync_calls(&record_fsync); }
#endif

// The sequences, per platform and flush mode. Group is a (read-write), d (read-only), b (read-write),
// then the directory; failed: d fails, so a is staged, b and the directory are not attempted, and
// no device is flushed. macOS: fsync each file and the directory, then, in full mode, one F_FULLFSYNC
// per device for SyncFiles and OrderFiles alike, through the first file on it; in plain mode, nothing
// more for SyncFiles and one F_BARRIERFSYNC for OrderFiles. Linux: writeback hints, then fsync in
// order. FreeBSD: fsync in order. Linux and FreeBSD are the same in both modes.
struct Sequences {
	Log sync_group, order_group, sync_read_only, order_read_only, sync_empty, order_empty, failed;
};
Sequences sequences_for([[maybe_unused]] c::FlushMode mode) {
#ifdef __APPLE__
	if (mode == c::FlushMode::full) {
		return {{"fsync:a", "fsync:d", "fsync:b", "fsync:dir", "full:a"}, {"fsync:a", "fsync:d", "fsync:b", "fsync:dir", "full:a"},
			{"fsync:d", "fsync:dir", "full:d"}, {"fsync:d", "fsync:dir", "full:d"}, {"fsync:dir", "full:dir"}, {"fsync:dir", "full:dir"},
			{"fsync:a", "fsync:d"}};
	}
	return {{"fsync:a", "fsync:d", "fsync:b", "fsync:dir"}, {"fsync:a", "fsync:d", "fsync:b", "fsync:dir", "barrier:a"},
		{"fsync:d", "fsync:dir"}, {"fsync:d", "fsync:dir", "barrier:d"}, {"fsync:dir"}, {"fsync:dir", "barrier:dir"},
		{"fsync:a", "fsync:d"}};
#elif defined(__linux__)
	const Log group{"writeback:a", "writeback:d", "writeback:b", "fsync:a", "fsync:d", "fsync:b", "fsync:dir"};
	const Log read_only{"writeback:d", "fsync:d", "fsync:dir"};
	return {group, group, read_only, read_only, {"fsync:dir"}, {"fsync:dir"}, {"writeback:a", "writeback:d", "writeback:b", "fsync:a", "fsync:d"}};
#else
	const Log group{"fsync:a", "fsync:d", "fsync:b", "fsync:dir"};
	return {group, group, {"fsync:d", "fsync:dir"}, {"fsync:d", "fsync:dir"}, {"fsync:dir"}, {"fsync:dir"}, {"fsync:a", "fsync:d"}};
#endif
}
// The single-file and directory call sites, with owner.lock held, per platform and flush mode.
struct CallSites {
	Log owner_create, sync, sync_data, sync_primitive, sync_data_primitive, directory_sync;
};
CallSites call_sites_for([[maybe_unused]] c::FlushMode mode) {
#ifdef __APPLE__
	if (mode == c::FlushMode::full) {
		return {{"full:owner.lock"}, {"full:a"}, {"full:a"}, {"fsync:a", "full:a"}, {"full:a"}, {"fsync:dir", "full:owner.lock"}};
	}
	return {{"fsync:owner.lock"}, {"fsync:a"}, {"fsync:a"}, {"fsync:a"}, {"fsync:a"}, {"fsync:dir"}};
#elif defined(__linux__)
	return {{"fsync:owner.lock"}, {"fsync:a"}, {"fdatasync:a"}, {"writeback:a", "fsync:a"}, {"fdatasync:a"}, {"fsync:dir"}};
#else
	return {{"fsync:owner.lock"}, {"fsync:a"}, {"fsync:a"}, {"fsync:a"}, {"fsync:a"}, {"fsync:dir"}};
#endif
}

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

// The recorder is installed before any file opens: a file keeps the sync calls it opened with.
struct Fixture {
	std::filesystem::path directory;
	std::shared_ptr<c::PosixIO> io;
	Group everything, read_only, empty;
	Fixture(const std::filesystem::path &where, c::FlushMode mode) : directory(where) {
		names.clear();
		under_test = directory;
		std::filesystem::remove_all(directory);
		std::filesystem::create_directory(directory);
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		io = std::make_shared<c::PosixIO>(directory, c::PosixIOOptions{.flush_mode = mode});
		install(*io);
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
	}
	std::vector<c::File *> raw(const Group &group) const {
		std::vector<c::File *> result;
		for (auto &file : group) { result.push_back(file.get()); }
		return result;
	}
};

using Route = std::function<void(Fixture &, c::Kind, const Group &)>;
// SyncFiles and OrderFiles take the group; Sync and SyncData its first file; DirectorySync nothing.
c::Request request_for(c::Kind kind, const Group &group) {
	c::Request request;
	request.token.operation = 1;
	request.kind = kind;
	if (kind == c::Kind::Sync || kind == c::Kind::SyncData) { request.file = group.at(0); }
	else if (kind != c::Kind::DirectorySync) { request.files = group; }
	return request;
}
// The IO method the primitive stands for.
void direct(Fixture &f, c::Kind kind, const Group &group) {
	auto files = f.raw(group);
	switch (kind) {
	case c::Kind::SyncFiles: f.io->sync_files_and_directory(files); break;
	case c::Kind::OrderFiles: f.io->order_files_and_directory(files); break;
	case c::Kind::Sync: f.io->sync_files(std::span(files).first(1)); break;
	case c::Kind::SyncData: f.io->sync_data_files(std::span(files).first(1)); break;
	case c::Kind::DirectorySync: f.io->sync_directory(); break;
	default: check(false, "a sync primitive");
	}
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
	if (kind == c::Kind::SyncFiles || kind == c::Kind::OrderFiles) {
		auto stats = queue.stats();
		const auto at = static_cast<std::size_t>(kind) - stats.native_by_primitive.size();
		check(stats.fallback_extensions.at(at) == 1 && stats.native_extensions.at(at) == 0, "SyncFiles and OrderFiles run on the fallback worker");
	}
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

std::string mode_name(c::FlushMode mode) { return mode == c::FlushMode::full ? " (full)" : " (plain)"; }

void sequences(const std::filesystem::path &base, c::FlushMode mode) {
	Fixture f(base / "sequences", mode);
	const auto want = sequences_for(mode);
	const struct { const char *route; Route run; } routes[] = {{"IO method", direct}, {"drive_synchronously", synchronous}, {"native queue", queued}};
	for (auto &route : routes) {
		const std::string at = std::string(" through the ") + route.route + mode_name(mode);
		expect(run(route.run, f, c::Kind::SyncFiles, f.everything), want.sync_group, "SyncFiles of a, d (read-only), b" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.everything), want.order_group, "OrderFiles of a, d (read-only), b" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.read_only), want.sync_read_only, "SyncFiles of a read-only file alone" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.read_only), want.order_read_only, "OrderFiles of a read-only file alone" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.empty), want.sync_empty, "SyncFiles of no files syncs the directory" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.empty), want.order_empty, "OrderFiles of no files orders the directory" + at);
		expect(run(route.run, f, c::Kind::SyncFiles, f.everything, EIO), want.failed, "a failed SyncFiles stops at the failing file" + at);
		expect(run(route.run, f, c::Kind::OrderFiles, f.everything, EIO), want.failed, "a failed OrderFiles stops at the failing file" + at);
	}
}

// sync_call, the one table from a site and a mode to the call, on every platform's row. Checked
// at run time, so a mutant of a row builds and fails here.
void table_rows() {
	using kronuz::journal::sync_call;
	using Site = kronuz::journal::SyncSite;
	using Call = kronuz::journal::SyncCall;
	using Platform = kronuz::journal::SyncPlatform;
	const auto full = c::FlushMode::full, plain = c::FlushMode::plain;
	for (auto site : {Site::file, Site::data, Site::directory, Site::order}) {
		check(sync_call(site, full, Platform::macos) == Call::full_fsync, "macOS full is F_FULLFSYNC at every site");
		check(sync_call(site, plain, Platform::macos) == (site == Site::order ? Call::barrier_fsync : Call::fsync),
			"macOS plain is fsync, and F_BARRIERFSYNC at an ordering point");
		for (auto mode : {full, plain}) {
			check(sync_call(site, mode, Platform::linux_kernel) == (site == Site::data ? Call::fdatasync : Call::fsync),
				"Linux is fsync, and fdatasync for sync_data, in both modes");
			check(sync_call(site, mode, Platform::other) == Call::fsync, "elsewhere fsync, in both modes");
		}
	}
	using kronuz::journal::batch_weakens;
	check(batch_weakens(full, plain, Platform::macos), "macOS: a plain batch would weaken a full file");
	check(!batch_weakens(plain, full, Platform::macos) && !batch_weakens(full, full, Platform::macos) && !batch_weakens(plain, plain, Platform::macos),
		"macOS: a batch weakens no plain file and no file of its own mode");
	for (auto platform : {Platform::linux_kernel, Platform::other}) {
		for (auto file : {full, plain}) {
			for (auto batch : {full, plain}) { check(!batch_weakens(file, batch, platform), "Linux and elsewhere: no batch weakens a file"); }
		}
	}
}

// No mode is implicit: FlushMode has no default and Value has no zero, so a value-initialized or
// out-of-range Value is refused when it becomes a FlushMode, and a PosixIO refuses options whose
// bytes hold one.
static_assert(!std::is_default_constructible_v<c::FlushMode>);
static_assert(c::FlushMode::plain != c::FlushMode::Value{} && c::FlushMode::full != c::FlushMode::Value{});
void flush_mode_values(const std::filesystem::path &base) {
	auto refused = [](auto &&body) {
		try { body(); } catch (const std::invalid_argument &) { return true; }
		return false;
	};
	for (unsigned raw : {0u, 3u, 255u}) {
		const auto value = static_cast<c::FlushMode::Value>(raw);
		check(refused([&] { c::FlushMode mode(value); (void)mode; }), "a FlushMode refuses a Value other than plain or full");
		check(refused([&] { c::PosixIOOptions options{.flush_mode = value}; (void)options; }), "options refuse a Value other than plain or full");
	}
	const c::FlushMode::Value unchosen{};
	check(refused([&] { c::PosixIOOptions options{unchosen}; (void)options; }), "options refuse a value-initialized Value");
	const auto directory = base / "mode-values";
	std::filesystem::create_directory(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	for (unsigned char raw : {0, 3, 255}) {
		c::PosixIOOptions options{.flush_mode = c::FlushMode::full};
		std::memset(static_cast<void *>(&options.flush_mode), raw, sizeof options.flush_mode);
		check(refused([&] { c::PosixIO io(directory, options); }), "a PosixIO refuses options whose mode is neither plain nor full");
	}
}

// The owner lock file's creation, File::sync and File::sync_data, and the Sync, SyncData (the
// turn barrier) and DirectorySync primitives, with owner.lock held. The native queue runs them on
// its fallback worker on macOS; Linux's ring and FreeBSD's AIO issue the kernel's own fsync and
// fdatasync, the same in both modes, past the seam, so they take the other two routes there.
void call_sites(const std::filesystem::path &base, c::FlushMode mode) {
	Fixture f(base / "call-sites", mode);
	const auto want = call_sites_for(mode);
	const std::string in = mode_name(mode);
	std::unique_ptr<c::OwnerLock> owner;
	calls.clear();
	owner = f.io->acquire_owner(c::OwnerLockOptions{c::OwnerLockMode::both, c::OwnerFile::create_exclusive});
	expect(calls, want.owner_create, "creating owner.lock syncs it" + in);
	calls.clear();
	f.everything[0]->sync();
	expect(calls, want.sync, "File::sync" + in);
	calls.clear();
	f.everything[0]->sync_data();
	expect(calls, want.sync_data, "File::sync_data" + in);
	std::vector<std::pair<const char *, Route>> routes{{"IO method", direct}, {"drive_synchronously", synchronous}};
#ifdef __APPLE__
	routes.emplace_back("native queue", queued);
#endif
	for (auto &[route, through] : routes) {
		const std::string at = std::string(" through the ") + route + in;
		expect(run(through, f, c::Kind::Sync, f.everything), want.sync_primitive, "Sync of a" + at);
		expect(run(through, f, c::Kind::SyncData, f.everything), want.sync_data_primitive, "SyncData of a, the turn barrier" + at);
		expect(run(through, f, c::Kind::DirectorySync, f.empty), want.directory_sync, "DirectorySync flushes through owner.lock" + at);
	}
}

// Every path where a file is synced by a backend other than its own, both ways across modes: the
// IO methods, the Sync, SyncFiles and OrderFiles primitives (drive_synchronously and the native
// queue), and SyncData. A file the batch would weaken (macOS: full in a plain batch) syncs itself
// first; a plain file in a full batch shares the batch's full flush. Files a (read-write), d
// (read-only), b of the `file_mode` backend; "batchdir" is the batching backend's directory.
struct Crossed {
	Log sync_group, order_group, sync, sync_data;
};
Crossed crossed_for([[maybe_unused]] c::FlushMode file_mode) {
#ifdef __APPLE__
	if (file_mode == c::FlushMode::full) {  // a plain batch
		const Log own{"full:a", "full:d", "full:b"};
		Log order = own;
		order.insert(order.end(), {"fsync:batchdir", "barrier:batchdir"});
		Log sync = own;
		sync.push_back("fsync:batchdir");
		return {sync, order, {"full:a"}, {"full:a"}};
	}
	const Log batch{"fsync:a", "fsync:d", "fsync:b", "fsync:batchdir", "full:a"};  // a full batch
	return {batch, batch, {"fsync:a", "full:a"}, {"fsync:a"}};
#elif defined(__linux__)
	const Log group{"writeback:a", "writeback:d", "writeback:b", "fsync:a", "fsync:d", "fsync:b", "fsync:batchdir"};
	return {group, group, {"writeback:a", "fsync:a"}, {"fdatasync:a"}};
#else
	const Log group{"fsync:a", "fsync:d", "fsync:b", "fsync:batchdir"};
	return {group, group, {"fsync:a"}, {"fsync:a"}};
#endif
}
void cross_backend(const std::filesystem::path &base, c::FlushMode file_mode) {
	const auto batch_mode = file_mode == c::FlushMode::full ? c::FlushMode::plain : c::FlushMode::full;
	Fixture batcher(base / "cross-batch", batch_mode);
	auto batcher_names = names;
	for (auto &[identity, name] : batcher_names) {
		if (name == "dir") { name = "batchdir"; }
	}
	Fixture f(base / "cross-files", file_mode);
	names.insert(batcher_names.begin(), batcher_names.end());
	const auto want = crossed_for(file_mode);
	const std::string in = std::string(" of") + mode_name(file_mode) + " files in a" + mode_name(batch_mode) + " batch";
	std::vector<std::pair<const char *, Route>> routes{{"IO method", direct}, {"drive_synchronously", synchronous}, {"native queue", queued}};
	for (auto &[route, through] : routes) {
		const std::string at = in + " through the " + route;
		expect(run(through, batcher, c::Kind::SyncFiles, f.everything), want.sync_group, "SyncFiles" + at);
		expect(run(through, batcher, c::Kind::OrderFiles, f.everything), want.order_group, "OrderFiles" + at);
#if !defined(__APPLE__)
		if (std::string_view(route) == "native queue") { continue; }  // the kernel's own fsync, past the seam
#endif
		expect(run(through, batcher, c::Kind::Sync, f.everything), want.sync, "Sync" + at);
		expect(run(through, batcher, c::Kind::SyncData, f.everything), want.sync_data, "SyncData" + at);
	}
	auto files = f.raw(f.everything);
	calls.clear();
	batcher.io->sync_files(files);
	Log batched = want.sync_group;
	batched.erase(std::remove(batched.begin(), batched.end(), "fsync:batchdir"), batched.end());
	expect(calls, batched, "sync_files" + in);
}

// A reusable file syncs in its backend's mode: make_closed_file gives it the mode, which no open
// into it changes (create_exclusive_into, open_existing_into, open_reclaim_candidate_into).
void reusable(const std::filesystem::path &base, c::FlushMode mode) {
	Fixture f(base / "reusable", mode);
	Log want_sync{"fsync:r"}, want_data{"fsync:r"};
#ifdef __APPLE__
	if (mode == c::FlushMode::full) { want_sync = want_data = {"full:r"}; }
#elif defined(__linux__)
	want_data = {"fdatasync:r"};
#endif
	const std::string in = mode_name(mode);
	auto closed = f.io->make_closed_file();
	auto syncs = [&](const std::string &opened) {
		calls.clear();
		closed->sync();
		expect(calls, want_sync, "File::sync of a reusable file after " + opened + in);
		calls.clear();
		closed->sync_data();
		expect(calls, want_data, "File::sync_data of a reusable file after " + opened + in);
		closed->close();
	};
	f.io->create_exclusive_into("r", *closed);
	check(closed->write_at(0, "bytes") == 5, "write a reusable file");
	syncs("create_exclusive_into");
	f.io->open_existing_into("r", *closed);
	syncs("open_existing_into");
	check(f.io->open_reclaim_candidate_into("r", *closed), "the reusable file reopens as a reclaim candidate");
	syncs("open_reclaim_candidate_into");
}

void read_only_descriptor(const std::filesystem::path &base) {
	Fixture f(base / "read-only", c::FlushMode::full);
	bool refused = false;
	try { f.everything[1]->write_at(0, "x"); } catch (const std::system_error &) { refused = true; }
	check(refused, "a read-only descriptor refuses writes");
	char buffer[8]{};
	check(f.everything[1]->read_at(0, buffer) == 5, "a read-only descriptor reads");
}

void invalid_group(const std::filesystem::path &base) {
	Fixture f(base / "invalid", c::FlushMode::full);
	Group with_null{f.everything[0], nullptr};
	bool refused = false;
	calls.clear();
	try { synchronous(f, c::Kind::SyncFiles, with_null); } catch (const std::invalid_argument &) { refused = true; }
	check(refused && calls.empty(), "a null file in the group is refused before any sync");
}

// Every open primitive has a read-only mode: the descriptor reads and syncs, and refuses writes.
void read_only_opens(const std::filesystem::path &base) {
	Fixture f(base / "read-only-opens", c::FlushMode::full);
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
			check(static_cast<bool>(result.file), "the open returns a file");
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
	Fixture f(base / "retention", c::FlushMode::full);
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
	expect(calls, sequences_for(c::FlushMode::full).sync_group, "the retained original syncs the group");
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
	table_rows();
	flush_mode_values(base);
	for (c::FlushMode mode : {c::FlushMode::plain, c::FlushMode::full}) {
		sequences(base, mode);
		call_sites(base, mode);
		cross_backend(base, mode);
		reusable(base, mode);
	}
	read_only_descriptor(base);
	invalid_group(base);
	read_only_opens(base);
	retention(base);
	std::cout << "sync_primitives: ok\n";
	return 0;
}

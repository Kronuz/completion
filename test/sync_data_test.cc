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

// The SyncData primitive on every backend this repository ships: drive_synchronously and the
// native queue (its worker for a foreign File, its native path for a PosixFile), each succeeding
// and failing. A failure surfaces with its errno and the token, leaves the written bytes in
// place, and counts under the extension statistics' SyncData slot. A foreign File records which
// of sync() and sync_data() ran. Linux also fails the native completion through the ring hook.
#include "completion/api.h"
#include "completion/native_completion.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace c = kronuz::io::completion;

namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}

// A File of no directory: it records which sync ran and fails sync_data on demand.
class RecordingFile final : public c::File {
  public:
	std::uint64_t size() override { return bytes.size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> out) override {
		auto count = std::min<std::size_t>(out.size(), offset < bytes.size() ? bytes.size() - offset : 0);
		bytes.copy(out.data(), count, offset);
		return count;
	}
	std::size_t write_at(std::uint64_t offset, std::string_view data) override {
		if (bytes.size() < offset + data.size()) { bytes.resize(offset + data.size()); }
		bytes.replace(offset, data.size(), data);
		return data.size();
	}
	void truncate(std::uint64_t size) override { bytes.resize(size); }
	void sync() override { ++syncs; }
	void sync_data() override {
		++data_syncs;
		if (fail_with) { throw std::system_error(fail_with, std::generic_category(), "data sync"); }
	}
	std::string bytes;
	int syncs = 0, data_syncs = 0, fail_with = 0;
};

// Write, then SyncData; stops at the first error. The shape of a turn write.
class WriteThenSyncData final : public c::Operation {
  public:
	WriteThenSyncData(c::IO &io, std::shared_ptr<c::File> file, std::string_view bytes, c::Kind first = c::Kind::Write) : io_(io) {
		request_.token.operation = 1;
		request_.kind = first;
		request_.file = std::move(file);
		request_.bytes = bytes;
	}
	const c::Request &request() const override { return request_; }
	void submitted() override { submitted_ = true; }
	bool complete(c::Result result) noexcept override {
		if (!submitted_ || done_ || result.token != request_.token) { return false; }
		submitted_ = false;
		++completed;
		if (result.error) { error = result.error; done_ = true; return true; }
		if (request_.kind == c::Kind::Write) {
			request_.kind = c::Kind::SyncData;
			++request_.token.step;
		} else {
			done_ = true;
		}
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return submitted_; }
	c::IO &io() const noexcept override { return io_; }
	std::exception_ptr error;
	unsigned completed = 0;

  private:
	c::IO &io_;
	c::Request request_;
	bool submitted_ = false, done_ = false;
};

void check_errno(const std::exception_ptr &error, int want, const char *what) {
	check(static_cast<bool>(error), what);
	try { std::rethrow_exception(error); } catch (const std::system_error &e) {
		check(e.code() == std::error_code(want, std::generic_category()), what);
	}
}

using Route = void (*)(c::IO &, c::Operation &);
void inline_route(c::IO &, c::Operation &operation) { c::drive_synchronously(operation); }
void queue_route(c::IO &, c::Operation &operation) {
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
	auto stats = queue.stats();
	const auto at = static_cast<std::size_t>(c::Kind::SyncData) - stats.native_by_primitive.size();
	check(at < stats.native_extensions.size() && at < stats.fallback_extensions.size(), "SyncData has an extension slot");
	check(stats.native_extensions[at] + stats.fallback_extensions[at] == 1, "SyncData is counted once");
}

// A foreign File runs on the worker; sync_data is what runs, not sync.
void foreign_file(c::IO &io, const char *route_name, Route route) {
	const std::string at = std::string(" through the ") + route_name;
	auto file = std::make_shared<RecordingFile>();
	WriteThenSyncData ok(io, file, "turn");
	route(io, ok);
	check(!ok.error && ok.completed == 2, ("a write then SyncData completes" + at).c_str());
	check(file->bytes == "turn" && file->data_syncs == 1 && file->syncs == 0, ("SyncData calls sync_data, not sync" + at).c_str());

	auto failing = std::make_shared<RecordingFile>();
	failing->fail_with = EIO;
	WriteThenSyncData failed(io, failing, "turn");
	route(io, failed);
	check_errno(failed.error, EIO, ("a failed SyncData surfaces its errno" + at).c_str());
	check(failed.completed == 2 && !failed.in_flight() && failed.done(), ("a failed SyncData settles the operation" + at).c_str());
	check(failing->bytes == "turn" && failing->data_syncs == 1 && failing->syncs == 0, ("the written bytes stay after the failure" + at).c_str());
}

// The real file: bytes written, SyncData, then read back through a fresh handle. On Linux and
// FreeBSD the queue takes it natively.
void posix_file(const std::filesystem::path &base, const char *route_name, Route route) {
	const std::string at = std::string(" through the ") + route_name;
	auto directory = base / route_name;
	std::filesystem::remove_all(directory);
	std::filesystem::create_directory(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	c::PosixIO io(directory);
	std::shared_ptr<c::File> file = io.create_exclusive("turn-log");
	WriteThenSyncData operation(io, file, "durable");
	route(io, operation);
	check(!operation.error && operation.completed == 2, ("a PosixFile write then SyncData completes" + at).c_str());
	char back[16]{};
	check(io.open_existing("turn-log")->read_at(0, back) == 7 && std::string_view(back, 7) == "durable", ("SyncData keeps the bytes" + at).c_str());

	// A descriptor no longer open: the data sync fails with EBADF, not silently.
	std::shared_ptr<c::File> closed = io.make_closed_file();
	WriteThenSyncData bad(io, closed, "", c::Kind::SyncData);
	route(io, bad);
	check(static_cast<bool>(bad.error) && bad.done() && !bad.in_flight(), ("SyncData on a closed file fails" + at).c_str());
}

#ifdef __linux__
// The ring completes SyncData natively with IORING_FSYNC_DATASYNC; the hook turns its completion
// into EIO, which must surface as the operation's error with the queue idle again.
bool corrupt_next = false;
unsigned hooked = 0;
void inspect_hook(io_uring_cqe &entry) {
	++hooked;
	if (corrupt_next) { entry.res = -EIO; }
}
void native_ring(const std::filesystem::path &base) {
	auto directory = base / "ring";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directory(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	c::PosixIO io(directory);
	std::shared_ptr<c::File> file = io.create_exclusive("turn-log");
	kronuz::journal::LinuxCompletionHooks hooks;
	hooks.inspect = &inspect_hook;
	kronuz::journal::LinuxCompletionQueue queue(kronuz::journal::LinuxCompletionPolicy::Automatic, hooks);
	if (!queue.native_available()) {
		std::cout << "sync_data: io_uring unavailable (" << queue.setup_error() << "), ring section skipped\n";
		return;
	}
	for (bool fault : {false, true}) {
		corrupt_next = false;
		hooked = 0;
		WriteThenSyncData operation(io, file, "ring");
		auto shared = std::shared_ptr<c::Operation>(std::shared_ptr<c::Operation>{}, &operation);
		while (!operation.done()) {
			check(queue.submit(shared), "the ring admits the primitive");
			for (;;) {
				if (auto completion = queue.poll()) {
					check(completion->operation->complete(std::move(completion->completion)), "the original completes");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			corrupt_next = fault && operation.completed == 1;
		}
		auto stats = queue.stats();
		const auto at = static_cast<std::size_t>(c::Kind::SyncData) - stats.native_by_primitive.size();
		check(stats.native_extensions[at] >= 1, "SyncData ran on the ring");
		if (fault) { check_errno(operation.error, EIO, "a failed ring SyncData surfaces its errno"); }
		else { check(!operation.error && operation.completed == 2, "the ring completes SyncData"); }
	}
}
#endif
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_data_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path base(argv[1]);
	std::filesystem::remove_all(base);
	std::filesystem::create_directory(base);
	std::filesystem::permissions(base, std::filesystem::perms::owner_all);
	c::PosixIO io(base);
	foreign_file(io, "drive_synchronously", inline_route);
	foreign_file(io, "native queue", queue_route);
	posix_file(base, "drive_synchronously", inline_route);
	posix_file(base, "native queue", queue_route);
#ifdef __linux__
	native_ring(base);
#endif
	check(c::Stats{}.native_extensions.size() == c::Stats{}.fallback_extensions.size() &&
			  c::Stats::supported(c::Kind::SyncData) && !c::Stats::supported(static_cast<c::Kind>(static_cast<int>(c::Kind::SyncData) + 1)),
		  "SyncData is the last supported kind");
	std::cout << "sync_data: ok\n";
	return 0;
}

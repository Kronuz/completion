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

// How PosixIO syncs files: the flush mode, the sync system calls a test replaces, file_sync and
// file_sync_data, and files_sync_devices, which stages a batch and flushes each device once.

#pragma once

#include "posix_support.h"
#include <algorithm>
#include <cerrno>
#include <span>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kronuz::journal {

// How far PosixIO's syncs push data on macOS, chosen by whoever builds the PosixIO: `full` issues
// F_FULLFSYNC for every durable sync and every ordering point; `plain` issues fsync for durable
// syncs, which leaves data in the drive's cache, and F_BARRIERFSYNC for ordering points. Linux and
// FreeBSD make the same calls in both modes. It has no default, so PosixIOOptions{} does not compile.
class FlushMode {
public:
	enum Value : unsigned char { plain, full };
	constexpr FlushMode(Value value) noexcept : value_(value) {}
	constexpr bool operator==(const FlushMode&) const noexcept = default;
private:
	Value value_;
};

namespace detail {
// The sync system calls PosixIO issues; a test replaces them through PosixIO::replace_sync_calls
// to record the real call sequence on this platform.
using FsyncCall = int (*)(int fd);
#ifdef __APPLE__
using FcntlCall = int (*)(int fd, int command);
inline int default_fcntl(int fd, int command) { return ::fcntl(fd, command); }
// Errors a filesystem without F_BARRIERFSYNC returns: ENOTSUP (the vnode default) or ENOTTY
// (an unrecognised command). APFS, HFS+, FAT32 and exFAT accept the barrier.
inline bool barrier_unsupported(int error) { return error == ENOTSUP || error == EOPNOTSUPP || error == ENOTTY; }
#endif
#ifdef __linux__
using WritebackCall = int (*)(int fd);
inline int default_writeback(int fd) { return ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE); }
#endif
// The flush mode and the calls that carry it out; every PosixFile keeps the copy it opened with.
struct SyncCalls {
	FlushMode mode;
	FsyncCall fsync_call = &::fsync;
	int sync(int fd) const { return fsync_call(fd); }
#ifdef __APPLE__
	FcntlCall fcntl_call = &default_fcntl;
	int stat(int fd, struct stat* status) const { return ::fstat(fd, status); }
	int full_sync(int fd) const { return fcntl_call(fd, F_FULLFSYNC); }
	// F_BARRIERFSYNC, 0 when it ordered; where the filesystem lacks the call, the stronger
	// full flush, 1 when it succeeded. Any other error (EIO, ...) is the barrier's own failure
	// and is returned, never retried.
	int barrier(int fd) const {
		int result;
		do { result = fcntl_call(fd, F_BARRIERFSYNC); } while (result < 0 && errno == EINTR);
		if (result >= 0 || !barrier_unsupported(errno)) { return result < 0 ? result : 0; }
		do { result = full_sync(fd); } while (result < 0 && errno == EINTR);
		return result < 0 ? result : 1;
	}
#endif
#ifdef __linux__
	FsyncCall fdatasync_call = &::fdatasync;
	int sync_data(int fd) const { return fdatasync_call(fd); }
	WritebackCall writeback_call = &default_writeback;
	int writeback(int fd) const { return writeback_call(fd); }
#endif
};
// macOS: F_FULLFSYNC in full mode, fsync in plain; fsync elsewhere. A failure fails the call.
inline void file_sync(int fd, const SyncCalls& calls) {
	int result;
#ifdef __APPLE__
	const bool full = calls.mode == FlushMode::full;
	do { result = full ? calls.full_sync(fd) : calls.sync(fd); } while (result < 0 && errno == EINTR);
#else
	do { result = calls.sync(fd); } while (result < 0 && errno == EINTR);
#endif
	if (result < 0) { system_failure("durably sync journal file"); }
}
// fdatasync on Linux; file_sync elsewhere (macOS's fdatasync is never the stronger call).
inline void file_sync_data(int fd, const SyncCalls& calls) {
#ifdef __linux__
	int result;
	do { result = calls.sync_data(fd); } while (result < 0 && errno == EINTR);
	if (result < 0) { system_failure("durably sync journal data"); }
#else
	file_sync(fd, calls);
#endif
}
// How files_sync_devices leaves the device: durable, or ordered on the directory's device
// (see PosixIO::order_files_and_directory).
enum class SyncMode { durable, ordered };
// Stages every file (and `directory`, when >= 0) with fsync; in full flush mode it then
// flushes each distinct device once. Ordered issues a barrier, not a full flush, on the
// directory's device; in full mode every other device is still flushed in full, in plain mode
// it keeps its fsync. Plain and durable is the fsyncs alone.
// `directory_flush` is the descriptor a flush of the directory's device goes through when
// no file shares it (the owner file when held, else the directory). True when the directory's
// device was ordered by a barrier that did not flush (a barrier returning 0).
template <class Calls> inline bool files_sync_devices(std::span<const int> fds, int directory, int directory_flush, SyncMode mode, Calls& calls) {
	if (fds.empty() && directory < 0) { return false; }
	const bool full = calls.mode == FlushMode::full;
	struct DeviceFile { dev_t device; int fd; bool directory; };
	std::vector<DeviceFile> devices;
	devices.reserve(fds.size() + 1);
	auto discover = [&](int fd, int flush_fd, bool is_directory) {
		struct stat status{};
		int result;
		do { result = calls.stat(fd, &status); } while (result < 0 && errno == EINTR);
		if (result < 0) { system_failure("stat journal file"); }
		devices.push_back({status.st_dev, flush_fd, is_directory});
	};
	if (full || mode == SyncMode::ordered) {
		for (int fd : fds) { discover(fd, fd, false); }
		if (directory >= 0) { discover(directory, directory_flush, true); }
	}
	// Files stay ahead of the directory within a device, so a flush goes through a file.
	std::stable_sort(devices.begin(), devices.end(), [](const DeviceFile& a, const DeviceFile& b) { return a.device < b.device; });
	for (int fd : fds) {
		int result;
		do { result = calls.sync(fd); } while (result < 0 && errno == EINTR);
		if (result < 0) { system_failure("sync journal file"); }
	}
	if (directory >= 0) {
		int result;
		do { result = calls.sync(directory); } while (result < 0 && errno == EINTR);
		if (result < 0) { system_failure("sync journal directory"); }
	}
	bool ordered_only = false;
	for (std::size_t i = 0; i < devices.size(); ++i) {
		if (i && devices[i].device == devices[i - 1].device) { continue; }
		bool holds_directory = false;
		for (std::size_t j = i; j < devices.size() && devices[j].device == devices[i].device; ++j) { holds_directory |= devices[j].directory; }
		int result;
		if (mode == SyncMode::ordered && holds_directory) {
			do { result = calls.barrier(devices[i].fd); } while (result < 0 && errno == EINTR);
			if (result < 0) { system_failure("order journal files"); }
			ordered_only = result == 0;
		} else if (full) {
			do { result = calls.full_sync(devices[i].fd); } while (result < 0 && errno == EINTR);
			if (result < 0) { system_failure("durably sync journal file"); }
		}
	}
	return ordered_only;
}
template <class Calls> inline void files_sync_devices(std::span<const int> fds, Calls& calls) {
	(void)files_sync_devices(fds, -1, -1, SyncMode::durable, calls);
}
// macOS stages every file before flushing each distinct device; Linux starts writeback
// on every file before the first fsync waits; elsewhere each file is fsync'd in order.
template <class Calls> inline void files_sync(std::span<const int> fds, Calls& calls) {
	if (fds.empty()) { return; }
#ifdef __APPLE__
	files_sync_devices(fds, calls);
#else
#ifdef __linux__
	for (int fd : fds) { (void)calls.writeback(fd); }  // a hint; fsync reports errors
#endif
	for (int fd : fds) {
		int result;
		do { result = calls.sync(fd); } while (result < 0 && errno == EINTR);
		if (result < 0) { system_failure("durably sync journal file"); }
	}
#endif
}
} // namespace detail

} // namespace kronuz::journal

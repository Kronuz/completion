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

// How PosixIO syncs files: the flush mode and sync_call, the one table from a sync site to the
// call it makes; the sync system calls a test replaces; file_sync; and files_sync_devices, which
// stages a batch and flushes each device once.

#pragma once

#include "posix_support.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kronuz::journal {

// How far PosixIO's syncs push data on macOS, chosen by whoever builds the PosixIO: `full` issues
// F_FULLFSYNC for every durable sync and every ordering point; `plain` issues fsync for durable
// syncs, which leaves data in the drive's cache, and F_BARRIERFSYNC for ordering points. Linux and
// FreeBSD make the same calls in both modes. It has no default, so PosixIOOptions{} does not compile,
// and Value has no zero: a value-initialized Value names no mode, and FlushMode refuses it.
class FlushMode {
public:
	enum Value : unsigned char { plain = 1, full = 2 };
	constexpr FlushMode(Value value)
		: value_(value == plain || value == full ? value : throw std::invalid_argument("flush mode must be plain or full")) {}
	constexpr bool operator==(const FlushMode&) const noexcept = default;
private:
	Value value_;
};

// Where PosixIO syncs: File::sync (also owner.lock's creation and each device of a batch),
// File::sync_data (the turn barrier's SyncData), sync_directory's flush, an ordering point.
enum class SyncSite { file, data, directory, order };
// The call a site makes after any staged fsync; `fsync` there means the staged fsync was all.
enum class SyncCall { fsync, fdatasync, full_fsync, barrier_fsync };
enum class SyncPlatform { macos, linux_kernel, other };
#if defined(__APPLE__)
inline constexpr SyncPlatform host_sync_platform = SyncPlatform::macos;
#elif defined(__linux__)
inline constexpr SyncPlatform host_sync_platform = SyncPlatform::linux_kernel;
#else
inline constexpr SyncPlatform host_sync_platform = SyncPlatform::other;
#endif
// The one place a flush mode becomes a call. macOS: full is F_FULLFSYNC everywhere; plain is
// fsync, and F_BARRIERFSYNC at an ordering point. Linux: fsync, and fdatasync for sync_data, in
// either mode. Elsewhere: fsync.
constexpr SyncCall sync_call(SyncSite site, FlushMode mode, SyncPlatform platform = host_sync_platform) noexcept {
	switch (platform) {
	case SyncPlatform::macos:
		if (mode == FlushMode::full) { return SyncCall::full_fsync; }
		switch (site) {
		case SyncSite::file: return SyncCall::fsync;
		case SyncSite::data: return SyncCall::fsync;
		case SyncSite::directory: return SyncCall::fsync;
		case SyncSite::order: return SyncCall::barrier_fsync;
		}
		break;
	case SyncPlatform::linux_kernel:
		return site == SyncSite::data ? SyncCall::fdatasync : SyncCall::fsync;
	case SyncPlatform::other:
		break;
	}
	return SyncCall::fsync;
}
// Whether a batch in `batch` mode would sync a `file`-mode file with a weaker call than the file's
// own File::sync (macOS: a full file in a plain batch). Such a file syncs itself.
constexpr bool batch_weakens(FlushMode file, FlushMode batch, SyncPlatform platform = host_sync_platform) noexcept {
	return file == FlushMode::full && sync_call(SyncSite::file, file, platform) != sync_call(SyncSite::file, batch, platform);
}

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
// The system calls the sync paths issue; the default is the system's.
struct SyncSeam {
	FsyncCall fsync_call = &::fsync;
#ifdef __APPLE__
	FcntlCall fcntl_call = &default_fcntl;
#endif
#ifdef __linux__
	FsyncCall fdatasync_call = &::fdatasync;
	WritebackCall writeback_call = &default_writeback;
#endif
	constexpr bool operator==(const SyncSeam&) const noexcept = default;
};
// Every seam in use, each once, so a file names its seam in one byte: 0 is the default, and an
// entry is written once, under the mutex, before its index is handed out, and never changes.
struct SyncSeams {
	std::array<SyncSeam, 256> entries{};
	unsigned count = 1;
	std::mutex mutex;
};
inline constinit SyncSeams sync_seams{};
// The index of `seam`, added when new; throws past 255 seams other than the default.
inline std::uint8_t intern_sync_seam(const SyncSeam& seam) {
	if (seam == SyncSeam{}) { return 0; }
	std::lock_guard lock(sync_seams.mutex);
	for (unsigned i = 1; i < sync_seams.count; ++i) {
		if (sync_seams.entries[i] == seam) { return static_cast<std::uint8_t>(i); }
	}
	if (sync_seams.count == sync_seams.entries.size()) { throw std::length_error("too many distinct sync seams"); }
	sync_seams.entries[sync_seams.count] = seam;
	return static_cast<std::uint8_t>(sync_seams.count++);
}
// The flush mode and the seam that carry out a sync, two bytes; every PosixFile keeps the copy it
// opened with.
struct SyncCalls {
	FlushMode mode;
	std::uint8_t seam = 0;  // sync_seams.entries' index
	const SyncSeam& calls() const noexcept { return sync_seams.entries[seam]; }
	int sync(int fd) const { return calls().fsync_call(fd); }
#ifdef __APPLE__
	int stat(int fd, struct stat* status) const { return ::fstat(fd, status); }
	int full_sync(int fd) const { return calls().fcntl_call(fd, F_FULLFSYNC); }
	// F_BARRIERFSYNC, 0 when it ordered; where the filesystem lacks the call, the stronger
	// full flush, 1 when it succeeded. Any other error (EIO, ...) is the barrier's own failure
	// and is returned, never retried.
	int barrier(int fd) const {
		int result;
		do { result = calls().fcntl_call(fd, F_BARRIERFSYNC); } while (result < 0 && errno == EINTR);
		if (result >= 0 || !barrier_unsupported(errno)) { return result < 0 ? result : 0; }
		do { result = full_sync(fd); } while (result < 0 && errno == EINTR);
		return result < 0 ? result : 1;
	}
#endif
#ifdef __linux__
	int sync_data(int fd) const { return calls().fdatasync_call(fd); }
	int writeback(int fd) const { return calls().writeback_call(fd); }
#endif
	// One call sync_call chose, once; a call this platform lacks fails with ENOTSUP. Never
	// replaced by another call: a failed F_FULLFSYNC fails, whatever its errno.
	int issue(SyncCall call, int fd) const {
		switch (call) {
		case SyncCall::fsync: return sync(fd);
#ifdef __linux__
		case SyncCall::fdatasync: return sync_data(fd);
#endif
#ifdef __APPLE__
		case SyncCall::full_fsync: return full_sync(fd);
		case SyncCall::barrier_fsync: return barrier(fd) < 0 ? -1 : 0;
#endif
		default: break;
		}
		errno = ENOTSUP;
		return -1;
	}
};
static_assert(sizeof(SyncCalls) == 2);
// One file's sync at `site` (file or data), the call sync_call names; a failure fails the call.
inline void file_sync(int fd, const SyncCalls& calls, SyncSite site) {
	const SyncCall call = sync_call(site, calls.mode);
	int result;
	do { result = calls.issue(call, fd); } while (result < 0 && errno == EINTR);
	if (result < 0) { system_failure(site == SyncSite::data ? "durably sync journal data" : "durably sync journal file"); }
}
// How files_sync_devices leaves the device: durable, or ordered on the directory's device
// (see PosixIO::order_files_and_directory).
enum class SyncMode { durable, ordered };
// Stages every file (and `directory`, when >= 0) with fsync, then makes each distinct device's
// call once, as sync_call names it on macOS (this is macOS's batch): the order site's on the
// directory's device when ordered, the file site's on every other. A device whose call is fsync
// keeps its staged fsync, so plain and durable is the fsyncs alone, with no device to find.
// `directory_flush` is the descriptor a flush of the directory's device goes through when
// no file shares it (the owner file when held, else the directory). True when the directory's
// device was ordered by a barrier that did not flush (a barrier returning 0).
template <class Calls> inline bool files_sync_devices(std::span<const int> fds, int directory, int directory_flush, SyncMode mode, Calls& calls) {
	if (fds.empty() && directory < 0) { return false; }
	const SyncCall device_call = sync_call(SyncSite::file, calls.mode, SyncPlatform::macos);
	const SyncCall order_call = mode == SyncMode::ordered ? sync_call(SyncSite::order, calls.mode, SyncPlatform::macos) : device_call;
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
	if (device_call != SyncCall::fsync || order_call != SyncCall::fsync) {
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
		switch (holds_directory ? order_call : device_call) {
		case SyncCall::barrier_fsync:
			do { result = calls.barrier(devices[i].fd); } while (result < 0 && errno == EINTR);
			if (result < 0) { system_failure("order journal files"); }
			ordered_only = result == 0;
			break;
		case SyncCall::full_fsync:
			do { result = calls.full_sync(devices[i].fd); } while (result < 0 && errno == EINTR);
			if (result < 0) { system_failure("durably sync journal file"); }
			break;
		default: break;  // fsync: the staged fsync was this device's sync
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

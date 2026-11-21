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

// The descriptors PosixIO hands out: PosixFile (reads, writes, syncs, leases, maps,
// preallocates and advises) and PosixCursor (a directory scan).

#pragma once

#include "posix_sync.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace kronuz::journal {

namespace detail {
inline off_t checked_offset(std::uint64_t offset) {
	if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
		throw std::length_error("journal offset exceeds platform range");
	}
	return static_cast<off_t>(offset);
}
// Whether the open file carries an ACL beyond its mode bits: macOS any extended entry, FreeBSD a
// non-trivial NFSv4 or POSIX.1e ACL, Linux a POSIX access ACL. As Detent's mode_only_acl, an ACL
// that cannot be read counts as one, except where the filesystem says it has none.
inline bool extended_acl(int fd) {
#if defined(__APPLE__)
	acl_t acl = ::acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
	if (!acl) { return errno != ENOENT; }  // ENOENT: no extended ACL
	::acl_free(acl);
	return true;
#elif defined(__FreeBSD__)
	acl_t acl = ::acl_get_fd_np(fd, ACL_TYPE_NFS4);
	if (!acl && (errno == EINVAL || errno == EOPNOTSUPP)) { acl = ::acl_get_fd_np(fd, ACL_TYPE_ACCESS); }
	if (!acl) {
		return !(errno == EOPNOTSUPP && ::fpathconf(fd, _PC_ACL_EXTENDED) == 0 && ::fpathconf(fd, _PC_ACL_NFS4) == 0);
	}
	int trivial = 0;
	const bool inspected = ::acl_is_trivial_np(acl, &trivial) == 0;
	const bool freed = ::acl_free(acl) == 0;
	return !(inspected && trivial == 1 && freed);
#elif defined(__linux__)
	// The kernel stores an access ACL only when it says more than the mode bits.
	if (::fgetxattr(fd, "system.posix_acl_access", nullptr, 0) >= 0) { return true; }
	return errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP;
#else
	(void)fd;
	return true;
#endif
}
// Errors of a filesystem that cannot reserve space; preallocate then grows the size only.
inline bool reservation_unsupported(int error) {
	return error == EOPNOTSUPP || error == ENOTSUP || error == ENOSYS || error == EINVAL;
}
class PosixCursor : public DirectoryCursor {
public:
	explicit PosixCursor(DIR* directory) : directory_(directory) {}
	~PosixCursor() override { if (directory_) { ::closedir(directory_); } }
	void install(DIR* directory) noexcept { directory_ = directory; }
	bool closed() const noexcept { return directory_ == nullptr; }
	std::optional<std::string> next() override {
		auto name = next_name();
		return name ? std::optional<std::string>(std::string(*name)) : std::nullopt;
	}
	std::optional<std::size_t> next_into(std::span<char> destination) override {
		auto name = next_name();
		if (!name) { return std::nullopt; }
		if (name->size() > destination.size()) { throw std::length_error("directory name exceeds caller buffer"); }
		std::copy(name->begin(), name->end(), destination.begin());
		return name->size();
	}
	void close() override {
		if (!directory_) { throw std::logic_error("directory cursor already closed"); }
		// closedir consumes the stream even when it reports an error.
		auto directory = std::exchange(directory_, nullptr);
		if (::closedir(directory)) { system_failure("close directory cursor"); }
	}
private:
	std::optional<std::string_view> next_name() {
		if (!directory_) { throw std::logic_error("directory cursor closed"); }
		for (;;) {
			errno = 0; auto entry = ::readdir(directory_);
			if (!entry) { if (errno) { system_failure("scan journal directory"); } return std::nullopt; }
			std::string_view name(entry->d_name);
			if (name != "." && name != "..") { return name; }
		}
	}
private:
	DIR* directory_;
};
class PosixFile : public File {
public:
	explicit PosixFile(int fd) : fd_(fd) {}
	~PosixFile() override { if (fd_ >= 0) { ::close(fd_); } }
	void install(int fd) noexcept { fd_ = fd; }
	// Trusted completion backend only. The owning File lease retains this FD.
	int native_handle() const noexcept { return fd_; }
	std::uint64_t size() override {
		struct stat status{};
		if (::fstat(fd_, &status)) { system_failure("stat journal size"); }
		if (status.st_size < 0) { throw std::runtime_error("negative journal size"); }
		return static_cast<std::uint64_t>(status.st_size);
	}
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override {
		ssize_t count;
		auto length = std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		do { count = ::pread(fd_, bytes.data(), length, checked_offset(offset)); } while (count < 0 && errno == EINTR);
		if (count < 0) { system_failure("read journal file"); }
		return static_cast<std::size_t>(count);
	}
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		ssize_t count;
		auto length = std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		do { count = ::pwrite(fd_, bytes.data(), length, checked_offset(offset)); } while (count < 0 && errno == EINTR);
		if (count < 0) { system_failure("write journal file"); }
		return static_cast<std::size_t>(count);
	}
	void truncate(std::uint64_t size) override {
		int result;
		do { result = ::ftruncate(fd_, checked_offset(size)); } while (result < 0 && errno == EINTR);
		if (result < 0) { system_failure("truncate journal tail"); }
	}
	void sync() override { file_sync(fd_); }
	void sync_data() override { file_sync_data(fd_); }
	EntryInspection inspect() override {
		struct stat status{};
		if (::fstat(fd_, &status)) { system_failure("inspect held file"); }
		auto inspection = inspect_status(status);
		inspection.extended_acl = extended_acl(fd_);
		return inspection;
	}
	bool try_lease(LeaseMode mode) override {
		if (mode != LeaseMode::Shared && mode != LeaseMode::Exclusive) {
			throw std::invalid_argument("unsupported file lease mode");
		}
		// Lock conversion is forbidden; GC opens a separate fresh handle.
		if (lease_) { throw std::logic_error("file lease already acquired"); }
		int result;
		do { result = ::flock(fd_, (mode == LeaseMode::Shared ? LOCK_SH : LOCK_EX) | LOCK_NB); }
		while (result < 0 && errno == EINTR);
		if (result < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) { return false; }
			system_failure("acquire nonblocking file lease");
		}
		lease_ = true;
		return true;
	}
	void close() override {
		if (fd_ < 0) { throw std::logic_error("file already closed"); }
		// Never retry close: the original descriptor may already be reused.
		auto descriptor = std::exchange(fd_, -1);
		lease_ = false;
		if (::close(descriptor)) { system_failure("close held file"); }
	}
	std::shared_ptr<const Mapping> map() override {
		const std::uint64_t length = size();
		if (length > std::numeric_limits<std::size_t>::max()) { throw std::runtime_error("file too large to map"); }
		return std::make_shared<PosixMapping>(fd_, static_cast<std::size_t>(length));
	}
	// Reserves with fallocate (Linux), F_PREALLOCATE then ftruncate (macOS) or posix_fallocate;
	// where the filesystem cannot reserve, grows the size as the default does.
	void preallocate(std::uint64_t offset, std::uint64_t length) override {
		if (length > std::numeric_limits<std::uint64_t>::max() - offset) {
			throw std::length_error("preallocation exceeds the file range");
		}
		const off_t end = checked_offset(offset + length);
		if (length && reserve(static_cast<off_t>(offset), static_cast<off_t>(length), end)) { return; }
		File::preallocate(offset, length);
	}
	// posix_fadvise (Linux, FreeBSD); F_RDADVISE and F_RDAHEAD (macOS). Errors are ignored.
	void advise(std::uint64_t offset, std::uint64_t length, Advice advice) noexcept override {
		constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
		if (fd_ < 0 || length == 0 || offset > limit) { return; }
		length = std::min(length, limit - offset);
#if defined(__APPLE__)
		switch (advice) {
		case Advice::WillNeed: {
			radvisory ahead{static_cast<off_t>(offset), static_cast<int>(std::min<std::uint64_t>(length, std::numeric_limits<int>::max()))};
			(void)::fcntl(fd_, F_RDADVISE, &ahead);
			break;
		}
		case Advice::Normal: case Advice::Sequential: (void)::fcntl(fd_, F_RDAHEAD, 1); break;
		case Advice::Random: (void)::fcntl(fd_, F_RDAHEAD, 0); break;
		case Advice::DontNeed: break;
		}
#elif defined(__linux__) || defined(__FreeBSD__)
		int native;
		switch (advice) {
		case Advice::Normal: native = POSIX_FADV_NORMAL; break;
		case Advice::Sequential: native = POSIX_FADV_SEQUENTIAL; break;
		case Advice::Random: native = POSIX_FADV_RANDOM; break;
		case Advice::WillNeed: native = POSIX_FADV_WILLNEED; break;
		case Advice::DontNeed: native = POSIX_FADV_DONTNEED; break;
		default: return;
		}
		(void)::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(length), native);
#else
		(void)advice;
#endif
	}
	std::shared_ptr<SharedMapping> map_shared(std::uint64_t length) override {
		if (length > size()) { throw std::invalid_argument("shared mapping exceeds the file"); }
		if (length > std::numeric_limits<std::size_t>::max()) { throw std::runtime_error("file too large to map"); }
		return std::make_shared<PosixSharedMapping>(fd_, static_cast<std::size_t>(length));
	}
private:
	// True when [offset, end) is reserved; false when the filesystem cannot reserve. Other
	// failures throw.
	bool reserve(off_t offset, off_t length, off_t end) {
#if defined(__APPLE__)
		(void)length;
		// F_PREALLOCATE reserves past the allocated end and leaves the size, so holes below the
		// size get zeros, then it reserves the growth and ftruncate sets the size: contiguous
		// when it can be, else in pieces.
		const auto current = static_cast<off_t>(size());
		fill_holes(offset, std::min(end, current));
		if (end <= current) { return true; }
		fstore_t store{F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, end - current, 0};
		int result;
		do { result = ::fcntl(fd_, F_PREALLOCATE, &store); } while (result < 0 && errno == EINTR);
		if (result < 0) {
			store.fst_flags = F_ALLOCATEALL;
			do { result = ::fcntl(fd_, F_PREALLOCATE, &store); } while (result < 0 && errno == EINTR);
		}
		if (result < 0) {
			if (reservation_unsupported(errno)) { return false; }
			system_failure("preallocate journal file");
		}
		truncate(static_cast<std::uint64_t>(end));
		return true;
#elif defined(__linux__)
		// fallocate and posix_fallocate allocate every unallocated block in the range, holes included.
		(void)end;
		int result;
		do { result = ::fallocate(fd_, 0, offset, length); } while (result < 0 && errno == EINTR);
		if (result == 0) { return true; }
		if (reservation_unsupported(errno)) { return false; }
		system_failure("preallocate journal file");
#else
		(void)end;
		int error;
		do { error = ::posix_fallocate(fd_, offset, length); } while (error == EINTR);
		if (error == 0) { return true; }
		if (reservation_unsupported(error)) { return false; }
		errno = error;
		system_failure("preallocate journal file");
#endif
	}
#if defined(__APPLE__)
	// Writes zeros into each hole of [from, to), found with SEEK_HOLE and SEEK_DATA, so the range
	// gets its blocks and still reads zeros there. A filesystem that cannot seek holes has none.
	void fill_holes(off_t from, off_t to) {
		static constexpr char zeros[65536] = {};
		while (from < to) {
			off_t hole = ::lseek(fd_, from, SEEK_HOLE);
			if (hole < 0) {
				if (errno == ENXIO || errno == EINVAL || errno == ENOTTY || errno == ENOTSUP) { return; }
				system_failure("find journal file hole");
			}
			if (hole >= to) { return; }
			off_t data = ::lseek(fd_, hole, SEEK_DATA);
			if (data < 0) {
				if (errno != ENXIO) { system_failure("find journal file data"); }
				data = to;
			}
			data = std::min(data, to);
			while (hole < data) {
				const auto count = static_cast<std::size_t>(std::min<off_t>(data - hole, sizeof zeros));
				ssize_t written;
				do { written = ::pwrite(fd_, zeros, count, hole); } while (written < 0 && errno == EINTR);
				if (written < 0) { system_failure("fill journal file hole"); }
				hole += written;
			}
			from = data;
		}
	}
#endif
	// Writable and shared; its pages outlive the descriptor and the name, as PosixMapping's do.
	class PosixSharedMapping final : public SharedMapping {
	public:
		PosixSharedMapping(int fd, std::size_t length) {
			if (length == 0) { return; }
			void* at = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (at == MAP_FAILED) { system_failure("map file shared"); }
			view(std::span<char>(static_cast<char*>(at), length));
		}
		~PosixSharedMapping() override {
			if (!words().empty()) { ::munmap(words().data(), words().size()); }
		}
		PosixSharedMapping(const PosixSharedMapping&) = delete;
		PosixSharedMapping& operator=(const PosixSharedMapping&) = delete;
	};
	// The mapping keeps its pages after the descriptor closes or the file is unlinked.
	class PosixMapping final : public Mapping {
	public:
		PosixMapping(int fd, std::size_t length) {
			if (length == 0) { return; }
			void* at = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
			if (at == MAP_FAILED) { system_failure("map file"); }
			bytes_ = std::string_view(static_cast<const char*>(at), length);
		}
		~PosixMapping() override {
			if (!bytes_.empty()) { ::munmap(const_cast<char*>(bytes_.data()), bytes_.size()); }
		}
		PosixMapping(const PosixMapping&) = delete;
		PosixMapping& operator=(const PosixMapping&) = delete;
	};
	int fd_;
	bool lease_ = false;
};
} // namespace detail

} // namespace kronuz::journal

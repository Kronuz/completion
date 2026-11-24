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

// PosixIO, the trusted-directory POSIX backend, with its options and make_posix_io; the
// pieces it builds on are the posix_*.h headers included here.

#pragma once

#include "io.h"
#include "allocation.h"
#include "posix_support.h"
#include "posix_sync.h"
#include "posix_volume.h"
#include "posix_file.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <dirent.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace kronuz::journal {

// How PosixIO syncs, and how strictly it trusts the directory it is given. The flush mode is
// required; the trust defaults refuse, either option admits what it names and
// PosixIO::directory_trust() reports what was found.
struct PosixIOOptions {
	FlushMode flush_mode;                 // every sync PosixIO and its files issue follows it
	bool follow_root_symlink = false;     // open through a final-component symlink; the descriptor is then held
	bool allow_shared_directory = false;  // admit a foreign owner and group/other-writable mode
	// macOS: called once per volume per process, after the first barrier that ordered without
	// flushing on storage other than an internal Apple SSD, on the thread of that ordering call.
	// An exception fails the call after its barrier, and the volume warns again. Never called
	// elsewhere or in full mode. `barrier_warning_context` is its first argument.
	BarrierWarningCall barrier_warning = nullptr;
	void* barrier_warning_context = nullptr;
};

class PosixIO final : public IO {
public:
	// The application establishes and durably publishes this directory first.
	// Trusted ancestors are a caller precondition. By default a final-component
	// symlink, a foreign owner, and a group/other-writable mode are rejected here;
	// `options` admits them (see directory_trust to warn). Entries stay no-follow.
	explicit PosixIO(const std::filesystem::path& directory, PosixIOOptions options, unsigned artifact_mode = 0600)
		: PosixIO(kronuz::io::completion::AllocationContext{}, directory, options, artifact_mode) {}
	PosixIO(kronuz::io::completion::AllocationContext context, const std::filesystem::path& directory,
		PosixIOOptions options, unsigned artifact_mode = 0600)
		: context_(std::move(context)), sync_calls_{options.flush_mode},
#ifdef __APPLE__
		  barrier_warning_(options.barrier_warning), barrier_warning_context_(options.barrier_warning_context),
#endif
		  artifact_mode_(artifact_mode) {
		if (artifact_mode != 0600 && artifact_mode != 0640 && artifact_mode != 0644) {
			throw std::invalid_argument("unsupported artifact permissions");
		}
		constexpr int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
		directory_ = ::open(directory.c_str(), flags | O_NOFOLLOW);
		// A final symlink fails O_NOFOLLOW with ELOOP, EMLINK (FreeBSD), or ENOTDIR (O_DIRECTORY).
		if (directory_ < 0 && options.follow_root_symlink && (errno == ELOOP || errno == EMLINK || errno == ENOTDIR)) {
			struct stat link{};
			if (!::lstat(directory.c_str(), &link) && S_ISLNK(link.st_mode)) {
				trust_.symlinked_root = true;
				directory_ = ::open(directory.c_str(), flags);
			}
		}
		if (directory_ < 0) { detail::system_failure("open journal directory"); }
		try {
			struct stat status{};
			if (::fstat(directory_, &status)) { detail::system_failure("stat journal directory"); }
			trust_.known = true;
			trust_.foreign_owner = status.st_uid != ::geteuid();
			trust_.shared_writable = (status.st_mode & 0022) != 0;
			if (!S_ISDIR(status.st_mode) || ((trust_.foreign_owner || trust_.shared_writable) && !options.allow_shared_directory)) {
				throw std::runtime_error("journal directory requires exclusive ownership");
			}
			device_ = status.st_dev;
		} catch (...) { ::close(directory_); directory_ = -1; throw; }
	}
	// Trusted completion backends retain the IO owner while using this capability.
	int native_directory_handle() const noexcept { return directory_; }
	~PosixIO() override { ::close(directory_); }
	PosixIO(const PosixIO&) = delete;
	PosixIO& operator=(const PosixIO&) = delete;
	ManagedCapabilities managed_capabilities() const noexcept override { return {true, true, true, true, true, true, true, true}; }

	using IO::acquire_owner;
	// Owner locks are tried on any filesystem (see directory_filesystem to warn); destroy one
	// before its IO. The legacy presets forward to lock_owner, the one implementation.
	std::unique_ptr<OwnerLock> acquire_owner(bool create) override {
		return acquire_owner(OwnerLockOptions::legacy_file(create));
	}
	std::unique_ptr<OwnerLock> acquire_directory_owner() override {
		return acquire_owner(OwnerLockOptions::legacy_directory());
	}
	// What the root was found to be at construction, for a caller to warn on.
	DirectoryTrust directory_trust() const override { return trust_; }
	// The directory's filesystem, so a caller can warn when it is not known local.
	FilesystemClass directory_filesystem() const override {
#ifdef KRONUZ_COMPLETION_STATFS
		struct statfs status{};
		int result;
		do { result = statfs_ ? statfs_(directory_, &status) : ::fstatfs(directory_, &status); }
		while (result < 0 && errno == EINTR);
		if (result < 0) { detail::system_failure("inspect journal directory filesystem"); }
		return classify_filesystem(status);
#else
		return {false, "a filesystem this platform cannot classify"};
#endif
	}
	using FlockCall = int (*)(int fd, int operation);
	// A test seam: the flock answer every owner-lock step reports; null is flock.
	void replace_flock(FlockCall call) noexcept { flock_ = call ? call : &::flock; }
	// A test seam: the system calls the sync paths issue on this platform; null is the system
	// call. Call before opening the files it should see: a file keeps the calls it opened with.
	// macOS: fsync and fcntl (F_FULLFSYNC, F_BARRIERFSYNC); Linux: fsync, fdatasync and the
	// sync_file_range writeback hint; elsewhere: fsync.
#ifdef __APPLE__
	void replace_sync_calls(detail::FsyncCall fsync_call, detail::FcntlCall fcntl_call) noexcept {
		sync_calls_.fsync_call = fsync_call ? fsync_call : &::fsync;
		sync_calls_.fcntl_call = fcntl_call ? fcntl_call : &detail::default_fcntl;
	}
#elif defined(__linux__)
	void replace_sync_calls(detail::FsyncCall fsync_call, detail::FsyncCall fdatasync_call, detail::WritebackCall writeback_call) noexcept {
		sync_calls_.fsync_call = fsync_call ? fsync_call : &::fsync;
		sync_calls_.fdatasync_call = fdatasync_call ? fdatasync_call : &::fdatasync;
		sync_calls_.writeback_call = writeback_call ? writeback_call : &detail::default_writeback;
	}
#else
	void replace_sync_calls(detail::FsyncCall fsync_call) noexcept {
		sync_calls_.fsync_call = fsync_call ? fsync_call : &::fsync;
	}
#endif
#ifdef KRONUZ_COMPLETION_STATFS
	using StatfsCall = int (*)(int fd, struct statfs* result);
	// A test seam: the filesystem answer directory_filesystem classifies; null is fstatfs.
	void replace_statfs(StatfsCall call) noexcept { statfs_ = call; }
#endif
#ifdef __APPLE__
	using BarrierStorageCall = BarrierStorage (*)(const struct statfs& volume);
	// A test seam: what lies under the directory's volume for the barrier warning; null is IOKit's.
	void replace_barrier_storage(BarrierStorageCall call) noexcept { barrier_storage_ = call ? call : &detail::barrier_storage; }
#endif
	std::unique_ptr<File> make_closed_file() override { return control<ReusableFile>(*this); }
	void create_exclusive_into(std::string_view name, File& destination) override {
		auto &target = file_destination(destination);
		target.install(open_file(name, true));
	}
	std::unique_ptr<DirectoryCursor> make_closed_cursor() override { return control<ReusableCursor>(*this); }
	void open_existing_into(std::string_view name, File& destination) override {
		file_destination(destination).install(open_file(name, false));
	}
	void open_existing_read_only_into(std::string_view name, File& destination) override {
		file_destination(destination).install(open_file(name, false, true));
	}
	bool open_reclaim_candidate_into(std::string_view name, File& destination) override {
		return reclaim_into(name, destination, false);
	}
	bool open_reclaim_candidate_read_only_into(std::string_view name, File& destination) override {
		return reclaim_into(name, destination, true);
	}
	void scan_directory_into(DirectoryCursor& destination) override {
		auto &target = cursor_destination(destination);
		int fd = ::openat(directory_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) { detail::system_failure("open reusable scan cursor"); }
		auto directory = ::fdopendir(fd);
		if (!directory) { auto error = errno; ::close(fd); errno = error; detail::system_failure("create reusable scan cursor"); }
		target.install(directory);
	}
	std::unique_ptr<File> open_existing(std::string_view name) override { return file(name, false); }
	std::unique_ptr<File> open_existing_read_only(std::string_view name) override { return file(name, false, true); }
	std::unique_ptr<File> create_exclusive(std::string_view name) override { return file(name, true); }
	void replace(std::string_view source, std::string_view destination) override {
		detail::PosixName from(source), to(destination);
		if (::renameat(directory_, from.c_str(), directory_, to.c_str())) { detail::system_failure("replace journal manifest"); }
	}
	// linkat on the directory handle, which never replaces an existing destination.
	void link(std::string_view source, std::string_view destination) override {
		detail::PosixName from(source), to(destination);
		if (::linkat(directory_, from.c_str(), directory_, to.c_str(), 0)) { detail::system_failure("link journal file"); }
	}
	void remove(std::string_view name) override {
		detail::PosixName component(name);
		if (::unlinkat(directory_, component.c_str(), 0) && errno != ENOENT) { detail::system_failure("remove journal file"); }
	}
	std::unique_ptr<DirectoryCursor> scan_directory() override {
		auto cursor = control<detail::PosixCursor>(nullptr);
		// dup() would share the original directory's file offset. Use an
		// independent open description for every reclamation pass.
		int fd = ::openat(directory_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) { detail::system_failure("open journal scan cursor"); }
		auto directory = ::fdopendir(fd);
		if (!directory) { auto error = errno; ::close(fd); errno = error; detail::system_failure("create journal scan cursor"); }
		cursor->install(directory);
		return cursor;
	}
	std::unique_ptr<File> open_reclaim_candidate(std::string_view name) override { return reclaim(name, false); }
	std::unique_ptr<File> open_reclaim_candidate_read_only(std::string_view name) override { return reclaim(name, true); }
	std::optional<EntryFootprint> entry_footprint(std::string_view name) override {
		auto inspection = inspect_entry(name);
		return inspection ? std::optional<EntryFootprint>(inspection->footprint) : std::nullopt;
	}
	std::optional<EntryInspection> inspect_entry(std::string_view name) override {
		// Census foreign names too, beyond the journal's own 128-byte bound.
		detail::PosixName component(name, 255); struct stat status{};
		if (::fstatat(directory_, component.c_str(), &status, AT_SYMLINK_NOFOLLOW)) {
			if (errno == ENOENT) { return std::nullopt; } detail::system_failure("inspect storage footprint");
		}
		return detail::inspect_status(status);
	}
	void sync_files(std::span<File* const> files) override {
		std::vector<int> fds;
		fds.reserve(files.size());
		for (auto* file : files) {
			if (auto* posix = dynamic_cast<detail::PosixFile*>(file)) {
				fds.push_back(posix->native_handle());
			} else {
				file->sync();
			}
		}
		detail::files_sync(fds, sync_calls_);
	}
	// macOS: one device flush shared by the files and the directory entries.
	void sync_files_and_directory(std::span<File* const> files) override {
#ifdef __APPLE__
		sync_staged(files, detail::SyncMode::durable);
#else
		sync_files(files);
		sync_directory();
#endif
	}
	// macOS: fsync everything, then sync_call's order call on the directory's device (plain: one
	// barrier; full: a full flush) and its file call on any other device.
	void order_files_and_directory(std::span<File* const> files) override {
#ifdef __APPLE__
		const bool ordered_only = sync_staged(files, detail::SyncMode::ordered);
		if (ordered_only && barrier_warning_ && !barrier_noted_.load(std::memory_order_relaxed)) { note_barrier_volume(); }
#else
		sync_files_and_directory(files);
#endif
	}
	void sync_directory() override {
		int result;
		do { result = sync_calls_.sync(directory_); } while (result < 0 && errno == EINTR);
		if (result < 0) { detail::system_failure("sync journal directory"); }
		// Directory fsync establishes namespace ordering; a stronger call (macOS full: F_FULLFSYNC)
		// then asks the storage device to flush its cache, through the owner file when held.
		const SyncCall call = sync_call(SyncSite::directory, sync_calls_.mode);
		if (call == SyncCall::fsync) { return; }
		const int owner = owner_fd_.load();
		do { result = sync_calls_.issue(call, owner >= 0 ? owner : directory_); } while (result < 0 && errno == EINTR);
		if (result < 0) { detail::system_failure("durably sync journal file"); }
	}

private:
	// Whether `name` is a regular file this process owns, alone on its link and on the directory's device.
	bool reclaimable(std::string_view name) {
		detail::PosixName component(name); struct stat status{};
		if (::fstatat(directory_, component.c_str(), &status, AT_SYMLINK_NOFOLLOW)) {
			if (errno == ENOENT) { return false; } detail::system_failure("inspect reclamation candidate");
		}
		return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() && status.st_nlink == 1 && !(status.st_mode & 0022) &&
			(status.st_mode & 0600) == 0600 && status.st_dev == device_;
	}
	bool reclaim_into(std::string_view name, File& destination, bool read_only) {
		auto &target = file_destination(destination);
		if (!reclaimable(name)) { return false; }
		target.install(open_file(name, false, read_only));
		return true;
	}
	std::unique_ptr<File> reclaim(std::string_view name, bool read_only) {
		return reclaimable(name) ? file(name, false, read_only) : nullptr;
	}
#ifdef __APPLE__
	// True when the directory's device was ordered by a barrier that did not flush.
	bool sync_staged(std::span<File* const> files, detail::SyncMode mode) {
		std::vector<int> fds;
		fds.reserve(files.size());
		for (auto* file : files) {
			if (auto* posix = dynamic_cast<detail::PosixFile*>(file)) {
				fds.push_back(posix->native_handle());
			} else {
				file->sync();
			}
		}
		if (fds.empty() && mode == detail::SyncMode::durable) { sync_directory(); return false; }
		const int owner = owner_fd_.load();
		return detail::files_sync_devices(fds, directory_, owner >= 0 ? owner : directory_, mode, sync_calls_);
	}
	// The first barrier this backend ordered without flushing: classifies the volume once per
	// process and warns when no internal Apple SSD is under it.
	void note_barrier_volume() {
		if (barrier_noted_.exchange(true, std::memory_order_relaxed)) { return; }
		auto& volumes = detail::barrier_volumes();
		const auto device = static_cast<std::uint64_t>(device_);
		{
			std::lock_guard lock(volumes.mutex);
			if (std::ranges::find(volumes.devices, device) != volumes.devices.end()) { return; }
			volumes.devices.push_back(device);
		}
		try {
			BarrierWarning warning{device, {}, {}, {}};
			struct statfs status{};
			int result;
			do { result = statfs_ ? statfs_(directory_, &status) : ::fstatfs(directory_, &status); }
			while (result < 0 && errno == EINTR);
			if (result < 0) {
				warning.storage = {false, "unidentified (" + std::error_code(errno, std::generic_category()).message() + ")"};
			} else {
				warning.volume.assign(status.f_mntonname, ::strnlen(status.f_mntonname, sizeof status.f_mntonname));
				warning.source.assign(status.f_mntfromname, ::strnlen(status.f_mntfromname, sizeof status.f_mntfromname));
				warning.storage = barrier_storage_(status);
			}
			if (!warning.storage.apple_ssd) { barrier_warning_(barrier_warning_context_, warning); }
		} catch (...) {
			{
				std::lock_guard lock(volumes.mutex);
				std::erase(volumes.devices, device);
			}
			barrier_noted_.store(false, std::memory_order_relaxed);
			throw;
		}
	}
#endif
	class ReusableFile : public detail::PosixFile {
	public:
		explicit ReusableFile(const PosixIO& io) : detail::PosixFile(-1, io.sync_calls_), owner(io.instance_) {}
		const std::uint64_t owner;
	};
	class ReusableCursor : public detail::PosixCursor {
	public:
		explicit ReusableCursor(const PosixIO& io) : detail::PosixCursor(nullptr), owner(io.instance_) {}
		const std::uint64_t owner;
	};
	ReusableFile& file_destination(File& destination) {
		auto target = dynamic_cast<ReusableFile*>(&destination);
		if (!target || target->owner != instance_ || target->native_handle() >= 0) {
			throw std::invalid_argument("reusable file destination must be closed and belong to its backend");
		}
		return *target;
	}
	ReusableCursor& cursor_destination(DirectoryCursor& destination) {
		auto target = dynamic_cast<ReusableCursor*>(&destination);
		if (!target || target->owner != instance_ || !target->closed()) {
			throw std::invalid_argument("reusable cursor destination must be closed and belong to its backend");
		}
		return *target;
	}
	// Holds the directory lock, owner.lock, or both; releases the file first. It releases only
	// the claims it made, and a claim clears after its lock is gone, so a loser never frees
	// what the winner holds.
	class Owner : public OwnerLock {
	public:
		explicit Owner(PosixIO& io) : io_(io) {}
		~Owner() override {
			if (fd_ >= 0) {
				int expected = fd_;
				io_.owner_fd_.compare_exchange_strong(expected, -1);
				::close(fd_);
			}
			if (file_) { io_.file_claimed_.store(false); }
			if (directory_) { (void)::flock(io_.directory_, LOCK_UN); io_.directory_claimed_.store(false); }
		}
		bool claim_file() noexcept { file_ = !io_.file_claimed_.exchange(true); return file_; }
		bool claim_directory() noexcept { directory_ = !io_.directory_claimed_.exchange(true); return directory_; }
		void hold_file(int fd) noexcept { fd_ = fd; }
		int fd() const noexcept { return fd_; }
	private:
		PosixIO& io_;
		int fd_ = -1;
		bool file_ = false, directory_ = false;
	};
	// Claim the held flags atomically, then the directory lock, then owner.lock. A failed
	// step unwinds only what this call claimed or took, through the Owner's destructor.
	std::unique_ptr<OwnerLock> lock_owner(const OwnerLockOptions& options) override {
		const bool directory = options.mode != OwnerLockMode::file;
		const bool file = options.mode != OwnerLockMode::directory;
		auto owner = control<Owner>(*this);
		if (file && !owner->claim_file()) { throw std::logic_error("journal directory already owned"); }
		if (directory && !owner->claim_directory()) { throw std::logic_error("journal directory already locked"); }
		if (directory) { lock_exclusive(directory_, "lock journal directory", "directories"); }
		if (file) {
			const bool create = options.owner_file == OwnerFile::create_exclusive;
			owner->hold_file(open_file("owner.lock", create));
			lock_exclusive(owner->fd(), "lock journal owner", "files");
			owner_fd_.store(owner->fd());
			if (create) { detail::file_sync(owner->fd(), sync_calls_, SyncSite::file); }
		}
		return owner;
	}
	// Exclusive and nonblocking, retried on EINTR. Busy is EWOULDBLOCK; ENOTSUP and
	// EOPNOTSUPP are errc::not_supported, naming the filesystem.
	void lock_exclusive(int fd, const char* operation, const char* what) const {
		int result;
		do { result = flock_(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
		if (result == 0) { return; }
		const int error = errno == EAGAIN ? EWOULDBLOCK : errno;
		if (error == EOPNOTSUPP || error == ENOTSUP) {
			std::string name = "this filesystem";
			try { name = directory_filesystem().name; } catch (const std::system_error&) {}
			throw std::system_error(std::make_error_code(std::errc::not_supported),
				std::string(operation) + ": " + name + " does not lock " + what);
		}
		throw std::system_error(error, std::generic_category(), operation);
	}
	int open_file(std::string_view name, bool create, bool read_only = false) {
		detail::PosixName component(name);
		int flags = (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
		if (create) { flags |= O_CREAT | O_EXCL; }
		int fd;
		do { fd = ::openat(directory_, component.c_str(), flags, 0600); } while (fd < 0 && errno == EINTR);
		if (fd < 0) { detail::system_failure(create ? "create journal file" : "open existing journal file"); }
		try {
			// Artifact readers may run under a different UID in a system service.
			// Apply the selected read-only sharing mode before Create can succeed;
			// the stable owner lock always remains private, regardless of umask.
			if (create) {
				auto mode = name == "owner.lock" ? 0600u : artifact_mode_;
				int result;
				do { result = ::fchmod(fd, mode); } while (result < 0 && errno == EINTR);
				if (result < 0) { detail::system_failure("set artifact permissions"); }
			}
			detail::validate_file(fd);
			struct stat status{};
			if (::fstat(fd, &status)) { detail::system_failure("stat journal device"); }
			if (status.st_dev != device_) { throw std::runtime_error("journal file must share its directory's device"); }
		}
		catch (...) { ::close(fd); throw; }
		return fd;
	}
	std::unique_ptr<File> file(std::string_view name, bool create, bool read_only = false) {
		auto result = control<detail::PosixFile>(-1, sync_calls_);
		result->install(open_file(name, create, read_only));
		return result;
	}
	template <class T, class... Args> std::unique_ptr<T> control(Args&&... args) {
		if (context_.managed()) {
			return std::unique_ptr<T>(new (context_) detail::ManagedPosixControl<T>(std::forward<Args>(args)...));
		}
		return std::make_unique<T>(std::forward<Args>(args)...);
	}
	const std::uint64_t instance_ = detail::next_posix_instance();
	kronuz::io::completion::AllocationContext context_;
	int directory_ = -1;
	std::atomic<int> owner_fd_{-1};
	std::atomic<bool> file_claimed_{false}, directory_claimed_{false};
	DirectoryTrust trust_;
	FlockCall flock_ = &::flock;
	detail::SyncCalls sync_calls_;
#ifdef KRONUZ_COMPLETION_STATFS
	StatfsCall statfs_ = nullptr;
#endif
#ifdef __APPLE__
	BarrierWarningCall barrier_warning_ = nullptr;
	void* barrier_warning_context_ = nullptr;
	BarrierStorageCall barrier_storage_ = &detail::barrier_storage;
	std::atomic<bool> barrier_noted_{false};
#endif
	dev_t device_{};
	unsigned artifact_mode_ = 0600;
};

inline std::shared_ptr<PosixIO> make_posix_io(kronuz::io::completion::AllocationContext context,
	const std::filesystem::path& directory, PosixIOOptions options, unsigned artifact_mode = 0600) {
	return std::allocate_shared<PosixIO>(kronuz::io::completion::OwnedAllocator<PosixIO>(context),
		context, directory, options, artifact_mode);
}

} // namespace kronuz::journal

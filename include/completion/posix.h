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

#pragma once

#include "io.h"
#include "allocation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <dirent.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#define KRONUZ_COMPLETION_STATFS 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/mount.h>
#define KRONUZ_COMPLETION_STATFS 1
#define KRONUZ_COMPLETION_MOUNT_STATFS 1
#endif
#include <unistd.h>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace kronuz::journal {

namespace detail {
// Concrete POSIX controls keep default unique_ptr deletion. Their allocation
// header retains the PMR owner until after the most-derived destructor runs.
template <class Base> class ManagedPosixControl final : public Base {
	using Context = kronuz::io::completion::AllocationContext;
	struct Header { Context context; };
	static constexpr std::size_t alignment() { return std::max(alignof(Header), alignof(ManagedPosixControl)); }
	static constexpr std::size_t prefix() { return (sizeof(Header) + alignment() - 1) / alignment() * alignment(); }
public:
	using Base::Base;
	static void* operator new(std::size_t bytes, const Context& context) {
		if (bytes != sizeof(ManagedPosixControl)) { throw std::bad_alloc(); }
		auto memory = context.resource()->allocate(prefix() + bytes, alignment());
		std::construct_at(static_cast<Header*>(memory), Header{context});
		return static_cast<char*>(memory) + prefix();
	}
	static void operator delete(void* pointer) noexcept {
		auto header = reinterpret_cast<Header*>(static_cast<char*>(pointer) - prefix());
		auto context = std::move(header->context);
		std::destroy_at(header);
		context.resource()->deallocate(header, prefix() + sizeof(ManagedPosixControl), alignment());
	}
	static void operator delete(void* pointer, const Context&) noexcept { operator delete(pointer); }
};
// Controls may outlive a backend; an address can be reused by a later backend.
inline std::uint64_t next_posix_instance() {
	static std::atomic<std::uint64_t> last{0};
	auto observed = last.load(std::memory_order_relaxed);
	for (;;) {
		if (observed == std::numeric_limits<std::uint64_t>::max()) {
			throw std::overflow_error("POSIX backend identity exhausted");
		}
		if (last.compare_exchange_weak(observed, observed + 1, std::memory_order_relaxed)) { return observed + 1; }
	}
}
[[noreturn]] inline void system_failure(const char* operation) {
	throw std::system_error(errno, std::generic_category(), operation);
}
inline void validate_name(std::string_view name, std::size_t maximum = 128) {
	if (name.empty() || name.size() > maximum || name == "." || name == ".." ||
		name.find('/') != name.npos || name.find('\0') != name.npos) {
		throw std::invalid_argument("invalid journal filename");
	}
}
class PosixName {
public:
	explicit PosixName(std::string_view name, std::size_t maximum = 128) {
		validate_name(name, std::min<std::size_t>(maximum, 255));
		std::copy(name.begin(), name.end(), bytes_.begin());
		bytes_[name.size()] = '\0';
	}
	const char* c_str() const noexcept { return bytes_.data(); }
private:
	std::array<char, 256> bytes_;
};
inline void validate_file(int fd) {
	struct stat status{};
	if (::fstat(fd, &status)) { system_failure("stat journal file"); }
	if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & 0022)) {
		throw std::runtime_error("journal file requires exclusive ownership and a singly linked regular inode");
	}
}
inline EntryInspection inspect_status(const struct stat& status) {
	if (status.st_size < 0 || status.st_blocks < 0 ||
		static_cast<std::uint64_t>(status.st_blocks) > std::numeric_limits<std::uint64_t>::max() / 512) {
		throw std::overflow_error("invalid storage footprint");
	}
	auto kind = S_ISREG(status.st_mode) ? EntryKind::Regular : (S_ISDIR(status.st_mode) ? EntryKind::Directory :
		(S_ISLNK(status.st_mode) ? EntryKind::Symlink : EntryKind::Other));
	return {{kind, static_cast<std::uint64_t>(status.st_size), static_cast<std::uint64_t>(status.st_blocks) * 512},
		static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino),
		static_cast<std::uint64_t>(status.st_uid), static_cast<std::uint64_t>(status.st_nlink),
		static_cast<std::uint32_t>(status.st_mode & 07777)};
}
// The sync system calls PosixIO issues; a test replaces them through PosixIO::replace_sync_calls
// to record the real call sequence on this platform.
using FsyncCall = int (*)(int fd);
#ifdef __APPLE__
using FcntlCall = int (*)(int fd, int command);
inline int default_fcntl(int fd, int command) { return ::fcntl(fd, command); }
// Errors a filesystem without F_BARRIERFSYNC returns: the vnode default is ENOTSUP and an
// unrecognised command is ENOTTY (ENOTTY observed on APFS, FAT and exFAT volumes).
inline bool barrier_unsupported(int error) { return error == ENOTSUP || error == EOPNOTSUPP || error == ENOTTY; }
#endif
#ifdef __linux__
using WritebackCall = int (*)(int fd);
inline int default_writeback(int fd) { return ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE); }
#endif
struct SyncCalls {
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
	WritebackCall writeback_call = &default_writeback;
	int writeback(int fd) const { return writeback_call(fd); }
#endif
};
inline void file_sync(int fd) {
	int result;
#ifdef __APPLE__
	do { result = ::fcntl(fd, F_FULLFSYNC); } while (result < 0 && errno == EINTR);
#else
	do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
#endif
	if (result < 0) { system_failure("durably sync journal file"); }
}
// fdatasync on Linux; F_FULLFSYNC on macOS, whose fdatasync leaves data in the drive's
// cache; fsync elsewhere.
inline void file_sync_data(int fd) {
#ifdef __linux__
	int result;
	do { result = ::fdatasync(fd); } while (result < 0 && errno == EINTR);
	if (result < 0) { system_failure("durably sync journal data"); }
#else
	file_sync(fd);
#endif
}
// How files_sync_devices leaves the device: durable (a full flush per distinct device), or
// ordered on the directory's device (see PosixIO::order_files_and_directory).
enum class SyncMode { durable, ordered };
// Stages every file (and `directory`, when >= 0) with fsync, then flushes each distinct
// device once. Ordered mode issues a barrier, not a full flush, on the directory's device;
// every other device is still flushed in full.
// `directory_flush` is the descriptor a flush of the directory's device goes through when
// no file shares it (the owner file when held, else the directory). True when the directory's
// device was ordered by a barrier that did not flush (a barrier returning 0).
template <class Calls> inline bool files_sync_devices(std::span<const int> fds, int directory, int directory_flush, SyncMode mode, Calls& calls) {
	if (fds.empty() && directory < 0) { return false; }
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
	for (int fd : fds) { discover(fd, fd, false); }
	if (directory >= 0) { discover(directory, directory_flush, true); }
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
		} else {
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
// Linux f_type: an allow-list of known-local filesystems; the others listed are only named.
inline FilesystemClass linux_filesystem(std::uint32_t magic) {
	struct Known { std::uint32_t magic; const char* name; bool local; };
	static constexpr Known known[] = {
		{0xEF53, "ext2/ext3/ext4", true}, {0x58465342, "xfs", true}, {0x9123683E, "btrfs", true},
		{0x01021994, "tmpfs", true}, {0xF2F52010, "f2fs", true}, {0x2FC12FC1, "zfs", true},
		{0xCA451A4E, "bcachefs", true}, {0x794C7630, "overlayfs", true},
		{0x6969, "nfs", false}, {0x517B, "smb", false}, {0xFF534D42, "cifs", false},
		{0xFE534D42, "smb2", false}, {0x65735546, "fuse", false}, {0x01021997, "9p", false},
		{0x00C36400, "ceph", false}, {0x5346414F, "afs", false}, {0x6B414653, "kafs", false},
		{0x01161970, "gfs2", false}, {0x7461636F, "ocfs2", false}, {0x0BD00BD0, "lustre", false},
		{0x858458F6, "ramfs", false}, {0x4D44, "vfat", false}, {0x2011BAB0, "exfat", false},
		{0x5346544E, "ntfs", false}, {0xF15F, "ecryptfs", false}, {0x73717368, "squashfs", false},
	};
	for (const auto& type : known) {
		if (type.magic == magic) { return {type.local, type.name}; }
	}
	char hex[16];
	std::snprintf(hex, sizeof hex, "0x%X", static_cast<unsigned>(magic));
	return {false, std::string("unknown type ") + hex};
}
// macOS and the BSDs: the kernel's MNT_LOCAL.
inline FilesystemClass mount_filesystem(bool mnt_local, std::string_view type) {
	return {mnt_local, type.empty() ? std::string("unnamed type") : std::string(type)};
}
} // namespace detail

#if defined(__linux__)
inline FilesystemClass classify_filesystem(const struct statfs& status) {
	return detail::linux_filesystem(static_cast<std::uint32_t>(status.f_type));
}
#elif defined(KRONUZ_COMPLETION_MOUNT_STATFS)
inline FilesystemClass classify_filesystem(const struct statfs& status) {
	const auto* name = status.f_fstypename;
	return detail::mount_filesystem((status.f_flags & MNT_LOCAL) != 0,
		std::string_view(name, ::strnlen(name, sizeof status.f_fstypename)));
}
#endif

// What lies under a volume, for the barrier warning: Apple guarantees F_BARRIERFSYNC's order
// only on its SSDs and documents nothing about the call on other storage.
struct BarrierStorage {
	bool apple_ssd = false;  // every storage device under the volume is an internal Apple SSD
	std::string name;        // what was found, e.g. "APPLE SSD AP1024M (PCI-Express, Internal)"
	bool operator==(const BarrierStorage&) const = default;
};
// A volume where an ordering barrier returned success on storage other than an internal Apple
// SSD: acknowledged commits stay durable through their final flush, but the barrier's order is
// not guaranteed (docs/mechanisms/barriers.md).
struct BarrierWarning {
	std::uint64_t device = 0;  // the volume's st_dev; each is reported once per process
	std::string volume;        // its mount point
	std::string source;        // what it is mounted from (/dev/disk4s1, //host/share)
	BarrierStorage storage;
};

#ifdef __APPLE__
namespace detail {
// IOKit and CoreFoundation, loaded at the first classification so consumers link nothing more.
// The types are the C ABI's: CFTypeRef, io_object_t (a mach port name), CFTypeID, CFIndex.
class AppleRegistry {
public:
	static const AppleRegistry& get() {
		static const AppleRegistry registry;
		return registry;
	}
	// Every entry above the BSD device `name` in the service plane that carries "Device
	// Characteristics": the volume is an Apple SSD when each one is "APPLE SSD ..." and Internal.
	BarrierStorage storage(const char* name) const {
		if (!loaded_) { return {false, "unidentified (IOKit unavailable)"}; }
		auto matching = bsd_name_matching_(0, 0, name);
		if (!matching) { return {false, std::string("unidentified (no matching for ") + name + ")"}; }
		const Object media = matching_service_(0, matching);  // consumes `matching`
		if (!media) { return {false, std::string("unidentified (no registry entry for ") + name + ")"}; }
		Object parents = 0;
		if (create_iterator_(media, "IOService", recursive_parents, &parents) != 0) {
			object_release_(media);
			return {false, std::string("unidentified (no parents for ") + name + ")"};
		}
		const Key device_key(*this, "Device Characteristics"), product_key(*this, "Product Name"),
			protocol_key(*this, "Protocol Characteristics"), interconnect_key(*this, "Physical Interconnect"),
			location_key(*this, "Physical Interconnect Location");
		BarrierStorage result{true, {}};
		bool found = false;
		while (const Object entry = iterator_next_(parents)) {
			if (const Ref device = property(entry, device_key)) {
				const Ref protocol = property(entry, protocol_key);
				auto product = text(device, product_key), interconnect = text(protocol, interconnect_key),
					location = text(protocol, location_key);
				result.apple_ssd &= product.starts_with("APPLE SSD ") && location == "Internal";
				if (found) { result.name += " + "; }
				result.name += (product.empty() ? "unnamed device" : product) + " (" +
					(interconnect.empty() ? "unknown interconnect" : interconnect) + ", " + (location.empty() ? "unknown location" : location) + ")";
				found = true;
				if (protocol) { release_(protocol); }
				release_(device);
			}
			object_release_(entry);
		}
		object_release_(parents);
		object_release_(media);
		if (!found) { return {false, std::string("unidentified (no storage device under ") + name + ")"}; }
		return result;
	}
	AppleRegistry(const AppleRegistry&) = delete;
	AppleRegistry& operator=(const AppleRegistry&) = delete;
private:
	using Ref = const void*;
	using Object = unsigned int;
	static constexpr std::uint32_t utf8 = 0x08000100;          // kCFStringEncodingUTF8
	static constexpr std::uint32_t recursive_parents = 1 | 2;  // kIORegistryIterateRecursively | kIORegistryIterateParents
	class Key {
	public:
		Key(const AppleRegistry& registry, const char* text) : registry_(registry), ref_(registry.string_(nullptr, text, utf8)) {}
		~Key() { if (ref_) { registry_.release_(ref_); } }
		Key(const Key&) = delete;
		Key& operator=(const Key&) = delete;
		Ref get() const noexcept { return ref_; }
	private:
		const AppleRegistry& registry_;
		Ref ref_;
	};
	AppleRegistry() {
		void* iokit = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
		void* cf = ::dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
		if (!iokit || !cf) { return; }
		loaded_ = bind(iokit, "IOBSDNameMatching", bsd_name_matching_) & bind(iokit, "IOServiceGetMatchingService", matching_service_) &
			bind(iokit, "IORegistryEntryCreateIterator", create_iterator_) & bind(iokit, "IOIteratorNext", iterator_next_) &
			bind(iokit, "IORegistryEntryCreateCFProperty", create_property_) & bind(iokit, "IOObjectRelease", object_release_) &
			bind(cf, "CFStringCreateWithCString", string_) & bind(cf, "CFRelease", release_) & bind(cf, "CFGetTypeID", type_of_) &
			bind(cf, "CFStringGetTypeID", string_type_) & bind(cf, "CFDictionaryGetTypeID", dictionary_type_) &
			bind(cf, "CFDictionaryGetValue", dictionary_value_) & bind(cf, "CFStringGetCString", string_bytes_);
		if (loaded_) {
			// Every key the walk reads must exist, or nothing is classified.
			const Key probe(*this, "Device Characteristics");
			loaded_ = probe.get() != nullptr;
		}
	}
	template <class F> static bool bind(void* library, const char* symbol, F& function) {
		function = reinterpret_cast<F>(::dlsym(library, symbol));
		return function != nullptr;
	}
	Ref property(Object entry, const Key& key) const {
		const Ref value = create_property_(entry, key.get(), nullptr, 0);
		if (value && type_of_(value) != dictionary_type_()) { release_(value); return nullptr; }
		return value;
	}
	// A dictionary's string value, without the padding drives report; empty when absent.
	std::string text(Ref dictionary, const Key& key) const {
		if (!dictionary) { return {}; }
		const Ref value = dictionary_value_(dictionary, key.get());
		char bytes[256] = {};
		if (!value || type_of_(value) != string_type_() || !string_bytes_(value, bytes, sizeof bytes, utf8)) { return {}; }
		std::string_view view(bytes);
		const auto first = view.find_first_not_of(' ');
		if (first == view.npos) { return {}; }
		view = view.substr(first, view.find_last_not_of(' ') - first + 1);
		return std::string(view);
	}
	bool loaded_ = false;
	Ref (*bsd_name_matching_)(Object, std::uint32_t, const char*) = nullptr;
	Object (*matching_service_)(Object, Ref) = nullptr;
	int (*create_iterator_)(Object, const char*, std::uint32_t, Object*) = nullptr;
	Object (*iterator_next_)(Object) = nullptr;
	Ref (*create_property_)(Object, Ref, Ref, std::uint32_t) = nullptr;
	int (*object_release_)(Object) = nullptr;
	Ref (*string_)(Ref, const char*, std::uint32_t) = nullptr;
	void (*release_)(Ref) = nullptr;
	unsigned long (*type_of_)(Ref) = nullptr;
	unsigned long (*string_type_)() = nullptr;
	unsigned long (*dictionary_type_)() = nullptr;
	Ref (*dictionary_value_)(Ref, Ref) = nullptr;
	unsigned char (*string_bytes_)(Ref, char*, long, std::uint32_t) = nullptr;
};
// The storage under a mounted volume: IOKit's devices for a /dev/disk mount, else unidentified.
inline BarrierStorage barrier_storage(const struct statfs& volume) {
	const std::string_view source(volume.f_mntfromname, ::strnlen(volume.f_mntfromname, sizeof volume.f_mntfromname));
	constexpr std::string_view disk = "/dev/";
	if (!source.starts_with(disk)) {
		const std::string_view type(volume.f_fstypename, ::strnlen(volume.f_fstypename, sizeof volume.f_fstypename));
		return {false, "unidentified (" + std::string(type) + " volume, not a disk)"};
	}
	return AppleRegistry::get().storage(std::string(source.substr(disk.size())).c_str());
}
// The volumes this process has classified for the barrier warning, each once.
struct BarrierVolumes {
	std::mutex mutex;
	std::vector<std::uint64_t> devices;
};
inline BarrierVolumes& barrier_volumes() {
	static BarrierVolumes volumes;
	return volumes;
}
} // namespace detail
#endif

namespace detail {
inline off_t checked_offset(std::uint64_t offset) {
	if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
		throw std::length_error("journal offset exceeds platform range");
	}
	return static_cast<off_t>(offset);
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
		return inspect_status(status);
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
private:
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

// How strictly PosixIO trusts the directory it is given. The defaults refuse; either option
// admits what it names and PosixIO::directory_trust() reports what was found.
struct PosixIOOptions {
	bool follow_root_symlink = false;     // open through a final-component symlink; the descriptor is then held
	bool allow_shared_directory = false;  // admit a foreign owner and group/other-writable mode
	// macOS: order_files_and_directory flushes in full (F_FULLFSYNC) instead of issuing
	// F_BARRIERFSYNC, whose ordering Apple guarantees only on Apple SSDs. Other platforms
	// already flush in full.
	bool full_flush_ordering = false;
	// macOS: called once per volume per process, after the first barrier that ordered without
	// flushing on storage other than an internal Apple SSD, on the thread of that ordering call.
	// An exception fails the call after its barrier, and the volume warns again. Never called
	// elsewhere or with full_flush_ordering.
	std::function<void(const BarrierWarning&)> barrier_warning;
};

class PosixIO final : public IO {
public:
	// The application establishes and durably publishes this directory first.
	// Trusted ancestors are a caller precondition. By default a final-component
	// symlink, a foreign owner, and a group/other-writable mode are rejected here;
	// `options` admits them (see directory_trust to warn). Entries stay no-follow.
	explicit PosixIO(const std::filesystem::path& directory, unsigned artifact_mode = 0600, PosixIOOptions options = {})
		: PosixIO(kronuz::io::completion::AllocationContext{}, directory, artifact_mode, std::move(options)) {}
	PosixIO(kronuz::io::completion::AllocationContext context, const std::filesystem::path& directory,
		unsigned artifact_mode = 0600, PosixIOOptions options = {})
		: context_(std::move(context)),
#ifdef __APPLE__
		  barrier_warning_(std::move(options.barrier_warning)),
#endif
		  artifact_mode_(artifact_mode), full_flush_ordering_(options.full_flush_ordering) {
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
	// call. Call before any sync. macOS: fsync and fcntl (F_FULLFSYNC, F_BARRIERFSYNC); Linux:
	// fsync and the sync_file_range writeback hint; elsewhere: fsync.
#ifdef __APPLE__
	void replace_sync_calls(detail::FsyncCall fsync_call, detail::FcntlCall fcntl_call) noexcept {
		sync_calls_.fsync_call = fsync_call ? fsync_call : &::fsync;
		sync_calls_.fcntl_call = fcntl_call ? fcntl_call : &detail::default_fcntl;
	}
#elif defined(__linux__)
	void replace_sync_calls(detail::FsyncCall fsync_call, detail::WritebackCall writeback_call) noexcept {
		sync_calls_.fsync_call = fsync_call ? fsync_call : &::fsync;
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
	// macOS: fsync everything, then one barrier on the directory's device (full flushes on any
	// other device the files sit on); the sync_directory that follows the publication flushes.
	// With PosixIOOptions::full_flush_ordering the barrier is a full flush.
	void order_files_and_directory(std::span<File* const> files) override {
#ifdef __APPLE__
		const bool ordered_only = sync_staged(files, full_flush_ordering_ ? detail::SyncMode::durable : detail::SyncMode::ordered);
		if (ordered_only && barrier_warning_ && !barrier_noted_.load(std::memory_order_relaxed)) { note_barrier_volume(); }
#else
		sync_files_and_directory(files);
#endif
	}
	void sync_directory() override {
		int result;
		do { result = sync_calls_.sync(directory_); } while (result < 0 && errno == EINTR);
		if (result < 0) { detail::system_failure("sync journal directory"); }
#ifdef __APPLE__
		// Directory fsync establishes namespace ordering; fullsync then asks the storage
		// device to flush its cache, through the owner file when held, else the directory.
		const int owner = owner_fd_.load();
		do { result = sync_calls_.full_sync(owner >= 0 ? owner : directory_); } while (result < 0 && errno == EINTR);
		if (result < 0) { detail::system_failure("durably sync journal file"); }
#endif
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
			if (!warning.storage.apple_ssd) { barrier_warning_(warning); }
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
		explicit ReusableFile(const PosixIO& io) : detail::PosixFile(-1), owner(io.instance_) {}
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
			if (create) { detail::file_sync(owner->fd()); }
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
		auto result = control<detail::PosixFile>(-1);
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
	std::function<void(const BarrierWarning&)> barrier_warning_;
	BarrierStorageCall barrier_storage_ = &detail::barrier_storage;
	std::atomic<bool> barrier_noted_{false};
#endif
	dev_t device_{};
	unsigned artifact_mode_ = 0600;
	bool full_flush_ordering_ = false;
};

inline std::shared_ptr<PosixIO> make_posix_io(kronuz::io::completion::AllocationContext context,
	const std::filesystem::path& directory, unsigned artifact_mode = 0600, PosixIOOptions options = {}) {
	return std::allocate_shared<PosixIO>(kronuz::io::completion::OwnedAllocator<PosixIO>(context),
		context, directory, artifact_mode, std::move(options));
}

} // namespace kronuz::journal

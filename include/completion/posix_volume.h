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

// What lies under the directory: the filesystem class an owner-lock caller warns on, and on
// macOS the storage under the volume (IOKit) for the barrier warning.

#pragma once

#include "io.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#if defined(__linux__)
#include <sys/vfs.h>
#define KRONUZ_COMPLETION_STATFS 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/mount.h>
#define KRONUZ_COMPLETION_STATFS 1
#define KRONUZ_COMPLETION_MOUNT_STATFS 1
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace kronuz::journal {

namespace detail {
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
// The host's barrier warning: a plain function and its context keep PosixIOOptions a literal type.
using BarrierWarningCall = void (*)(void* context, const BarrierWarning& warning);

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
	static auto& volumes = *new BarrierVolumes;  // leaked: no exit-time destructor
	return volumes;
}
} // namespace detail
#endif

} // namespace kronuz::journal

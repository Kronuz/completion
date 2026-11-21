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

// What every piece of PosixIO shares: the managed control allocation, the backend identity,
// errno failures, name and file validation, and stat inspection.

#pragma once

#include "io.h"
#include "allocation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

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
} // namespace detail

} // namespace kronuz::journal

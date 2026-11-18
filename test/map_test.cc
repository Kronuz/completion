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

// The File and IO members no primitive drives. File::map: a whole file read-only; the view
// outlives its handle and the file's removal; an empty file maps to no bytes. The faultlab seam
// (F8): preallocate reserves, advise is a hint that never fails, map_shared's stores reach the
// file, link never replaces, inspect reports an extended ACL, and create_exclusive applies the
// IO's mode exactly. Each default holds for a backend that overrides nothing.
#include "completion/posix.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__APPLE__)
#include <membership.h>
#endif

namespace j = kronuz::journal;

namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}
template <class Action> int error_of(Action action) {
	try {
		action();
	} catch (const std::system_error &error) {
		return error.code().value();
	}
	return 0;
}
class Plain final : public j::File {
  public:
	std::uint64_t size() override { return size_; }
	std::size_t read_at(std::uint64_t, std::span<char>) override { return 0; }
	std::size_t write_at(std::uint64_t, std::string_view) override { return 0; }
	void truncate(std::uint64_t size) override { size_ = size; }
	void sync() override {}

  private:
	std::uint64_t size_ = 0;
};
// An IO that overrides only what it must.
class Bare final : public j::IO {
  public:
	std::unique_ptr<j::OwnerLock> acquire_owner(bool) override { return nullptr; }
	void open_existing_read_only_into(std::string_view, j::File &) override {}
	bool open_reclaim_candidate_read_only_into(std::string_view, j::File &) override { return false; }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view) override { return nullptr; }
	std::unique_ptr<j::File> open_existing(std::string_view) override { return nullptr; }
	std::unique_ptr<j::File> open_existing_read_only(std::string_view) override { return nullptr; }
	std::unique_ptr<j::File> create_exclusive(std::string_view) override { return nullptr; }
	void replace(std::string_view, std::string_view) override {}
	void remove(std::string_view) override {}
	void sync_directory() override {}
};

// The signatures F8 states.
static_assert(std::is_same_v<decltype(&j::File::preallocate), void (j::File::*)(std::uint64_t, std::uint64_t)>);
static_assert(std::is_same_v<decltype(&j::File::advise), void (j::File::*)(std::uint64_t, std::uint64_t, j::Advice) noexcept>);
static_assert(std::is_same_v<decltype(&j::File::map_shared), std::shared_ptr<j::SharedMapping> (j::File::*)(std::uint64_t)>);
static_assert(std::is_same_v<decltype(&j::IO::link), void (j::IO::*)(std::string_view, std::string_view)>);
static_assert(std::is_base_of_v<j::Mapping, j::SharedMapping>);
static_assert(std::is_same_v<decltype(std::declval<const j::SharedMapping &>().words()), std::span<char>>);
static_assert(std::is_same_v<decltype(j::EntryInspection{}.extended_acl), bool>);

std::string contents(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void defaults() {
	Plain file;
	file.preallocate(10, 20);
	check(file.size() == 30, "the default preallocate grows the size to offset + length");
	file.preallocate(0, 5);
	check(file.size() == 30, "the default preallocate never shrinks");
	bool refused = false;
	try {
		file.preallocate(~std::uint64_t{0}, 2);
	} catch (const std::length_error &) {
		refused = true;
	}
	check(refused && file.size() == 30, "an overflowing range is refused");
	file.advise(0, 30, j::Advice::WillNeed);
	check(file.size() == 30, "the default advise does nothing");
	check(file.map_shared(30) == nullptr, "the default map_shared is null");
	check(Plain().map() == nullptr, "a backend without mapping returns null");
	check(!j::EntryInspection{}.extended_acl, "an inspection reports no extended ACL by default");
	Bare io;
	check(error_of([&] { io.link("a", "b"); }) == int(std::errc::not_supported), "the default link is not_supported");
}

// Whether the filesystem reserves space; preallocate only grows the size where it does not.
bool reserves(int fd) {
#if defined(__APPLE__)
	fstore_t store{F_ALLOCATEALL, F_PEOFPOSMODE, 0, 4096, 0};
	return ::fcntl(fd, F_PREALLOCATE, &store) == 0 || !j::detail::reservation_unsupported(errno);
#elif defined(__linux__)
	return ::fallocate(fd, 0, 0, 4096) == 0 || !j::detail::reservation_unsupported(errno);
#else
	const int error = ::posix_fallocate(fd, 0, 4096);
	return error == 0 || !j::detail::reservation_unsupported(error);
#endif
}

void preallocation(j::PosixIO &io) {
	constexpr std::uint64_t length = 4 << 20;
	bool reserving;
	{
		auto probe = io.create_exclusive("reserve-probe");
		reserving = reserves(static_cast<j::detail::PosixFile &>(*probe).native_handle());
	}
	io.remove("reserve-probe");
	auto file = io.create_exclusive("preallocated");
	check(file->write_at(0, "abc") == 3, "write before preallocating");
	file->preallocate(0, length);
	check(file->size() == length, "preallocate grows the size to offset + length");
	std::string head(8, '?'), tail(8, '?');
	check(file->read_at(0, head) == 8 && head == std::string("abc\0\0\0\0\0", 8), "the bytes stay and the growth reads zeros");
	check(file->read_at(length - 8, tail) == 8 && tail == std::string(8, '\0'), "the growth is zero-filled");
	const auto allocated = file->inspect().footprint.allocated_bytes.value_or(0);
	if (reserving) {
		check(allocated >= length, "preallocate reserves the range");
	} else {
		std::cout << "completion_map_test: this filesystem cannot reserve; preallocate grew the size only\n";
	}
	file->preallocate(16, 16);
	check(file->size() == length, "preallocate inside the file keeps its size");
	file->preallocate(length, 4096);
	check(file->size() == length + 4096, "preallocate past the end grows again");
	check(file->write_at(length + 4000, std::string(96, 'z')) == 96, "a write in the preallocated range");
	// A sparse file: the holes below the size are reserved too.
	auto sparse = io.create_exclusive("sparse");
	sparse->truncate(length);
	check(sparse->write_at(length / 2, "mid") == 3, "write inside the sparse file");
	sparse->preallocate(0, 2 * length);
	check(sparse->size() == 2 * length, "preallocate grows the sparse file");
	std::string middle(3, '?'), zero(4, '?');
	check(sparse->read_at(length / 2, middle) == 3 && middle == "mid", "filling the holes keeps the data");
	check(sparse->read_at(4096, zero) == 4 && zero == std::string(4, '\0'), "a filled hole reads zeros");
	if (reserving) {
		check(sparse->inspect().footprint.allocated_bytes.value_or(0) >= 2 * length, "preallocate reserves the holes below the size");
	}
}

// Pages of the mapping in memory.
std::size_t resident(const j::Mapping &mapped) {
	const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
	const auto bytes = mapped.bytes();
#if defined(__linux__)
	std::vector<unsigned char> pages((bytes.size() + page - 1) / page);
#else
	std::vector<char> pages((bytes.size() + page - 1) / page);
#endif
	if (::mincore(const_cast<char *>(bytes.data()), bytes.size(), pages.data())) { j::detail::system_failure("mincore"); }
	std::size_t count = 0;
	for (auto state : pages) { count += (state & 1) ? 1 : 0; }
	return count;
}
template <class Done> bool eventually(Done done) {
	for (int i = 0; i < 200; ++i) {
		if (done()) { return true; }
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done();
}

void advice(j::PosixIO &io) {
	const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
	const std::size_t length = 64 * page;
	auto file = io.create_exclusive("advised");
	const int fd = static_cast<j::detail::PosixFile &>(*file).native_handle();
	// Write past the page cache where the platform can, so the file starts out of memory.
	void *buffer = nullptr;
	check(::posix_memalign(&buffer, page, length) == 0, "aligned buffer");
	std::memset(buffer, 'w', length);
#if defined(__APPLE__)
	(void)::fcntl(fd, F_NOCACHE, 1);
#endif
	check(file->write_at(0, std::string_view(static_cast<const char *>(buffer), length)) == length, "write the advised file");
	std::free(buffer);
	file->sync();
#if defined(__APPLE__)
	(void)::fcntl(fd, F_NOCACHE, 0);
#elif defined(__linux__) || defined(__FreeBSD__)
	(void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
	auto mapped = file->map();
	if (resident(*mapped) == 0) {
		file->advise(0, length, j::Advice::WillNeed);
		check(eventually([&] { return resident(*mapped) > 0; }), "WillNeed reads the range in");
#if defined(__linux__)
		file->advise(0, length, j::Advice::DontNeed);
		check(eventually([&] { return resident(*mapped) == 0; }), "DontNeed drops the clean range");
#endif
	} else {
		std::cout << "completion_map_test: the file stayed in memory; advise's effect is not observed here\n";
	}
	// A hint never fails: every advice, any range, a read-only handle, a closed control.
	for (auto each : {j::Advice::Normal, j::Advice::Sequential, j::Advice::Random, j::Advice::WillNeed, j::Advice::DontNeed,
		  static_cast<j::Advice>(99)}) {
		file->advise(0, length, each);
		file->advise(~std::uint64_t{0}, ~std::uint64_t{0}, each);
		file->advise(length * 2, 0, each);
	}
	auto read_only = io.open_existing_read_only("advised");
	read_only->advise(0, length, j::Advice::WillNeed);
	read_only->advise(0, length, j::Advice::DontNeed);
	auto closed = io.make_closed_file();
	closed->advise(0, length, j::Advice::WillNeed);
	check(mapped->bytes() == std::string(length, 'w'), "advice never changes the bytes");
}

void shared_mapping(j::PosixIO &io, const std::filesystem::path &directory) {
	const std::size_t length = 3 * 4096 + 17;
	std::shared_ptr<j::SharedMapping> mapped;
	{
		auto file = io.create_exclusive("shared");
		file->truncate(length);
		mapped = file->map_shared(length);
		check(mapped && mapped->words().size() == length && mapped->bytes().size() == length, "map_shared views the range");
		for (std::size_t i = 0; i < length; ++i) { mapped->words()[i] = char('A' + i % 23); }
		std::string read(length, '?');
		check(file->read_at(0, read) == length && read == std::string(mapped->bytes()), "stores reach the file without a write");
		file->sync();
		bool refused = false;
		try {
			(void)file->map_shared(length + 1);
		} catch (const std::invalid_argument &) {
			refused = true;
		}
		check(refused, "map_shared requires size() >= length");
		auto empty = file->map_shared(0);
		check(empty && empty->words().empty(), "an empty range maps to no bytes");
	}
	auto stored = std::string(mapped->bytes());
	check(contents(directory / "shared") == stored, "synced stores are the file's bytes after its handle closes");
	mapped->words()[0] = '!';
	stored[0] = '!';
	check(contents(directory / "shared") == stored, "the view outlives its handle and still reaches the file");
	mapped.reset();
	auto read_only = io.open_existing_read_only("shared");
	check(error_of([&] { (void)read_only->map_shared(length); }) == EACCES, "a read-only handle cannot map shared");
}

void linking(j::PosixIO &io, const std::filesystem::path &directory) {
	{
		auto a = io.create_exclusive("a");
		check(a->write_at(0, "alpha") == 5, "write a");
		auto b = io.create_exclusive("b");
		check(b->write_at(0, "beta") == 4, "write b");
	}
	io.link("a", "c");
	check(contents(directory / "a") == "alpha" && contents(directory / "c") == "alpha", "link names the source's file");
	const auto a = io.inspect_entry("a"), c = io.inspect_entry("c");
	check(a && c && a->inode == c->inode && c->links == 2, "both names reach one inode");
	check(error_of([&] { io.link("a", "b"); }) == EEXIST, "link refuses an existing destination with EEXIST");
	check(contents(directory / "b") == "beta", "the existing destination keeps its bytes");
	check(contents(directory / "a") == "alpha" && io.inspect_entry("a")->links == 2, "the source keeps its bytes and name");
	check(error_of([&] { io.link("missing", "d"); }) == ENOENT && !io.inspect_entry("d"), "a missing source links nothing");
	io.sync_directory();
}

// Adds an ACL entry granting this user read; false where the filesystem has no ACLs.
bool add_acl(int fd) {
#if defined(__APPLE__)
	acl_t acl = ::acl_init(1);
	acl_entry_t entry;
	acl_permset_t permissions;
	uuid_t user;
	bool added = acl && ::acl_create_entry(&acl, &entry) == 0 && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
		::mbr_uid_to_uuid(::geteuid(), user) == 0 && ::acl_set_qualifier(entry, user) == 0 &&
		::acl_get_permset(entry, &permissions) == 0 && ::acl_add_perm(permissions, ACL_READ_DATA) == 0 &&
		::acl_set_permset(entry, permissions) == 0;
	added = added && ::acl_set_fd_np(fd, acl, ACL_TYPE_EXTENDED) == 0;
	if (acl) { ::acl_free(acl); }
	return added;
#elif defined(__FreeBSD__)
	const std::string text = "user::rw-,user:" + std::to_string(::geteuid()) + ":r--,group::---,mask::r--,other::---";
	acl_t acl = ::acl_from_text(text.c_str());
	const bool added = acl && ::acl_set_fd_np(fd, acl, ACL_TYPE_ACCESS) == 0;
	if (acl) { ::acl_free(acl); }
	return added;
#elif defined(__linux__)
	// The kernel's POSIX ACL xattr: a version, then (tag, permissions, id) sorted by tag.
	struct Entry { std::uint16_t tag, permissions; std::uint32_t id; };
	const std::uint32_t none = ~std::uint32_t{0};
	struct { std::uint32_t version; Entry entries[5]; } acl{2, {{0x01, 6, none}, {0x02, 4, ::geteuid()},
		{0x04, 0, none}, {0x10, 4, none}, {0x20, 0, none}}};
	return ::fsetxattr(fd, "system.posix_acl_access", &acl, sizeof acl, 0) == 0;
#else
	(void)fd;
	return false;
#endif
}

void acl_and_mode(const std::filesystem::path &directory) {
	j::PosixIO io(directory, 0644);
	{
		auto file = io.create_exclusive("plain-acl");
		check(!file->inspect().extended_acl, "a fresh file carries no extended ACL");
		if (add_acl(static_cast<j::detail::PosixFile &>(*file).native_handle())) {
			check(file->inspect().extended_acl, "inspect reports an extended ACL");
			check(!io.inspect_entry("plain-acl")->extended_acl, "inspect_entry opens nothing and reports false");
		} else {
			std::cout << "completion_map_test: this filesystem takes no ACL; extended_acl's true case is not observed here\n";
		}
	}
	// The umask cannot narrow the IO's file mode.
	const auto saved = ::umask(077);
	auto created = io.create_exclusive("mode");
	auto reused = io.make_closed_file();
	io.create_exclusive_into("mode-into", *reused);
	::umask(saved);
	check(created->inspect().permissions == 0644, "create_exclusive applies the IO's mode exactly");
	check(reused->inspect().permissions == 0644, "create_exclusive_into applies the IO's mode exactly");
	check(io.inspect_entry("mode")->permissions == 0644, "the created entry carries the mode");
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_map_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path directory(argv[1]);
	std::filesystem::remove_all(directory);
	std::filesystem::create_directory(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	defaults();
	j::PosixIO io(directory);
	auto owner = io.acquire_owner(true);
	std::string bytes;
	for (int i = 0; i < 3 * 4096 + 17; ++i) { bytes.push_back(char('a' + i % 26)); }
	std::shared_ptr<const j::Mapping> mapped;
	{
		auto file = io.create_exclusive("m");
		check(file->write_at(0, bytes) == bytes.size(), "write");
		mapped = file->map();
		check(mapped && mapped->bytes() == bytes, "mapped bytes equal the file's");
	}
	io.remove("m");
	io.sync_directory();
	check(mapped->bytes() == bytes, "the view outlives its handle and the file");
	{
		auto empty = io.create_exclusive("e");
		auto none = empty->map();
		check(none && none->bytes().empty(), "an empty file maps to no bytes");
	}
	preallocation(io);
	advice(io);
	shared_mapping(io, directory);
	linking(io, directory);
	acl_and_mode(directory);
	std::cout << "completion_map_test: ok\n";
}

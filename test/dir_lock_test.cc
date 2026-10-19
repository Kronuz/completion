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

// The directory owner lock: busy pairs in one process and across processes write nothing,
// every filesystem locks and is classified for the caller to warn on, and the macOS barrier
// needs no owner file.
#include "completion/posix.h"
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <source_location>
#include <spawn.h>
#include <string>
#include <sys/wait.h>

extern char** environ;

namespace j = kronuz::journal;
namespace fs = std::filesystem;

namespace {
void check(bool value, const char* what, std::source_location where = std::source_location::current()) {
	if (!value) {
		std::cerr << "failed at line " << where.line() << ": " << what << '\n';
		std::exit(1);
	}
}

// Everything a writer could change: each entry's name and size, and the directory's mtime.
struct Snapshot {
	std::map<std::string, std::uintmax_t> entries;
	fs::file_time_type modified;
	bool operator==(const Snapshot&) const = default;
};
Snapshot snapshot(const fs::path& directory) {
	Snapshot result{{}, fs::last_write_time(directory)};
	for (const auto& entry : fs::directory_iterator(directory)) {
		result.entries[entry.path().filename().string()] = entry.is_regular_file() ? entry.file_size() : 0;
	}
	return result;
}
fs::path fresh(const fs::path& path) {
	fs::remove_all(path);
	fs::create_directory(path);
	fs::permissions(path, fs::perms::owner_all);
	return path;
}
bool busy(const std::system_error& error) {
	return error.code() == std::errc::operation_would_block || error.code() == std::errc::resource_unavailable_try_again;
}
// Busy, with nothing written.
void expect_busy(j::PosixIO& io, const fs::path& directory, const char* what) {
	const auto before = snapshot(directory);
	try {
		auto lock = io.acquire_directory_owner();
		check(false, what);
	} catch (const std::system_error& error) {
		check(busy(error), what);
	}
	check(snapshot(directory) == before, "a busy attempt writes nothing");
}

void classifier() {
	for (auto [magic, name] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
			{0xEF53, "ext2/ext3/ext4"}, {0x58465342, "xfs"}, {0x9123683E, "btrfs"}, {0x01021994, "tmpfs"},
			{0xF2F52010, "f2fs"}, {0x2FC12FC1, "zfs"}, {0xCA451A4E, "bcachefs"}, {0x794C7630, "overlayfs"}}) {
		check(j::detail::linux_filesystem(magic) == j::FilesystemClass{true, name}, "Linux allow-list");
	}
	for (auto [magic, name] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
			{0x6969, "nfs"}, {0x517B, "smb"}, {0xFF534D42, "cifs"}, {0xFE534D42, "smb2"}, {0x65735546, "fuse"},
			{0x01021997, "9p"}, {0x00C36400, "ceph"}, {0x858458F6, "ramfs"}}) {
		check(j::detail::linux_filesystem(magic) == j::FilesystemClass{false, name}, "Linux named non-local");
	}
	check(j::detail::linux_filesystem(0x12345678) == j::FilesystemClass{false, "unknown type 0x12345678"},
		"an unknown Linux type is non-local, named by its magic");
	check(j::detail::mount_filesystem(true, "apfs") == j::FilesystemClass{true, "apfs"}, "MNT_LOCAL is local");
	check(j::detail::mount_filesystem(true, "ufs").local && j::detail::mount_filesystem(true, "zfs").local, "UFS, ZFS");
	for (auto name : {"nfs", "smbfs", "afpfs", "webdav", "p9fs", "weirdfs"}) {
		check(j::detail::mount_filesystem(false, name) == j::FilesystemClass{false, name}, "no MNT_LOCAL is non-local");
	}
	for (auto name : {"fusefs", "fusefs.sshfs", "macfuse", "osxfuse"}) {
		check(j::detail::mount_filesystem(true, name) == j::FilesystemClass{true, name}, "FUSE is just MNT_LOCAL");
	}
	check(j::detail::mount_filesystem(false, "") == j::FilesystemClass{false, "unnamed type"}, "unnamed type");
}

#ifdef KRONUZ_COMPLETION_STATFS
struct statfs injected{};
int injected_errno = 0;
int inject(int, struct statfs* result) {
	if (injected_errno) { errno = injected_errno; return -1; }
	*result = injected;
	return 0;
}
struct Injection { const char* name; std::uint32_t magic; bool mnt_local; const char* type; };
void set_injection(const Injection& injection) {
	injected = {};
	injected_errno = 0;
#if defined(__linux__)
	injected.f_type = static_cast<decltype(injected.f_type)>(injection.magic);
#else
	injected.f_flags = injection.mnt_local ? MNT_LOCAL : 0;
	std::snprintf(injected.f_fstypename, sizeof injected.f_fstypename, "%s", injection.type);
#endif
}

// Each injected type locks, writes nothing, and is classified non-local by name.
void warnings(const fs::path& directory) {
	j::PosixIO io(fresh(directory));
	(void)io.create_exclusive("meta-1");
	io.replace_statfs(inject);
	const Injection foreign[] = {
		{"nfs", 0x6969, false, "nfs"},
		{"smb", 0xFE534D42, false, "smbfs"},
		{"fuse", 0x65735546, false, "fusefs"},
		{"9p", 0x01021997, false, "p9fs"},
		{"unknown", 0x0BADF00D, false, "weirdfs"},
	};
	for (const auto& injection : foreign) {
		set_injection(injection);
		const auto filesystem = io.directory_filesystem();
		check(!filesystem.local, "an injected filesystem is non-local");
#if defined(__linux__)
		const std::string expected = injection.magic == 0x0BADF00D ? "unknown type 0xBADF00D" : injection.name;
#else
		const std::string expected = injection.type;
#endif
		check(filesystem.name == expected, "the classification names the filesystem");
		const auto before = snapshot(directory);
		{
			auto lock = io.acquire_directory_owner();
			j::PosixIO other(directory);
			expect_busy(other, directory, "a non-local filesystem still excludes in this process");
		}
		check(snapshot(directory) == before, "locking writes nothing");
	}
	// FUSE reported as MNT_LOCAL is local off Linux, where the allow-list leaves it non-local.
#if !defined(__linux__)
	set_injection({"fuse", 0, true, "macfuse"});
	check(io.directory_filesystem() == j::FilesystemClass{true, "macfuse"}, "FUSE is just MNT_LOCAL");
#endif
	set_injection({"ext4", 0xEF53, true, "apfs"});
	check(io.directory_filesystem().local, "an injected local filesystem is local");
	{ auto lock = io.acquire_directory_owner(); }
	// The lock never inspects the filesystem; only the classification reports statfs's error.
	set_injection({"io error", 0, false, ""});
	injected_errno = EIO;
	{ auto lock = io.acquire_directory_owner(); }
	try {
		(void)io.directory_filesystem();
		check(false, "a failed statfs is reported");
	} catch (const std::system_error& error) {
		check(error.code() == std::error_code(EIO, std::generic_category()), "statfs's error survives");
	}
	io.replace_statfs(nullptr);
	check(io.directory_filesystem().local, "the real filesystem is classified");
	{ auto lock = io.acquire_directory_owner(); }
}
#endif

int enotsup_flock(int, int) {
	errno = ENOTSUP;
	return -1;
}

// A flock the filesystem does not support is not_supported, for the caller to decide on.
void unsupported(const fs::path& directory) {
	j::PosixIO io(fresh(directory));
	io.replace_flock(enotsup_flock);
	try {
		auto lock = io.acquire_directory_owner();
		check(false, "an unsupported flock fails");
	} catch (const std::system_error& error) {
		check(error.code() == std::errc::not_supported, "an unsupported flock is not_supported");
	}
	io.replace_flock(nullptr);
	auto lock = io.acquire_directory_owner();
}

void in_process(const fs::path& directory, const char* expect) {
#ifdef KRONUZ_COMPLETION_STATFS
	struct statfs status{};
	check(::statfs(fresh(directory).c_str(), &status) == 0, "statfs the state directory");
	const auto filesystem = j::classify_filesystem(status);
	std::cout << "dir_lock: state directory on " << filesystem.name << '\n';
	check(filesystem.local, "the test's own filesystem is known local");
	if (expect) { check(filesystem.name == expect, "the state directory is on the expected filesystem"); }
#endif
	// create then create: an empty directory, the first holder writing its first file.
	{
		j::PosixIO first(fresh(directory)), second(directory);
		auto lock = first.acquire_directory_owner();
		check(fs::is_empty(directory), "the lock writes nothing");
		expect_busy(second, directory, "create then create");
		(void)first.create_exclusive("meta-1");
		expect_busy(second, directory, "create then create, after the first write");
		try {
			(void)first.acquire_directory_owner();
			check(false, "a second lock on one IO");
		} catch (const std::logic_error&) {}
	}
	// create then open, and open then open: a populated directory.
	{
		j::PosixIO creator(directory), opener(directory), third(directory);
		auto created = creator.acquire_directory_owner();
		expect_busy(opener, directory, "create then open");
		created.reset();
		auto opened = opener.acquire_directory_owner();
		expect_busy(third, directory, "open then open");
		expect_busy(creator, directory, "open then reopen through the creator's IO");
		// The file lock is separate: one IO may hold both, and neither releases the other.
		auto file_owner = opener.acquire_owner(true);
		expect_busy(third, directory, "the file lock leaves the directory lock held");
		file_owner.reset();
		expect_busy(third, directory, "releasing the file lock keeps the directory lock");
		opened.reset();
		auto reacquired = third.acquire_directory_owner();
		check(fs::exists(directory / "owner.lock"), "the file lock still creates owner.lock");
	}
	// The barrier needs no owner file (on macOS F_FULLFSYNC runs on the directory).
	{
		j::PosixIO io(directory);
		io.sync_directory();
		auto lock = io.acquire_directory_owner();
		(void)io.create_exclusive("named");
		io.sync_directory();
	}
}

// Child modes: "try" exits 0 busy, 3 acquired, 4 otherwise; "hold" signals on stdout and
// holds until stdin closes.
int child(const std::string& mode, const fs::path& directory) {
	try {
		j::PosixIO io(directory);
		if (mode == "try") {
			try {
				auto lock = io.acquire_directory_owner();
				return 3;
			} catch (const std::system_error& error) {
				return busy(error) ? 0 : 4;
			}
		}
		auto lock = io.acquire_directory_owner();
		char byte = 'x';
		if (::write(1, &byte, 1) != 1) { return 5; }
		while (::read(0, &byte, 1) > 0) {}
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "child: " << error.what() << '\n';
		return 6;
	}
}
struct Child {
	pid_t pid = -1;
	int to = -1, from = -1;
};
Child spawn(const char* self, const char* mode, const fs::path& directory) {
	int in[2], out[2];
	check(::pipe(in) == 0 && ::pipe(out) == 0, "pipes");
	for (int fd : {in[0], in[1], out[0], out[1]}) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, in[0], 0);
	posix_spawn_file_actions_adddup2(&actions, out[1], 1);
	const std::string path = directory.string();
	char* argv[] = {const_cast<char*>(self), const_cast<char*>("--child"), const_cast<char*>(mode),
		const_cast<char*>(path.c_str()), nullptr};
	Child result;
	check(::posix_spawn(&result.pid, self, &actions, nullptr, argv, environ) == 0, "spawn");
	posix_spawn_file_actions_destroy(&actions);
	::close(in[0]);
	::close(out[1]);
	result.to = in[1];
	result.from = out[0];
	return result;
}
int reap(Child& child) {
	if (child.to >= 0) { ::close(child.to); }
	if (child.from >= 0) { ::close(child.from); }
	int status = 0;
	while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void cross_process(const char* self, const fs::path& directory) {
	j::PosixIO io(fresh(directory));
	// Another process tries while this one holds, before and after the first file.
	{
		auto lock = io.acquire_directory_owner();
		auto before = snapshot(directory);
		auto other = spawn(self, "try", directory);
		check(reap(other) == 0, "another process is busy (create then create)");
		check(snapshot(directory) == before, "the other process wrote nothing");
		(void)io.create_exclusive("meta-1");
		before = snapshot(directory);
		other = spawn(self, "try", directory);
		check(reap(other) == 0, "another process is busy (create then open)");
		check(snapshot(directory) == before, "the other process wrote nothing");
	}
	auto released = spawn(self, "try", directory);
	check(reap(released) == 3, "a released lock is free to another process");
	// Another process holds; this one is busy until it is killed.
	auto holder = spawn(self, "hold", directory);
	char byte = 0;
	check(::read(holder.from, &byte, 1) == 1, "the holder took the lock");
	expect_busy(io, directory, "open while another process holds");
	j::PosixIO fresh_io(directory);
	expect_busy(fresh_io, directory, "open then open across processes");
	// The lock follows the directory's inode across a rename.
	const auto renamed = directory.string() + ".renamed";
	fs::remove_all(renamed);
	fs::rename(directory, renamed);
	{
		j::PosixIO moved(renamed);
		expect_busy(moved, renamed, "the lock follows a renamed directory");
	}
	fs::rename(renamed, directory);
	::kill(holder.pid, SIGKILL);
	check(reap(holder) == 128 + SIGKILL, "the holder was killed");
	auto lock = io.acquire_directory_owner();
}
} // namespace

int main(int argc, char** argv) {
	if (argc == 4 && std::string(argv[1]) == "--child") { return child(argv[2], argv[3]); }
	if (argc != 2 && argc != 3) {
		std::cerr << "usage: completion_dir_lock_test DIRECTORY [EXPECTED-FILESYSTEM]\n";
		return 2;
	}
	const fs::path directory(argv[1]);
	classifier();
#ifdef KRONUZ_COMPLETION_STATFS
	warnings(directory);
#endif
	unsupported(directory);
	in_process(directory, argc == 3 ? argv[2] : nullptr);
	cross_process(argv[0], directory);
	fs::remove_all(directory);
	std::cout << "dir_lock: ok\n";
	return 0;
}

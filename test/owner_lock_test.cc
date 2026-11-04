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

// The one owner lock: every file/directory/both pairing, in both first-holder orders,
// through the legacy and options calls, across processes and through distinct and shared
// IO objects; then partial failure, EINTR, unsupported locks, create and recover, aliases,
// the lock's lifetime through an image job, and owner.lock and root refusals.
#include "completion/api.h"
#include "completion/image.h"
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <source_location>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace j = kronuz::journal;
namespace c = kronuz::io::completion;
namespace fs = std::filesystem;
using Mode = j::OwnerLockMode;

namespace {
void check(bool value, const char* what, std::source_location where = std::source_location::current()) {
	if (!value) {
		std::cerr << "failed at line " << where.line() << ": " << what << '\n';
		std::exit(1);
	}
}

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
// The lowest free descriptor: unchanged when a failed acquisition leaks nothing.
int lowest_fd() {
	int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
	check(fd >= 0, "open /dev/null");
	::close(fd);
	return fd;
}
template <class F> std::error_code error_of(F&& f) {
	try {
		f();
	} catch (const std::system_error& error) {
		return error.code();
	}
	return {};
}
template <class E, class F> bool throws(F&& f) {
	try {
		f();
	} catch (const E&) {
		return true;
	}
	return false;
}

// One way to take an owner lock: a mode through the legacy or the options call.
struct Holder {
	Mode mode;
	bool legacy;
	const char* name;
};
constexpr Holder holders[] = {
	{Mode::file, true, "file(bool)"}, {Mode::file, false, "file"},
	{Mode::directory, true, "directory()"}, {Mode::directory, false, "directory"},
	{Mode::both, false, "both"},
};
constexpr int holder_count = sizeof holders / sizeof holders[0];

// Creates owner.lock when it is absent, else opens it: the create and recover paths.
std::unique_ptr<j::OwnerLock> acquire(j::IO& io, const Holder& holder, const fs::path& directory) {
	const bool create = holder.mode != Mode::directory && !fs::exists(fs::symlink_status(directory / "owner.lock"));
	if (holder.legacy) { return holder.mode == Mode::file ? io.acquire_owner(create) : io.acquire_directory_owner(); }
	return io.acquire_owner(j::OwnerLockOptions{holder.mode,
		create ? j::OwnerFile::create_exclusive : j::OwnerFile::open_existing});
}
enum class Outcome { acquired, busy, refused, failed };
const char* name(Outcome outcome) {
	switch (outcome) {
	case Outcome::acquired: return "acquired";
	case Outcome::busy: return "busy";
	case Outcome::refused: return "refused";
	case Outcome::failed: return "failed";
	}
	return "?";
}
Outcome attempt(j::IO& io, const Holder& holder, const fs::path& directory) {
	try {
		auto lock = acquire(io, holder, directory);
		return lock ? Outcome::acquired : Outcome::failed;
	} catch (const std::logic_error&) {
		return Outcome::refused;
	} catch (const std::system_error& error) {
		if (!busy(error)) { std::cerr << "owner_lock: " << holder.name << ": " << error.what() << '\n'; }
		return busy(error) ? Outcome::busy : Outcome::failed;
	} catch (const std::exception& error) {
		std::cerr << "owner_lock: " << holder.name << ": " << error.what() << '\n';
		return Outcome::failed;
	}
}
// Only the legacy file/directory pairing fails to exclude: they lock different inodes.
bool excludes(Mode first, Mode second) {
	return !((first == Mode::file && second == Mode::directory) || (first == Mode::directory && second == Mode::file));
}

// Child modes: "try" exits 0 busy, 3 acquired, 5 refused, 4 otherwise; "hold" writes a byte
// once it holds and holds until stdin closes.
int child(const std::string& action, int index, const fs::path& directory) {
	try {
		j::PosixIO io(directory);
		const auto& holder = holders[index];
		if (action == "try") {
			switch (attempt(io, holder, directory)) {
			case Outcome::busy: return 0;
			case Outcome::acquired: return 3;
			case Outcome::refused: return 5;
			case Outcome::failed: return 4;
			}
		}
		auto lock = acquire(io, holder, directory);
		char byte = 'x';
		if (::write(1, &byte, 1) != 1) { return 6; }
		while (::read(0, &byte, 1) > 0) {}
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "child: " << error.what() << '\n';
		return 7;
	}
}
struct Child {
	pid_t pid = -1;
	int to = -1, from = -1;
};
Child spawn(const char* self, const char* action, int index, const fs::path& directory) {
	int in[2], out[2];
	check(::pipe(in) == 0 && ::pipe(out) == 0, "pipes");
	for (int fd : {in[0], in[1], out[0], out[1]}) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, in[0], 0);
	posix_spawn_file_actions_adddup2(&actions, out[1], 1);
	const std::string path = directory.string(), which = std::to_string(index);
	char* argv[] = {const_cast<char*>(self), const_cast<char*>("--child"), const_cast<char*>(action),
		const_cast<char*>(which.c_str()), const_cast<char*>(path.c_str()), nullptr};
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
	child.to = child.from = -1;
	int status = 0;
	while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
Outcome outcome_of(int code) {
	switch (code) {
	case 0: return Outcome::busy;
	case 3: return Outcome::acquired;
	case 5: return Outcome::refused;
	default: return Outcome::failed;
	}
}
Child hold(const char* self, int index, const fs::path& directory) {
	auto holder = spawn(self, "hold", index, directory);
	char byte = 0;
	check(::read(holder.from, &byte, 1) == 1, "the holder took its lock");
	return holder;
}

enum class Topology { processes, distinct, shared };
const char* name(Topology topology) {
	switch (topology) {
	case Topology::processes: return "separate processes";
	case Topology::distinct: return "distinct IO objects";
	case Topology::shared: return "one shared IO object";
	}
	return "?";
}

// Every ordered pairing in one topology. A competing owner is busy, or refused through the
// holder's own IO, and writes nothing; after the holder releases, it acquires.
void matrix(const char* self, const fs::path& directory, Topology topology) {
	std::cout << "owner_lock: " << name(topology) << " (rows hold first; x excludes, A acquires)\n  " << std::setw(12) << "";
	for (const auto& second : holders) { std::cout << ' ' << second.name; }
	std::cout << '\n';
	int excluded = 0, unsafe = 0;
	for (int a = 0; a < holder_count; ++a) {
		std::cout << "  " << std::left << std::setw(12) << holders[a].name << std::right;
		for (int b = 0; b < holder_count; ++b) {
			const auto& first = holders[a];
			const auto& second = holders[b];
			fresh(directory);
			const bool expect = excludes(first.mode, second.mode);
			const Outcome blocked = topology == Topology::shared ? Outcome::refused : Outcome::busy;
			Outcome got;
			if (topology == Topology::processes) {
				auto holder = hold(self, a, directory);
				const auto before = snapshot(directory);
				auto other = spawn(self, "try", b, directory);
				got = outcome_of(reap(other));
				if (got != Outcome::acquired) { check(snapshot(directory) == before, "a competing process writes nothing"); }
				// The failed attempt left the holder's lock in place.
				auto again = spawn(self, "try", a, directory);
				check(outcome_of(reap(again)) == Outcome::busy, "the holder still excludes its own mode");
				check(reap(holder) == 0, "the holder released cleanly");
				auto after = spawn(self, "try", b, directory);
				check(outcome_of(reap(after)) == Outcome::acquired, "a released lock is free to the competitor");
			} else {
				j::PosixIO io(directory), other_io(directory), third(directory);
				j::IO& competitor = topology == Topology::shared ? static_cast<j::IO&>(io) : other_io;
				auto lock = acquire(io, first, directory);
				const auto before = snapshot(directory);
				got = attempt(competitor, second, directory);
				if (got != Outcome::acquired) { check(snapshot(directory) == before, "a competing owner writes nothing"); }
				check(attempt(third, first, directory) == Outcome::busy, "the holder still excludes its own mode");
				lock.reset();
				check(attempt(competitor, second, directory) == Outcome::acquired, "a released lock is free to the competitor");
			}
			if (expect ? got != blocked : got != Outcome::acquired) {
				std::cerr << "\nowner_lock: " << name(topology) << ": " << first.name << " then " << second.name
					<< ": " << name(got) << ", expected " << (expect ? name(blocked) : "acquired") << '\n';
				std::exit(1);
			}
			(expect ? excluded : unsafe) += 1;
			std::cout << ' ' << std::string(std::string_view(second.name).size() - 1, ' ') << (expect ? 'x' : 'A');
		}
		std::cout << '\n';
	}
	std::cout << "owner_lock: " << name(topology) << ": " << excluded << " excluded, " << unsafe
		<< " legacy file/directory controls acquired\n";
	check(excluded == 17 && unsafe == 8, "17 excluding pairings and 8 unsafe controls");
}

// What a successful acquisition leaves: one zero-length 0600 owner.lock for a create,
// whatever the umask, and nothing for a directory lock.
void bootstrap(const fs::path& directory) {
	const auto mask = ::umask(0);
	for (auto options : {j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive},
			 j::OwnerLockOptions::legacy_file(true)}) {
		j::PosixIO io(fresh(directory));
		auto lock = io.acquire_owner(options);
		const auto after = snapshot(directory);
		check(after.entries.size() == 1 && after.entries.at("owner.lock") == 0, "a create adds one empty owner.lock");
		check((fs::status(directory / "owner.lock").permissions() & fs::perms::mask) ==
			(fs::perms::owner_read | fs::perms::owner_write), "owner.lock is 0600 under umask 0");
	}
	::umask(mask);
	for (auto options : {j::OwnerLockOptions{Mode::directory, j::OwnerFile::open_existing},
			 j::OwnerLockOptions::legacy_directory()}) {
		j::PosixIO io(fresh(directory));
		const auto before = snapshot(directory);
		auto lock = io.acquire_owner(options);
		check(snapshot(directory) == before, "a directory lock writes nothing");
	}
	j::PosixIO io(fresh(directory));
	const auto before = snapshot(directory);
	check(throws<std::invalid_argument>([&] {
		(void)io.acquire_owner(j::OwnerLockOptions{Mode::directory, j::OwnerFile::create_exclusive});
	}), "a directory lock creates no file");
	check(throws<std::invalid_argument>([&] { (void)io.acquire_owner(j::OwnerLockOptions{static_cast<Mode>(7)}); }),
		"an unknown mode");
	check(throws<std::invalid_argument>([&] {
		(void)io.acquire_owner(j::OwnerLockOptions{Mode::both, static_cast<j::OwnerFile>(7)});
	}), "an unknown file policy");
	check(snapshot(directory) == before, "invalid options write nothing");
	check(j::OwnerLockOptions{} == j::OwnerLockOptions{Mode::both, j::OwnerFile::open_existing}, "both, opening, by default");
}

// Create once, then recover: a second create is file_exists, opening a missing owner.lock is
// no_such_file_or_directory, and either unwinds the directory lock it took.
void create_and_recover(const char* self, const fs::path& directory) {
	const j::OwnerLockOptions create{Mode::both, j::OwnerFile::create_exclusive}, recover{Mode::both, j::OwnerFile::open_existing};
	j::PosixIO io(fresh(directory)), other(directory);
	const int fds = lowest_fd();
	check(error_of([&] { (void)io.acquire_owner(recover); }) == std::errc::no_such_file_or_directory, "recover before create");
	check(fs::is_empty(directory), "a failed recover writes nothing");
	{ auto lock = other.acquire_directory_owner(); }
	{ auto lock = io.acquire_owner(create); }
	check(error_of([&] { (void)io.acquire_owner(create); }) == std::errc::file_exists, "create twice");
	{ auto lock = other.acquire_directory_owner(); }
	check(lowest_fd() == fds, "failed creates and recovers leak no descriptor");
	{ auto lock = io.acquire_owner(recover); }
	// A killed holder releases both locks; the survivor recovers.
	auto holder = hold(self, 4, directory);
	check(attempt(io, holders[4], directory) == Outcome::busy, "another process holds both");
	::kill(holder.pid, SIGKILL);
	check(reap(holder) == 128 + SIGKILL, "the holder was killed");
	auto lock = io.acquire_owner(recover);
	check(error_of([&] { (void)other.acquire_owner(true); }) == std::errc::file_exists, "legacy create of an existing owner.lock");
	check(error_of([&] { (void)other.acquire_owner(false); }) == std::errc::operation_would_block, "legacy recover is busy");
	check(error_of([&] { (void)other.acquire_directory_owner(); }) == std::errc::operation_would_block, "legacy directory is busy");
}

// The flock seam: per step, a count of EINTRs and then an errno or the real answer.
struct Injection {
	int directory_fd = -1;
	int directory_eintr = 0, file_eintr = 0;
	int directory_errno = 0, file_errno = 0;
	int calls = 0;
} injection;
int injected_flock(int fd, int operation) {
	++injection.calls;
	const bool directory = fd == injection.directory_fd;
	int& eintr = directory ? injection.directory_eintr : injection.file_eintr;
	if (eintr > 0) {
		--eintr;
		errno = EINTR;
		return -1;
	}
	if (int error = directory ? injection.directory_errno : injection.file_errno) {
		errno = error;
		return -1;
	}
	return ::flock(fd, operation);
}
void inject(j::PosixIO& io, Injection value) {
	value.directory_fd = io.native_directory_handle();
	injection = value;
	io.replace_flock(injected_flock);
}
bool directory_free(const fs::path& directory) {
	j::PosixIO probe(directory);
	return attempt(probe, holders[3], directory) == Outcome::acquired;
}
bool file_free(const fs::path& directory) {
	j::PosixIO probe(directory);
	return attempt(probe, holders[1], directory) == Outcome::acquired;
}

// A failed step releases what earlier steps took and leaves the IO able to lock again.
void partial_failure(const fs::path& directory) {
	const j::OwnerLockOptions create{Mode::both, j::OwnerFile::create_exclusive}, recover{Mode::both, j::OwnerFile::open_existing};
	j::PosixIO io(fresh(directory));
	const int fds = lowest_fd();
	for (int error : {EIO, ENOLCK, EWOULDBLOCK}) {
		inject(io, {.file_errno = error});
		const auto code = error_of([&] { (void)io.acquire_owner(fs::exists(directory / "owner.lock") ? recover : create); });
		check(code == std::error_code(error, std::generic_category()), "the file step's error survives");
		check(lowest_fd() == fds, "the owner.lock descriptor is closed");
		check(directory_free(directory), "the directory lock is released");
		check(fs::exists(directory / "owner.lock"), "a failed create leaves owner.lock for recovery");
	}
	// The file step unsupported (EOPNOTSUPP differs from ENOTSUP on macOS): not_supported.
	for (int error : {ENOTSUP, EOPNOTSUPP}) {
		inject(io, {.file_errno = error});
		check(error_of([&] { (void)io.acquire_owner(recover); }) == std::errc::not_supported, "an unsupported file lock");
		check(directory_free(directory) && lowest_fd() == fds, "unwound after an unsupported file lock");
	}
	io.replace_flock(nullptr);
	{
		auto lock = io.acquire_owner(recover);
		check(!directory_free(directory) && !file_free(directory), "the IO locks both again after the failures");
	}
	io.replace_flock(nullptr);
	// The directory step fails first: no owner.lock is opened or created.
	j::PosixIO empty(fresh(directory));
	const int empty_fds = lowest_fd();
	for (int error : {EIO, EWOULDBLOCK}) {
		inject(empty, {.directory_errno = error});
		check(error_of([&] { (void)empty.acquire_owner(create); }) == std::error_code(error, std::generic_category()),
			"the directory step's error survives");
		check(fs::is_empty(directory) && lowest_fd() == empty_fds, "nothing created after a failed directory step");
	}
	empty.replace_flock(nullptr);
}

// EINTR is retried at either step; a busy answer behind EINTR is still busy.
void interrupted(const fs::path& directory) {
	j::PosixIO io(fresh(directory)), other(directory);
	inject(io, {.directory_eintr = 3, .file_eintr = 2});
	{
		auto lock = io.acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive});
		check(injection.calls == 7, "three directory retries, two file retries, one success each");
	}
	for (int index = 0; index < holder_count; ++index) {
		inject(io, {.directory_eintr = 1, .file_eintr = 1});
		auto lock = acquire(io, holders[index], directory);
	}
	auto held = other.acquire_owner(j::OwnerLockOptions{});
	inject(io, {.directory_eintr = 2});
	check(error_of([&] { (void)io.acquire_owner(j::OwnerLockOptions{}); }) == std::errc::operation_would_block,
		"busy behind EINTR");
	inject(io, {.file_eintr = 2});
	check(error_of([&] { (void)io.acquire_owner(false); }) == std::errc::operation_would_block, "busy behind EINTR (file)");
	io.replace_flock(nullptr);
}

// A directory lock the filesystem does not support fails closed in every mode that needs
// it: no file-only fallback, nothing created. The file-only mode is the caller's choice.
void unsupported(const fs::path& directory) {
	j::PosixIO io(fresh(directory));
	for (int error : {ENOTSUP, EOPNOTSUPP}) {
		inject(io, {.directory_errno = error});
		check(error_of([&] { (void)io.acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive}); }) ==
			std::errc::not_supported, "both is not_supported");
		check(error_of([&] { (void)io.acquire_owner(j::OwnerLockOptions{Mode::directory}); }) == std::errc::not_supported,
			"directory is not_supported");
		check(error_of([&] { (void)io.acquire_directory_owner(); }) == std::errc::not_supported, "legacy directory too");
		check(fs::is_empty(directory), "no owner.lock as a fallback");
	}
	{
		auto lock = io.acquire_owner(j::OwnerLockOptions::legacy_file(true));
		check(!file_free(directory), "the file lock still excludes when only the directory is unsupported");
	}
	io.replace_flock(nullptr);
}

// One IO holds one lock per inode: any reacquisition touching a held inode is refused before
// it locks or writes anything, so no second, independently releasable handle exists.
void aliases(const fs::path& directory) {
	{
		j::PosixIO io(fresh(directory));
		auto lock = io.acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive});
		const auto before = snapshot(directory);
		for (const auto& holder : holders) {
			check(attempt(io, holder, directory) == Outcome::refused, "reacquiring through the holder's IO is refused");
		}
		check(snapshot(directory) == before && !directory_free(directory) && !file_free(directory), "still held, unchanged");
		lock.reset();
		check(directory_free(directory) && file_free(directory), "one release frees both");
		auto again = io.acquire_owner(j::OwnerLockOptions{});
	}
	{
		j::PosixIO io(fresh(directory));
		auto lock = io.acquire_directory_owner();
		check(attempt(io, holders[4], directory) == Outcome::refused, "both over a held directory lock");
		check(fs::is_empty(directory) && !directory_free(directory), "nothing created, the directory still held");
		lock.reset();
		check(directory_free(directory), "released once");
	}
	// The legacy pairing through one IO: two locks on two inodes, each released by its own
	// (the unsafe control again, in one process).
	{
		j::PosixIO io(fresh(directory));
		auto file = io.acquire_owner(true);
		auto both = attempt(io, holders[4], directory);
		check(both == Outcome::refused, "both over a held file lock");
		auto dir = io.acquire_directory_owner();
		file.reset();
		check(file_free(directory) && !directory_free(directory), "the file released alone");
		dir.reset();
		check(directory_free(directory), "then the directory");
	}
}

// Two threads acquire through one IO at once: exactly one wins, and the loser's unwind never
// releases what the winner holds, so a fresh IO still finds it busy.
void concurrent(const fs::path& directory) {
	constexpr int iterations = 300;
	for (const auto mode : {Mode::both, Mode::directory, Mode::file}) {
		for (int i = 0; i < iterations; ++i) {
			j::PosixIO io(fresh(directory));
			{ auto seed = io.acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive}); }
			std::atomic<int> ready{0}, acquired{0};
			std::unique_ptr<j::OwnerLock> locks[2];
			auto take = [&](int index) {
				ready.fetch_add(1);
				while (ready.load() < 2) {}
				try {
					locks[index] = io.acquire_owner(j::OwnerLockOptions{mode, j::OwnerFile::open_existing});
					acquired.fetch_add(1);
				} catch (const std::exception&) {}
			};
			std::thread first(take, 0), second(take, 1);
			first.join();
			second.join();
			check(acquired.load() == 1, "exactly one thread acquires through one IO");
			if (mode != Mode::file) { check(!directory_free(directory), "the winner still holds the directory"); }
			if (mode != Mode::directory) { check(!file_free(directory), "the winner still holds owner.lock"); }
			locks[0].reset();
			locks[1].reset();
			check(directory_free(directory) && file_free(directory), "released once the winner is dropped");
			(void)io.acquire_owner(j::OwnerLockOptions{mode, j::OwnerFile::open_existing});
		}
	}
}

// An IO with only the legacy bool call: the options call reports not_supported in every
// mode and never forwards to it. A wrapper forwards lock_owner to its backend's acquire_owner.
struct LegacyIO final : j::IO {
	int calls = 0;
	std::unique_ptr<j::OwnerLock> acquire_owner(bool) override { ++calls; throw std::logic_error("unused"); }
	std::unique_ptr<j::File> open_existing(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> create_exclusive(std::string_view) override { throw std::logic_error("unused"); }
	void replace(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
	void remove(std::string_view) override { throw std::logic_error("unused"); }
	void sync_directory() override { throw std::logic_error("unused"); }
};
struct WrapperIO final : j::IO {
	explicit WrapperIO(j::IO& inner) : inner(inner) {}
	j::IO& inner;
	std::unique_ptr<j::OwnerLock> acquire_owner(bool create) override { return inner.acquire_owner(create); }
	std::unique_ptr<j::File> open_existing(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> create_exclusive(std::string_view) override { throw std::logic_error("unused"); }
	void replace(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
	void remove(std::string_view) override { throw std::logic_error("unused"); }
	void sync_directory() override { throw std::logic_error("unused"); }
protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& options) override { return inner.acquire_owner(options); }
};
void base(const fs::path& directory) {
	LegacyIO legacy;
	j::IO& io = legacy;
	for (auto mode : {Mode::file, Mode::directory, Mode::both}) {
		check(error_of([&] { (void)io.acquire_owner(j::OwnerLockOptions{mode}); }) == std::errc::not_supported,
			"a backend without lock_owner is not_supported");
	}
	check(legacy.calls == 0, "and never reaches the legacy call");
	check(throws<std::logic_error>([&] { (void)io.acquire_directory_owner(); }), "the base directory call is unchanged");
	j::PosixIO posix(fresh(directory));
	WrapperIO wrapper(posix);
	auto lock = static_cast<j::IO&>(wrapper).acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive});
	check(!directory_free(directory) && !file_free(directory), "a forwarding wrapper excludes in both");
}

// An image job retains the caller's owner (lock, then backend) until its original settles:
// a competitor is busy in every mode while it is outstanding and acquires after.
void lifetime(const fs::path& directory) {
	struct Owner {
		std::shared_ptr<j::PosixIO> io;
		std::unique_ptr<j::OwnerLock> lock;
		~Owner() { lock.reset(); }
	};
	fresh(directory);
	auto io = std::make_shared<j::PosixIO>(directory);
	auto owner = std::make_shared<Owner>();
	owner->io = io;
	owner->lock = io->acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive});
	auto bytes = std::make_shared<const std::string>(4096, 'x');
	c::Token token;
	token.operation = 1;
	{
		auto job = std::make_shared<c::ImagePreparation>(io, owner, token, "generation-1", bytes, bytes->size());
		owner.reset();
		io.reset();
		job->submitted();
		auto original = j::detail::execute_primitive(job->io(), job->request());
		j::PosixIO competitor(directory);
		for (const auto& holder : holders) {
			check(attempt(competitor, holder, directory) == Outcome::busy, "busy while the original is outstanding");
		}
		check(job->complete(std::move(original)), "the original completes");
		c::drive_synchronously(*job);
		if (job->error()) { std::rethrow_exception(job->error()); }
	}
	j::PosixIO competitor(directory);
	for (const auto& holder : holders) {
		check(attempt(competitor, holder, directory) == Outcome::acquired, "free once the job is gone");
	}
}

// owner.lock must be a private, singly linked regular file reached without following a
// symlink; a refusal unwinds the directory lock. The directory lock ignores owner.lock.
void refusals(const fs::path& directory) {
	const j::OwnerLockOptions recover{Mode::both, j::OwnerFile::open_existing};
	const auto target = directory.string() + ".target";
	auto expect_refused = [&](const char* what) {
		j::PosixIO io(directory);
		const auto before = snapshot(directory);
		const int fds = lowest_fd();
		check(throws<std::exception>([&] { (void)io.acquire_owner(recover); }), what);
		check(throws<std::exception>([&] { (void)io.acquire_owner(false); }), what);
		check(snapshot(directory) == before && lowest_fd() == fds && directory_free(directory), "refused and unwound");
		auto lock = io.acquire_owner(j::OwnerLockOptions{Mode::directory});
	};
	fresh(directory);
	fs::remove_all(target);
	{ std::ofstream(target).put('t'); }
	fs::create_symlink(target, directory / "owner.lock");
	expect_refused("a symlinked owner.lock");
	check(fs::file_size(target) == 1, "its target untouched");
	fresh(directory);
	{ j::PosixIO io(directory); auto lock = io.acquire_owner(true); }
	fs::create_hard_link(directory / "owner.lock", directory / "alias");
	expect_refused("a hard-linked owner.lock");
	fs::remove(directory / "alias");
	fs::permissions(directory / "owner.lock", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_write);
	expect_refused("a group-writable owner.lock");
	fresh(directory);
	fs::create_directory(directory / "owner.lock");
	expect_refused("a directory named owner.lock");
	fs::remove_all(target);
	// The root: a symlink is refused by default; followed, the lock is the target's.
	fresh(directory);
	const fs::path link = directory.string() + ".link";
	fs::remove(link);
	fs::create_directory_symlink(directory, link);
	check(throws<std::system_error>([&] { j::PosixIO refused(link); }), "a symlinked root is refused");
	{
		j::PosixIO followed(link, 0600, {.follow_root_symlink = true});
		auto lock = followed.acquire_owner(j::OwnerLockOptions{Mode::both, j::OwnerFile::create_exclusive});
		check(!directory_free(directory) && !file_free(directory), "a followed root locks its target");
	}
	fs::remove(link);
	fs::permissions(directory, fs::perms::owner_all | fs::perms::group_write);
	check(throws<std::runtime_error>([&] { j::PosixIO refused(directory); }), "a shared-writable root is refused");
	fs::permissions(directory, fs::perms::owner_all);
}
} // namespace

int main(int argc, char** argv) {
	if (argc == 5 && std::string(argv[1]) == "--child") { return child(argv[2], std::stoi(argv[3]), argv[4]); }
	if (argc != 2) {
		std::cerr << "usage: completion_owner_lock_test DIRECTORY\n";
		return 2;
	}
	const fs::path directory = fs::absolute(argv[1]);  // the root-symlink case links to it
	for (auto topology : {Topology::processes, Topology::distinct, Topology::shared}) { matrix(argv[0], directory, topology); }
	bootstrap(directory);
	create_and_recover(argv[0], directory);
	partial_failure(directory);
	interrupted(directory);
	unsupported(directory);
	aliases(directory);
	concurrent(directory);
	base(directory);
	lifetime(directory);
	refusals(directory);
	fs::remove_all(directory);
	std::cout << "owner_lock: ok\n";
	return 0;
}

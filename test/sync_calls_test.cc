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

// PosixIO's real sync call sequence on a real directory, recorded through replace_sync_calls:
// which files and the directory are fsync'd, what the one ordering point issues, which barrier
// errors fail the commit, and when the barrier warning fires (once per volume, macOS only).
#include "completion/posix.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <functional>
#include <system_error>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace j = kronuz::journal;

#ifdef __APPLE__
namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}
using Log = std::vector<std::string>;
Log calls;
int barrier_error = 0;   // fails the next barriers with this errno
int barrier_failures = 0;
int full_error = 0;      // fails the next full flush with this errno
std::string full_target; // the name behind the descriptor of the last full flush
std::vector<j::BarrierWarning> warnings;  // every warning a callback returned from
int warning_calls = 0;   // every callback invocation, thrown or returned
int warning_throws = 0;  // the next callbacks throw

std::string name_of(int fd) {
	char path[1024] = {};
	check(::fcntl(fd, F_GETPATH, path) == 0, "fd has a path");
	return std::filesystem::path(path).filename().string();
}
int record_fsync(int fd) {
	calls.push_back("fsync:" + name_of(fd));
	return ::fsync(fd);
}
int record_fcntl(int fd, int command) {
	if (command == F_BARRIERFSYNC) {
		calls.push_back("barrier");
		if (barrier_error && barrier_failures > 0) { --barrier_failures; errno = barrier_error; return -1; }
		return ::fcntl(fd, command);
	}
	check(command == F_FULLFSYNC, "only F_BARRIERFSYNC and F_FULLFSYNC are issued");
	calls.push_back("full");
	full_target = name_of(fd);
	if (full_error) { errno = full_error; return -1; }
	return ::fcntl(fd, command);
}
Log run(const std::function<void()>& body) {
	calls.clear();
	body();
	return calls;
}
void expect(const Log& got, const Log& want, const char *what) {
	if (got != want) {
		std::cerr << "failed: " << what << "\n  got:";
		for (auto& c : got) { std::cerr << ' ' << c; }
		std::cerr << "\n  want:";
		for (auto& c : want) { std::cerr << ' ' << c; }
		std::cerr << '\n';
		std::exit(1);
	}
}
// A file of another backend, which PosixIO cannot batch.
class Foreign final : public j::File {
  public:
	Foreign(std::unique_ptr<j::File> inner, Log& log) : inner_(std::move(inner)), log_(&log) {}
	std::uint64_t size() override { return inner_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner_->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner_->truncate(size); }
	void sync() override { log_->push_back("foreign"); inner_->sync(); }
  private:
	std::unique_ptr<j::File> inner_;
	Log* log_;
};
struct Fixture {
	std::filesystem::path directory;
	std::unique_ptr<j::PosixIO> io;
	std::unique_ptr<j::OwnerLock> owner;
	std::vector<std::unique_ptr<j::File>> files;
	std::vector<j::File*> all;
	Fixture(const std::filesystem::path& where, j::PosixIOOptions options) : directory(where) {
		std::filesystem::remove_all(directory);
		std::filesystem::create_directory(directory);
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		io = std::make_unique<j::PosixIO>(directory, 0600, options);
		owner = io->acquire_owner(true);
		for (const char *name : {"a", "b", "c"}) {
			auto file = io->create_exclusive(name);
			check(file->write_at(0, "bytes") == 5, "write");
			all.push_back(file.get());
			files.push_back(std::move(file));
		}
		io->replace_sync_calls(&record_fsync, &record_fcntl);
	}
	std::string dir() const { return directory.filename().string(); }
};
Log cat(Log a, const Log& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

void ordering_protocol(const std::filesystem::path& base) {
	Fixture f(base / "ordered", {});
	const Log stage{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir()};
	// One ordering point: every file and the directory fsync'd, then one barrier and no flush.
	expect(run([&] { f.io->order_files_and_directory(f.all); }), cat(stage, {"barrier"}),
		"order_files_and_directory fsyncs each file and the directory, then issues one barrier");
	// The directory alone is still ordered.
	expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "barrier"}, "a directory-only ordering point");
	// The publication's sync_directory is the first full flush.
	expect(run([&] { f.io->sync_directory(); }), {"fsync:" + f.dir(), "full"}, "sync_directory fsyncs the directory then flushes in full");
	// Durable batches flush in full and never barrier.
	expect(run([&] { f.io->sync_files_and_directory(f.all); }), cat(stage, {"full"}), "sync_files_and_directory is one full flush");
	expect(run([&] { f.io->sync_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "an empty durable batch syncs the directory");
	expect(run([&] { f.io->sync_files(f.all); }), {"fsync:a", "fsync:b", "fsync:c", "full"}, "sync_files is one full flush");
	// A foreign file syncs itself; the directory is still ordered.
	Log foreign_log;
	Foreign foreign(f.io->open_existing("a"), foreign_log);
	std::vector<j::File*> only_foreign{&foreign};
	expect(run([&] { f.io->order_files_and_directory(only_foreign); }), {"fsync:" + f.dir(), "barrier"},
		"a foreign file leaves the directory ordered");
	check(foreign_log == Log{"foreign"}, "a foreign file syncs itself once");
}

// The publication's flush goes through the owner file while one is held, and through the directory otherwise.
void flush_target(const std::filesystem::path& base) {
	Fixture f(base / "flush-target", {});
	run([&] { f.io->sync_directory(); });
	check(full_target == "owner.lock", "sync_directory flushes through the owner file while it is held");
	f.owner.reset();
	run([&] { f.io->sync_directory(); });
	check(full_target == f.dir(), "sync_directory flushes through the directory once the owner file is released");
}

void errno_rule(const std::filesystem::path& base) {
	Fixture f(base / "errno-rule", {});
	for (int error : {EIO, ENOSPC, EDQUOT, EINVAL, EBADF}) {
		barrier_error = error; barrier_failures = 1000;
		bool threw = false;
		calls.clear();
		try { f.io->order_files_and_directory(f.all); }
		catch (const std::system_error& e) { threw = e.code() == std::error_code(error, std::generic_category()); }
		check(threw, "a barrier error other than unsupported fails the commit with its errno");
		check(std::count(calls.begin(), calls.end(), "barrier") == 1 && std::count(calls.begin(), calls.end(), "full") == 0,
			"a failed barrier is not retried as a full flush");
	}
	for (int error : {ENOTSUP, EOPNOTSUPP, ENOTTY}) {
		barrier_error = error; barrier_failures = 1000;
		expect(run([&] { f.io->order_files_and_directory(f.all); }),
			{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "barrier", "full"}, "an unsupported barrier falls back to a full flush");
		full_error = EIO;
		bool threw = false;
		try { f.io->order_files_and_directory(f.all); }
		catch (const std::system_error& e) { threw = e.code() == std::error_code(EIO, std::generic_category()); }
		check(threw, "a failing fallback flush fails the commit");
		full_error = 0;
	}
	barrier_error = EINTR; barrier_failures = 1;
	expect(run([&] { f.io->order_files_and_directory(f.all); }),
		{"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "barrier", "barrier"}, "an interrupted barrier retries the barrier");
	barrier_error = 0; barrier_failures = 0;
}

void full_flush_option(const std::filesystem::path& base) {
	Fixture f(base / "full-flush", {.full_flush_ordering = true});
	expect(run([&] { f.io->order_files_and_directory(f.all); }), {"fsync:a", "fsync:b", "fsync:c", "fsync:" + f.dir(), "full"},
		"full_flush_ordering replaces the barrier with one full flush");
	expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "full_flush_ordering orders a directory alone in full");
}

// The context reaches the callback unchanged.
int warning_context = 0;
j::PosixIOOptions warned(j::PosixIOOptions options = {}) {
	options.barrier_warning = [](void* context, const j::BarrierWarning& warning) {
		check(context == &warning_context, "the callback receives its context");
		++warning_calls;
		if (warning_throws > 0) { --warning_throws; throw std::runtime_error("host callback failed"); }
		warnings.push_back(warning);
	};
	options.barrier_warning_context = &warning_context;
	return options;
}
[[maybe_unused]] constexpr j::PosixIOOptions literal_options{.full_flush_ordering = true};  // options stay a literal type
j::BarrierStorage other_storage(const struct statfs&) { return {false, "test storage"}; }
j::BarrierStorage apple_storage(const struct statfs&) { return {true, "APPLE SSD test (PCI-Express, Internal)"}; }
std::string mount_point(const struct statfs& status) { return std::string(status.f_mntonname, ::strnlen(status.f_mntonname, sizeof status.f_mntonname)); }
std::string mount_source(const struct statfs& status) { return std::string(status.f_mntfromname, ::strnlen(status.f_mntfromname, sizeof status.f_mntfromname)); }
std::uint64_t device_of(const std::filesystem::path& path) {
	struct stat status{};
	check(::stat(path.c_str(), &status) == 0, "stat a volume");
	return static_cast<std::uint64_t>(status.st_dev);
}
int quiet_fsync(int) { calls.push_back("fsync"); return 0; }
int quiet_fcntl(int, int command) { calls.push_back(command == F_BARRIERFSYNC ? "barrier" : "full"); return 0; }
// A PosixIO on another volume's root, whose sync calls are recorded and never reach it.
std::unique_ptr<j::PosixIO> volume_io(const std::filesystem::path& root, j::PosixIO::BarrierStorageCall storage) {
	auto io = std::make_unique<j::PosixIO>(root, 0600, warned({.allow_shared_directory = true}));
	io->replace_sync_calls(&quiet_fsync, &quiet_fcntl);
	io->replace_barrier_storage(storage);
	return io;
}
// Two mounted local volumes other than `base`'s, each with its own st_dev, whose roots PosixIO opens.
std::vector<std::filesystem::path> other_volumes(const std::filesystem::path& base) {
	struct statfs* mounts = nullptr;
	const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
	std::vector<std::uint64_t> seen{device_of(base)};
	std::vector<std::filesystem::path> found;
	for (int i = 0; i < count && found.size() < 2; ++i) {
		const std::string_view type(mounts[i].f_fstypename);
		if (!(mounts[i].f_flags & MNT_LOCAL) || type == "autofs") { continue; }
		const std::filesystem::path root = mount_point(mounts[i]);
		struct stat status{};
		if (::stat(root.c_str(), &status) || std::ranges::find(seen, static_cast<std::uint64_t>(status.st_dev)) != seen.end()) { continue; }
		try { j::PosixIO probe(root, 0600, {.allow_shared_directory = true}); } catch (const std::exception&) { continue; }
		seen.push_back(static_cast<std::uint64_t>(status.st_dev));
		found.push_back(root);
	}
	check(found.size() == 2, "two other local volumes are mounted (devfs and an APFS system volume)");
	return found;
}

// The warning fires after a barrier that ordered without flushing, once per volume per process,
// only where the storage is not an internal Apple SSD. Runs before any other test on base's volume
// classifies it.
void barrier_warning(const std::filesystem::path& base) {
	const Log stage{"fsync:a", "fsync:b", "fsync:c"};
	{
		Fixture f(base / "warn-full-flush", warned({.full_flush_ordering = true}));
		f.io->replace_barrier_storage(&other_storage);
		expect(run([&] { f.io->order_files_and_directory(f.all); }), cat(stage, {"fsync:" + f.dir(), "full"}), "full_flush_ordering flushes");
		expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "full_flush_ordering flushes a directory alone");
		check(warning_calls == 0, "full_flush_ordering never warns: no barrier ordered without flushing");
	}
	Fixture f(base / "warn", warned());
	f.io->replace_barrier_storage(&other_storage);
	const Log ordered = cat(stage, {"fsync:" + f.dir(), "barrier"});
	expect(run([&] { f.io->sync_files_and_directory(f.all); }), cat(stage, {"fsync:" + f.dir(), "full"}), "a durable batch flushes");
	expect(run([&] { f.io->sync_files_and_directory({}); }), {"fsync:" + f.dir(), "full"}, "an empty durable batch flushes");
	expect(run([&] { f.io->sync_files(f.all); }), cat(stage, {"full"}), "sync_files flushes");
	expect(run([&] { f.io->sync_directory(); }), {"fsync:" + f.dir(), "full"}, "sync_directory flushes");
	check(warning_calls == 0, "durable calls never warn");
	for (int error : {ENOTSUP, EOPNOTSUPP, ENOTTY}) {
		barrier_error = error; barrier_failures = 1;
		expect(run([&] { f.io->order_files_and_directory(f.all); }), cat(ordered, {"full"}), "an unsupported barrier falls back to a full flush");
		check(warning_calls == 0, "a barrier that fell back to a full flush does not warn");
	}
	barrier_error = EIO; barrier_failures = 1;
	bool failed = false;
	try { f.io->order_files_and_directory(f.all); } catch (const std::system_error&) { failed = true; }
	check(failed && warning_calls == 0, "a failed barrier fails the call and does not warn");
	barrier_error = 0; barrier_failures = 0;

	// A callback that throws fails the call after its barrier; the volume then warns again.
	warning_throws = 1;
	bool threw = false;
	calls.clear();
	try { f.io->order_files_and_directory(f.all); } catch (const std::runtime_error& e) { threw = std::string_view(e.what()) == "host callback failed"; }
	check(threw && warning_calls == 1 && warnings.empty(), "a throwing callback fails the ordering call");
	check(calls == ordered, "the throwing callback ran after the barrier");
	expect(run([&] { f.io->order_files_and_directory(f.all); }), ordered, "the warning adds no sync call");
	check(warning_calls == 2 && warnings.size() == 1, "the volume warns again after a callback threw");
	struct statfs volume{};
	check(::statfs(base.c_str(), &volume) == 0, "statfs base");
	const auto& warning = warnings.front();
	check(warning.device == device_of(f.directory), "the warning names the volume's st_dev");
	check(warning.volume == mount_point(volume) && warning.source == mount_source(volume), "the warning names the mount point and its source");
	check(warning.storage == j::BarrierStorage{false, "test storage"}, "the warning carries the storage found");

	// Once per volume: the same backend, a directory-only ordering point, and another backend on it.
	expect(run([&] { f.io->order_files_and_directory(f.all); }), ordered, "a second ordering point");
	expect(run([&] { f.io->order_files_and_directory({}); }), {"fsync:" + f.dir(), "barrier"}, "a directory-only ordering point");
	Fixture g(base / "warn-again", warned());
	g.io->replace_barrier_storage(&other_storage);
	expect(run([&] { g.io->order_files_and_directory(g.all); }), cat(stage, {"fsync:" + g.dir(), "barrier"}), "another backend orders");
	check(warning_calls == 2 && warnings.size() == 1, "a volume warns once per process");

	// Another volume warns once more; an internal Apple SSD never does, and stays classified.
	const auto volumes = other_volumes(base);
	auto other = volume_io(volumes[0], &other_storage);
	expect(run([&] { other->order_files_and_directory({}); }), {"fsync", "barrier"}, "an ordering point on another volume");
	check(warnings.size() == 2 && warnings.back().device == device_of(volumes[0]) && warnings.back().volume == volumes[0].string(),
		"another volume warns with its own device and mount point");
	auto again = volume_io(volumes[0], &other_storage);
	run([&] { again->order_files_and_directory({}); });
	check(warning_calls == 3 && warnings.size() == 2, "the other volume warns once");
	auto apple = volume_io(volumes[1], &apple_storage);
	expect(run([&] { apple->order_files_and_directory({}); }), {"fsync", "barrier"}, "an ordering point on an Apple SSD");
	auto later = volume_io(volumes[1], &other_storage);
	run([&] { later->order_files_and_directory({}); });
	check(warning_calls == 3, "an internal Apple SSD never warns, and its volume is classified once");
}

// What IOKit finds under base's volume. With COMPLETION_EXPECT_STORAGE=apple or other (a run on a
// known device: an internal Apple SSD, a disk image), the real classifier and the real sync calls
// warn exactly when it is other; the seamed barrier_warning would then find the volume classified.
bool real_storage(const std::filesystem::path& base) {
	struct statfs volume{};
	check(::statfs(base.c_str(), &volume) == 0, "statfs base");
	const auto storage = j::detail::barrier_storage(volume);
	std::cout << "barrier storage of " << mount_point(volume) << " (" << mount_source(volume) << "): " << storage.name
		<< (storage.apple_ssd ? ", an internal Apple SSD\n" : ", warns\n");
	check(storage == j::detail::barrier_storage(volume), "the classification is stable");
	const char* expected = std::getenv("COMPLETION_EXPECT_STORAGE");
	if (!expected) { return false; }
	const std::string_view want(expected);
	check(want == "apple" || want == "other", "COMPLETION_EXPECT_STORAGE is apple or other");
	check(storage.apple_ssd == (want == "apple"), "the storage is classified as expected");
	std::filesystem::create_directory(base / "real");
	std::filesystem::permissions(base / "real", std::filesystem::perms::owner_all);
	j::PosixIO io(base / "real", 0600, warned());
	auto file = io.create_exclusive("a");
	check(file->write_at(0, "bytes") == 5, "write");
	j::File* group[] = {file.get()};
	io.order_files_and_directory(group);
	io.order_files_and_directory(group);
	io.sync_directory();
	check(warning_calls == (want == "other" ? 1 : 0), "the real barrier warns once on other storage and never on an Apple SSD");
	if (want == "other") {
		check(warnings.size() == 1 && warnings.front().storage == storage && warnings.front().volume == mount_point(volume),
			"the real warning names the volume and its storage");
	}
	return true;
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_calls_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path base(argv[1]);
	std::filesystem::remove_all(base);
	std::filesystem::create_directory(base);
	if (!real_storage(base)) { barrier_warning(base); }
	ordering_protocol(base);
	flush_target(base);
	errno_rule(base);
	full_flush_option(base);
	std::cout << "sync_calls: ok\n";
	return 0;
}
#else
// Linux and FreeBSD order with the durable sequence and never call the barrier warning.
namespace {
std::vector<std::string> calls;
int record_fsync(int fd) { calls.push_back("fsync:" + std::to_string(fd)); return ::fsync(fd); }
}
int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_calls_test DIRECTORY\n";
		return 2;
	}
	const std::filesystem::path base(argv[1]);
	std::filesystem::remove_all(base);
	std::filesystem::create_directories(base / "never-warns");
	std::filesystem::permissions(base / "never-warns", std::filesystem::perms::owner_all);
	static int warnings = 0;
	constexpr j::PosixIOOptions options{.barrier_warning = [](void*, const j::BarrierWarning&) { ++warnings; }};
	j::PosixIO io(base / "never-warns", 0600, options);
	std::vector<std::unique_ptr<j::File>> files;
	std::vector<j::File*> all;
	for (const char *name : {"a", "b", "c"}) {
		files.push_back(io.create_exclusive(name));
		all.push_back(files.back().get());
	}
#ifdef __linux__
	io.replace_sync_calls(&record_fsync, nullptr);
#else
	io.replace_sync_calls(&record_fsync);
#endif
	io.order_files_and_directory(all);
	const auto ordered = std::exchange(calls, {});
	io.sync_files_and_directory(all);
	const auto durable = std::exchange(calls, {});
	io.order_files_and_directory({});
	if (ordered != durable || ordered.size() != 4 || calls.size() != 1 || warnings != 0) {
		std::cerr << "failed: ordering is the durable sequence and never warns\n";
		return 1;
	}
	std::cout << "sync_calls: ok (no barrier, no warning)\n";
	return 0;
}
#endif

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

// Device flushes cover every staged file; foreign backends retain their own barrier.
#include "completion/posix.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace j = kronuz::journal;

namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}
class SyncModel {
  public:
	enum class Call { Stat, Sync, Flush, Barrier };
	struct Fault { Call call; int fd; int error; int remaining = 1; };
	explicit SyncModel(std::vector<dev_t> devices)
		: devices_(std::move(devices)), staged_(devices_.size()), durable_(devices_.size()), ordered_(devices_.size()) {}
	int stat(int fd, struct stat* status) {
		++stats;
		if (fails(Call::Stat, fd)) { return -1; }
		status->st_dev = devices_.at(fd);
		return 0;
	}
	int sync(int fd) {
		check(full_syncs == 0 && barriers == 0, "every file stages before the first device flush");
		++syncs;
		if (fails(Call::Sync, fd)) { return -1; }
		staged_.at(fd) = true;
		return 0;
	}
	int full_sync(int fd) {
		++full_syncs;
		if (fails(Call::Flush, fd)) { return -1; }
		for (std::size_t i = 0; i < devices_.size(); ++i) {
			if (devices_[i] == devices_.at(fd) && staged_[i]) { durable_[i] = true; }
		}
		return 0;
	}
	int barrier(int fd) {
		++barriers;
		if (fails(Call::Barrier, fd)) { return -1; }
		for (std::size_t i = 0; i < devices_.size(); ++i) {
			if (devices_[i] == devices_.at(fd) && staged_[i]) { ordered_[i] = true; }
		}
		return 0;
	}
	bool is_ordered(std::size_t i) const { return ordered_.at(i); }
	bool is_durable(std::size_t i) const { return durable_.at(i); }
	bool ordered() const { return std::ranges::all_of(ordered_, [](bool value) { return value; }); }
	bool durable() const { return std::ranges::all_of(durable_, [](bool value) { return value; }); }
	int stats = 0;
	int syncs = 0;
	int full_syncs = 0;
	int barriers = 0;
	std::optional<Fault> fault;
  private:
	bool fails(Call call, int fd) {
		if (!fault || fault->call != call || fault->fd != fd || !fault->remaining) { return false; }
		--fault->remaining;
		errno = fault->error;
		return true;
	}
	std::vector<dev_t> devices_;
	std::vector<bool> staged_;
	std::vector<bool> durable_;
	std::vector<bool> ordered_;
};
void device_barriers() {
	const std::array<int, 3> fds{0, 1, 2};
	SyncModel mixed({9, 3, 9});
	j::detail::files_sync_devices(fds, mixed);
	check(mixed.durable(), "files on every device become durable");
	check(mixed.stats == 3 && mixed.syncs == 3 && mixed.full_syncs == 2, "one full flush per distinct device");
	SyncModel same({9, 9, 9});
	j::detail::files_sync_devices(fds, same);
	check(same.durable() && same.full_syncs == 1, "one full flush for a same-device batch");
	const std::array<int, 3> repeated{0, 1, 0};
	SyncModel duplicates({9, 3});
	j::detail::files_sync_devices(repeated, duplicates);
	check(duplicates.durable() && duplicates.syncs == 3 && duplicates.full_syncs == 2, "repeated descriptors preserve both barriers");
	SyncModel empty({});
	j::detail::files_sync_devices({}, empty);
	check(empty.stats == 0 && empty.syncs == 0 && empty.full_syncs == 0, "empty batch has no syscalls");
	for (auto call : {SyncModel::Call::Stat, SyncModel::Call::Sync, SyncModel::Call::Flush}) {
		SyncModel interrupted({9, 3, 9});
		interrupted.fault = SyncModel::Fault{call, 1, EINTR};
		j::detail::files_sync_devices(fds, interrupted);
		check(interrupted.fault->remaining == 0 && interrupted.durable(), "interrupted syscalls retry");
		check(interrupted.stats == 3 + (call == SyncModel::Call::Stat) &&
			interrupted.syncs == 3 + (call == SyncModel::Call::Sync) &&
			interrupted.full_syncs == 2 + (call == SyncModel::Call::Flush), "only interrupted calls repeat");
		SyncModel failed({9, 3, 9});
		failed.fault = SyncModel::Fault{call, 1, EIO};
		try {
			j::detail::files_sync_devices(fds, failed);
			check(false, "a failed syscall rejects the barrier");
		} catch (const std::system_error& error) {
			check(error.code() == std::error_code(EIO, std::generic_category()), "original syscall error survives");
		}
		check(failed.fault->remaining == 0 && !failed.durable(), "a failed barrier does not report durability");
		if (call == SyncModel::Call::Stat) { check(failed.syncs == 0, "device discovery fails before staging"); }
		if (call != SyncModel::Call::Flush) { check(failed.full_syncs == 0, "failed staging never full-flushes"); }
	}
}
// The directory's fd (index 3 below) joins the batch: staged with the files, flushed with the
// device it shares. Ordered mode barriers that device and still flushes the others in full.
void directory_batches() {
	const std::array<int, 3> fds{0, 1, 2};
	using j::detail::SyncMode;
	SyncModel same({9, 9, 9, 9});
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::durable, same);
	check(same.durable() && same.syncs == 4 && same.full_syncs == 1 && same.barriers == 0, "files and directory share one full flush");
	SyncModel split({9, 3, 9, 9});
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::durable, split);
	check(split.durable() && split.syncs == 4 && split.full_syncs == 2, "a second device adds its own flush");
	SyncModel foreign_directory({9, 9, 9, 5});
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::durable, foreign_directory);
	check(foreign_directory.durable() && foreign_directory.full_syncs == 2, "a directory on its own device is flushed");
	SyncModel ordered({9, 9, 9, 9});
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::ordered, ordered);
	check(ordered.ordered() && ordered.syncs == 4 && ordered.barriers == 1 && ordered.full_syncs == 0 && !ordered.durable(),
		"an ordered batch on the directory's device is one barrier and no full flush");
	SyncModel mixed({9, 3, 9, 9});
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::ordered, mixed);
	check(mixed.barriers == 1 && mixed.full_syncs == 1, "ordering never relies on a barrier for another device");
	check(mixed.is_ordered(0) && mixed.is_ordered(2) && mixed.is_ordered(3) && !mixed.is_ordered(1) && mixed.is_durable(1) && !mixed.is_durable(0),
		"the directory's device is ordered and the other device is durable");
	SyncModel only_directory({9});
	j::detail::files_sync_devices({}, 0, 0, SyncMode::ordered, only_directory);
	check(only_directory.syncs == 1 && only_directory.barriers == 1 && only_directory.full_syncs == 0, "a directory alone is ordered");
	SyncModel no_directory({9, 9, 9});
	j::detail::files_sync_devices(fds, -1, -1, SyncMode::ordered, no_directory);
	check(no_directory.durable() && no_directory.barriers == 0, "ordered mode without a directory is durable");
	for (auto call : {SyncModel::Call::Stat, SyncModel::Call::Sync, SyncModel::Call::Barrier, SyncModel::Call::Flush}) {
		SyncModel failed({9, 3, 9, 9});
		failed.fault = SyncModel::Fault{call, 3, EIO};
		if (call == SyncModel::Call::Flush) { failed.fault->fd = 1; }
		if (call == SyncModel::Call::Barrier) { failed.fault->fd = 0; }
		try {
			j::detail::files_sync_devices(fds, 3, 3, SyncMode::ordered, failed);
			check(false, "a failed syscall rejects the batch");
		} catch (const std::system_error& error) {
			check(error.code() == std::error_code(EIO, std::generic_category()), "the syscall error survives");
		}
		check(failed.fault->remaining == 0 && !failed.ordered(), "a failed batch does not report ordering");
	}
	SyncModel interrupted({9, 9, 9, 9});
	interrupted.fault = SyncModel::Fault{SyncModel::Call::Barrier, 0, EINTR};
	j::detail::files_sync_devices(fds, 3, 3, SyncMode::ordered, interrupted);
	check(interrupted.ordered() && interrupted.barriers == 2, "an interrupted barrier retries");
}
// Another backend's file, which PosixIO cannot batch.
class Counted final : public j::File {
  public:
	explicit Counted(std::unique_ptr<j::File> inner, int &syncs) : inner_(std::move(inner)), syncs_(&syncs) {}
	std::uint64_t size() override { return inner_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner_->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner_->truncate(size); }
	void sync() override {
		++*syncs_;
		inner_->sync();
	}

  private:
	std::unique_ptr<j::File> inner_;
	int *syncs_;
};
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_sync_files_test DIRECTORY\n";
		return 2;
	}
	device_barriers();
	directory_batches();
	const std::filesystem::path directory(argv[1]);
	std::filesystem::remove_all(directory);
	std::filesystem::create_directory(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	j::PosixIO io(directory);
	auto owner = io.acquire_owner(true);
	int foreign = 0;
	std::vector<std::unique_ptr<j::File>> files;
	for (int i = 0; i < 5; ++i) {
		auto file = io.create_exclusive("f" + std::to_string(i));
		const std::string bytes(4096 * (i + 1), char('a' + i));
		check(file->write_at(0, bytes) == bytes.size(), "write");
		files.push_back(i == 2 ? std::make_unique<Counted>(std::move(file), foreign) : std::move(file));
	}
	std::vector<j::File *> all;
	for (auto &f : files)
		all.push_back(f.get());
	io.sync_files(all);
	io.sync_files({});
	check(foreign == 1, "a foreign file syncs itself once");
	io.sync_directory();
	io.sync_files_and_directory(all);
	io.sync_files_and_directory({});
	check(foreign == 2, "a foreign file syncs itself once per batch");
	foreign = 1;
	for (int i = 0; i < 5; ++i) {
		auto file = io.open_existing("f" + std::to_string(i));
		std::string back(file->size(), '\0');
		check(back.size() == std::size_t(4096 * (i + 1)), "size");
		check(file->read_at(0, back) == back.size() && back == std::string(back.size(), char('a' + i)), "content");
	}
	const auto other_directory = directory / "other";
	std::filesystem::create_directory(other_directory);
	std::filesystem::permissions(other_directory, std::filesystem::perms::owner_all);
	j::PosixIO other(other_directory);
	auto other_owner = other.acquire_owner(true);
	auto other_file = other.create_exclusive("other");
	check(other_file->write_at(0, "other backend") == 13, "write through another POSIX backend");
	const std::array<j::File*, 2> cross_backend{files.front().get(), other_file.get()};
	io.sync_files(cross_backend);
	other.sync_directory();
	std::string other_back(13, '\0');
	check(other_file->read_at(0, other_back) == other_back.size() && other_back == "other backend", "sync files from different POSIX backends");
	// sync_data: PosixIO's data barrier, and the default (sync) for another backend.
	auto data = io.create_exclusive("data");
	check(data->write_at(0, "durable") == 7, "write before sync_data");
	data->sync_data();
	Counted counted(io.open_existing("f0"), foreign);
	counted.sync_data();
	check(foreign == 2, "a backend without sync_data syncs");
	std::string back(7, '\0');
	check(io.open_existing("data")->read_at(0, back) == 7 && back == "durable", "sync_data keeps the bytes");
	std::cout << "sync_files: ok\n";
	return 0;
}

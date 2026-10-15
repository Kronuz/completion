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

// IO::sync_files: PosixIO's files share the batched flush, a file of another backend
// falls back to its own sync, and every file reads back whole afterwards. File::sync_data
// likewise.
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
	for (int i = 0; i < 5; ++i) {
		auto file = io.open_existing("f" + std::to_string(i));
		std::string back(file->size(), '\0');
		check(back.size() == std::size_t(4096 * (i + 1)), "size");
		check(file->read_at(0, back) == back.size() && back == std::string(back.size(), char('a' + i)), "content");
	}
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

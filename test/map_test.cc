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

// File::map: PosixIO maps a whole file read-only; the view outlives its handle and the
// file's removal; an empty file maps to no bytes; another backend's default is null.
#include "completion/posix.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace j = kronuz::journal;

namespace {
void check(bool value, const char *what) {
	if (!value) {
		std::cerr << "failed: " << what << '\n';
		std::exit(1);
	}
}
class Plain final : public j::File {
  public:
	std::uint64_t size() override { return 0; }
	std::size_t read_at(std::uint64_t, std::span<char>) override { return 0; }
	std::size_t write_at(std::uint64_t, std::string_view) override { return 0; }
	void truncate(std::uint64_t) override {}
	void sync() override {}
};
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
	check(Plain().map() == nullptr, "a backend without mapping returns null");
	std::cout << "completion_map_test: ok\n";
}

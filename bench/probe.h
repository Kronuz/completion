/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

// Shared helpers of the platform probes: a private scratch directory on the device under
// test, files of a fixed size written through the library, and order statistics.
#pragma once

#include "completion/posix.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace probe {

namespace j = kronuz::journal;
using Clock = std::chrono::steady_clock;

inline double since_ms(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// A fresh 0700 directory under `parent`, removed with its contents on destruction.
class ScratchDirectory {
public:
	explicit ScratchDirectory(const std::filesystem::path& parent) {
		std::filesystem::create_directories(parent);
		std::string pattern = (parent / "probe-XXXXXX").string();
		if (!::mkdtemp(pattern.data())) { throw std::runtime_error("mkdtemp " + pattern); }
		path_ = pattern;
	}
	~ScratchDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
	ScratchDirectory(const ScratchDirectory&) = delete;
	ScratchDirectory& operator=(const ScratchDirectory&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }
private:
	std::filesystem::path path_;
};

// Writes `bytes` of a repeating, non-zero pattern, so no filesystem can store it as a hole.
inline void fill(j::File& file, std::uint64_t bytes, std::uint64_t seed = 1) {
	static thread_local std::string block;
	if (block.empty()) {
		block.resize(1 << 20);
		std::uint64_t state = 0x9e3779b97f4a7c15ull;
		for (auto& c : block) { state = state * 6364136223846793005ull + 1442695040888963407ull; c = static_cast<char>((state >> 56) | 1); }
	}
	for (std::uint64_t offset = 0; offset < bytes;) {
		const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(block.size() - 8, bytes - offset));
		std::string_view piece(block.data() + (seed & 7), length);
		std::size_t done = 0;
		while (done < piece.size()) {
			const auto count = file.write_at(offset + done, piece.substr(done));
			if (!count) { throw std::runtime_error("write made no progress"); }
			done += count;
		}
		offset += done;
	}
}

inline int native_fd(j::File& file) {
	auto* posix = dynamic_cast<j::detail::PosixFile*>(&file);
	if (!posix) { throw std::runtime_error("not a PosixIO file"); }
	return posix->native_handle();
}

// `count` new files of `bytes` each, named <prefix>-<index>.
struct Batch {
	std::vector<std::string> names;
	std::vector<std::unique_ptr<j::File>> files;
	std::vector<j::File*> pointers() const {
		std::vector<j::File*> result;
		for (auto& file : files) { result.push_back(file.get()); }
		return result;
	}
	std::vector<int> descriptors() const {
		std::vector<int> result;
		for (auto& file : files) { result.push_back(native_fd(*file)); }
		return result;
	}
};
inline Batch make_batch(j::IO& io, const std::string& prefix, int count, std::uint64_t bytes) {
	Batch batch;
	for (int i = 0; i < count; ++i) {
		batch.names.push_back(prefix + "-" + std::to_string(i));
		batch.files.push_back(io.create_exclusive(batch.names.back()));
		fill(*batch.files.back(), bytes, static_cast<std::uint64_t>(i) + 1);
	}
	return batch;
}
// Removes a batch's files and makes the removals durable, outside any timed region.
inline void discard(j::IO& io, Batch& batch) {
	batch.files.clear();
	for (auto& name : batch.names) { io.remove(name); }
	io.sync_directory();
}

// Order statistic of `values` (sorted in place): fraction 0.5 is the median.
inline double quantile(std::vector<double>& values, double fraction) {
	if (values.empty()) { return 0; }
	std::sort(values.begin(), values.end());
	const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
	return values[std::min(index, values.size() - 1)];
}

inline std::filesystem::path directory_argument(int argc, char** argv, int index) {
	if (argc > index) { return argv[index]; }
	const char* tmp = std::getenv("TMPDIR");
	return tmp && *tmp ? tmp : "/tmp";
}

inline long long integer_argument(int argc, char** argv, int index, long long fallback) {
	return argc > index ? std::atoll(argv[index]) : fallback;
}

} // namespace probe

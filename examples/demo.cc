/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */

#include "completion/posix.h"
#include <array>
#include <iostream>

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "usage: completion_demo FIRST_PRIVATE_DIRECTORY SECOND_PRIVATE_DIRECTORY\n";
		return 2;
	}
	try {
		kronuz::journal::PosixIO first(argv[1]), second(argv[2]);
		auto first_owner = first.acquire_owner(true);
		auto second_owner = second.acquire_owner(true);
		auto a = first.create_exclusive("batch-example");
		auto b = second.create_exclusive("batch-example");
		for (auto* file : {a.get(), b.get()}) {
			constexpr std::string_view bytes = "durable batch\n";
			std::size_t offset = 0;
			while (offset != bytes.size()) {
				auto count = file->write_at(offset, bytes.substr(offset));
				if (!count) { throw std::runtime_error("write made no progress"); }
				offset += count;
			}
		}
		const std::array<kronuz::journal::File*, 2> files{a.get(), b.get()};
		first.sync_files(files);
		first.sync_directory();
		second.sync_directory();
		std::cout << "both files and directory entries are durable\n";
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}

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


// The directory trust checks: a final-component symlink, a foreign owner, and a group- or
// other-writable mode are refused by default; each option admits what it names, the held
// descriptor stays on the directory it opened, and directory_trust() reports what was found.
#include "completion/posix.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <string>
#include <unistd.h>

namespace j = kronuz::journal;
namespace fs = std::filesystem;

namespace {
void check(bool value, const char* what, std::source_location where = std::source_location::current()) {
	if (!value) {
		std::cerr << "failed at line " << where.line() << ": " << what << '\n';
		std::exit(1);
	}
}

fs::path fresh(const fs::path& path, fs::perms mode = fs::perms::owner_all) {
	fs::remove_all(path);
	fs::create_directories(path);
	fs::permissions(path, mode);
	return path;
}
template <class F> bool refuses(F&& open) {
	try { open(); } catch (const std::exception&) { return true; }
	return false;
}
// Opens `path` under `options`; refused by default, admitted by exactly `admit`.
bool opens(const fs::path& path, j::PosixIOOptions options) {
	return !refuses([&] { j::PosixIO io(path, 0600, options); });
}
j::DirectoryTrust trust_of(const fs::path& path, j::PosixIOOptions options) {
	j::PosixIO io(path, 0600, options);
	const j::IO& base = io;
	check(base.directory_trust() == io.directory_trust(), "PosixIO answers through the base");
	return io.directory_trust();
}

constexpr j::PosixIOOptions none{}, follow{true, false}, shared{false, true}, both{true, true};

struct UnknownIO final : j::IO {
	std::unique_ptr<j::OwnerLock> acquire_owner(bool) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> open_existing(std::string_view) override { throw std::logic_error("unused"); }
	std::unique_ptr<j::File> create_exclusive(std::string_view) override { throw std::logic_error("unused"); }
	void replace(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
	void remove(std::string_view) override { throw std::logic_error("unused"); }
	void sync_directory() override { throw std::logic_error("unused"); }
};

void clean(const fs::path& root) {
	const auto dir = fresh(root / "clean");
	for (auto options : {none, follow, shared, both}) {
		check(opens(dir, options), "an owned, private directory opens under every option");
		const auto trust = trust_of(dir, options);
		check(trust.known && trust.trusted() && !trust.symlinked_root && !trust.foreign_owner && !trust.shared_writable,
			"and is reported trusted");
	}
	j::PosixIO with_context(kronuz::io::completion::AllocationContext{}, dir, 0600, both);
	check(with_context.directory_trust().trusted(), "the allocation-context constructor takes the options");
}

void base_is_unknown() {
	UnknownIO io;
	const j::IO& base = io;
	check(base.directory_trust() == j::DirectoryTrust{}, "an IO that does not inspect reports unknown");
	check(!base.directory_trust().known && !base.directory_trust().trusted(), "and is not trusted");
}

void symlinked_root(const fs::path& root) {
	const auto target = fresh(root / "target");
	const auto link = root / "link";
	fs::remove(link);
	fs::create_directory_symlink(target, link);
	check(!opens(link, none), "a symlinked root is refused by default");
	check(!opens(link, shared), "allow_shared_directory does not admit a symlink");
	try { j::PosixIO io(link); check(false, "unreachable"); } catch (const std::system_error&) {}
	check(opens(link, follow) && opens(link, both), "follow_root_symlink admits it");
	const auto trust = trust_of(link, follow);
	check(trust.known && trust.symlinked_root && trust.trusted(), "and reports it symlinked, otherwise trusted");
	check(!trust_of(target, follow).symlinked_root, "a plain root with the option is not symlinked");

	// The descriptor is held: retargeting the link afterwards moves nothing.
	j::PosixIO io(link, 0600, follow);
	const auto other = fresh(root / "other");
	fs::remove(link);
	fs::create_directory_symlink(other, link);
	io.create_exclusive("held.bin")->close();
	check(fs::exists(target / "held.bin") && !fs::exists(other / "held.bin"), "the held descriptor stays on the opened directory");

	// A symlink to a file, a dangling one, and a loop stay refused.
	{ std::ofstream(root / "file") << "x"; }
	fs::remove(link);
	fs::create_symlink(root / "file", link);
	check(!opens(link, both), "a symlink to a file is refused");
	fs::remove(link);
	fs::create_directory_symlink(root / "missing", link);
	check(!opens(link, both), "a dangling symlink is refused");
	fs::remove(link);
	fs::create_directory_symlink(link, link);
	check(!opens(link, both), "a symlink loop is refused");
	fs::remove(link);

	// Entries inside the directory stay no-follow.
	const auto inner = fresh(root / "inner");
	fs::create_symlink(root / "file", inner / "entry");
	j::PosixIO inside(root / "inner", 0600, both);
	check(refuses([&] { (void)inside.open_existing("entry"); }), "a symlink entry is refused under both options");
}

void shared_writable(const fs::path& root) {
	struct Case { const char* name; fs::perms mode; };
	for (auto [name, mode] : {Case{"group-writable", fs::perms::owner_all | fs::perms::group_write},
			Case{"other-writable", fs::perms::owner_all | fs::perms::others_write},
			Case{"group- and other-writable", fs::perms::owner_all | fs::perms::group_write | fs::perms::others_write}}) {
		const auto dir = fresh(root / "shared", mode);
		check(!opens(dir, none), name);
		check(!opens(dir, follow), name);
		check(opens(dir, shared) && opens(dir, both), name);
		const auto trust = trust_of(dir, shared);
		check(trust.known && trust.shared_writable && !trust.foreign_owner && !trust.symlinked_root && !trust.trusted(), name);
		// Through a symlink, both options are needed.
		const auto link = root / "shared-link";
		fs::remove(link);
		fs::create_directory_symlink(dir, link);
		check(!opens(link, follow) && !opens(link, shared) && opens(link, both), name);
		const auto through = trust_of(link, both);
		check(through.symlinked_root && through.shared_writable, name);
		fs::remove(link);
		check(opens(dir, shared), "an admitted directory is usable");
		{ j::PosixIO io(dir, 0600, shared); io.create_exclusive("a.bin")->close(); }
	}
	fs::remove_all(root / "shared");
}

// Foreign ownership: root's directory for an ordinary user; for root, one chowned away.
void foreign_owner(const fs::path& root) {
	fs::path dir = "/";
	if (::geteuid() == 0) {
		dir = fresh(root / "foreign");
		if (::chown(dir.c_str(), 12345, static_cast<gid_t>(-1))) { std::cout << "foreign owner: skipped (chown failed)\n"; return; }
	}
	check(!opens(dir, none), "a foreign owner is refused by default");
	check(!opens(dir, follow), "follow_root_symlink does not admit a foreign owner");
	check(opens(dir, shared) && opens(dir, both), "allow_shared_directory admits it");
	const auto trust = trust_of(dir, shared);
	check(trust.known && trust.foreign_owner && !trust.symlinked_root && !trust.trusted(), "and reports it");
	const auto link = root / "foreign-link";
	fs::remove(link);
	fs::create_directory_symlink(dir, link);
	check(!opens(link, follow) && !opens(link, shared) && opens(link, both), "a symlink to it needs both options");
	const auto through = trust_of(link, both);
	check(through.symlinked_root && through.foreign_owner, "and reports both");
	fs::remove(link);
}
} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::cerr << "usage: completion_dir_trust_test DIRECTORY\n";
		return 2;
	}
	const fs::path root = fs::absolute(argv[1]);
	fs::remove_all(root);
	fs::create_directories(root);
	fs::permissions(root, fs::perms::owner_all);
	clean(root);
	base_is_unknown();
	symlinked_root(root);
	shared_writable(root);
	foreign_owner(root);
	fs::remove_all(root);
	std::cout << "dir_trust: ok\n";
	return 0;
}

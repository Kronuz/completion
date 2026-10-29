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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace kronuz::journal {

enum class EntryKind { Regular, Directory, Symlink, Other };
struct EntryFootprint {
	EntryKind kind = EntryKind::Other;
	std::uint64_t logical_bytes = 0;
	std::optional<std::uint64_t> allocated_bytes;
	bool operator==(const EntryFootprint&) const = default;
};
struct EntryInspection {
	EntryFootprint footprint;
	std::uint64_t device = 0, inode = 0, owner = 0, links = 0;
	std::uint32_t permissions = 0;
	bool operator==(const EntryInspection&) const = default;
};
enum class LeaseMode { Shared, Exclusive };
struct ManagedCapabilities {
	bool caller_scan = false, entry_inspection = false, file_inspection = false;
	bool nonblocking_leases = false, file_close = false;
	bool cursor_close = false, reusable_controls = false;
	// Separate mutation capability; complete() retains its collector-profile meaning.
	bool reusable_create = false;
	bool complete() const noexcept {
		return caller_scan && entry_inspection && file_inspection && nonblocking_leases && file_close && cursor_close && reusable_controls;
	}
};

// A read-only view of a whole file as it was when mapped, valid while the handle lives,
// even after the file is unlinked or replaced.
class Mapping {
public:
	virtual ~Mapping() = default;
	std::string_view bytes() const noexcept { return bytes_; }
protected:
	std::string_view bytes_;
};

// Implementations own a trusted, already-created directory. Names are single
// components; ownership and file-type checks belong to the implementation.
class File {
public:
	virtual ~File() = default;
	virtual std::uint64_t size() = 0;
	virtual std::size_t read_at(std::uint64_t offset, std::span<char> bytes) = 0;
	virtual std::size_t write_at(std::uint64_t offset, std::string_view bytes) = 0;
	virtual void truncate(std::uint64_t size) = 0;
	virtual void sync() = 0;
	// Makes written bytes durable, with the metadata needed to read them back; other
	// metadata (times) may lag. For preallocated files, where no write changes the size.
	virtual void sync_data() { sync(); }
	virtual EntryInspection inspect() { throw std::logic_error("held-file inspection unsupported"); }
	// Fresh handles only: converting a shared lock may release its protection.
	// False means busy, never a blocking wait. Errors remain exceptional.
	virtual bool try_lease(LeaseMode) { throw std::logic_error("nonblocking file leases unsupported"); }
	// Called by a retained worker-side original after preceding work settles.
	// Supporting backends consume the native handle even on error; never retry.
	virtual void close() { throw std::logic_error("explicit file close unsupported"); }
	// The whole file, read-only, without copying it; null when the backend cannot map,
	// so callers read instead. Writes through other handles are not promised to show.
	virtual std::shared_ptr<const Mapping> map() { return nullptr; }
};

class OwnerLock {
public:
	virtual ~OwnerLock() = default;
};

// Which inodes an owner lock holds: owner.lock, the directory itself, or both (the
// directory first, released last). Only pairings involving `both` exclude every other mode;
// `file` and `directory` lock different inodes and do not exclude each other.
enum class OwnerLockMode { file, directory, both };
// How file and both modes reach owner.lock. A directory-only lock takes no file.
enum class OwnerFile { open_existing, create_exclusive };
struct OwnerLockOptions {
	OwnerLockMode mode = OwnerLockMode::both;
	OwnerFile owner_file = OwnerFile::open_existing;
	// What the legacy acquire_owner(bool) and acquire_directory_owner() take.
	static constexpr OwnerLockOptions legacy_file(bool create) noexcept {
		return {OwnerLockMode::file, create ? OwnerFile::create_exclusive : OwnerFile::open_existing};
	}
	static constexpr OwnerLockOptions legacy_directory() noexcept {
		return {OwnerLockMode::directory, OwnerFile::open_existing};
	}
	bool operator==(const OwnerLockOptions&) const = default;
};

class DirectoryCursor {
public:
	virtual ~DirectoryCursor() = default;
	// One bounded basename, or end of pass. Concurrent namespace mutations
	// may cause repeats/omissions; callers must recheck roots before removal.
	virtual std::optional<std::string> next() = 0;
	// Managed callers supply retained scratch storage. A present result is the
	// number of copied name bytes (no NUL); null means end of pass. The default
	// deliberately does not call the allocating legacy interface.
	virtual std::optional<std::size_t> next_into(std::span<char>) {
		throw std::logic_error("caller-buffer directory scanning unsupported");
	}
	virtual void close() { throw std::logic_error("explicit cursor close unsupported"); }
};

// A directory's filesystem as classified for the caller to warn on; the lock never refuses by it.
struct FilesystemClass {
	bool local = false;  // known local, so a directory flock is known to exclude every other holder
	std::string name;
	bool operator==(const FilesystemClass&) const = default;
};

// What the directory's root was found to be, so a caller can warn without the IO refusing.
// `known` is false for an IO that does not inspect its directory; the flags are then unset
// and say nothing, so a caller that wants a trusted directory checks trusted().
struct DirectoryTrust {
	bool known = false;
	bool symlinked_root = false;   // the final component was a symlink, followed to the directory
	bool foreign_owner = false;    // not owned by the effective uid
	bool shared_writable = false;  // writable by group or others
	bool trusted() const noexcept { return known && !foreign_owner && !shared_writable; }
	bool operator==(const DirectoryTrust&) const = default;
};

class IO {
public:
	// Only the deleted overload below takes this; its name is the diagnostic for `{}`.
	enum class braced_owner_options_need_a_type {};
	virtual ~IO() = default;
	// The directory's filesystem. An IO that does not classify reports it unclassified, not
	// local, so a caller that refuses non-local filesystems refuses it. A wrapper forwards.
	virtual FilesystemClass directory_filesystem() const { return {false, "unclassified"}; }
	// What the directory's root was found to be. An IO that does not inspect it reports it
	// unknown, so a caller that refuses untrusted directories refuses it. A wrapper forwards.
	virtual DirectoryTrust directory_trust() const { return {}; }
	virtual ManagedCapabilities managed_capabilities() const noexcept { return {}; }
	// The owner lock, held while the returned lock lives. Creation is exclusive; recovery
	// opens the existing stable owner.lock; neither manifest replacement nor fencing releases
	// it. Successfully creating adds exactly one zero-length owner.lock and no other entries
	// (format-layer bootstrap accounting relies on this); a directory-only lock writes
	// nothing. Busy is EWOULDBLOCK and an unsupported lock errc::not_supported, in every
	// mode, with no fallback to another mode. A failed create may leave owner.lock behind,
	// for open_existing to recover. `acquire_owner({})` is refused at compile time: it would
	// otherwise take the legacy file-only bool overload. Name the options (`OwnerLockOptions{}`
	// is `both`).
	[[nodiscard]] std::unique_ptr<OwnerLock> acquire_owner(const OwnerLockOptions& options) {
		if (options.mode != OwnerLockMode::file && options.mode != OwnerLockMode::directory &&
			options.mode != OwnerLockMode::both) {
			throw std::invalid_argument("unknown owner lock mode");
		}
		if (options.owner_file != OwnerFile::open_existing && options.owner_file != OwnerFile::create_exclusive) {
			throw std::invalid_argument("unknown owner file policy");
		}
		if (options.mode == OwnerLockMode::directory && options.owner_file != OwnerFile::open_existing) {
			throw std::invalid_argument("a directory owner lock creates no file");
		}
		return lock_owner(options);
	}
	// `{}` converts identically to this and to bool, so it is ambiguous; true, false, a bool
	// and 0 reach only the bool overload, as no implicit conversion yields a scoped enum.
	void acquire_owner(braced_owner_options_need_a_type) = delete;
	// Legacy presets, kept source-compatible: OwnerLockOptions::legacy_file(create) and
	// legacy_directory(). A backend implementing lock_owner forwards both to it.
	virtual std::unique_ptr<OwnerLock> acquire_owner(bool create) = 0;
	virtual std::unique_ptr<OwnerLock> acquire_directory_owner() {
		throw std::logic_error("directory owner lock unsupported");
	}
	// Admit reusable closed controls during startup. Into hooks use this same
	// backend, reject live/foreign destinations before IO, and allocate no control.
	virtual std::unique_ptr<File> make_closed_file() { throw std::logic_error("reusable file controls unsupported"); }
	// No control allocation. With a valid closed destination, failure consumes
	// temporary descriptors but may leave an exclusively created name behind.
	virtual void create_exclusive_into(std::string_view, File&) { throw std::logic_error("reusable creation unsupported"); }
	virtual std::unique_ptr<DirectoryCursor> make_closed_cursor() { throw std::logic_error("reusable cursor controls unsupported"); }
	virtual void open_existing_into(std::string_view, File&) { throw std::logic_error("reusable file open unsupported"); }
	virtual bool open_reclaim_candidate_into(std::string_view, File&) { throw std::logic_error("reusable candidate open unsupported"); }
	virtual void scan_directory_into(DirectoryCursor&) { throw std::logic_error("reusable cursor scan unsupported"); }
	virtual std::unique_ptr<File> open_existing(std::string_view name) = 0;
	virtual std::unique_ptr<File> create_exclusive(std::string_view name) = 0;
	virtual void replace(std::string_view source, std::string_view destination) = 0;
	// Missing files are an idempotent success.
	virtual void remove(std::string_view name) = 0;
	virtual std::unique_ptr<DirectoryCursor> scan_directory() { throw std::logic_error("directory scanning unsupported"); }
	// Inspect the directory entry itself without following symlinks or opening
	// its target. Missing names return null; genuine errors throw. A quiescent
	// inventory requires exact-once enumeration with no concurrent mutations.
	virtual std::optional<EntryFootprint> entry_footprint(std::string_view) { throw std::logic_error("entry accounting unsupported"); }
	virtual std::optional<EntryInspection> inspect_entry(std::string_view) { throw std::logic_error("entry inspection unsupported"); }
	// Null means absent or unsafe/nonregular: preserve it. Genuine I/O
	// failures throw. A backend supporting reclamation overrides both hooks.
	virtual std::unique_ptr<File> open_reclaim_candidate(std::string_view name) { return open_existing(name); }
	virtual void sync_directory() = 0;
	// Makes every file durable, as File::sync on each would; a backend may share one
	// device flush among them. After a failure, which files are durable is unspecified.
	virtual void sync_files(std::span<File* const> files) {
		for (auto* file : files) { file->sync(); }
	}
	// Makes every file durable and the directory's entries durable (creations, replacements and
	// removals so far), as sync_files(files) then sync_directory() would; a backend may share
	// one device flush among them. After a failure, which are durable is unspecified.
	virtual void sync_files_and_directory(std::span<File* const> files) {
		sync_files(files);
		sync_directory();
	}
	// An ordering point before a publication. On return the files' bytes and the directory's
	// entries so far (creations, replacements and removals) are staged ahead of every write issued
	// after the call: a replace() made next cannot reach stable storage without them. They are
	// not necessarily durable on return; the next sync_directory() makes them, and everything
	// after, durable. A backend without an ordering-only barrier makes them durable here.
	virtual void order_files_and_directory(std::span<File* const> files) { sync_files_and_directory(files); }

protected:
	// The one owner-lock implementation, given validated options. A backend that cannot
	// exclude in a mode throws not_supported there; it never returns a lock that excludes
	// nothing. The default supports no mode and never forwards to the legacy calls.
	virtual std::unique_ptr<OwnerLock> lock_owner(const OwnerLockOptions&) {
		throw std::system_error(std::make_error_code(std::errc::not_supported), "owner lock options unsupported by this IO");
	}
};

} // namespace kronuz::journal

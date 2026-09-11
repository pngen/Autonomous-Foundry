#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"

// Filesystem safety.
//
// Candidate output is produced by an untrusted external process. Every path the
// runtime derives from worker-influenced input is canonicalized, checked to be
// lexically inside its workspace root, checked component by component for
// reparse points, and rejected on any violation. Publication is transactional:
// content is written to a staging file, flushed and closed, then atomically
// replaced into place.

namespace autonomous_foundry {

/// Longest accepted relative path, in bytes.
inline constexpr std::size_t kMaxRelativePathLength = 200;

/// Validate a relative path produced by an untrusted source.
///
/// Rejects: empty paths, absolute paths, drive-relative paths ("C:foo"),
/// UNC prefixes, any ".." component, "." components, empty components,
/// backslash separators on platforms where they are not separators, control
/// characters, characters outside printable ASCII, trailing dots or spaces,
/// over-long paths, and Windows reserved device names.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_relative_path(std::string_view relative);

/// True when a single path component is a Windows reserved device name.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool is_reserved_device_name(std::string_view name) noexcept;

/// A directory that confines all derived paths.
class AUTONOMOUS_FOUNDRY_API WorkspaceRoot {
 public:
  WorkspaceRoot() = default;
  explicit WorkspaceRoot(std::filesystem::path root);

  /// Create a fresh empty workspace below base. Fails if it already exists and
  /// is not empty, so that a stale directory can never be mistaken for this
  /// attempt's output.
  [[nodiscard]] static Result<WorkspaceRoot> create(const std::filesystem::path& base,
                                                    std::string_view leaf);

  /// Adopt an existing directory after validating that it is a plain
  /// directory and not a reparse point.
  [[nodiscard]] static Result<WorkspaceRoot> open_existing(const std::filesystem::path& root);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }
  [[nodiscard]] bool valid() const noexcept { return !root_.empty(); }

  [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view relative) const;

  Status ensure_directory(std::string_view relative) const;
  Status write_file(std::string_view relative, std::string_view content) const;

  [[nodiscard]] Result<std::string> read_file(std::string_view relative,
                                              std::uint64_t max_bytes) const;

  /// Deterministically sorted relative paths of the regular files directly
  /// inside a directory of this workspace, bounded by max_entries.
  [[nodiscard]] Result<std::vector<std::string>> list_files(std::string_view relative,
                                                            std::size_t max_entries) const;

  /// Remove the whole workspace tree. Bounded: refuses trees deeper than
  /// kMaxCleanupDepth or with more than kMaxCleanupEntries entries.
  Status remove_all() const;

 private:
  std::filesystem::path root_;
};

inline constexpr std::size_t kMaxCleanupDepth = 32;
inline constexpr std::size_t kMaxCleanupEntries = 100000;

/// Verify a path is a plain directory: exists, is a directory, and is not a
/// symlink, junction or other reparse point.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status ensure_directory_plain(
    const std::filesystem::path& path);

/// Verify that no component of child below ancestor is a reparse point.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status verify_no_reparse_escape(
    const std::filesystem::path& ancestor, const std::filesystem::path& child);

/// Write bytes to a temporary sibling file, flush and close it, then atomically
/// replace the target. On Windows this uses ReplaceFileW/MoveFileExW rather
/// than assuming POSIX rename semantics.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status atomic_write_file(
    const std::filesystem::path& target, std::string_view bytes);

/// Read at most max_bytes. Fails with LengthOutOfRange when the file is
/// larger, so that a hostile file cannot drive an unbounded allocation.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> read_file_bounded(
    const std::filesystem::path& path, std::uint64_t max_bytes);

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::uint64_t> file_size(
    const std::filesystem::path& path);

/// Bounded recursive removal.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status remove_tree_bounded(const std::filesystem::path& path);

/// UTF-8 rendering of a path, for diagnostics. Never used to construct paths.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string path_to_utf8(const std::filesystem::path& p);

/// Create a unique temporary directory below base.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::filesystem::path> make_unique_directory(
    const std::filesystem::path& base, std::string_view prefix);

}  // namespace autonomous_foundry

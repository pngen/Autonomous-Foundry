#include "autonomous_foundry/workspace.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

// Filesystem trust boundary.
//
// Every path in this translation unit originates from a worker-influenced
// string. Validation is deliberately stricter than either platform: a path
// that Windows would quietly reinterpret (reserved device names, trailing
// dots and spaces, drive-relative prefixes) or that POSIX would follow
// (intermediate symlinks) is rejected before it reaches the filesystem, so
// Windows and POSIX accept exactly the same set of relative paths.

namespace autonomous_foundry {
namespace {

[[nodiscard]] bool is_path_separator(char byte) noexcept {
  return byte == '/' || byte == '\\';
}

[[nodiscard]] char to_lower_ascii(char byte) noexcept {
  if (byte >= 'A' && byte <= 'Z') {
    return static_cast<char>(byte - 'A' + 'a');
  }
  return byte;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (to_lower_ascii(left[index]) != to_lower_ascii(right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool same_component(const std::filesystem::path& left,
                                   const std::filesystem::path& right) {
#if defined(_WIN32)
  return ascii_iequals(left.string(), right.string());
#else
  return left == right;
#endif
}

[[nodiscard]] bool is_component_separator(char byte) noexcept {
  return byte == '.' || byte == ' ' || byte == ':' || is_path_separator(byte);
}

// Index of the last byte Windows would keep, or npos when the component holds
// nothing but the spaces and dots the platform strips.
[[nodiscard]] std::size_t last_effective_index(std::string_view name) noexcept {
  std::size_t index = name.size();
  while (index > 0) {
    const char byte = name[index - 1];
    if (byte != ' ' && byte != '.') {
      return index - 1;
    }
    --index;
  }
  return std::string_view::npos;
}

// The name Windows would actually open, i.e. everything before the first dot,
// colon or separator.
[[nodiscard]] std::string_view device_name_prefix(std::string_view name) noexcept {
  std::size_t index = 0;
  while (index < name.size() && !is_component_separator(name[index])) {
    ++index;
  }
  return name.substr(0, index);
}

// Offending-component diagnostics are truncated so that a hostile megabyte
// path cannot be reflected back in full through an error message.
[[nodiscard]] std::string describe_component(std::string_view name) {
  constexpr std::size_t kMaxQuotedComponent = 64;
  const std::string quoted(name.substr(0, kMaxQuotedComponent));
  if (name.size() <= kMaxQuotedComponent) {
    return "'" + quoted + "'";
  }
  return "'" + quoted + "...'";
}

[[nodiscard]] Status compose(ErrorCode code, std::string_view detail,
                             std::string_view offending) {
  std::string message(detail);
  if (!offending.empty()) {
    message.push_back(' ');
    message.append(offending);
  }
  return Status(code, message);
}

// Resolves a child path and reports whether it is lexically inside ancestor.
// Comparisons are component-wise, lexically normal and case-insensitive on
// Windows, which matches how the platform resolves paths.
[[nodiscard]] bool is_lexically_inside(const std::filesystem::path& ancestor,
                                       const std::filesystem::path& child) noexcept {
  try {
    if (ancestor.empty()) {
      return false;
    }
    const std::filesystem::path root = ancestor.lexically_normal();
    const std::filesystem::path candidate = child.lexically_normal();
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
      if (candidate_it == candidate.end()) {
        return false;
      }
      const std::string root_part = root_it->string();
      const std::string candidate_part = candidate_it->string();
#if defined(_WIN32)
      if (!ascii_iequals(root_part, candidate_part)) {
        return false;
      }
#else
      if (root_part != candidate_part) {
        return false;
      }
#endif
    }
    return true;
  } catch (const std::filesystem::filesystem_error&) {
    return false;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

enum class reparse_state { no, yes, indeterminate };

// A component that does not exist yet (the target of a pending write, or a
// directory the caller is about to create) is simply not a reparse point; only a
// genuine inspection failure is indeterminate.
[[nodiscard]] reparse_state probe_reparse_point(const std::filesystem::path& path,
                                                std::error_code& failure) noexcept {
  failure.clear();
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, failure);
  if (failure) {
    failure.clear();
  } else if (std::filesystem::is_symlink(status)) {
    return reparse_state::yes;
  }
#if defined(_WIN32)
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_INVALID_NAME || code == ERROR_BAD_PATHNAME) {
      return reparse_state::no;
    }
    failure.assign(static_cast<int>(code), std::system_category());
    return reparse_state::indeterminate;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return reparse_state::yes;
  }
#endif
  return reparse_state::no;
}

[[nodiscard]] std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

// Walks the tree that starts at root with an explicit stack so that a hostile
// depth cannot exhaust the native stack. Directories are counted when they are
// popped, files when they are pushed, but always with a depth of at least one,
// so an empty directory still counts as exactly one entry.
[[nodiscard]] Status walk_bounded(const std::filesystem::path& root) {
  struct frame {
    std::filesystem::path path;
    std::size_t depth;
  };
  std::vector<frame> pending;
  pending.push_back(frame{root, 0});
  std::size_t entries = 0;
  std::error_code failure;
  while (!pending.empty()) {
    frame current = std::move(pending.back());
    pending.pop_back();
    ++entries;
    if (entries > kMaxCleanupEntries) {
      return compose(ErrorCode::ResourceExhausted,
                     "cleanup refused: tree exceeds the entry bound near", root.string());
    }
    const std::size_t child_depth = current.depth + 1;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(current.path, failure);
    if (failure) {
      return compose(ErrorCode::IoFailure, "cleanup could not inspect",
                     current.path.string());
    }
    if (!std::filesystem::is_directory(status)) {
      continue;
    }
    std::filesystem::directory_iterator iterator(current.path, failure);
    if (failure) {
      return compose(ErrorCode::IoFailure, "cleanup could not open", current.path.string());
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
      std::error_code step;
      const std::filesystem::directory_entry entry = *iterator;
      if (entry.is_directory(step) && !entry.is_symlink(step)) {
        if (child_depth > kMaxCleanupDepth) {
          return compose(ErrorCode::ResourceExhausted,
                         "cleanup refused: tree exceeds the depth bound at",
                         entry.path().string());
        }
        pending.push_back(frame{entry.path(), child_depth});
      } else {
        ++entries;
        if (entries > kMaxCleanupEntries) {
          return compose(ErrorCode::ResourceExhausted,
                         "cleanup refused: tree exceeds the entry bound near",
                         root.string());
        }
      }
      iterator.increment(step);
      if (step) {
        return compose(ErrorCode::IoFailure, "cleanup could not enumerate",
                       current.path.string());
      }
    }
  }
  return Status();
}

// Replacement step shared by the Windows and POSIX publication paths. The
// caller removes the staging file afterwards on every branch.
[[nodiscard]] Status replace_with_staging(const std::filesystem::path& target,
                                          const std::filesystem::path& staging) {
#if defined(_WIN32)
  // A concurrent publisher that swaps the target between our own check and the
  // replace call makes these calls fail with a sharing violation. Each caller
  // owns a private staging file, so re-offering it is safe: the attempts yield
  // the processor so competing writers retire their own staging files instead
  // of spinning against each other, and the loop converges.
  constexpr unsigned long kReplaceAttempts = 10000UL;
  DWORD replace_error = ERROR_SUCCESS;
  DWORD move_error = ERROR_SUCCESS;
  bool replaced = false;
  for (unsigned long attempt = 0; attempt < kReplaceAttempts; ++attempt) {
    if (::ReplaceFileW(target.c_str(), staging.c_str(), nullptr,
                       REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != 0) {
      replaced = true;
      break;
    }
    replace_error = ::GetLastError();
    if (::MoveFileExW(staging.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      replaced = true;
      break;
    }
    move_error = ::GetLastError();
    if (move_error == ERROR_FILE_NOT_FOUND) {
      // Nobody holds the target open any more: the fallback path below is safe.
      break;
    }
    std::this_thread::yield();
  }
  if (replaced) {
    return Status();
  }
  // Both atomic replacement calls failed. The only remaining question is
  // whether the target exists.
  //
  // When it does not, a plain rename publishes the staging file in a single
  // step and is atomic, so it is the correct publication.
  //
  // When it does exist, the previous durable image must NOT be deleted to make
  // room for the replacement. Removing the target and then renaming is not
  // atomic: a crash, a power loss, or a second failure between the two steps
  // would destroy the only durable copy of the state and leave nothing behind.
  // Refusing keeps the previous image intact and hands the failure to the
  // caller, which is the only outcome that preserves durability.
  std::error_code present;
  const bool target_exists = std::filesystem::exists(target, present);
  if (present) {
    return compose(ErrorCode::PersistenceIoFailure,
                   "atomic replace could not inspect the target", target.string());
  }
  if (target_exists) {
    std::ostringstream message;
    message << "atomic replace failed after " << kReplaceAttempts
            << " attempts (ReplaceFileW " << static_cast<unsigned long>(replace_error)
            << ", MoveFileExW " << static_cast<unsigned long>(move_error)
            << ") for " << target.string()
            << "; the previous durable image was left intact";
    return Status(ErrorCode::PersistenceIoFailure, message.str());
  }
  std::error_code renamed;
  std::filesystem::rename(staging, target, renamed);
  if (renamed) {
    return compose(ErrorCode::PersistenceIoFailure,
                   "atomic rename of a new target failed", target.string());
  }
  return Status();
#else
  // std::rename is atomic within one POSIX filesystem and replaces the target
  // in a single step.
  if (std::rename(staging.string().c_str(), target.string().c_str()) == 0) {
    return Status();
  }
  return compose(ErrorCode::PersistenceIoFailure,
                 "atomic rename failed for", target.string());
#endif
}

[[nodiscard]] Status write_staging_file(const std::filesystem::path& target,
                                        const std::filesystem::path& staging,
                                        std::string_view bytes) {
  {
    std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      std::error_code ignored;
      std::filesystem::remove(staging, ignored);
      return compose(ErrorCode::PersistenceIoFailure, "could not create staging file",
                     staging.string());
    }
    if (!bytes.empty()) {
      stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
      stream.close();
      std::error_code ignored;
      std::filesystem::remove(staging, ignored);
      return compose(ErrorCode::PersistenceIoFailure, "could not write staging file",
                     staging.string());
    }
    stream.close();
    if (stream.fail()) {
      std::error_code ignored;
      std::filesystem::remove(staging, ignored);
      return compose(ErrorCode::PersistenceIoFailure, "could not close staging file",
                     staging.string());
    }
  }
  // The staging file is unconditional cleanup: it must not survive either the
  // success or the failure branch.
  const Status replaced = replace_with_staging(target, staging);
  std::error_code ignored;
  std::filesystem::remove(staging, ignored);
  return replaced;
}

[[nodiscard]] std::filesystem::path weakly_canonical_or(const std::filesystem::path& value,
                                                        const std::filesystem::path& fallback) {
  std::error_code failure;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(value, failure);
  if (failure) {
    return fallback.lexically_normal();
  }
  return canonical;
}

[[nodiscard]] Result<std::uint64_t> bounded_file_size(const std::filesystem::path& path) {
  std::error_code failure;
  const std::filesystem::file_status status = std::filesystem::status(path, failure);
  if (failure) {
    return compose(ErrorCode::IoFailure, "cannot stat", path.string());
  }
  if (!std::filesystem::exists(status)) {
    return compose(ErrorCode::IoFailure, "path does not exist", path.string());
  }
  if (!std::filesystem::is_regular_file(status)) {
    return compose(ErrorCode::IoFailure, "path is not a regular file", path.string());
  }
  const std::uintmax_t reported = std::filesystem::file_size(path, failure);
  if (failure) {
    return compose(ErrorCode::IoFailure, "cannot size", path.string());
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(reported));
}

[[nodiscard]] std::string to_utf8_bytes(const std::filesystem::path& input) {
#if defined(__cpp_lib_char8_t)
  const std::u8string encoded = input.u8string();
  return std::string(encoded.begin(), encoded.end());
#else
  return input.u8string();
#endif
}

}  // namespace

bool is_reserved_device_name(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  // Windows strips trailing spaces before it decides, so "CON " and "CON" name
  // the same device. Only an unadorned name is compared verbatim.
  std::string_view effective = name;
  {
    const std::size_t last = last_effective_index(effective);
    effective = (last == std::string_view::npos) ? std::string_view() : effective.substr(0, last + 1);
  }
  if (effective.empty()) {
    return false;
  }
  const std::string_view candidate = device_name_prefix(effective);
  if (candidate.empty()) {
    return false;
  }
  if (candidate.size() == 3) {
    return ascii_iequals(candidate, "CON") || ascii_iequals(candidate, "PRN") ||
           ascii_iequals(candidate, "AUX") || ascii_iequals(candidate, "NUL");
  }
  if (candidate.size() == 4) {
    const bool prefix = ascii_iequals(candidate.substr(0, 3), "COM") ||
                        ascii_iequals(candidate.substr(0, 3), "LPT");
    return prefix && candidate[3] >= '0' && candidate[3] <= '9';
  }
  return false;
}

Status validate_relative_path(std::string_view relative) {
  if (relative.empty()) {
    return Status(ErrorCode::UnsafePath, "relative path is empty");
  }
  if (relative.size() > kMaxRelativePathLength) {
    std::ostringstream message;
    message << "relative path length " << relative.size() << " exceeds the limit "
            << kMaxRelativePathLength;
    return Status(ErrorCode::LengthOutOfRange, message.str());
  }

  if (is_path_separator(relative[0])) {
    if (relative.size() >= 2 && is_path_separator(relative[1])) {
      return Status(ErrorCode::PathEscape, "UNC path prefixes are not permitted");
    }
    return Status(ErrorCode::PathEscape, "absolute paths are not permitted");
  }
  for (const char byte : relative) {
    if (byte == ':') {
      return Status(ErrorCode::PathEscape,
                    "drive and stream prefixes are not permitted: ':' is rejected outright");
    }
  }

  std::size_t index = 0;
  for (;;) {
    std::size_t end = index;
    while (end < relative.size() && !is_path_separator(relative[end])) {
      ++end;
    }
    const std::string_view component = relative.substr(index, end - index);
    const std::string quoted = describe_component(component);
    if (component.empty()) {
      return compose(ErrorCode::UnsafePath, "empty path components are not permitted:", quoted);
    }
    if (component == ".") {
      return compose(ErrorCode::PathEscape, "'.' components are not permitted:", quoted);
    }
    if (component == "..") {
      return compose(ErrorCode::PathEscape, "'..' components are not permitted:", quoted);
    }
    for (const char byte : component) {
      const unsigned char value = static_cast<unsigned char>(byte);
      if (value < 0x20U || value > 0x7EU) {
        return compose(ErrorCode::UnsafePath,
                       "component contains a control character or a byte outside printable ASCII:",
                       quoted);
      }
    }
    if (is_reserved_device_name(component)) {
      return compose(ErrorCode::UnsafePath, "component is a reserved device name:", quoted);
    }
    const char final_byte = component.back();
    if (final_byte == '.' || final_byte == ' ') {
      return compose(ErrorCode::UnsafePath,
                     "component ends with a dot or a space, which Windows strips:", quoted);
    }
    const char first = component.front();
    if (first == '<' || first == '>' || first == '"' || first == '|' || first == '?' ||
        first == '*') {
      return compose(ErrorCode::UnsafePath,
                     "component begins with a character Windows rejects in file names:", quoted);
    }
    if (end >= relative.size()) {
      break;
    }
    index = end + 1;
    if (index >= relative.size()) {
      break;
    }
  }
  return Status();
}

WorkspaceRoot::WorkspaceRoot(std::filesystem::path root) : root_(std::move(root)) {}

Result<WorkspaceRoot> WorkspaceRoot::create(const std::filesystem::path& base,
                                            std::string_view leaf) {
  AF_TRY(validate_relative_path(leaf));
  const std::filesystem::path target = base / std::filesystem::path(std::string(leaf));
  std::error_code failure;
  if (std::filesystem::exists(target, failure) && !failure) {
    std::filesystem::directory_iterator probe(target, failure);
    if (failure) {
      return compose(ErrorCode::WorkspaceFailure,
                     "workspace path exists but cannot be inspected", target.string());
    }
    if (probe != std::filesystem::directory_iterator()) {
      return compose(ErrorCode::WorkspaceFailure,
                     "workspace path already exists and is not empty", target.string());
    }
  }
  std::filesystem::create_directories(target, failure);
  if (failure) {
    return compose(ErrorCode::WorkspaceFailure, "cannot create workspace", target.string());
  }
  AF_TRY(ensure_directory_plain(target));
  return Result<WorkspaceRoot>(WorkspaceRoot(weakly_canonical_or(target, target)));
}

Result<WorkspaceRoot> WorkspaceRoot::open_existing(const std::filesystem::path& root) {
  AF_TRY(ensure_directory_plain(root));
  return Result<WorkspaceRoot>(WorkspaceRoot(weakly_canonical_or(root, root)));
}

Result<std::filesystem::path> WorkspaceRoot::resolve(std::string_view relative) const {
  AF_TRY(validate_relative_path(relative));
  if (root_.empty()) {
    return Status(ErrorCode::WorkspaceFailure, "workspace root is not initialized");
  }
  // A trailing separator ("dir/") names the same entry; the empty component it
  // would leave behind must not reach path::filename().
  std::string_view normalized = relative;
  while (!normalized.empty() && is_path_separator(normalized.back())) {
    normalized.remove_suffix(1);
  }
  const std::filesystem::path target = root_ / std::filesystem::path(std::string(normalized));
  const std::filesystem::path parent = target.parent_path();
  const std::filesystem::path resolved =
      weakly_canonical_or(parent, parent) / target.filename();
  const std::filesystem::path canonical_root = weakly_canonical_or(root_, root_);
  if (!is_lexically_inside(canonical_root, resolved)) {
    return compose(ErrorCode::PathEscape, "resolved path leaves the workspace root:",
                   resolved.string());
  }
  AF_TRY(verify_no_reparse_escape(canonical_root, resolved));
  return Result<std::filesystem::path>(resolved);
}

Status WorkspaceRoot::ensure_directory(std::string_view relative) const {
  std::filesystem::path target;
  AF_TRY_ASSIGN(target, resolve(relative));
  std::error_code failure;
  std::filesystem::create_directories(target, failure);
  if (failure) {
    return compose(ErrorCode::WorkspaceFailure, "cannot create directory", target.string());
  }
  return ensure_directory_plain(target);
}

Status WorkspaceRoot::write_file(std::string_view relative, std::string_view content) const {
  std::filesystem::path target;
  AF_TRY_ASSIGN(target, resolve(relative));
  return atomic_write_file(target, content);
}

Result<std::string> WorkspaceRoot::read_file(std::string_view relative,
                                             std::uint64_t max_bytes) const {
  std::filesystem::path target;
  AF_TRY_ASSIGN(target, resolve(relative));
  return read_file_bounded(target, max_bytes);
}

Result<std::vector<std::string>> WorkspaceRoot::list_files(std::string_view relative,
                                                           std::size_t max_entries) const {
  std::filesystem::path directory;
  AF_TRY_ASSIGN(directory, resolve(relative));
  AF_TRY(ensure_directory_plain(directory));

  std::vector<std::string> names;
  std::error_code failure;
  std::filesystem::directory_iterator iterator(directory, failure);
  if (failure) {
    return compose(ErrorCode::IoFailure, "cannot enumerate", directory.string());
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    std::error_code entry_failure;
    if (entry.is_regular_file(entry_failure) && !entry.is_symlink(entry_failure) &&
        !entry_failure) {
      const std::string name = entry.path().filename().string();
      if (names.size() >= max_entries) {
        std::ostringstream message;
        message << "directory holds more than " << max_entries << " entries: "
                << directory.string();
        return Status(ErrorCode::QueueCapacityExceeded, message.str());
      }
      names.push_back(name);
    }
    iterator.increment(failure);
    if (failure) {
      return compose(ErrorCode::IoFailure, "cannot enumerate", directory.string());
    }
  }
  std::sort(names.begin(), names.end());
  return Result<std::vector<std::string>>(std::move(names));
}

Status WorkspaceRoot::remove_all() const {
  if (root_.empty()) {
    return Status(ErrorCode::WorkspaceFailure, "workspace root is not initialized");
  }
  return remove_tree_bounded(root_);
}

Status ensure_directory_plain(const std::filesystem::path& path) {
  if (path.empty()) {
    return Status(ErrorCode::WorkspaceFailure, "workspace path is empty");
  }
  std::error_code failure;
  const std::filesystem::file_status link_status =
      std::filesystem::symlink_status(path, failure);
  if (failure) {
    return compose(ErrorCode::IoFailure, "cannot inspect", path.string());
  }
  if (!std::filesystem::exists(link_status)) {
    return compose(ErrorCode::IoFailure, "path does not exist", path.string());
  }
  // The plain-directory test follows the link on purpose: a junction or symlink
  // to a directory is reported as its own file type, and the reparse rejection
  // below is what actually refuses it.
  const std::filesystem::file_status target_status = std::filesystem::status(path, failure);
  if (failure) {
    return compose(ErrorCode::IoFailure, "cannot inspect", path.string());
  }
  if (!std::filesystem::is_directory(target_status)) {
    return compose(ErrorCode::WorkspaceFailure, "path is not a directory", path.string());
  }
  std::error_code reparse_failure;
  const reparse_state state = probe_reparse_point(path, reparse_failure);
  if (state == reparse_state::yes) {
    return compose(ErrorCode::ReparsePointRejected,
                   "path is a symlink, junction or other reparse point", path.string());
  }
  if (state == reparse_state::indeterminate) {
    return compose(ErrorCode::IoFailure, "cannot inspect", path.string());
  }
  return Status();
}

Status verify_no_reparse_escape(const std::filesystem::path& ancestor,
                                const std::filesystem::path& child) {
  const std::filesystem::path root = ancestor.lexically_normal();
  const std::filesystem::path target = child.lexically_normal();
  if (!is_lexically_inside(root, target)) {
    return compose(ErrorCode::PathEscape, "path is not inside the workspace root:",
                   target.string());
  }
  // The walk is lexical on purpose. std::filesystem::relative resolves the
  // path it is given, so a junction would be reported as ".." and the escape
  // would be missed before the junction itself was ever inspected.
  auto target_it = target.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++target_it) {
    if (target_it == target.end() || !same_component(*root_it, *target_it)) {
      return compose(ErrorCode::PathEscape, "path is not inside the workspace root:",
                     target.string());
    }
  }
  std::filesystem::path walked = root;
  for (; target_it != target.end(); ++target_it) {
    walked /= *target_it;
    std::error_code reparse_failure;
    // A component that does not exist yet is not a redirect; absence is not an
    // inspection failure.
    const reparse_state state = probe_reparse_point(walked, reparse_failure);
    if (state == reparse_state::yes) {
      return compose(ErrorCode::ReparsePointRejected,
                     "path component is a symlink, junction or other reparse point",
                     walked.string());
    }
    if (state == reparse_state::indeterminate) {
      return compose(ErrorCode::IoFailure, "cannot inspect", walked.string());
    }
  }
  return Status();
}

Status atomic_write_file(const std::filesystem::path& target, std::string_view bytes) {
  if (target.empty() || target.filename().empty()) {
    return Status(ErrorCode::PersistenceIoFailure, "atomic write target is empty");
  }
  const std::filesystem::path parent = target.parent_path();
  std::error_code failure;
  if (!parent.empty()) {
    const std::filesystem::file_status status = std::filesystem::status(parent, failure);
    if (failure || !std::filesystem::exists(status) || !std::filesystem::is_directory(status)) {
      return compose(ErrorCode::PersistenceIoFailure,
                     "parent directory does not exist", parent.string());
    }
  }
  static std::atomic<std::uint64_t> staging_counter{0};
  const std::uint64_t sequence = staging_counter.fetch_add(1, std::memory_order_relaxed);
  const std::string suffix = ".afltmp-" + std::to_string(process_identifier()) + "-" +
                             std::to_string(sequence);
  const std::filesystem::path staging = target.parent_path() /
                                        std::filesystem::path(target.filename().string() +
                                                              suffix);
  return write_staging_file(target, staging, bytes);
}

Result<std::string> read_file_bounded(const std::filesystem::path& path,
                                      std::uint64_t max_bytes) {
  std::uint64_t observed = 0;
  AF_TRY_ASSIGN(observed, bounded_file_size(path));
  if (observed > max_bytes) {
    std::ostringstream message;
    message << "file size " << observed << " exceeds the accepted bound " << max_bytes << ": "
            << path.string();
    return Status(ErrorCode::LengthOutOfRange, message.str());
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return compose(ErrorCode::IoFailure, "cannot open", path.string());
  }

  std::string content;
  content.resize(static_cast<std::size_t>(observed));
  std::size_t total = 0;
  while (total < content.size()) {
    const std::size_t remaining = content.size() - total;
    const std::size_t chunk = std::min<std::size_t>(remaining, 64U * 1024U);
    stream.read(content.data() + total, static_cast<std::streamsize>(chunk));
    const std::streamsize produced = stream.gcount();
    if (produced <= 0) {
      return compose(ErrorCode::IoFailure, "short read from", path.string());
    }
    total += static_cast<std::size_t>(produced);
  }
  if (!stream) {
    return compose(ErrorCode::IoFailure, "read failed for", path.string());
  }
  return Result<std::string>(std::move(content));
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  return bounded_file_size(path);
}

Status remove_tree_bounded(const std::filesystem::path& path) {
  std::error_code failure;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, failure);
  if (failure) {
    if (failure == std::errc::no_such_file_or_directory) {
      return Status();
    }
    return compose(ErrorCode::IoFailure, "cannot inspect", path.string());
  }
  if (!std::filesystem::exists(status)) {
    return Status();
  }
  AF_TRY(walk_bounded(path));
  std::error_code removed;
  std::filesystem::remove_all(path, removed);
  if (removed) {
    return compose(ErrorCode::IoFailure, "cannot remove", path.string());
  }
  return Status();
}

std::string path_to_utf8(const std::filesystem::path& p) {
  return to_utf8_bytes(p);
}

Result<std::filesystem::path> make_unique_directory(const std::filesystem::path& base,
                                                    std::string_view prefix) {
  if (prefix.empty()) {
    return Status(ErrorCode::UnsafePath, "unique directory prefix is empty");
  }
  if (prefix.size() > kMaxRelativePathLength) {
    std::ostringstream message;
    message << "unique directory prefix length " << prefix.size() << " exceeds the limit "
            << kMaxRelativePathLength;
    return Status(ErrorCode::LengthOutOfRange, message.str());
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    const char byte = prefix[index];
    if (is_path_separator(byte)) {
      return compose(ErrorCode::UnsafePath,
                     "unique directory prefix contains a path separator:", describe_component(prefix));
    }
    if (byte == ':') {
      return compose(ErrorCode::UnsafePath,
                     "unique directory prefix contains a drive or stream separator:",
                     describe_component(prefix));
    }
    const char first = prefix.front();
    if (first == '<' || first == '>' || first == '"' || first == '|' || first == '?' ||
        first == '*') {
      return compose(ErrorCode::UnsafePath,
                     "unique directory prefix begins with a character Windows rejects:",
                     describe_component(prefix));
    }
    const unsigned char value = static_cast<unsigned char>(byte);
    if (value < 0x20U || value > 0x7EU) {
      return compose(ErrorCode::UnsafePath,
                     "unique directory prefix contains a control character or a byte outside "
                     "printable ASCII:",
                     describe_component(prefix));
    }
  }
  if (is_reserved_device_name(prefix)) {
    return compose(ErrorCode::UnsafePath, "unique directory prefix is a reserved device name:",
                   describe_component(prefix));
  }
  const char prefix_last = prefix.back();
  if (prefix_last == '.' || prefix_last == ' ') {
    return compose(ErrorCode::UnsafePath,
                   "unique directory prefix ends with a dot or a space:", describe_component(prefix));
  }

  const std::filesystem::path root = weakly_canonical_or(base, base);
  std::error_code base_failure;
  const std::filesystem::file_status base_status = std::filesystem::status(root, base_failure);
  if (base_failure || !std::filesystem::exists(base_status) ||
      !std::filesystem::is_directory(base_status)) {
    return compose(ErrorCode::WorkspaceFailure, "unique directory base is not a directory",
                   root.string());
  }

  static std::atomic<std::uint64_t> unique_counter{0};
  for (;;) {
    const std::uint64_t sequence = unique_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string name = std::string(prefix) + "-" +
                             std::to_string(process_identifier()) + "-" +
                             std::to_string(sequence);
    const std::filesystem::path candidate = root / std::filesystem::path(name);
    std::error_code exists_failure;
    if (std::filesystem::exists(candidate, exists_failure)) {
      continue;
    }
    if (exists_failure) {
      return compose(ErrorCode::IoFailure, "cannot inspect unique directory candidate",
                     candidate.string());
    }
    std::error_code create_failure;
    if (std::filesystem::create_directory(candidate, create_failure)) {
      if (!is_lexically_inside(root, candidate)) {
        return compose(ErrorCode::PathEscape, "unique directory left its base:",
                       candidate.string());
      }
      return Result<std::filesystem::path>(candidate);
    }
    if (create_failure) {
      // A concurrent creator won the race; the next counter value is tried.
      continue;
    }
  }
}

}  // namespace autonomous_foundry

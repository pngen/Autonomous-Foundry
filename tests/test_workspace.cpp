// Workspace suite.
//
// The workspace layer is the filesystem trust boundary: every path it sees was
// produced by an untrusted external process. This suite proves that path
// validation and resolution refuse traversal, absolute, UNC, drive-relative,
// reserved-device, over-long and non-printable forms; that a legitimate nested
// path still resolves inside the root; that a planted reparse point can never
// widen the root; and that publication is transactional, bounded and leaves no
// staging file behind.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/workspace.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    char left_byte = left[index];
    char right_byte = right[index];
    if (left_byte >= 'A' && left_byte <= 'Z') {
      left_byte = static_cast<char>(left_byte - 'A' + 'a');
    }
    if (right_byte >= 'A' && right_byte <= 'Z') {
      right_byte = static_cast<char>(right_byte - 'A' + 'a');
    }
    if (left_byte != right_byte) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool component_equals(const std::filesystem::path& left,
                                    const std::filesystem::path& right) {
#if defined(_WIN32)
  // Windows resolves path components case-insensitively, so containment has to
  // be compared the same way the platform compares it.
  return ascii_iequals(left.string(), right.string());
#else
  return left == right;
#endif
}

/// True when child is lexically inside ancestor, component by component.
[[nodiscard]] bool is_inside(const std::filesystem::path& ancestor,
                             const std::filesystem::path& child) {
  const std::filesystem::path root = ancestor.lexically_normal();
  const std::filesystem::path target = child.lexically_normal();
  auto root_it = root.begin();
  auto target_it = target.begin();
  for (; root_it != root.end(); ++root_it, ++target_it) {
    if (target_it == target.end()) {
      return false;
    }
    if (!component_equals(*root_it, *target_it)) {
      return false;
    }
  }
  return true;
}

/// Sorted file names directly inside a directory, without following links.
[[nodiscard]] std::vector<std::string> enumerate_names(const std::filesystem::path& directory,
                                                       std::error_code& failure) {
  std::vector<std::string> names;
  failure.clear();
  std::filesystem::directory_iterator iterator(directory, failure);
  if (failure) {
    return names;
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    names.push_back(iterator->path().filename().string());
    iterator.increment(failure);
    if (failure) {
      return names;
    }
  }
  return names;
}

[[nodiscard]] bool holds_name(const std::vector<std::string>& names, std::string_view probe) {
  for (const std::string& name : names) {
    if (name == probe) {
      return true;
    }
  }
  return false;
}

/// A relative path of exactly n printable 'a' bytes.
[[nodiscard]] std::string repeated_path(std::size_t length) {
  return std::string(length, 'a');
}

}  // namespace

AF_TEST_CASE(workspace, relative_path_validation_refuses_every_unsafe_form) {
  af_ctx.phase("SETUP");
  struct Rejected {
    const char* path;
    ErrorCode code;
  };
  const Rejected rejected[] = {
      // Traversal, in both separator spellings and in an interior position.
      {"..", ErrorCode::PathEscape},
      {"a/../b", ErrorCode::PathEscape},
      {"..\\x", ErrorCode::PathEscape},
      {"a/..", ErrorCode::PathEscape},
      // Absolute paths and UNC prefixes.
      {"/etc/passwd", ErrorCode::PathEscape},
      {"/", ErrorCode::PathEscape},
      {"\\server\\share", ErrorCode::PathEscape},
      // Drive-relative prefixes and any ':' anywhere in the path.
      {"C:foo", ErrorCode::PathEscape},
      {"C:/foo", ErrorCode::PathEscape},
      {"a/b:c", ErrorCode::PathEscape},
      {"file.txt:stream", ErrorCode::PathEscape},
      // Empty components and the '.' component.
      {"a//b", ErrorCode::UnsafePath},
      {".", ErrorCode::PathEscape},
      {"a/./b", ErrorCode::PathEscape},
      {"./a", ErrorCode::PathEscape},
      // Names Windows strips or reinterprets.
      {"name.", ErrorCode::UnsafePath},
      {"name ", ErrorCode::UnsafePath},
      {"a/name. ", ErrorCode::UnsafePath},
      // Reserved device names, with and without an extension and in any case.
      {"CON", ErrorCode::UnsafePath},
      {"con.txt", ErrorCode::UnsafePath},
      {"NUL", ErrorCode::UnsafePath},
      {"nul.dat", ErrorCode::UnsafePath},
      {"COM1", ErrorCode::UnsafePath},
      {"LPT9", ErrorCode::UnsafePath},
      {"PRN", ErrorCode::UnsafePath},
      {"AUX", ErrorCode::UnsafePath},
      {"dir/com1.bin", ErrorCode::UnsafePath},
      // Characters Windows refuses at the start of a component.
      {"<bad", ErrorCode::UnsafePath},
      {">bad", ErrorCode::UnsafePath},
      {"\"bad", ErrorCode::UnsafePath},
      {"|bad", ErrorCode::UnsafePath},
      {"?bad", ErrorCode::UnsafePath},
      {"*bad", ErrorCode::UnsafePath},
      {"a/<b", ErrorCode::UnsafePath},
  };

  af_ctx.phase("VERIFY_REJECTED_FORMS");
  for (const Rejected& probe : rejected) {
    const Status status = validate_relative_path(probe.path);
    if (status.code() != probe.code) {
      af_ctx.fail_at(__FILE__, __LINE__,
                     std::string("validate_relative_path('") + probe.path + "') reported " +
                         std::string(error_code_name(status.code())) + " but " +
                         std::string(error_code_name(probe.code)) + " was expected: '" +
                         status.message() + "'");
    }
  }

  af_ctx.phase("VERIFY_CONTROL_AND_NON_ASCII_BYTES");
  {
    std::string control = "a";
    control.push_back(static_cast<char>(0x01));
    control.append("b");
    const Status control_status = validate_relative_path(control);
    EXPECT_STATUS_CODE(af_ctx, control_status, ErrorCode::UnsafePath);

    std::string newline = "a";
    newline.push_back('\n');
    newline.append("b");
    const Status newline_status = validate_relative_path(newline);
    EXPECT_STATUS_CODE(af_ctx, newline_status, ErrorCode::UnsafePath);

    std::string utf8 = "caf";
    utf8.push_back(static_cast<char>(0xC3));
    utf8.push_back(static_cast<char>(0xA9));
    const Status utf8_status = validate_relative_path(utf8);
    EXPECT_STATUS_CODE(af_ctx, utf8_status, ErrorCode::UnsafePath);

    std::string high = "a";
    high.push_back(static_cast<char>(0xFF));
    const Status high_status = validate_relative_path(high);
    EXPECT_STATUS_CODE(af_ctx, high_status, ErrorCode::UnsafePath);
  }

  af_ctx.phase("VERIFY_EMPTY_AND_OVER_LONG");
  const Status empty_status = validate_relative_path("");
  EXPECT_STATUS_CODE(af_ctx, empty_status, ErrorCode::UnsafePath);

  const std::string over_long = repeated_path(kMaxRelativePathLength + 1);
  const Status over_long_status = validate_relative_path(over_long);
  EXPECT_STATUS_CODE(af_ctx, over_long_status, ErrorCode::LengthOutOfRange);
  EXPECT_TRUE(af_ctx, contains(over_long_status.message(),
                               std::to_string(kMaxRelativePathLength + 1)));

  // The bound is inclusive: exactly the limit is a legal relative path.
  const std::string at_limit = repeated_path(kMaxRelativePathLength);
  const Status at_limit_status = validate_relative_path(at_limit);
  EXPECT_OK(af_ctx, at_limit_status);

  af_ctx.phase("VERIFY_LEGITIMATE_FORMS");
  const Status nested = validate_relative_path("out/bin/result.txt");
  EXPECT_OK(af_ctx, nested);
  const Status nested_backslash = validate_relative_path("out\\bin\\result.txt");
  EXPECT_OK(af_ctx, nested_backslash);
  const Status dotted = validate_relative_path("result.final.txt");
  EXPECT_OK(af_ctx, dotted);
  const Status dashed = validate_relative_path("out/a-b_c.txt");
  EXPECT_OK(af_ctx, dashed);
  const Status trailing_slash = validate_relative_path("out/bin/");
  EXPECT_OK(af_ctx, trailing_slash);
}

AF_TEST_CASE(workspace, legitimate_nested_path_resolves_inside_the_root) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory temp("af-workspace-resolve");
  REQUIRE_VALUE(af_ctx, WorkspaceRoot, root, WorkspaceRoot::create(temp.path(), "ws"));
  EXPECT_TRUE(af_ctx, root.valid());
  EXPECT_FALSE(af_ctx, root.path().empty());

  af_ctx.phase("VERIFY");
  REQUIRE_VALUE(af_ctx, std::filesystem::path, resolved,
                root.resolve("out/bin/result.txt"));
  EXPECT_TRUE(af_ctx, is_inside(root.path(), resolved));
  EXPECT_EQ(af_ctx, resolved.filename().string(), std::string("result.txt"));
  EXPECT_EQ(af_ctx, resolved.parent_path().filename().string(), std::string("bin"));
  EXPECT_EQ(af_ctx, resolved.parent_path().parent_path().filename().string(), std::string("out"));

  const Result<std::filesystem::path> direct =
      root.resolve("out");
  EXPECT_OK(af_ctx, direct);
  EXPECT_TRUE(af_ctx, direct.value() == (root.path() / "out"));

  af_ctx.phase("VERIFY_ESCAPES_ARE_REFUSED_BY_RESOLVE");
  const Result<std::filesystem::path> traversal =
      root.resolve("../escape.txt");
  EXPECT_STATUS_CODE(af_ctx, traversal, ErrorCode::PathEscape);

  const Result<std::filesystem::path> absolute =
      root.resolve("/etc/passwd");
  EXPECT_STATUS_CODE(af_ctx, absolute, ErrorCode::PathEscape);

  const Result<std::filesystem::path> drive =
      root.resolve("C:foo");
  EXPECT_STATUS_CODE(af_ctx, drive, ErrorCode::PathEscape);

  const Result<std::filesystem::path> empty_component =
      root.resolve("a//b");
  EXPECT_STATUS_CODE(af_ctx, empty_component, ErrorCode::UnsafePath);

  const Result<std::filesystem::path> device =
      root.resolve("CON");
  EXPECT_STATUS_CODE(af_ctx, device, ErrorCode::UnsafePath);

  const Result<std::filesystem::path> over_long =
      root.resolve(repeated_path(kMaxRelativePathLength + 4));
  EXPECT_STATUS_CODE(af_ctx, over_long, ErrorCode::LengthOutOfRange);

  af_ctx.phase("VERIFY_UNINITIALIZED_ROOT");
  const WorkspaceRoot uninitialized;
  EXPECT_FALSE(af_ctx, uninitialized.valid());
  const Result<std::filesystem::path> no_root = uninitialized.resolve("out/result.txt");
  EXPECT_STATUS_CODE(af_ctx, no_root, ErrorCode::WorkspaceFailure);
}

AF_TEST_CASE(workspace, create_write_read_list_and_remove_round_trip) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory temp("af-workspace-roundtrip");
  REQUIRE_VALUE(af_ctx, WorkspaceRoot, root, WorkspaceRoot::create(temp.path(), "ws"));

  af_ctx.phase("COMMIT");
  const Status created = root.ensure_directory("out/bin");
  EXPECT_OK(af_ctx, created);

  const Status wrote = root.write_file("out/bin/result.txt", "payload");
  EXPECT_OK(af_ctx, wrote);

  af_ctx.phase("VERIFY_CONTENT");
  const Result<std::string> content = root.read_file("out/bin/result.txt", 64);
  EXPECT_OK(af_ctx, content);
  EXPECT_EQ(af_ctx, content.value(), std::string("payload"));

  // An empty file is a legal artifact, and reading it back must be an empty
  // string rather than a failure.
  const Status empty_write = root.write_file("out/empty.bin", "");
  EXPECT_OK(af_ctx, empty_write);
  const Result<std::string> empty_read = root.read_file("out/empty.bin", 1);
  EXPECT_OK(af_ctx, empty_read);
  EXPECT_TRUE(af_ctx, empty_read.value().empty());

  af_ctx.phase("VERIFY_DETERMINISTIC_LISTING");
  const Status zeta = root.write_file("out/zeta.txt", "z");
  EXPECT_OK(af_ctx, zeta);
  const Status alpha = root.write_file("out/alpha.txt", "a");
  EXPECT_OK(af_ctx, alpha);
  const Status middle = root.write_file("out/Middle.txt", "m");
  EXPECT_OK(af_ctx, middle);

  const Result<std::vector<std::string>> names = root.list_files("out", 16);
  EXPECT_OK(af_ctx, names);
  EXPECT_EQ(af_ctx, names.value().size(), static_cast<std::size_t>(4));
  // Sorted by byte value, so uppercase sorts before lowercase: the list is a
  // canonical value, not a function of directory iteration order.
  const std::vector<std::string>& listed = names.value();
  if (listed.size() == 4) {
    EXPECT_EQ(af_ctx, listed[0], std::string("Middle.txt"));
    EXPECT_EQ(af_ctx, listed[1], std::string("alpha.txt"));
    EXPECT_EQ(af_ctx, listed[2], std::string("empty.bin"));
    EXPECT_EQ(af_ctx, listed[3], std::string("zeta.txt"));
  }
  // The listing is directly inside the named directory only: the nested
  // "bin/result.txt" is not reported, and neither is the directory itself.
  EXPECT_FALSE(af_ctx, holds_name(listed, "bin"));
  EXPECT_FALSE(af_ctx, holds_name(listed, "result.txt"));

  af_ctx.phase("VERIFY_BOUNDED_LISTING");
  const Result<std::vector<std::string>> too_many = root.list_files("out", 3);
  EXPECT_STATUS_CODE(af_ctx, too_many, ErrorCode::QueueCapacityExceeded);
  const Result<std::vector<std::string>> exact = root.list_files("out", 4);
  EXPECT_OK(af_ctx, exact);
  const Result<std::vector<std::string>> missing_directory = root.list_files("absent", 4);
  EXPECT_NOT_OK(af_ctx, missing_directory);

  af_ctx.phase("SHUTDOWN");
  const Status removed = root.remove_all();
  EXPECT_OK(af_ctx, removed);
  std::error_code failure;
  EXPECT_FALSE(af_ctx, std::filesystem::exists(root.path(), failure));
  // remove_all is idempotent: a second call on the removed tree is not an error.
  const Status removed_again = root.remove_all();
  EXPECT_OK(af_ctx, removed_again);
}

AF_TEST_CASE(workspace, reparse_point_planted_in_the_root_cannot_widen_it) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory temp("af-workspace-reparse");
  REQUIRE_VALUE(af_ctx, WorkspaceRoot, root, WorkspaceRoot::create(temp.path(), "ws"));

  const Status real_directory = root.ensure_directory("real");
  EXPECT_OK(af_ctx, real_directory);
  const Status real_file = root.write_file("real/inside.txt", "content");
  EXPECT_OK(af_ctx, real_file);

  af_ctx.phase("PLANT");
  // A dangling directory symlink inside the root. The target deliberately does
  // not exist, so nothing canonicalizes the link away and the link itself is
  // what resolution has to inspect.
  const std::filesystem::path dangling_link = root.path() / "dangling";
  std::error_code failure;
  std::filesystem::create_directory_symlink(root.path() / "absent-target", dangling_link, failure);
  if (failure) {
    af_ctx.note(std::string("UNSUPPORTED: create_directory_symlink failed: ") +
                failure.message() + " (the platform requires a privilege this process lacks)");
    af_ctx.phase("UNSUPPORTED");
    return;
  }

  af_ctx.phase("VERIFY_REPARSE_REFUSAL");
  // The component walk refuses the link itself, component by component.
  const Status walk = verify_no_reparse_escape(root.path(), root.path() / "dangling" / "child.txt");
  EXPECT_STATUS_CODE(af_ctx, walk, ErrorCode::ReparsePointRejected);

  // Resolution through the link is refused as well, and never returns a path.
  const Result<std::filesystem::path> through_dangling =
      root.resolve("dangling/child.txt");
  EXPECT_STATUS_CODE(af_ctx, through_dangling, ErrorCode::ReparsePointRejected);
  const Result<std::filesystem::path> directory_through_dangling =
      root.resolve("dangling");
  EXPECT_STATUS_CODE(af_ctx, directory_through_dangling, ErrorCode::ReparsePointRejected);
  const Status created_through_link = root.ensure_directory("dangling/sub");
  EXPECT_STATUS_CODE(af_ctx, created_through_link, ErrorCode::ReparsePointRejected);
  const Status written_through_link = root.write_file("dangling/child.txt", "escape");
  EXPECT_STATUS_CODE(af_ctx, written_through_link, ErrorCode::ReparsePointRejected);

  af_ctx.phase("VERIFY_OUTWARD_LINK_CANNOT_ESCAPE");
  // A link whose target is the workspace's parent directory, i.e. strictly
  // outside the root. Whatever the platform does with the link, the resolved
  // path must never leave the root.
  const std::filesystem::path outward_link = root.path() / "outward";
  std::error_code outward_failure;
  std::filesystem::create_directory_symlink(temp.path(), outward_link, outward_failure);
  if (outward_failure) {
    af_ctx.note(std::string("UNSUPPORTED: outward create_directory_symlink failed: ") +
                outward_failure.message());
  } else {
    const Result<std::filesystem::path> escaped =
        root.resolve("outward/leaked.txt");
    EXPECT_NOT_OK(af_ctx, escaped);
    if (escaped.ok()) {
      EXPECT_TRUE(af_ctx, is_inside(root.path(), escaped.value()));
    } else {
      EXPECT_STATUS_IN(af_ctx, escaped, ErrorCode::PathEscape, ErrorCode::ReparsePointRejected);
    }
    const Status outward_walk =
        verify_no_reparse_escape(root.path(), root.path() / "outward" / "leaked.txt");
    EXPECT_STATUS_CODE(af_ctx, outward_walk, ErrorCode::ReparsePointRejected);
  }

  af_ctx.phase("VERIFY_LEGITIMATE_TREE_STILL_WORKS");
  // The planted links must not have broken ordinary resolution inside the root.
  const Result<std::filesystem::path> still_fine =
      root.resolve("real/inside.txt");
  EXPECT_OK(af_ctx, still_fine);
  EXPECT_TRUE(af_ctx, is_inside(root.path(), still_fine.value()));
  const Result<std::string> still_readable = root.read_file("real/inside.txt", 64);
  EXPECT_OK(af_ctx, still_readable);
  EXPECT_EQ(af_ctx, still_readable.value(), std::string("content"));
}

AF_TEST_CASE(workspace, atomic_write_leaves_no_temporary_sibling_behind) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory temp("af-workspace-atomic");
  REQUIRE_VALUE(af_ctx, WorkspaceRoot, root, WorkspaceRoot::create(temp.path(), "ws"));
  const std::filesystem::path target = root.path() / "result.txt";

  af_ctx.phase("COMMIT");
  const Status first = atomic_write_file(target, "first revision");
  EXPECT_OK(af_ctx, first);

  // A second write over a much larger payload exercises the replacement path
  // with a staging file that cannot be confused with the target.
  const std::string large(64u * 1024u, 'x');
  const Status second = atomic_write_file(target, large);
  EXPECT_OK(af_ctx, second);

  const Status third = atomic_write_file(target, "");
  EXPECT_OK(af_ctx, third);

  af_ctx.phase("VERIFY");
  std::error_code failure;
  const std::vector<std::string> names = enumerate_names(root.path(), failure);
  EXPECT_FALSE(af_ctx, static_cast<bool>(failure));
  EXPECT_EQ(af_ctx, names.size(), static_cast<std::size_t>(1));
  EXPECT_TRUE(af_ctx, holds_name(names, "result.txt"));
  for (const std::string& name : names) {
    EXPECT_TRUE(af_ctx, name.find(".afltmp-") == std::string::npos);
  }

  const std::filesystem::path temp_sibling = root.path() / "result.txt.afltmp-probe";
  EXPECT_FALSE(af_ctx, std::filesystem::exists(temp_sibling, failure));

  af_ctx.phase("VERIFY_TARGET_IS_THE_LAST_CONTENT");
  const Result<std::uint64_t> size = autonomous_foundry::file_size(target);
  EXPECT_OK(af_ctx, size);
  EXPECT_EQ(af_ctx, size.value(), std::uint64_t{0});

  const Status rewritten = atomic_write_file(target, large);
  EXPECT_OK(af_ctx, rewritten);
  const Result<std::string> content = read_file_bounded(target, 128u * 1024u);
  EXPECT_OK(af_ctx, content);
  EXPECT_EQ(af_ctx, content.value().size(), large.size());
  EXPECT_TRUE(af_ctx, content.value() == large);

  // The parent directory still holds exactly the target after every write.
  std::error_code recount_failure;
  const std::vector<std::string> after = enumerate_names(root.path(), recount_failure);
  EXPECT_FALSE(af_ctx, static_cast<bool>(recount_failure));
  EXPECT_EQ(af_ctx, after.size(), static_cast<std::size_t>(1));

  af_ctx.phase("VERIFY_MISSING_PARENT_IS_REFUSED");
  const Status no_parent =
      atomic_write_file(root.path() / "absent" / "file.txt", "content");
  EXPECT_NOT_OK(af_ctx, no_parent);
  const Status empty_target = atomic_write_file(std::filesystem::path(), "content");
  EXPECT_NOT_OK(af_ctx, empty_target);
}

AF_TEST_CASE(workspace, bounded_reads_refuse_oversized_files_and_report_real_size) {
  af_ctx.phase("SETUP");
  af_test::TempDirectory temp("af-workspace-bounded");
  REQUIRE_VALUE(af_ctx, WorkspaceRoot, root, WorkspaceRoot::create(temp.path(), "ws"));

  const std::string payload(4096, 'q');
  const Status wrote = root.write_file("data.bin", payload);
  EXPECT_OK(af_ctx, wrote);

  af_ctx.phase("VERIFY_REAL_SIZE");
  const Result<std::uint64_t> observed = autonomous_foundry::file_size(root.path() / "data.bin");
  EXPECT_OK(af_ctx, observed);
  EXPECT_EQ(af_ctx, observed.value(), static_cast<std::uint64_t>(payload.size()));

  af_ctx.phase("VERIFY_BOUND_IS_INCLUSIVE");
  const Result<std::string> exact =
      read_file_bounded(root.path() / "data.bin", static_cast<std::uint64_t>(payload.size()));
  EXPECT_OK(af_ctx, exact);
  EXPECT_EQ(af_ctx, exact.value(), payload);

  const Result<std::string> generous =
      read_file_bounded(root.path() / "data.bin", 1024u * 1024u);
  EXPECT_OK(af_ctx, generous);
  EXPECT_EQ(af_ctx, generous.value().size(), payload.size());

  af_ctx.phase("VERIFY_OVERSIZED_FILE_IS_REFUSED");
  const Result<std::string> refused = read_file_bounded(root.path() / "data.bin", 4095);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::LengthOutOfRange);
  EXPECT_TRUE(af_ctx, contains(refused.status().message(), "4096"));

  const Result<std::string> zero_bound = read_file_bounded(root.path() / "data.bin", 0);
  EXPECT_STATUS_CODE(af_ctx, zero_bound, ErrorCode::LengthOutOfRange);

  // The workspace read path applies the same bound, so a hostile file cannot
  // drive an unbounded allocation through the higher-level API either.
  const Result<std::string> through_root = root.read_file("data.bin", 16);
  EXPECT_STATUS_CODE(af_ctx, through_root, ErrorCode::LengthOutOfRange);

  af_ctx.phase("VERIFY_NON_FILE_TARGETS");
  const Result<std::uint64_t> directory_size = autonomous_foundry::file_size(root.path());
  EXPECT_NOT_OK(af_ctx, directory_size);
  const Result<std::uint64_t> missing_size = autonomous_foundry::file_size(root.path() / "absent.bin");
  EXPECT_NOT_OK(af_ctx, missing_size);
  const Result<std::string> missing_read = root.read_file("absent.bin", 16);
  EXPECT_NOT_OK(af_ctx, missing_read);
}

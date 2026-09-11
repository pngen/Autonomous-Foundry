#include "autonomous_foundry/version.hpp"

#include <string>

namespace autonomous_foundry {

namespace {

// Compiler identity. The most specific macro is tested first because clang
// also defines __GNUC__ and clang-cl also defines _MSC_VER.
std::string compiler_description() {
#if defined(__clang__)
  return std::string("Clang ") + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
  return std::string("MSVC ") + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
  return std::string("GCC ") + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) +
         "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
  return std::string("unknown-compiler");
#endif
}

// Platform identity. Nothing here depends on the clock, the host name or the
// build environment: the description of a build is reproducible byte for byte
// from the same toolchain and the same source.
std::string platform_description() {
#if defined(_WIN64)
  return std::string("windows-x64");
#elif defined(_WIN32)
  return std::string("windows-x86");
#elif defined(__APPLE__)
#  if defined(__aarch64__) || defined(__arm64__)
  return std::string("macos-arm64");
#  else
  return std::string("macos-x64");
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
  return std::string("linux-arm64");
#  elif defined(__x86_64__)
  return std::string("linux-x64");
#  else
  return std::string("linux-unknown-arch");
#  endif
#else
  return std::string("unknown-platform");
#endif
}

}  // namespace

std::string_view version_string() noexcept {
  return std::string_view(AUTONOMOUS_FOUNDRY_VERSION_STRING);
}

std::uint32_t version_number() noexcept {
  return (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;
}

std::string build_description() {
  std::string text("Autonomous Foundry ");
  text.append(version_string());
  text.append(" (");
  text.append(compiler_description());
  text.append(", ");
  text.append(platform_description());
  text.push_back(')');
  return text;
}

}  // namespace autonomous_foundry

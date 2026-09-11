#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/export.hpp"

// Deterministic hashing and integrity primitives.
//
// Autonomous Foundry deliberately avoids std::hash for anything that crosses a
// process, a file or a version boundary: those values must be reproducible.
// This header provides the three primitives the runtime actually needs.

namespace autonomous_foundry {

namespace detail {

struct Crc32cTable {
  std::array<std::uint32_t, 256> entries{};

  constexpr Crc32cTable() noexcept {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
      }
      entries[static_cast<std::size_t>(i)] = crc;
    }
  }
};

inline constexpr Crc32cTable kCrc32cTable{};

}  // namespace detail

/// CRC-32C (Castagnoli). Used for snapshot payloads and protocol frames.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::uint32_t crc32c(const void* data, std::size_t length,
                                                          std::uint32_t seed = 0) noexcept;
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::uint32_t crc32c(std::string_view text,
                                                          std::uint32_t seed = 0) noexcept;

/// FNV-1a 64-bit. Used for small stable identifiers such as canonical policy
/// factor keys; never used as a security primitive.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::uint64_t fnv1a64(std::string_view text) noexcept;

/// SHA-256. Used for artifact content digests and canonical state digests,
/// where a real content-addressed integrity check is required.
class AUTONOMOUS_FOUNDRY_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  /// Finalize and return the digest. The object must be reset before reuse.
  [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<std::uint8_t, kDigestBytes> digest(const void* data,
                                                                     std::size_t length) noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockBytes> buffer_{};
  std::size_t buffer_length_{0};
  std::uint64_t total_length_{0};
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string to_hex(const std::uint8_t* data,
                                                        std::size_t length);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string sha256_hex(std::string_view text);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string sha256_hex(const void* data, std::size_t length);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool is_lowercase_hex(std::string_view text,
                                                           std::size_t expected_length);

}  // namespace autonomous_foundry

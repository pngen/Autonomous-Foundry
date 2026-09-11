#include "autonomous_foundry/hash.hpp"

#include <cstddef>
#include <cstdint>

namespace autonomous_foundry {

namespace {

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
  return (value >> amount) | (value << (32u - amount));
}

// FIPS 180-4 round constants: the first 32 bits of the fractional parts of the
// cube roots of the first 64 prime numbers.
constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u,
    0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu,
    0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u,
    0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u,
    0xC67178F2u};

// FIPS 180-4 initial hash value: the first 32 bits of the fractional parts of
// the square roots of the first 8 prime numbers.
constexpr std::array<std::uint32_t, 8> kSha256InitialState{
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

void compress_block(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t first = schedule[index - 15];
    const std::uint32_t second = schedule[index - 2];
    const std::uint32_t sigma0 = rotate_right(first, 7) ^ rotate_right(first, 18) ^ (first >> 3);
    const std::uint32_t sigma1 = rotate_right(second, 17) ^ rotate_right(second, 19) ^ (second >> 10);
    schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t big_sigma1 =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 =
        h + big_sigma1 + choose + kSha256RoundConstants[index] + schedule[index];
    const std::uint32_t big_sigma0 =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t index = 0; index < length; ++index) {
    const std::uint8_t slot = static_cast<std::uint8_t>((crc ^ bytes[index]) & 0xFFu);
    crc = detail::kCrc32cTable.entries[static_cast<std::size_t>(slot)] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view text, std::uint32_t seed) noexcept {
  return crc32c(text.data(), text.size(), seed);
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const char character : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
    hash *= 0x100000001B3ull;
  }
  return hash;
}

void Sha256::reset() noexcept {
  state_ = kSha256InitialState;
  buffer_.fill(0);
  buffer_length_ = 0;
  total_length_ = 0;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
  if (length == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_length_ += static_cast<std::uint64_t>(length);

  std::size_t offset = 0;
  if (buffer_length_ != 0) {
    while (offset < length && buffer_length_ < kBlockBytes) {
      buffer_[buffer_length_] = bytes[offset];
      buffer_length_ += 1;
      offset += 1;
    }
    if (buffer_length_ == kBlockBytes) {
      process_block(buffer_.data());
      buffer_length_ = 0;
    }
  }

  while ((length - offset) >= kBlockBytes) {
    process_block(bytes + offset);
    offset += kBlockBytes;
  }

  while (offset < length) {
    buffer_[buffer_length_] = bytes[offset];
    buffer_length_ += 1;
    offset += 1;
  }
}

void Sha256::process_block(const std::uint8_t* block) noexcept { compress_block(state_, block); }

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
  // Finalization works on copies: the object is left in the state it held
  // before finish() so that a later reset() starts from a defined point and a
  // caller that inspects the object sees no half-written state.
  std::array<std::uint32_t, 8> state = state_;
  std::array<std::uint8_t, 2 * kBlockBytes> tail{};
  const std::size_t remainder = buffer_length_;
  for (std::size_t index = 0; index < remainder; ++index) {
    tail[index] = buffer_[index];
  }
  tail[remainder] = 0x80u;

  const std::size_t padded_length =
      (remainder < (kBlockBytes - 8)) ? kBlockBytes : (2 * kBlockBytes);
  const std::uint64_t bit_length = total_length_ * 8u;
  for (std::size_t index = 0; index < 8; ++index) {
    const unsigned shift = static_cast<unsigned>(56 - (index * 8));
    tail[padded_length - 8 + index] =
        static_cast<std::uint8_t>((bit_length >> shift) & 0xFFull);
  }

  compress_block(state, tail.data());
  if (padded_length == (2 * kBlockBytes)) {
    compress_block(state, tail.data() + kBlockBytes);
  }

  std::array<std::uint8_t, kDigestBytes> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      const unsigned shift = static_cast<unsigned>(24 - (byte * 8));
      digest[(index * 4) + byte] =
          static_cast<std::uint8_t>((state[index] >> shift) & 0xFFu);
    }
  }
  return digest;
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::digest(const void* data,
                                                              std::size_t length) noexcept {
  Sha256 hasher;
  hasher.update(data, length);
  return hasher.finish();
}

std::string to_hex(const std::uint8_t* data, std::size_t length) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(length * 2);
  for (std::size_t index = 0; index < length; ++index) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[index]);
    text.push_back(kHexDigits[static_cast<std::size_t>((value >> 4) & 0x0Fu)]);
    text.push_back(kHexDigits[static_cast<std::size_t>(value & 0x0Fu)]);
  }
  return text;
}

std::string sha256_hex(std::string_view text) { return sha256_hex(text.data(), text.size()); }

std::string sha256_hex(const void* data, std::size_t length) {
  const std::array<std::uint8_t, Sha256::kDigestBytes> digest = Sha256::digest(data, length);
  return to_hex(digest.data(), digest.size());
}

bool is_lowercase_hex(std::string_view text, std::size_t expected_length) {
  if (text.size() != expected_length) {
    return false;
  }
  for (const char character : text) {
    const bool decimal_digit = character >= '0' && character <= '9';
    const bool lower_hex_letter = character >= 'a' && character <= 'f';
    if (!decimal_digit && !lower_hex_letter) {
      return false;
    }
  }
  return true;
}

}  // namespace autonomous_foundry

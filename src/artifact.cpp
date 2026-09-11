// src/artifact.cpp
//
// Artifact reference validation and the canonical artifact-set encoding.
//
// Canonical encoding
// ------------------
//
// Every canonical encoding produced by this file is an explicit, field-tagged,
// length-prefixed byte string of the form
//
//     <field-name> '=' <decimal value length> ':' <raw value bytes> ';'
//
// Field names are fixed ASCII tags that never contain '=', ':' or ';'. The
// length counts raw value bytes, so a value may contain any byte -- including
// '=', ':' and ';' -- without making the encoding ambiguous. Nothing is escaped
// and nothing is normalised: the encoding is a pure function of the logical
// value, which is what makes it usable as the input to SHA-256. No two
// different logical values can produce the same bytes.
//
// A vector is emitted as a decimal count field followed by exactly that many
// element groups; a group is the fixed sequence of fields documented for the
// element type. The count makes truncation or extension detectable, and the
// fixed field order makes the grouping unambiguous.
//
// Artifact set encoding (order independent):
//
//     artifact_count=<n>;
//     name=<len>:<bytes>;size_bytes=<len>:<digits>;content_digest=<len>:<hex>;
//     ... repeated n times ...
//
// The references are sorted first by (name, size_bytes, content_digest) -- the
// total order ArtifactRef::operator< already defines -- so two sets that differ
// only in the order they were supplied produce identical bytes. Two references
// that compare equal emit identical bytes, so their relative order cannot
// change the result either.

#include "autonomous_foundry/artifact.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace autonomous_foundry {

namespace {

/// Device names the Windows filesystem resolves regardless of extension. A
/// logical artifact name is never a path, but the same string travels into
/// logs, JSON and downstream promotion systems, so it must not be a name that
/// a downstream filesystem would silently reinterpret.
/// Length of a lowercase hexadecimal SHA-256 digest.
constexpr std::size_t kArtifactDigestHexLength = 64;

constexpr std::string_view kReservedDeviceNames[] = {
    "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
    "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

void append_field(std::string& out, std::string_view name, std::string_view value) {
  out.append(name);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

void append_count(std::string& out, std::string_view name, std::size_t value) {
  append_field(out, name, std::to_string(value));
}

void append_size(std::string& out, std::string_view name, std::uint64_t value) {
  append_field(out, name, std::to_string(value));
}

std::string hex_byte(unsigned char value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string text(2, '0');
  text[0] = kDigits[(value >> 4) & 0x0Fu];
  text[1] = kDigits[value & 0x0Fu];
  return text;
}

bool is_allowed_name_byte(char ch) noexcept {
  const bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  const bool digit = ch >= '0' && ch <= '9';
  return letter || digit || ch == '.' || ch == '_' || ch == '-';
}

bool is_reserved_device_name(std::string_view base) noexcept {
  for (const std::string_view reserved : kReservedDeviceNames) {
    if (base.size() != reserved.size()) {
      continue;
    }
    bool equal = true;
    for (std::size_t index = 0; index < base.size(); ++index) {
      const char ch = base[index];
      const char lowered = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
      if (lowered != reserved[index]) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return true;
    }
  }
  return false;
}

}  // namespace

Status validate_artifact_name(std::string_view name) {
  if (name.empty()) {
    return Status(ErrorCode::UnsafePath, std::string_view("artifact name is empty"));
  }
  if (name.size() > kMaxArtifactNameLength) {
    return Status(ErrorCode::UnsafePath,
                  "artifact name is " + std::to_string(name.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxArtifactNameLength) + " bytes");
  }
  for (std::size_t index = 0; index < name.size(); ++index) {
    if (!is_allowed_name_byte(name[index])) {
      return Status(ErrorCode::UnsafePath,
                    "artifact name contains byte 0x" +
                        hex_byte(static_cast<unsigned char>(name[index])) + " at index " +
                        std::to_string(index) +
                        "; only ASCII letters, digits, '.', '_' and '-' are allowed");
    }
  }
  if (name.front() == '.') {
    return Status(ErrorCode::UnsafePath,
                  "artifact name '" + std::string(name) + "' starts with '.'");
  }
  if (name.back() == '.') {
    return Status(ErrorCode::UnsafePath,
                  "artifact name '" + std::string(name) + "' ends with '.'");
  }
  if (name.find("..") != std::string_view::npos) {
    return Status(ErrorCode::UnsafePath,
                  "artifact name '" + std::string(name) +
                      "' contains the path traversal sequence '..'");
  }
  const std::size_t dot = name.find('.');
  const std::string_view base = dot == std::string_view::npos ? name : name.substr(0, dot);
  if (is_reserved_device_name(base)) {
    return Status(ErrorCode::UnsafePath,
                  "artifact name '" + std::string(name) +
                      "' starts with the reserved Windows device name '" + std::string(base) +
                      "'; the device name is reserved with or without an extension");
  }
  return Status();
}

Status validate_artifact_ref(const ArtifactRef& ref) {
  AF_TRY(validate_artifact_name(ref.name));
  if (ref.size_bytes > kMaxArtifactBytes) {
    return Status(ErrorCode::LengthOutOfRange,
                  "artifact '" + ref.name + "' declares " + std::to_string(ref.size_bytes) +
                      " bytes, which exceeds the maximum of " +
                      std::to_string(kMaxArtifactBytes) + " bytes");
  }
  if (ref.content_digest.size() != kArtifactDigestHexLength) {
    return Status(ErrorCode::MalformedEncoding,
                  "artifact '" + ref.name + "' content digest must be " +
                      std::to_string(kArtifactDigestHexLength) +
                      " lowercase hexadecimal characters but is " +
                      std::to_string(ref.content_digest.size()) + " characters");
  }
  if (!is_lowercase_hex(ref.content_digest, kArtifactDigestHexLength)) {
    return Status(ErrorCode::MalformedEncoding,
                  "artifact '" + ref.name +
                      "' content digest is not lowercase hexadecimal: '" + ref.content_digest +
                      "'");
  }
  return Status();
}

std::string encode_artifact_set(const std::vector<ArtifactRef>& artifacts) {
  std::vector<ArtifactRef> ordered = artifacts;
  std::sort(ordered.begin(), ordered.end());

  std::string out;
  append_count(out, "artifact_count", ordered.size());
  for (const ArtifactRef& ref : ordered) {
    append_field(out, "name", ref.name);
    append_size(out, "size_bytes", ref.size_bytes);
    append_field(out, "content_digest", ref.content_digest);
  }
  return out;
}

std::string artifact_set_digest(const std::vector<ArtifactRef>& artifacts) {
  const std::string encoding = encode_artifact_set(artifacts);
  return sha256_hex(encoding);
}

}  // namespace autonomous_foundry

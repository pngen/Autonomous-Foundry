#pragma once

// src/local_promotion_sink.hpp
//
// Private declaration of the reference promotion sink.
//
// This header is deliberately outside the public include tree: the sink is a
// reference deployment detail, not part of the runtime contract. The public
// contract is autonomous_foundry/promotion.hpp, which defines PromotionSink.
//
// WHAT THIS SINK IS
//
//   local-file-sink writes the canonical rendering of a PromotionRequest into a
//   directory as a pair of files:
//
//     <request-id>.json          the request, exactly as received
//     <request-id>.receipt.json  the receipt this sink decided on
//
//   It is a handoff recorder. It makes "the runtime produced a promotion
//   request" observable and durable on disk, and it does nothing else. It has
//   no artifact store, no signature check, no trust decision and no release
//   channel.
//
// WHAT THIS SINK CANNOT SAY
//
//   The receipt it returns always carries
//   PromotionHandoffState::AcceptedForProcessing. That state means "this
//   receiver took responsibility for making its own promotion decision". It
//   does not mean the artifact was promoted. There is deliberately no code path
//   in this file that can report Promoted, because PromotionHandoffState has no
//   such value and no other system's decision may be fabricated here.

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "autonomous_foundry/promotion.hpp"

namespace autonomous_foundry {

/// Reference sink name reported in every receipt it produces.
inline constexpr std::string_view kLocalPromotionSinkName = "local-file-sink";

/// Writes each promotion request and its receipt into one directory.
///
/// The sink is usable as std::shared_ptr<PromotionSink>. It is safe to call
/// submit() concurrently from several coordinator threads: the directory is
/// prepared once under a mutex and each file is written with the transactional
/// atomic_write_file helper, so a reader never observes a half-written request
/// or receipt.
class LocalPromotionSink final : public PromotionSink {
 public:
  /// Construct against a directory. The directory is created lazily on the
  /// first submit, so constructing a sink never performs I/O.
  explicit LocalPromotionSink(std::filesystem::path directory);

  [[nodiscard]] std::string name() const override;

  /// Validate, render, write the request, then write the receipt.
  ///
  /// Returns a failure Status - and writes nothing at all - when the request
  /// fails local validation: no request identity, an empty artifact set or an
  /// artifact set larger than the runtime's own bound. The decision returned on
  /// success is always AcceptedForProcessing.
  [[nodiscard]] Result<PromotionReceipt> submit(const PromotionRequest& request) override;

  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

 private:
  std::filesystem::path directory_;
  /// Serializes directory preparation. No lock is ever held across a socket
  /// operation or a process wait; this one only guards a one-time mkdir.
  mutable std::mutex prepare_mutex_;
  bool prepared_{false};
};

}  // namespace autonomous_foundry

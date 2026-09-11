#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/protocol.hpp"

// Framed TCP transport.
//
// Each connection owns exactly one reader thread and one writer thread. Reads
// use a growable buffer and the frame decoder, so a message split across
// several recv calls and several messages coalesced into one recv call are
// both handled. Writes are serialized by the single writer thread, so two
// coordinator threads can never interleave partial frames on one socket.
//
// No foundry state lock is ever held while a socket operation runs. A caller
// hands a frame to the outbound queue and returns; the writer thread is the
// only code that ever calls send().

namespace autonomous_foundry {

/// Process-wide socket subsystem lifetime. Constructing one of these before
/// any socket use is required on Windows.
class AUTONOMOUS_FOUNDRY_API SocketSubsystem {
 public:
  SocketSubsystem();
  ~SocketSubsystem();
  SocketSubsystem(const SocketSubsystem&) = delete;
  SocketSubsystem& operator=(const SocketSubsystem&) = delete;

  [[nodiscard]] static Status ensure_initialized();
  [[nodiscard]] static bool initialized() noexcept;
};

/// Owned platform socket handle. Movable, not copyable.
class AUTONOMOUS_FOUNDRY_API SocketHandle {
 public:
  SocketHandle() noexcept = default;
  explicit SocketHandle(std::uintptr_t raw) noexcept : raw_(raw) {}

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  SocketHandle(SocketHandle&& other) noexcept;
  SocketHandle& operator=(SocketHandle&& other) noexcept;
  ~SocketHandle();

  [[nodiscard]] bool valid() const noexcept { return raw_ != kInvalid; }
  [[nodiscard]] std::uintptr_t raw() const noexcept { return raw_; }
  [[nodiscard]] std::uintptr_t release() noexcept;

  void close() noexcept;

  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~0ull);

 private:
  std::uintptr_t raw_{kInvalid};
};

struct Endpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};

  [[nodiscard]] std::string to_string() const;
};

/// Resolve "host:port" into an endpoint. Only numeric or loopback names are
/// accepted; the runtime never performs a DNS lookup for a worker address.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<Endpoint> parse_endpoint(std::string_view text);

/// Blocking listener. Binding to port 0 selects an ephemeral port, which the
/// caller reads back through port().
class AUTONOMOUS_FOUNDRY_API TcpListener {
 public:
  TcpListener() = default;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&&) noexcept = default;
  TcpListener& operator=(TcpListener&&) noexcept = default;
  ~TcpListener() = default;

  [[nodiscard]] static Result<TcpListener> bind(const Endpoint& endpoint);

  /// Block until a connection arrives or the listener is closed.
  [[nodiscard]] Result<SocketHandle> accept();

  void close();
  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  SocketHandle socket_;
  std::uint16_t port_{0};
};

struct ConnectionCallbacks {
  std::function<void(const Frame& frame)> on_frame;
  std::function<void(const Status& reason)> on_closed;
};

/// One framed byte stream. The reader thread delivers whole frames; the writer
/// thread owns the socket for sending.
class AUTONOMOUS_FOUNDRY_API Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(SocketHandle socket, std::string peer, ConnectionCallbacks callbacks);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  Status start();
  Status send(MessageType type, std::string payload);
  Status send_frame(FrameHeader header, std::string payload);

  /// Request closure. Safe to call from the reader thread, the writer thread
  /// or any other thread; it never joins. The destructor joins both threads.
  void request_close(const Status& reason);

  [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }
  [[nodiscard]] std::uint64_t next_sequence() noexcept {
    return sequence_.fetch_add(1, std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t queued_frames() const;

 private:
  void reader_loop();
  void writer_loop();
  void shutdown_socket();

  SocketHandle socket_;
  std::string peer_;
  ConnectionCallbacks callbacks_;

  std::thread reader_;
  std::thread writer_;

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::string> outbound_;
  bool writer_stop_{false};

  std::atomic<bool> closed_{false};
  std::atomic<bool> started_{false};
  std::atomic<std::uint64_t> sequence_{1};
  std::mutex close_mutex_;
  Status close_reason_;
};

/// Connect to a coordinator endpoint.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<SocketHandle> connect_to(const Endpoint& endpoint);

/// Configure a socket for framed traffic: TCP_NODELAY on, keepalive on, no
/// inherited handles, no lingering close that could block shutdown.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status configure_socket(SocketHandle& socket);

/// True when a recv/send failure means "the peer closed cleanly".
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool socket_error_is_disconnect() noexcept;

/// Last socket error as a Status, for diagnostics.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status last_socket_error(std::string_view context);

}  // namespace autonomous_foundry

// src/transport.cpp
//
// Framed TCP transport.
//
// Three boundaries are enforced here and nowhere else.
//
// Handle ownership: a socket lives inside exactly one SocketHandle. Closing is
// idempotent, moving transfers the raw value and invalidates the source, and no
// path can close one handle twice -- on Windows that would close an unrelated
// descriptor the kernel had already recycled.
//
// Read framing: the peer is untrusted, so bytes accumulate in a growable buffer
// and are handed to decode_frame. A partial frame means "read more", a corrupt
// frame or a protocol violation is fatal for that connection, and a clean peer
// close is an ordinary end of stream. No frame length is ever trusted before the
// decoder has bounded it.
//
// Write serialization: exactly one thread ever calls send(). Callers hand a frame
// to the outbound queue and return immediately, so a slow or hung peer can never
// block a coordinator thread and two producers can never interleave two frames on
// one socket.
//
// No foundry state lock is ever held across a socket operation.

#include "autonomous_foundry/transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
// winsock2.h must precede windows.h; windows.h is never included here.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
/// The keepalive tuning control lives in mstcpip.h. It is declared here under
/// a guard so the socket headers keep their required order and no second
/// Windows SDK header is pulled in for one control code.
#  ifndef SIO_KEEPALIVE_VALS
#    define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
struct tcp_keepalive {
  u_long onoff;
  u_long keepalivetime;
  u_long keepaliveinterval;
};
#  endif
using socket_length_t = int;
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_length_t = socklen_t;
#  ifndef INVALID_SOCKET
#    define INVALID_SOCKET (-1)
#  endif
#  ifndef SOCKET_ERROR
#    define SOCKET_ERROR (-1)
#  endif
#endif

#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/protocol.hpp"

namespace autonomous_foundry {

namespace {

/// Live SocketSubsystem instances. WSAStartup runs for the first one and
/// WSACleanup for the last one.
std::mutex g_subsystem_mutex;
int g_subsystem_references = 0;
bool g_subsystem_ready = false;

/// Scratch buffer used by the reader thread. A stack array, not a heap
/// allocation: every connection owns one for its whole lifetime.
constexpr std::size_t kReceiveChunkBytes = 64u * 1024u;

/// Starting capacity of the per-connection reassembly buffer.
constexpr std::size_t kInitialReadBufferBytes = 8u * 1024u;

constexpr int kBacklog = SOMAXCONN;

#if defined(_WIN32)

using native_socket_t = SOCKET;

native_socket_t native_socket_from_raw(std::uintptr_t raw) noexcept {
  return static_cast<native_socket_t>(raw);
}

std::uintptr_t raw_from_native_socket(native_socket_t socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}

bool handle_is_valid(native_socket_t socket) noexcept { return socket != INVALID_SOCKET; }

/// Stable textual name for a Windows socket error. The string is part of the
/// observable diagnostic surface, so a failure can be reasoned about without
/// parsing a numeric code.
const char* winsock_error_name(int code) noexcept {
  switch (code) {
    case WSAEINTR:
      return "WSAEINTR";
    case WSAEBADF:
      return "WSAEBADF";
    case WSAEACCES:
      return "WSAEACCES";
    case WSAEFAULT:
      return "WSAEFAULT";
    case WSAEINVAL:
      return "WSAEINVAL";
    case WSAEMFILE:
      return "WSAEMFILE";
    case WSAEWOULDBLOCK:
      return "WSAEWOULDBLOCK";
    case WSAEINPROGRESS:
      return "WSAEINPROGRESS";
    case WSAEALREADY:
      return "WSAEALREADY";
    case WSAENOTSOCK:
      return "WSAENOTSOCK";
    case WSAEDESTADDRREQ:
      return "WSAEDESTADDRREQ";
    case WSAEMSGSIZE:
      return "WSAEMSGSIZE";
    case WSAEPROTOTYPE:
      return "WSAEPROTOTYPE";
    case WSAENOPROTOOPT:
      return "WSAENOPROTOOPT";
    case WSAEPROTONOSUPPORT:
      return "WSAEPROTONOSUPPORT";
    case WSAESOCKTNOSUPPORT:
      return "WSAESOCKTNOSUPPORT";
    case WSAEOPNOTSUPP:
      return "WSAEOPNOTSUPP";
    case WSAEPFNOSUPPORT:
      return "WSAEPFNOSUPPORT";
    case WSAEAFNOSUPPORT:
      return "WSAEAFNOSUPPORT";
    case WSAEADDRINUSE:
      return "WSAEADDRINUSE";
    case WSAEADDRNOTAVAIL:
      return "WSAEADDRNOTAVAIL";
    case WSAENETDOWN:
      return "WSAENETDOWN";
    case WSAENETUNREACH:
      return "WSAENETUNREACH";
    case WSAENETRESET:
      return "WSAENETRESET";
    case WSAECONNABORTED:
      return "WSAECONNABORTED";
    case WSAECONNRESET:
      return "WSAECONNRESET";
    case WSAENOBUFS:
      return "WSAENOBUFS";
    case WSAEISCONN:
      return "WSAEISCONN";
    case WSAENOTCONN:
      return "WSAENOTCONN";
    case WSAESHUTDOWN:
      return "WSAESHUTDOWN";
    case WSAETOOMANYREFS:
      return "WSAETOOMANYREFS";
    case WSAETIMEDOUT:
      return "WSAETIMEDOUT";
    case WSAECONNREFUSED:
      return "WSAECONNREFUSED";
    case WSAELOOP:
      return "WSAELOOP";
    case WSAENAMETOOLONG:
      return "WSAENAMETOOLONG";
    case WSAEHOSTDOWN:
      return "WSAEHOSTDOWN";
    case WSAEHOSTUNREACH:
      return "WSAEHOSTUNREACH";
    case WSAENOTEMPTY:
      return "WSAENOTEMPTY";
    case WSAEDISCON:
      return "WSAEDISCON";
    case WSASYSNOTREADY:
      return "WSASYSNOTREADY";
    case WSAVERNOTSUPPORTED:
      return "WSAVERNOTSUPPORTED";
    case WSANOTINITIALISED:
      return "WSANOTINITIALISED";
    default:
      return "WSAErrorUnknown";
  }
}

#else

using native_socket_t = int;

native_socket_t native_socket_from_raw(std::uintptr_t raw) noexcept {
  return static_cast<native_socket_t>(static_cast<int>(raw));
}

std::uintptr_t raw_from_native_socket(native_socket_t socket) noexcept {
  return static_cast<std::uintptr_t>(static_cast<unsigned int>(socket));
}

bool handle_is_valid(native_socket_t socket) noexcept { return socket >= 0; }

const char* posix_error_name(int code) noexcept {
  switch (code) {
    case EINTR:
      return "EINTR";
    case EBADF:
      return "EBADF";
    case EACCES:
      return "EACCES";
    case EFAULT:
      return "EFAULT";
    case EINVAL:
      return "EINVAL";
    case EMFILE:
      return "EMFILE";
    case EWOULDBLOCK:
      return "EWOULDBLOCK";
    case EINPROGRESS:
      return "EINPROGRESS";
    case EALREADY:
      return "EALREADY";
    case ENOTSOCK:
      return "ENOTSOCK";
    case EDESTADDRREQ:
      return "EDESTADDRREQ";
    case EMSGSIZE:
      return "EMSGSIZE";
    case EPROTOTYPE:
      return "EPROTOTYPE";
    case ENOPROTOOPT:
      return "ENOPROTOOPT";
    case EPROTONOSUPPORT:
      return "EPROTONOSUPPORT";
    case ESOCKTNOSUPPORT:
      return "ESOCKTNOSUPPORT";
    case EOPNOTSUPP:
      return "EOPNOTSUPP";
    case EPFNOSUPPORT:
      return "EPFNOSUPPORT";
    case EAFNOSUPPORT:
      return "EAFNOSUPPORT";
    case EADDRINUSE:
      return "EADDRINUSE";
    case EADDRNOTAVAIL:
      return "EADDRNOTAVAIL";
    case ENETDOWN:
      return "ENETDOWN";
    case ENETUNREACH:
      return "ENETUNREACH";
    case ENETRESET:
      return "ENETRESET";
    case ECONNABORTED:
      return "ECONNABORTED";
    case ECONNRESET:
      return "ECONNRESET";
    case ENOBUFS:
      return "ENOBUFS";
    case EISCONN:
      return "EISCONN";
    case ENOTCONN:
      return "ENOTCONN";
    case ESHUTDOWN:
      return "ESHUTDOWN";
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case ECONNREFUSED:
      return "ECONNREFUSED";
    case ELOOP:
      return "ELOOP";
    case ENAMETOOLONG:
      return "ENAMETOOLONG";
    case EHOSTDOWN:
      return "EHOSTDOWN";
    case EHOSTUNREACH:
      return "EHOSTUNREACH";
    case ENOTEMPTY:
      return "ENOTEMPTY";
    default:
      return "ErrorUnknown";
  }
}

#endif

int last_native_socket_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string describe_socket_error(int code) {
  std::string text;
#if defined(_WIN32)
  text = winsock_error_name(code);
  text += '(';
  text += std::to_string(code);
  text += ')';
#else
  text = posix_error_name(code);
  text += '(';
  text += std::to_string(code);
  text += ')';
  if (code != 0) {
    text += ": ";
    text += std::error_code(code, std::generic_category()).message();
  }
#endif
  return text;
}

/// Build a failure Status from a literal or an assembled message. The explicit
/// string_view parameter removes the ambiguity between Status' string and
/// string_view constructors for a plain string literal argument.
Status af_status(ErrorCode code, std::string_view message) {
  return Status(code, message);
}

Status socket_error_status(ErrorCode code, std::string_view context, int native) {
  std::string message(context);
  message += ": socket error ";
  message += describe_socket_error(native);
  return Status(code, std::move(message));
}

struct ParsedAddress {
  int family{AF_INET};
  std::string canonical;
};

bool is_numeric_ipv6(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  in6_addr address{};
  const std::string owned(text);
  return ::inet_pton(AF_INET6, owned.c_str(), &address) == 1;
}

bool is_numeric_ipv4(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  in_addr address{};
  const std::string owned(text);
  return ::inet_pton(AF_INET, owned.c_str(), &address) == 1;
}

/// Validate one host token. Exactly the loopback and any-address spellings the
/// runtime itself emits are accepted; every other input is rejected outright
/// rather than resolved, because a peer address supplied by an operator must
/// never trigger a name lookup.
Result<ParsedAddress> classify_host(std::string_view host) {
  if (host.empty()) {
    return af_status(ErrorCode::InvalidArgument, "endpoint host is empty");
  }

  if (host.front() == '[' && host.size() >= 2 && host.back() == ']') {
    const std::string_view inner = host.substr(1, host.size() - 2);
    if (!is_numeric_ipv6(inner)) {
      return af_status(ErrorCode::InvalidArgument,
                    "bracketed host '" + std::string(inner) +
                        "' is not a numeric IPv6 address; the runtime never resolves a peer "
                        "name");
    }
    ParsedAddress parsed;
    parsed.family = AF_INET6;
    parsed.canonical = std::string(inner);
    return parsed;
  }

  if (host == "localhost" || host == "loopback") {
    ParsedAddress parsed;
    parsed.canonical = "127.0.0.1";
    return parsed;
  }
  if (host == "any") {
    ParsedAddress parsed;
    parsed.canonical = "0.0.0.0";
    return parsed;
  }
  if (is_numeric_ipv4(host)) {
    ParsedAddress parsed;
    parsed.canonical = std::string(host);
    return parsed;
  }

  return af_status(ErrorCode::InvalidArgument,
                "host '" + std::string(host) +
                    "' is not a numeric IPv4 address, a bracketed numeric IPv6 address, or one of "
                    "the loopback names; the runtime never performs a DNS lookup for a peer");
}

/// Decimal port in [0, 65535]. No sign, no prefix, no empty string.
Result<std::uint16_t> parse_decimal_port(std::string_view text) {
  if (text.empty()) {
    return af_status(ErrorCode::InvalidArgument, "endpoint port is empty");
  }
  if (text.size() > 5) {
    return af_status(ErrorCode::InvalidArgument,
                  "endpoint port '" + std::string(text) + "' is longer than five digits");
  }
  std::uint32_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return af_status(ErrorCode::InvalidArgument,
                    "endpoint port '" + std::string(text) + "' contains a non-digit character");
    }
    value = (value * 10u) + static_cast<std::uint32_t>(character - '0');
  }
  if (value > 65535u) {
    return af_status(ErrorCode::InvalidArgument,
                  "endpoint port '" + std::string(text) + "' is greater than 65535");
  }
  return static_cast<std::uint16_t>(value);
}

/// Split "host:port". An IPv6 host must be bracketed, so the final colon is
/// unambiguously the port separator.
Result<std::pair<std::string_view, std::string_view>> split_host_port(std::string_view text) {
  const std::size_t separator = text.rfind(':');
  if (separator == std::string_view::npos) {
    return af_status(ErrorCode::InvalidArgument,
                  "endpoint '" + std::string(text) + "' has no ':' separating host from port");
  }
  const std::string_view host = text.substr(0, separator);
  const std::string_view port = text.substr(separator + 1);
  if (host.empty()) {
    return af_status(ErrorCode::InvalidArgument,
                  "endpoint '" + std::string(text) + "' has an empty host");
  }
  return std::make_pair(host, port);
}

/// Both helpers below are called only with a host that classify_host already
/// accepted, so inet_pton cannot fail.
sockaddr_in make_address_v4(const Endpoint& endpoint) noexcept {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  ::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr);
  return address;
}

sockaddr_in6 make_address_v6(const Endpoint& endpoint) noexcept {
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(endpoint.port);
  ::inet_pton(AF_INET6, endpoint.host.c_str(), &address.sin6_addr);
  address.sin6_flowinfo = 0;
  address.sin6_scope_id = 0;
  return address;
}

}  // namespace

// ---------------------------------------------------------------------------
// SocketSubsystem
// ---------------------------------------------------------------------------

SocketSubsystem::SocketSubsystem() {
  const Status status = ensure_initialized();
  if (!status.ok()) {
    return;
  }
  const std::lock_guard<std::mutex> guard(g_subsystem_mutex);
  if (g_subsystem_ready) {
    ++g_subsystem_references;
  }
}

SocketSubsystem::~SocketSubsystem() {
#if defined(_WIN32)
  bool last = false;
  {
    const std::lock_guard<std::mutex> guard(g_subsystem_mutex);
    if (g_subsystem_references > 0) {
      --g_subsystem_references;
    }
    last = (g_subsystem_references == 0) && g_subsystem_ready;
  }
  if (last) {
    ::WSACleanup();
  }
#endif
}

Status SocketSubsystem::ensure_initialized() {
#if defined(_WIN32)
  int failure = 0;
  {
    // The whole probe is serialized: WSAStartup is itself reference counted, so
    // a second startup overlapping the first would leak a reference.
    const std::lock_guard<std::mutex> guard(g_subsystem_mutex);
    if (g_subsystem_ready) {
      return Status();
    }
    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      failure = result;
    } else {
      g_subsystem_ready = true;
    }
  }
  if (failure != 0) {
    return af_status(ErrorCode::Unavailable,
                  "WSAStartup(2.2) failed with " + describe_socket_error(failure));
  }
  return Status();
#else
  return Status();
#endif
}

bool SocketSubsystem::initialized() noexcept {
#if defined(_WIN32)
  const std::lock_guard<std::mutex> guard(g_subsystem_mutex);
  return g_subsystem_ready;
#else
  return true;
#endif
}

// ---------------------------------------------------------------------------
// SocketHandle
// ---------------------------------------------------------------------------

SocketHandle::SocketHandle(SocketHandle&& other) noexcept : raw_(other.raw_) {
  other.raw_ = kInvalid;
}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
  if (this != &other) {
    close();
    raw_ = other.raw_;
    other.raw_ = kInvalid;
  }
  return *this;
}

SocketHandle::~SocketHandle() { close(); }

std::uintptr_t SocketHandle::release() noexcept {
  const std::uintptr_t raw = raw_;
  raw_ = kInvalid;
  return raw;
}

void SocketHandle::close() noexcept {
  if (!valid()) {
    return;
  }
  const native_socket_t socket = native_socket_from_raw(raw_);
  raw_ = kInvalid;
  if (!handle_is_valid(socket)) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

// ---------------------------------------------------------------------------
// Endpoint
// ---------------------------------------------------------------------------

std::string Endpoint::to_string() const {
  std::string text = host;
  text += ':';
  text += std::to_string(port);
  return text;
}

Result<Endpoint> parse_endpoint(std::string_view text) {
  if (text.empty()) {
    return af_status(ErrorCode::InvalidArgument, "endpoint text is empty");
  }

  const Result<std::pair<std::string_view, std::string_view>> parts = split_host_port(text);
  if (!parts.ok()) {
    return parts.status();
  }

  const Result<ParsedAddress> host = classify_host(parts.value().first);
  if (!host.ok()) {
    return host.status();
  }
  const Result<std::uint16_t> port = parse_decimal_port(parts.value().second);
  if (!port.ok()) {
    return port.status();
  }

  Endpoint endpoint;
  endpoint.host = host.value().canonical;
  endpoint.port = port.value();
  return endpoint;
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

Result<TcpListener> TcpListener::bind(const Endpoint& endpoint) {
  AF_TRY(SocketSubsystem::ensure_initialized());

  const Result<ParsedAddress> host = classify_host(endpoint.host);
  if (!host.ok()) {
    return host.status();
  }
  const int family = host.value().family;

  const native_socket_t raw = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (!handle_is_valid(raw)) {
    return socket_error_status(ErrorCode::TransportFailure, "socket", last_native_socket_error());
  }
  SocketHandle socket(raw_from_native_socket(raw));

#if defined(_WIN32)
  // SO_EXCLUSIVEADDRUSE is the Windows binding guarantee: no second socket may
  // take over a port this listener already owns. SO_REUSEADDR on Windows means
  // the opposite of its POSIX meaning, so it is only a last resort for a
  // platform that does not implement the exclusive option.
  {
    const BOOL exclusive = TRUE;
    if (::setsockopt(native_socket_from_raw(socket.raw()), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive),
                     static_cast<int>(sizeof(exclusive))) == SOCKET_ERROR) {
      const BOOL reuse = TRUE;
      if (::setsockopt(native_socket_from_raw(socket.raw()), SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse),
                       static_cast<int>(sizeof(reuse))) == SOCKET_ERROR) {
        return socket_error_status(ErrorCode::TransportFailure, "setsockopt(bind)",
                                   last_native_socket_error());
      }
    }
  }
#endif

  socket_length_t address_length = 0;
  sockaddr_storage address{};
  if (family == AF_INET6) {
    const sockaddr_in6 local = make_address_v6(endpoint);
    std::memcpy(&address, &local, sizeof(local));
    address_length = static_cast<socket_length_t>(sizeof(local));
  } else {
    const sockaddr_in local = make_address_v4(endpoint);
    std::memcpy(&address, &local, sizeof(local));
    address_length = static_cast<socket_length_t>(sizeof(local));
  }

  if (::bind(native_socket_from_raw(socket.raw()), reinterpret_cast<const sockaddr*>(&address), address_length) ==
      SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "bind", last_native_socket_error());
  }

  if (::listen(native_socket_from_raw(socket.raw()), kBacklog) == SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "listen", last_native_socket_error());
  }

  // Read the bound port back: binding to port 0 selects an ephemeral port and
  // the caller can only learn it from the socket itself.
  sockaddr_storage bound{};
  socket_length_t bound_length = static_cast<socket_length_t>(sizeof(bound));
  if (::getsockname(native_socket_from_raw(socket.raw()), reinterpret_cast<sockaddr*>(&bound), &bound_length) ==
      SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "getsockname",
                               last_native_socket_error());
  }
  if (bound.ss_family != family) {
    return af_status(ErrorCode::Internal, "getsockname reported an address family the listener "
                                        "did not bind");
  }

  std::uint16_t port = 0;
  if (family == AF_INET6) {
    sockaddr_in6 bound_v6{};
    std::memcpy(&bound_v6, &bound, sizeof(bound_v6));
    port = ntohs(bound_v6.sin6_port);
  } else {
    sockaddr_in bound_v4{};
    std::memcpy(&bound_v4, &bound, sizeof(bound_v4));
    port = ntohs(bound_v4.sin_port);
  }
  if (port == 0) {
    return af_status(ErrorCode::Internal, "listener bound but the operating system reported port 0");
  }

  TcpListener listener;
  listener.socket_ = std::move(socket);
  listener.port_ = port;
  return listener;
}

Result<SocketHandle> TcpListener::accept() {
  if (!socket_.valid()) {
    return af_status(ErrorCode::ConnectionClosed, "accept on a listener that is not bound");
  }

  sockaddr_storage peer{};
  socket_length_t peer_length = static_cast<socket_length_t>(sizeof(peer));
  const native_socket_t accepted =
      ::accept(native_socket_from_raw(socket_.raw()), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (!handle_is_valid(accepted)) {
    const int native = last_native_socket_error();
    // Closing the listening socket interrupts a blocked accept. That is a
    // shutdown, not a transport failure.
    if (!socket_.valid()) {
      return af_status(ErrorCode::ConnectionClosed,
                    "listener closed while a connection was pending: " +
                        describe_socket_error(native));
    }
    if (socket_error_is_disconnect()) {
      return af_status(ErrorCode::ConnectionClosed,
                    "connection abandoned before it was accepted: " +
                        describe_socket_error(native));
    }
    return socket_error_status(ErrorCode::TransportFailure, "accept", native);
  }

  SocketHandle connection(raw_from_native_socket(accepted));
  AF_TRY(configure_socket(connection));
  return connection;
}

void TcpListener::close() {
  // Closing the listening socket is the only mechanism that unblocks a caller
  // parked in accept(); the failing accept then observes an invalid handle.
  socket_.close();
}

// ---------------------------------------------------------------------------
// connect / configure / diagnostics
// ---------------------------------------------------------------------------

Result<SocketHandle> connect_to(const Endpoint& endpoint) {
  AF_TRY(SocketSubsystem::ensure_initialized());

  const Result<ParsedAddress> host = classify_host(endpoint.host);
  if (!host.ok()) {
    return host.status();
  }
  const int family = host.value().family;

  const native_socket_t raw = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (!handle_is_valid(raw)) {
    return socket_error_status(ErrorCode::TransportFailure, "socket", last_native_socket_error());
  }
  SocketHandle socket(raw_from_native_socket(raw));

  if (family == AF_INET6) {
    const sockaddr_in6 remote = make_address_v6(endpoint);
    if (::connect(native_socket_from_raw(socket.raw()), reinterpret_cast<const sockaddr*>(&remote),
                  static_cast<socket_length_t>(sizeof(remote))) == SOCKET_ERROR) {
      return socket_error_status(ErrorCode::TransportFailure,
                                 "connect to " + endpoint.to_string(),
                                 last_native_socket_error());
    }
  } else {
    const sockaddr_in remote = make_address_v4(endpoint);
    if (::connect(native_socket_from_raw(socket.raw()), reinterpret_cast<const sockaddr*>(&remote),
                  static_cast<socket_length_t>(sizeof(remote))) == SOCKET_ERROR) {
      return socket_error_status(ErrorCode::TransportFailure,
                                 "connect to " + endpoint.to_string(),
                                 last_native_socket_error());
    }
  }

  AF_TRY(configure_socket(socket));
  return socket;
}

Status configure_socket(SocketHandle& socket) {
  if (!socket.valid()) {
    return af_status(ErrorCode::NotConnected, "configure_socket on an invalid handle");
  }
  const native_socket_t raw = native_socket_from_raw(socket.raw());

  const BOOL nodelay = TRUE;
  if (::setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                   static_cast<int>(sizeof(nodelay))) == SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "setsockopt(TCP_NODELAY)",
                               last_native_socket_error());
  }

  const BOOL keepalive = TRUE;
  if (::setsockopt(raw, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive),
                   static_cast<int>(sizeof(keepalive))) == SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "setsockopt(SO_KEEPALIVE)",
                               last_native_socket_error());
  }

#if defined(_WIN32)
  // Keepalive with an actually useful probe interval; the Windows default is
  // two hours, which tells a coordinator nothing about a dead worker.
  tcp_keepalive parameters{};
  parameters.onoff = 1;
  parameters.keepalivetime = 10000;
  parameters.keepaliveinterval = 3000;
  DWORD returned = 0;
  if (::WSAIoctl(raw, SIO_KEEPALIVE_VALS, &parameters,
                 static_cast<DWORD>(sizeof(parameters)), nullptr, 0, &returned, nullptr,
                 nullptr) == SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "WSAIoctl(SIO_KEEPALIVE_VALS)",
                               last_native_socket_error());
  }
#endif

  // Linger with a zero timeout: close() sends a reset rather than waiting for
  // an unacknowledged send buffer to drain, so a hung peer cannot hold up
  // shutdown of this process.
  linger option{};
  option.l_onoff = 1;
  option.l_linger = 0;
  if (::setsockopt(raw, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&option),
                   static_cast<int>(sizeof(option))) == SOCKET_ERROR) {
    return socket_error_status(ErrorCode::TransportFailure, "setsockopt(SO_LINGER)",
                               last_native_socket_error());
  }

#if defined(_WIN32)
  // A child process must not inherit this socket. CreateProcess inherits every
  // inheritable handle, so the flag has to be cleared explicitly.
  if (::SetHandleInformation(reinterpret_cast<HANDLE>(raw), HANDLE_FLAG_INHERIT, 0) == 0) {
    const int native = static_cast<int>(::GetLastError());
    return af_status(ErrorCode::TransportFailure,
                  "SetHandleInformation(clear inherit) failed with Win32 error " +
                      std::to_string(native));
  }
#endif

  return Status();
}

bool socket_error_is_disconnect() noexcept {
  const int native = last_native_socket_error();
#if defined(_WIN32)
  switch (native) {
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
    case WSAEDISCON:
      return true;
    default:
      return false;
  }
#else
  switch (native) {
    case ECONNRESET:
    case ECONNABORTED:
    case ENOTCONN:
    case ESHUTDOWN:
    case EPIPE:
      return true;
    default:
      return false;
  }
#endif
}

Status last_socket_error(std::string_view context) {
  return socket_error_status(ErrorCode::TransportFailure, context, last_native_socket_error());
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(SocketHandle socket, std::string peer, ConnectionCallbacks callbacks)
    : socket_(std::move(socket)), peer_(std::move(peer)), callbacks_(std::move(callbacks)) {}

Connection::~Connection() {
  {
    const std::lock_guard<std::mutex> guard(queue_mutex_);
    writer_stop_ = true;
  }
  queue_cv_.notify_all();
  shutdown_socket();

  // A connection can legitimately be released by the very callback it is
  // delivering: a peer that disconnects is reported through on_closed, and the
  // owner of that callback typically drops its last reference right there, on
  // the reader thread. Because each thread holds a reference of its own for as
  // long as it runs (see start()), that cannot happen while a loop is still
  // executing: by the time this destructor runs, the thread it is running on
  // has already returned from its loop and is releasing its last reference. Such
  // a thread can therefore be detached -- it must never be joined, because a
  // thread cannot join itself, and leaving it joinable would let the std::thread
  // destructor call std::terminate and kill the process.
  const std::thread::id current = std::this_thread::get_id();
  if (reader_.joinable()) {
    if (reader_.get_id() == current) {
      reader_.detach();
    } else {
      reader_.join();
    }
  }
  if (writer_.joinable()) {
    if (writer_.get_id() == current) {
      writer_.detach();
    } else {
      writer_.join();
    }
  }
}

Status Connection::start() {
  if (started_.exchange(true, std::memory_order_acq_rel)) {
    return af_status(ErrorCode::AlreadyExists, "connection reader and writer threads are already "
                                            "running");
  }

  // Each loop holds a reference to the connection for as long as it is running,
  // so the object outlives both of its threads even when the owner releases it
  // from inside one of the callbacks those threads deliver.
  const std::shared_ptr<Connection> self = shared_from_this();

  try {
    reader_ = std::thread([this, self] { reader_loop(); });
  } catch (...) {
    started_.store(false, std::memory_order_release);
    return af_status(ErrorCode::ResourceExhausted, "could not create the connection reader thread");
  }

  try {
    writer_ = std::thread([this, self] { writer_loop(); });
  } catch (...) {
    {
      const std::lock_guard<std::mutex> guard(queue_mutex_);
      writer_stop_ = true;
    }
    queue_cv_.notify_all();
    shutdown_socket();
    reader_.join();
    started_.store(false, std::memory_order_release);
    return af_status(ErrorCode::ResourceExhausted, "could not create the connection writer thread");
  }

  return Status();
}

void Connection::shutdown_socket() { socket_.close(); }

void Connection::request_close(const Status& reason) {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(close_mutex_);
    close_reason_ = reason;
  }
  // Closing the socket is what unblocks a reader parked in recv and a writer
  // parked in send. Both threads then observe closed_ and unwind.
  shutdown_socket();
  if (callbacks_.on_closed) {
    callbacks_.on_closed(close_reason_);
  }
}

std::size_t Connection::queued_frames() const {
  const std::lock_guard<std::mutex> guard(queue_mutex_);
  return outbound_.size();
}

void Connection::reader_loop() {
  std::string buffer;
  buffer.reserve(kInitialReadBufferBytes);
  std::string scratch(kReceiveChunkBytes, '\0');
  const native_socket_t raw = native_socket_from_raw(socket_.raw());
  Status reason;

  for (;;) {
    // Decode every complete frame already in the buffer before reading again,
    // so several coalesced frames are delivered in one pass.
    for (;;) {
      if (buffer.empty()) {
        break;
      }
      std::size_t consumed = 0;
      const Result<Frame> decoded = decode_frame(std::string_view(buffer), &consumed);
      if (!decoded.ok()) {
        const Status& status = decoded.status();
        if (status_is_incomplete(status)) {
          break;
        }
        // FrameCorrupt and ProtocolViolation are fatal: the byte stream is no
        // longer parseable, so continuing would only reinterpret the peer's
        // garbage as framing. Every other decoder failure is fatal too; only
        // "incomplete" means read more.
        reason = status;
        break;
      }
      if (consumed == 0 || consumed > buffer.size()) {
        reason = af_status(ErrorCode::ProtocolViolation,
                        "frame decoder consumed " + std::to_string(consumed) +
                            " bytes of a " + std::to_string(buffer.size()) + " byte buffer");
        break;
      }
      Frame frame = std::move(decoded).value();
      buffer.erase(0, consumed);
      if (callbacks_.on_frame) {
        callbacks_.on_frame(frame);
      }
    }
    if (!reason.ok()) {
      break;
    }

    const int received =
        ::recv(raw, scratch.data(), static_cast<int>(kReceiveChunkBytes), 0);
    if (received == 0) {
      // Orderly shutdown by the peer.
      break;
    }
    if (received < 0) {
      if (socket_error_is_disconnect()) {
        break;
      }
      reason = socket_error_status(ErrorCode::TransportFailure, "recv",
                                   last_native_socket_error());
      break;
    }
    buffer.append(scratch.data(), static_cast<std::size_t>(received));
  }

  if (!reason.ok()) {
    request_close(reason);
  } else {
    request_close(af_status(ErrorCode::ConnectionClosed, "peer closed the connection"));
  }
}

void Connection::writer_loop() {
  const native_socket_t raw = native_socket_from_raw(socket_.raw());

  for (;;) {
    std::string frame;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return writer_stop_ || !outbound_.empty(); });
      if (outbound_.empty()) {
        if (writer_stop_) {
          return;
        }
        continue;
      }
      frame = std::move(outbound_.front());
      outbound_.pop_front();
      // The lock is released here, before the first send. No socket operation
      // ever runs while the queue mutex is held.
    }

    std::size_t offset = 0;
    while (offset < frame.size()) {
      const std::size_t remaining = frame.size() - offset;
      const int chunk = remaining > static_cast<std::size_t>(0x7FFFFFFF)
                            ? 0x7FFFFFFF
                            : static_cast<int>(remaining);
      const int sent = ::send(raw, frame.data() + offset, chunk, 0);
      if (sent < 0) {
        if (closed_.load(std::memory_order_acquire)) {
          return;
        }
        request_close(socket_error_status(ErrorCode::TransportFailure, "send",
                                          last_native_socket_error()));
        return;
      }
      offset += static_cast<std::size_t>(sent);
    }
  }
}

Status Connection::send(MessageType type, std::string payload) {
  FrameHeader header;
  header.type = type;
  return send_frame(header, std::move(payload));
}

Status Connection::send_frame(FrameHeader header, std::string payload) {
  if (closed_.load(std::memory_order_acquire)) {
    return af_status(ErrorCode::ConnectionClosed, "frame not queued: the connection is closed");
  }

  header.payload_length = static_cast<std::uint32_t>(payload.size());
  header.payload_crc32c = crc32c(payload);
  header.sequence = next_sequence();

  // Encoding happens outside the queue lock, so a large frame never delays
  // another producer and never delays the writer thread.
  std::string frame = encode_frame(header, payload);
  payload.clear();
  payload.shrink_to_fit();

  std::size_t depth = 0;
  {
    const std::lock_guard<std::mutex> guard(queue_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
      return af_status(ErrorCode::ConnectionClosed, "frame not queued: the connection is closed");
    }
    outbound_.push_back(std::move(frame));
    depth = outbound_.size();
  }
  queue_cv_.notify_one();

  if (depth > kMaxOutboundQueueDepth) {
    const Status reason(ErrorCode::QueueCapacityExceeded,
                        "outbound queue holds " + std::to_string(depth) +
                            " frames, which exceeds the limit of " +
                            std::to_string(kMaxOutboundQueueDepth));
    request_close(reason);
    return reason;
  }
  return Status();
}

}  // namespace autonomous_foundry

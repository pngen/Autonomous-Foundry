// src/process.cpp
//
// Child process management.
//
// Every child is launched from an explicit executable path plus an argument
// vector. No shell is ever involved, so no argument can be reinterpreted as
// shell syntax and no operator-supplied value can inject a command. The child
// is created without a console window and without inheriting arbitrary handles,
// so automated validation never flashes a Command Prompt and never produces an
// interactive error dialog.
//
// Argument quoting is the CommandLineToArgvW inverse: an argument is wrapped in
// quotes when it is empty or contains a space, a tab or a quote; a run of
// backslashes immediately preceding a quote is doubled; and a trailing run of
// backslashes is doubled before the closing quote. That is exactly the rule the
// C runtime's own parser applies, so a path with a space, an embedded quote or a
// trailing backslash survives the round trip unchanged.
//
// No execution duration limit is imposed anywhere in this layer. An evaluation
// runs to natural completion; a hang is a defect to diagnose, not something to
// paper over with a deadline.

#include "autonomous_foundry/process.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace autonomous_foundry {

namespace {

/// Bytes drained from the capture pipe per ReadFile/read call.
constexpr std::size_t kCaptureChunkBytes = 64u * 1024u;

/// Attempts made to create a unique transient directory before giving up.
constexpr int kTransientDirectoryAttempts = 64;

std::atomic<std::uint64_t> g_transient_counter{0};
std::atomic<std::uint64_t> g_spawn_counter{0};

/// Build a failure Status from a literal or an assembled message.
Status af_status(ErrorCode code, std::string_view message) {
  return Status(code, message);
}

#if defined(_WIN32)

using native_handle_t = HANDLE;

bool handle_is_valid(native_handle_t handle) noexcept { return handle != nullptr; }

constexpr native_handle_t kNoHandle = nullptr;

std::wstring widen(std::string_view text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  const int produced = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), wide.data(), required);
  if (produced <= 0) {
    return std::wstring();
  }
  wide.resize(static_cast<std::size_t>(produced));
  return wide;
}

/// Narrow a wide path for a diagnostic message. Non-printable characters
/// become '?', so the message is always plain ASCII text.
std::string dump_wide(std::wstring_view text) {
  std::string narrowed;
  narrowed.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const wchar_t character = text[index];
    narrowed.push_back((character >= 0x20 && character < 0x7F) ? static_cast<char>(character)
                                                                 : '?');
  }
  return narrowed;
}

/// Quote one argument so the child's CommandLineToArgvW parses it back
/// unchanged. This is the exact inverse of that parser.
std::wstring quote_argument(std::wstring_view argument) {
  const bool needs_quotes =
      argument.empty() || argument.find_first_of(L" \t\"") != std::wstring_view::npos;
  std::wstring quoted;
  if (needs_quotes) {
    quoted.push_back(L'"');
  }
  std::size_t backslashes = 0;
  for (std::size_t index = 0; index < argument.size(); ++index) {
    const wchar_t character = argument[index];
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      // Double the pending run, then escape the quote itself.
      quoted.append((backslashes * 2) + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    if (backslashes > 0) {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
    }
    quoted.push_back(character);
  }
  if (needs_quotes) {
    // A trailing backslash run would otherwise escape the closing quote.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
  } else if (backslashes > 0) {
    quoted.append(backslashes, L'\\');
  }
  return quoted;
}

std::wstring build_command_line(const std::filesystem::path& executable,
                                const std::vector<std::string>& arguments) {
  std::wstring command_line = quote_argument(executable.wstring());
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    command_line.push_back(L' ');
    command_line.append(quote_argument(widen(arguments[index])));
  }
  return command_line;
}

std::string win32_failure(std::string_view operation, std::uint32_t error) {
  return std::string(operation) + " failed with Win32 error " + std::to_string(error);
}

/// The block a child receives must be sorted, must keep the parent's exact
/// names and case, and must be overlaid with the explicit overrides.
Result<std::wstring> build_environment_block(const ChildProcessOptions& options) {
  std::map<std::wstring, std::wstring, std::less<>> environment;

  if (options.inherit_environment) {
    wchar_t* block = ::GetEnvironmentStringsW();
    if (block == nullptr) {
      return af_status(ErrorCode::ProcessSpawnFailure,
                       win32_failure("GetEnvironmentStringsW", ::GetLastError()));
    }
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
      // Entries beginning with '=' are per-drive current-directory variables.
      // They are not name=value pairs and must never reach a child.
      if (*entry == L'=') {
        continue;
      }
      const wchar_t* separator = std::wcschr(entry, L'=');
      if (separator == nullptr || separator == entry) {
        ::FreeEnvironmentStringsW(block);
        return af_status(ErrorCode::ProcessSpawnFailure,
                         "the inherited environment block holds an entry that is not a "
                         "name=value pair");
      }
      environment[std::wstring(entry, static_cast<std::size_t>(separator - entry))] =
          std::wstring(separator + 1);
    }
    ::FreeEnvironmentStringsW(block);
  }

  for (std::map<std::string, std::string>::const_iterator entry = options.environment.begin();
       entry != options.environment.end(); ++entry) {
    if (entry->first.empty() || entry->first.find('=') != std::string::npos) {
      return af_status(ErrorCode::InvalidArgument,
                       "environment variable name '" + entry->first +
                           "' is empty or contains '='");
    }
    const std::wstring name = widen(entry->first);
    environment.erase(name);
    environment[name] = widen(entry->second);
  }

  std::wstring block;
  for (std::map<std::wstring, std::wstring, std::less<>>::const_iterator entry =
           environment.begin();
       entry != environment.end(); ++entry) {
    block.append(entry->first);
    block.push_back(L'=');
    block.append(entry->second);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

struct PlatformProcess {
  native_handle_t process{kNoHandle};
  native_handle_t thread{kNoHandle};
  std::uint32_t process_id{0};

  void close_handles() noexcept {
    if (handle_is_valid(thread)) {
      ::CloseHandle(thread);
      thread = kNoHandle;
    }
    if (handle_is_valid(process)) {
      ::CloseHandle(process);
      process = kNoHandle;
    }
  }
};

Status wait_for_exit(native_handle_t process) {
  const DWORD waited = ::WaitForSingleObject(process, INFINITE);
  if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
    return Status();
  }
  if (waited == WAIT_FAILED) {
    return af_status(ErrorCode::ProcessWaitFailure,
                     win32_failure("WaitForSingleObject", ::GetLastError()));
  }
  return af_status(ErrorCode::ProcessWaitFailure,
                   "WaitForSingleObject returned wait result " + std::to_string(waited));
}

bool process_is_running(native_handle_t process) {
  return ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

void close_descriptor(native_handle_t& handle) noexcept {
  if (handle_is_valid(handle)) {
    ::CloseHandle(handle);
    handle = kNoHandle;
  }
}

void drain_capture(native_handle_t read_end, std::string& captured, std::size_t maximum,
                   bool& truncated) {
  std::vector<char> buffer(kCaptureChunkBytes);
  for (;;) {
    DWORD received = 0;
    const BOOL ok = ::ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()),
                               &received, nullptr);
    if (ok == 0 || received == 0) {
      // Zero bytes means the last writer closed the pipe.
      return;
    }
    const std::size_t count = static_cast<std::size_t>(received);
    if (captured.size() < maximum) {
      const std::size_t room = maximum - captured.size();
      const std::size_t take = count < room ? count : room;
      captured.append(buffer.data(), take);
      if (take < count) {
        truncated = true;
      }
    } else {
      // The bound is reached: keep draining so the child can never block on a
      // full pipe, but stop appending.
      truncated = true;
    }
  }
}

Result<std::filesystem::path> current_executable() {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return af_status(ErrorCode::IoFailure,
                       win32_failure("GetModuleFileNameW", ::GetLastError()));
    }
    if (static_cast<std::size_t>(written) < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), written));
    }
    if (buffer.size() >= 32768u) {
      return af_status(ErrorCode::IoFailure,
                       "GetModuleFileNameW did not return a path within the maximum length");
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path temporary_root() {
  std::vector<wchar_t> buffer(MAX_PATH + 1);
  const DWORD capacity = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (capacity == 0 || static_cast<std::size_t>(capacity) > buffer.size()) {
    return std::filesystem::path();
  }
  std::filesystem::path root(std::wstring(buffer.data(), capacity));
  root /= L"autonomous-foundry";
  return root;
}

#else  // POSIX

using native_handle_t = int;

bool handle_is_valid(native_handle_t handle) noexcept { return handle >= 0; }

constexpr native_handle_t kNoHandle = -1;

struct PlatformProcess {
  native_handle_t process{kNoHandle};
  std::uint32_t process_id{0};

  void close_handles() noexcept {
    // The child is reaped by waitpid, so no handle needs releasing.
  }
};

Status wait_for_exit(native_handle_t process) {
  int status = 0;
  for (;;) {
    const pid_t waited = ::waitpid(static_cast<pid_t>(process), &status, 0);
    if (waited >= 0) {
      return Status();
    }
    if (errno != EINTR) {
      return af_status(ErrorCode::ProcessWaitFailure,
                       "waitpid failed with errno " + std::to_string(errno));
    }
  }
}

bool process_is_running(native_handle_t process) {
  int status = 0;
  return ::waitpid(static_cast<pid_t>(process), &status, WNOHANG) == 0;
}

void close_descriptor(native_handle_t& handle) noexcept {
  if (handle_is_valid(handle)) {
    ::close(handle);
    handle = kNoHandle;
  }
}

void drain_capture(native_handle_t read_end, std::string& captured, std::size_t maximum,
                   bool& truncated) {
  std::vector<char> buffer(kCaptureChunkBytes);
  for (;;) {
    const ssize_t received = ::read(read_end, buffer.data(), buffer.size());
    if (received <= 0) {
      return;
    }
    const std::size_t count = static_cast<std::size_t>(received);
    if (captured.size() < maximum) {
      const std::size_t room = maximum - captured.size();
      const std::size_t take = count < room ? count : room;
      captured.append(buffer.data(), take);
      if (take < count) {
        truncated = true;
      }
    } else {
      truncated = true;
    }
  }
}

Result<std::filesystem::path> current_executable() {
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    return af_status(ErrorCode::IoFailure,
                     "cannot determine the current executable path: " + error.message());
  }
  return resolved;
}

std::filesystem::path temporary_root() {
  const char* environment = std::getenv("TMPDIR");
  if (environment != nullptr && environment[0] != '\0') {
    return std::filesystem::path(environment) / "autonomous-foundry";
  }
  return std::filesystem::path("/tmp") / "autonomous-foundry";
}

#endif

/// Path of the log file a managed child writes its output to.
std::filesystem::path make_log_path(const std::filesystem::path& directory,
                                    std::uint64_t counter) {
#if defined(_WIN32)
  const unsigned long parent = ::GetCurrentProcessId();
#else
  const unsigned long parent = static_cast<unsigned long>(::getpid());
#endif
  return directory / (std::to_string(parent) + "-" + std::to_string(counter) + ".log");
}

}  // namespace

// ---------------------------------------------------------------------------
// run_process
// ---------------------------------------------------------------------------

Result<ProcessResult> run_process(const ChildProcessOptions& options) {
  ProcessResult result;

  if (options.executable.empty()) {
    return af_status(ErrorCode::InvalidArgument, "no executable path was supplied");
  }

#if defined(_WIN32)
  const Result<std::wstring> environment_block = build_environment_block(options);
  if (!environment_block.ok()) {
    return environment_block.status();
  }
  if (environment_block.value().size() > 32767u) {
    return af_status(ErrorCode::InvalidArgument,
                     "the environment block exceeds the 32767 character limit imposed by "
                     "CreateProcessW");
  }

  native_handle_t pipe_read = kNoHandle;
  native_handle_t pipe_write = kNoHandle;
  if (options.capture_output) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = nullptr;
    attributes.bInheritHandle = TRUE;
    if (::CreatePipe(&pipe_read, &pipe_write, &attributes, 0) == 0) {
      return af_status(ErrorCode::ProcessSpawnFailure,
                       win32_failure("CreatePipe", ::GetLastError()));
    }
  }

  // The read end belongs to the parent alone. If the child inherited it, the
  // pipe would never report end of stream and the drain below would not end.
  if (handle_is_valid(pipe_read) &&
      ::SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0) == 0) {
    const std::uint32_t error = ::GetLastError();
    close_descriptor(pipe_write);
    close_descriptor(pipe_read);
    return af_status(ErrorCode::ProcessSpawnFailure,
                     win32_failure("SetHandleInformation", error));
  }

  const std::wstring command_line = build_command_line(options.executable, options.arguments);
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  std::wstring working_directory;
  const wchar_t* working_directory_pointer = nullptr;
  if (!options.working_directory.empty()) {
    working_directory = options.working_directory.wstring();
    working_directory_pointer = working_directory.c_str();
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = handle_is_valid(pipe_write) ? pipe_write : ::GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = handle_is_valid(pipe_write) ? pipe_write : ::GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION information{};
  const std::wstring application = options.executable.wstring();
  const BOOL created =
      ::CreateProcessW(application.c_str(), mutable_command_line.data(), nullptr, nullptr,
                       handle_is_valid(pipe_write) ? TRUE : FALSE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       const_cast<wchar_t*>(environment_block.value().data()),
                       working_directory_pointer, &startup, &information);
  const std::uint32_t create_error = ::GetLastError();

  // The parent's copy of the write end goes away immediately, whatever
  // happened, or the read end would never see end of stream.
  close_descriptor(pipe_write);

  if (created == 0) {
    close_descriptor(pipe_read);
    return af_status(ErrorCode::ProcessSpawnFailure,
                     win32_failure("CreateProcessW for " + dump_wide(application),
                                   create_error));
  }

  ::CloseHandle(information.hThread);
  result.started = true;

  if (handle_is_valid(pipe_read)) {
    drain_capture(pipe_read, result.standard_output, options.max_capture_bytes,
                  result.output_truncated);
    close_descriptor(pipe_read);
  }

  AF_TRY(wait_for_exit(information.hProcess));

  DWORD exit_code = 0;
  if (::GetExitCodeProcess(information.hProcess, &exit_code) == 0) {
    const std::uint32_t error = ::GetLastError();
    ::CloseHandle(information.hProcess);
    return af_status(ErrorCode::ProcessWaitFailure,
                     win32_failure("GetExitCodeProcess", error));
  }
  ::CloseHandle(information.hProcess);

  result.exited = true;
  result.exit_code = static_cast<int>(exit_code);
  result.termination_status = static_cast<std::uint32_t>(exit_code);
  // A process killed by TerminateProcess or by an unhandled exception reports
  // an exit code that is not a normal completion value.
  result.terminated_abnormally = exit_code != 0 && exit_code != 1;
  // Both streams are directed at the same pipe, so the capture is the child's
  // interleaved output exactly as the child produced it.
  result.standard_error = result.standard_output;
  return result;
#else
  int pipe_descriptors[2] = {-1, -1};
  if (options.capture_output && ::pipe(pipe_descriptors) != 0) {
    return af_status(ErrorCode::ProcessSpawnFailure,
                     "pipe failed with errno " + std::to_string(errno));
  }

  std::vector<std::string> owned_arguments;
  owned_arguments.push_back(options.executable.string());
  for (std::size_t index = 0; index < options.arguments.size(); ++index) {
    owned_arguments.push_back(options.arguments[index]);
  }
  std::vector<char*> argument_vector;
  argument_vector.reserve(owned_arguments.size() + 1);
  for (std::size_t index = 0; index < owned_arguments.size(); ++index) {
    argument_vector.push_back(owned_arguments[index].data());
  }
  argument_vector.push_back(nullptr);

  std::vector<std::string> owned_environment;
  if (!options.inherit_environment) {
    for (std::map<std::string, std::string>::const_iterator entry = options.environment.begin();
         entry != options.environment.end(); ++entry) {
      if (entry->first.empty() || entry->first.find('=') != std::string::npos) {
        if (handle_is_valid(pipe_descriptors[0])) {
          ::close(pipe_descriptors[0]);
        }
        if (handle_is_valid(pipe_descriptors[1])) {
          ::close(pipe_descriptors[1]);
        }
        return af_status(ErrorCode::InvalidArgument,
                         "environment variable name '" + entry->first +
                             "' is empty or contains '='");
      }
      owned_environment.push_back(entry->first + "=" + entry->second);
    }
  }
  std::vector<char*> environment_vector;
  if (options.inherit_environment) {
    environment_vector.push_back(nullptr);
  } else {
    for (std::size_t index = 0; index < owned_environment.size(); ++index) {
      environment_vector.push_back(owned_environment[index].data());
    }
    environment_vector.push_back(nullptr);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (handle_is_valid(pipe_descriptors[1])) {
    posix_spawn_file_actions_adddup2(&actions, pipe_descriptors[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_descriptors[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_descriptors[0]);
  }

  pid_t child = -1;
  const int spawned = ::posix_spawn(&child, owned_arguments[0].c_str(), &actions, nullptr,
                                    argument_vector.data(), environment_vector.data());
  posix_spawn_file_actions_destroy(&actions);

  if (spawned != 0) {
    if (handle_is_valid(pipe_descriptors[0])) {
      ::close(pipe_descriptors[0]);
    }
    if (handle_is_valid(pipe_descriptors[1])) {
      ::close(pipe_descriptors[1]);
    }
    return af_status(ErrorCode::ProcessSpawnFailure,
                     "posix_spawn failed with errno " + std::to_string(spawned));
  }

  result.started = true;
  if (handle_is_valid(pipe_descriptors[1])) {
    ::close(pipe_descriptors[1]);
  }
  if (handle_is_valid(pipe_descriptors[0])) {
    drain_capture(pipe_descriptors[0], result.standard_output, options.max_capture_bytes,
                  result.output_truncated);
    ::close(pipe_descriptors[0]);
  }

  int status = 0;
  for (;;) {
    const pid_t waited = ::waitpid(child, &status, 0);
    if (waited >= 0) {
      break;
    }
    if (errno != EINTR) {
      return af_status(ErrorCode::ProcessWaitFailure,
                       "waitpid failed with errno " + std::to_string(errno));
    }
  }

  result.exited = true;
  if (WIFSIGNALED(status)) {
    result.terminated_abnormally = true;
    result.termination_status = static_cast<std::uint32_t>(WTERMSIG(status));
    result.exit_code = 128 + static_cast<int>(WTERMSIG(status));
  } else {
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.exit_code = exit_code;
    result.termination_status = static_cast<std::uint32_t>(exit_code);
    result.terminated_abnormally = false;
  }
  result.standard_error = result.standard_output;
  return result;
#endif
}

// ---------------------------------------------------------------------------
// ManagedProcess
// ---------------------------------------------------------------------------

struct ManagedProcess::Impl {
  PlatformProcess process;
};

ManagedProcess::ManagedProcess() : impl_(new Impl()) {}

ManagedProcess::~ManagedProcess() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->process.process_id != 0 && process_is_running(impl_->process.process)) {
    // Never leave a child running behind a destroyed handle.
    const Status killed = kill();
    if (!killed.ok()) {
      (void)wait(nullptr);
    }
  }
  impl_->process.close_handles();
}

ManagedProcess::ManagedProcess(ManagedProcess&& other) noexcept
    : impl_(std::move(other.impl_)),
      process_id_(other.process_id_),
      log_path_(std::move(other.log_path_)) {
  other.process_id_ = 0;
  other.log_path_.clear();
}

ManagedProcess& ManagedProcess::operator=(ManagedProcess&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    process_id_ = other.process_id_;
    log_path_ = std::move(other.log_path_);
    other.process_id_ = 0;
    other.log_path_.clear();
  }
  return *this;
}

Status ManagedProcess::start(const ChildProcessOptions& options) {
  if (options.executable.empty()) {
    return af_status(ErrorCode::InvalidArgument, "no executable path was supplied");
  }
  if (impl_ == nullptr) {
    impl_.reset(new Impl());
  }
  if (running()) {
    return af_status(ErrorCode::AlreadyExists,
                     "this managed process is already running with identifier " +
                         std::to_string(process_id_));
  }

  const Result<std::filesystem::path> root = make_transient_directory("process");
  if (!root.ok()) {
    return root.status();
  }
  const std::filesystem::path log_directory = root.value() / "logs";
  std::error_code error;
  std::filesystem::create_directories(log_directory, error);
  if (error) {
    return af_status(ErrorCode::IoFailure,
                     "cannot create the process log directory: " + error.message());
  }
  const std::filesystem::path log_file =
      make_log_path(log_directory, g_spawn_counter.fetch_add(1, std::memory_order_relaxed));

  ChildProcessOptions effective = options;
  // Output goes to the log file, never into this process's memory.
  effective.capture_output = false;

#if defined(_WIN32)
  const Result<std::wstring> environment_block = build_environment_block(effective);
  if (!environment_block.ok()) {
    return environment_block.status();
  }
  if (environment_block.value().size() > 32767u) {
    return af_status(ErrorCode::InvalidArgument,
                     "the environment block exceeds the 32767 character limit imposed by "
                     "CreateProcessW");
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const std::wstring log_wide = log_file.wstring();
  HANDLE log_handle = ::CreateFileW(log_wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log_handle == INVALID_HANDLE_VALUE) {
    return af_status(ErrorCode::IoFailure,
                     win32_failure("CreateFileW for the process log", ::GetLastError()));
  }

  const std::wstring command_line = build_command_line(effective.executable, effective.arguments);
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  std::wstring working_directory;
  const wchar_t* working_directory_pointer = nullptr;
  if (!effective.working_directory.empty()) {
    working_directory = effective.working_directory.wstring();
    working_directory_pointer = working_directory.c_str();
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_handle;
  startup.hStdError = log_handle;

  PROCESS_INFORMATION information{};
  const std::wstring application = effective.executable.wstring();
  const BOOL created =
      ::CreateProcessW(application.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       const_cast<wchar_t*>(environment_block.value().data()),
                       working_directory_pointer, &startup, &information);
  const std::uint32_t create_error = ::GetLastError();
  ::CloseHandle(log_handle);

  if (created == 0) {
    return af_status(ErrorCode::ProcessSpawnFailure,
                     win32_failure("CreateProcessW for " + dump_wide(application),
                                   create_error));
  }

  impl_->process.process = information.hProcess;
  impl_->process.thread = information.hThread;
  impl_->process.process_id = static_cast<std::uint32_t>(information.dwProcessId);
  process_id_ = impl_->process.process_id;
  log_path_ = log_file;
  return Status();
#else
  const int log_descriptor = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (log_descriptor < 0) {
    return af_status(ErrorCode::IoFailure,
                     "cannot create the process log: errno " + std::to_string(errno));
  }

  std::vector<std::string> owned_arguments;
  owned_arguments.push_back(effective.executable.string());
  for (std::size_t index = 0; index < effective.arguments.size(); ++index) {
    owned_arguments.push_back(effective.arguments[index]);
  }
  std::vector<char*> argument_vector;
  for (std::size_t index = 0; index < owned_arguments.size(); ++index) {
    argument_vector.push_back(owned_arguments[index].data());
  }
  argument_vector.push_back(nullptr);

  std::vector<std::string> owned_environment;
  if (!effective.inherit_environment) {
    for (std::map<std::string, std::string>::const_iterator entry = effective.environment.begin();
         entry != effective.environment.end(); ++entry) {
      owned_environment.push_back(entry->first + "=" + entry->second);
    }
  }
  std::vector<char*> environment_vector;
  if (effective.inherit_environment) {
    environment_vector.push_back(nullptr);
  } else {
    for (std::size_t index = 0; index < owned_environment.size(); ++index) {
      environment_vector.push_back(owned_environment[index].data());
    }
    environment_vector.push_back(nullptr);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, log_descriptor, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, log_descriptor, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, log_descriptor);

  pid_t child = -1;
  const int spawned = ::posix_spawn(&child, owned_arguments[0].c_str(), &actions, nullptr,
                                    argument_vector.data(), environment_vector.data());
  posix_spawn_file_actions_destroy(&actions);
  ::close(log_descriptor);
  if (spawned != 0) {
    return af_status(ErrorCode::ProcessSpawnFailure,
                     "posix_spawn failed with errno " + std::to_string(spawned));
  }

  impl_->process.process = static_cast<native_handle_t>(child);
  impl_->process.process_id = static_cast<std::uint32_t>(child);
  process_id_ = impl_->process.process_id;
  log_path_ = log_file;
  return Status();
#endif
}

bool ManagedProcess::running() const {
  if (impl_ == nullptr || impl_->process.process_id == 0 ||
      !handle_is_valid(impl_->process.process)) {
    return false;
  }
  return process_is_running(impl_->process.process);
}

Status ManagedProcess::kill() {
  if (impl_ == nullptr || impl_->process.process_id == 0) {
    return af_status(ErrorCode::NotConnected, "no process has been started");
  }
#if defined(_WIN32)
  if (::TerminateProcess(impl_->process.process, 1) == 0) {
    return af_status(ErrorCode::ProcessWaitFailure,
                     win32_failure("TerminateProcess", ::GetLastError()));
  }
#else
  if (::kill(static_cast<pid_t>(impl_->process.process), SIGKILL) != 0 && errno != ESRCH) {
    return af_status(ErrorCode::ProcessWaitFailure,
                     "kill failed with errno " + std::to_string(errno));
  }
#endif
  int exit_code = 0;
  return wait(&exit_code);
}

Status ManagedProcess::wait(int* exit_code) {
  if (impl_ == nullptr || impl_->process.process_id == 0) {
    return af_status(ErrorCode::NotConnected, "no process has been started");
  }
  AF_TRY(wait_for_exit(impl_->process.process));

  int code = 0;
#if defined(_WIN32)
  DWORD value = 0;
  if (::GetExitCodeProcess(impl_->process.process, &value) == 0) {
    return af_status(ErrorCode::ProcessWaitFailure,
                     win32_failure("GetExitCodeProcess", ::GetLastError()));
  }
  code = static_cast<int>(value);
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(impl_->process.process), &status, WNOHANG) < 0) {
    return af_status(ErrorCode::ProcessWaitFailure,
                     "waitpid failed with errno " + std::to_string(errno));
  }
  code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
  if (exit_code != nullptr) {
    *exit_code = code;
  }
  return Status();
}

Result<std::string> ManagedProcess::read_log(std::uint64_t max_bytes) const {
  if (log_path_.empty()) {
    return std::string();
  }
  std::ifstream file(log_path_, std::ios::binary);
  if (!file) {
    return std::string();
  }
  std::string text;
  std::vector<char> buffer(kCaptureChunkBytes);
  while (static_cast<std::uint64_t>(text.size()) < max_bytes) {
    const std::uint64_t remaining = max_bytes - static_cast<std::uint64_t>(text.size());
    const std::size_t chunk = remaining < static_cast<std::uint64_t>(buffer.size())
                                  ? static_cast<std::size_t>(remaining)
                                  : buffer.size();
    file.read(buffer.data(), static_cast<std::streamsize>(chunk));
    const std::streamsize received = file.gcount();
    if (received <= 0) {
      break;
    }
    text.append(buffer.data(), static_cast<std::size_t>(received));
  }
  return text;
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

Result<std::filesystem::path> current_executable_path() { return current_executable(); }

bool process_is_alive(std::uint32_t process_id) {
  if (process_id == 0) {
    return false;
  }
#if defined(_WIN32)
  HANDLE handle =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
  if (handle == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  const BOOL queried = ::GetExitCodeProcess(handle, &exit_code);
  ::CloseHandle(handle);
  return queried != 0 && exit_code == STILL_ACTIVE;
#else
  if (::kill(static_cast<pid_t>(process_id), 0) == 0) {
    return true;
  }
  return errno == EPERM;
#endif
}

std::filesystem::path default_transient_root() {
  const std::filesystem::path root = temporary_root();
  if (root.empty()) {
    return root;
  }
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return std::filesystem::path();
  }
  return root;
}

Result<std::filesystem::path> make_transient_directory(std::string_view prefix) {
  if (prefix.empty()) {
    return af_status(ErrorCode::InvalidArgument, "a transient directory needs a prefix");
  }
  const char* const reserved = "/\\:*?\"<>|";
  if (prefix.find_first_of(reserved) != std::string_view::npos) {
    return af_status(ErrorCode::InvalidArgument,
                     "transient directory prefix '" + std::string(prefix) +
                         "' contains a path separator or a reserved character");
  }

  const std::filesystem::path root = default_transient_root();
  if (root.empty()) {
    return af_status(ErrorCode::IoFailure,
                     "the transient root directory is unavailable, so no transient directory "
                     "can be created");
  }

#if defined(_WIN32)
  const unsigned long parent = ::GetCurrentProcessId();
#else
  const unsigned long parent = static_cast<unsigned long>(::getpid());
#endif
  const std::string stem = std::string(prefix) + "-" + std::to_string(parent);
  for (int attempt = 0; attempt < kTransientDirectoryAttempts; ++attempt) {
    const std::uint64_t counter = g_transient_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path candidate = root / (stem + "-" + std::to_string(counter));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
    if (error && error != std::errc::file_exists) {
      return af_status(ErrorCode::IoFailure,
                       "cannot create '" + candidate.string() + "': " + error.message());
    }
  }
  return af_status(ErrorCode::ResourceExhausted,
                   "no unique transient directory name was available after " +
                       std::to_string(kTransientDirectoryAttempts) + " attempts");
}

}  // namespace autonomous_foundry

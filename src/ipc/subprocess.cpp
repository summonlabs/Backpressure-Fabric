// Backpressure Fabric - real OS process management.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/ipc/subprocess.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <vector>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
extern char** environ;
#endif

namespace backpressure {
namespace {

#if defined(_WIN32)

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                        needed);
  return wide;
}

[[nodiscard]] std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (needed <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed, nullptr, nullptr);
  return out;
}

/// CommandLineToArgvW-compatible quoting.
[[nodiscard]] std::wstring quote_argument(const std::wstring& arg) {
  const bool needs_quotes = arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) {
    return arg;
  }
  std::wstring out;
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2u + 1u, L'\\');
      backslashes = 0;
      out.push_back(L'"');
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2u, L'\\');
  out.push_back(L'"');
  return out;
}

#endif

}  // namespace

Result<ProcessHandle> spawn_process(const std::vector<std::string>& argv,
                                    const SpawnOptions& options) {
  if (argv.empty() || argv[0].empty()) {
    return fail<ProcessHandle>(ErrorCode::InvalidArgument, "empty argv");
  }
#if defined(_WIN32)
  std::wstring command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      command_line.push_back(L' ');
    }
    command_line.append(quote_argument(widen(argv[i])));
  }
  if (command_line.empty()) {
    return fail<ProcessHandle>(ErrorCode::InvalidArgument, "empty command line");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};

  std::wstring working_directory;
  const wchar_t* working_directory_ptr = nullptr;
  if (!options.working_directory.empty()) {
    working_directory = widen(options.working_directory);
    working_directory_ptr = working_directory.c_str();
  }

  const DWORD flags = options.inherit_stdio ? 0u : CREATE_NO_WINDOW;
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  if (::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                       options.inherit_stdio ? TRUE : FALSE, flags, nullptr,
                       working_directory_ptr, &startup, &info) == 0) {
    return fail<ProcessHandle>(ErrorCode::IoError, "CreateProcess",
                               static_cast<std::uint64_t>(::GetLastError()));
  }
  ::CloseHandle(info.hThread);
  ProcessHandle handle;
  handle.native = info.hProcess;
  handle.pid = static_cast<std::uint32_t>(info.dwProcessId);
  return Result<ProcessHandle>(handle);
#else
  std::vector<std::string> storage = argv;
  std::vector<char*> raw;
  raw.reserve(storage.size() + 1u);
  for (std::string& s : storage) {
    raw.push_back(s.data());
  }
  raw.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!options.inherit_stdio) {
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  }
  pid_t pid = 0;
  const char* cwd = options.working_directory.empty() ? nullptr : options.working_directory.c_str();
  const int rc = ::posix_spawn(&pid, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    return fail<ProcessHandle>(ErrorCode::IoError, "posix_spawn", static_cast<std::uint64_t>(rc));
  }
  (void)cwd;
  ProcessHandle handle;
  handle.native = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  handle.pid = static_cast<std::uint32_t>(pid);
  return Result<ProcessHandle>(handle);
#endif
}

Result<bool> process_running(ProcessHandle& handle) {
  if (!handle.valid()) {
    return fail<bool>(ErrorCode::InvalidArgument, "invalid process handle");
  }
#if defined(_WIN32)
  const DWORD rc = ::WaitForSingleObject(static_cast<HANDLE>(handle.native), 0);
  if (rc == WAIT_TIMEOUT) {
    return Result<bool>(true);
  }
  if (rc == WAIT_OBJECT_0) {
    return Result<bool>(false);
  }
  return fail<bool>(ErrorCode::IoError, "WaitForSingleObject",
                    static_cast<std::uint64_t>(::GetLastError()));
#else
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle.native));
  int status = 0;
  const pid_t rc = ::waitpid(pid, &status, WNOHANG);
  if (rc == 0) {
    return Result<bool>(true);
  }
  if (rc == pid) {
    return Result<bool>(false);
  }
  return fail<bool>(ErrorCode::IoError, "waitpid", static_cast<std::uint64_t>(errno));
#endif
}

Result<int> wait_process(ProcessHandle& handle) {
  if (!handle.valid()) {
    return fail<int>(ErrorCode::InvalidArgument, "invalid process handle");
  }
#if defined(_WIN32)
  const DWORD rc = ::WaitForSingleObject(static_cast<HANDLE>(handle.native), INFINITE);
  if (rc != WAIT_OBJECT_0) {
    return fail<int>(ErrorCode::IoError, "WaitForSingleObject",
                     static_cast<std::uint64_t>(::GetLastError()));
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(handle.native), &exit_code) == 0) {
    return fail<int>(ErrorCode::IoError, "GetExitCodeProcess");
  }
  ::CloseHandle(static_cast<HANDLE>(handle.native));
  handle.native = nullptr;
  return Result<int>(static_cast<int>(exit_code));
#else
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle.native));
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) {
    return fail<int>(ErrorCode::IoError, "waitpid", static_cast<std::uint64_t>(errno));
  }
  handle.native = nullptr;
  if (WIFEXITED(status)) {
    return Result<int>(WEXITSTATUS(status));
  }
  return Result<int>(128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
#endif
}

Status terminate_process(ProcessHandle& handle) {
  if (!handle.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "invalid process handle");
  }
#if defined(_WIN32)
  if (::TerminateProcess(static_cast<HANDLE>(handle.native), 137u) == 0) {
    return Status::error(ErrorCode::IoError, "TerminateProcess",
                         static_cast<std::uint64_t>(::GetLastError()));
  }
  const DWORD rc = ::WaitForSingleObject(static_cast<HANDLE>(handle.native), INFINITE);
  if (rc != WAIT_OBJECT_0) {
    return Status::error(ErrorCode::IoError, "wait terminated process");
  }
  ::CloseHandle(static_cast<HANDLE>(handle.native));
  handle.native = nullptr;
  return Status::success();
#else
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle.native));
  if (::kill(pid, SIGKILL) != 0) {
    return Status::error(ErrorCode::IoError, "kill", static_cast<std::uint64_t>(errno));
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  handle.native = nullptr;
  return Status::success();
#endif
}

void close_process(ProcessHandle& handle) noexcept {
  if (!handle.valid()) {
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle.native));
#endif
  handle.native = nullptr;
}

Result<std::string> current_executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return fail<std::string>(ErrorCode::IoError, "GetModuleFileName");
    }
    if (written < buffer.size()) {
      return Result<std::string>(narrow(std::wstring(buffer.data(), written)));
    }
    if (buffer.size() > (1u << 16)) {
      return fail<std::string>(ErrorCode::OversizedInput, "executable path");
    }
    buffer.resize(buffer.size() * 2u);
  }
#else
  std::vector<char> buffer(1024);
  for (;;) {
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (written <= 0) {
      return fail<std::string>(ErrorCode::IoError, "readlink /proc/self/exe");
    }
    if (static_cast<std::size_t>(written) < buffer.size()) {
      return Result<std::string>(std::string(buffer.data(), static_cast<std::size_t>(written)));
    }
    buffer.resize(buffer.size() * 2u);
  }
#endif
}

Result<std::string> current_executable_directory() {
  BPFAB_TRY_DECL(const std::string, path, current_executable_path());
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return fail<std::string>(ErrorCode::Internal, "executable path has no directory");
  }
  return Result<std::string>(path.substr(0, slash));
}

Result<std::string> current_working_directory() {
#if defined(_WIN32)
  const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
  if (needed == 0) {
    return fail<std::string>(ErrorCode::IoError, "GetCurrentDirectory");
  }
  std::vector<wchar_t> buffer(needed + 1u, L'\0');
  const DWORD written = ::GetCurrentDirectoryW(needed + 1u, buffer.data());
  if (written == 0) {
    return fail<std::string>(ErrorCode::IoError, "GetCurrentDirectory");
  }
  return Result<std::string>(narrow(std::wstring(buffer.data(), written)));
#else
  std::vector<char> buffer(4096);
  if (::getcwd(buffer.data(), buffer.size()) == nullptr) {
    return fail<std::string>(ErrorCode::IoError, "getcwd");
  }
  return Result<std::string>(std::string(buffer.data()));
#endif
}

}  // namespace backpressure

#pragma once

// BCrypt depends on Windows SDK types declared by windows.h.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace opennr {
namespace fs = std::filesystem;

inline std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::string Hash(const std::string& bytes) {
  std::array<unsigned char, 32> digest{};
  if (bytes.size() > std::numeric_limits<ULONG>::max()) throw std::runtime_error("Hash input too large");
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) {
    throw std::runtime_error("SHA256 failed");
  }
  std::string result;
  for (auto byte : digest) {
    result += std::format("{:02X}", byte);
  }
  return result;
}

inline void Write(const fs::path& path, const std::string& bytes) {
  if (fs::exists(path)) {
    if (Read(path) != bytes) throw std::runtime_error("Refusing differing existing file " + path.string());
    return;
  }
  fs::create_directories(path.parent_path());
  std::unique_ptr<void, decltype(&CloseHandle)> file(
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr), &CloseHandle);
  if (file.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create " + path.string());
  DWORD written = 0;
  if (bytes.size() > MAXDWORD || !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) throw std::runtime_error("Write failed " + path.string());
}

inline void Check(const std::string& bytes, const std::string& expected) {
  const auto actual = Hash(bytes);
  if (actual != expected) throw std::runtime_error("SHA256 mismatch: " + actual + " expected " + expected);
}

inline void Run(const fs::path& tool, const std::vector<std::wstring>& arguments, const fs::path& cwd, const fs::path& log) {
  // Every argument is quoted using the Windows CRT backslash/quote rules.
  std::wstring command;
  auto all = arguments;
  all.reserve(arguments.size() + 1);
  all.insert(all.begin(), tool.wstring());
  for (const auto& argument : all) {
    command += L'"';
    size_t slashes = 0;
    for (wchar_t character : argument) {
      if (character == L'\\') {
        ++slashes;
        continue;
      }
      command.append(slashes * (character == L'"' ? 2 : 1), L'\\');
      slashes = 0;
      if (character == L'"') {
        command += L'\\';
      }
      command += character;
    }
    command.append(slashes * 2, L'\\');
    command += L"\" ";
  }
  fs::create_directories(log.parent_path());
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const auto close = [](HANDLE handle) { CloseHandle(handle); };
  std::unique_ptr<void, decltype(close)> output(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr), close);
  if (output.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create tool log");
  std::unique_ptr<void, decltype(close)> input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), close);
  if (input.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open process input");
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<unsigned char> attribute_storage(attribute_bytes);
  auto* attributes_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!InitializeProcThreadAttributeList(attributes_list, 1, 0, &attribute_bytes)) {
    throw std::runtime_error("Cannot initialize process attributes");
  }
  const auto delete_attributes = [](PPROC_THREAD_ATTRIBUTE_LIST attributes) {
    DeleteProcThreadAttributeList(attributes);
  };
  const std::unique_ptr<std::remove_pointer_t<PPROC_THREAD_ATTRIBUTE_LIST>, decltype(delete_attributes)> attributes_guard(attributes_list, delete_attributes);
  HANDLE inherited_handles[]{input.get(), output.get()};
  if (!UpdateProcThreadAttribute(attributes_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles, sizeof(inherited_handles), nullptr, nullptr)) {
    throw std::runtime_error("Cannot restrict inherited process handles");
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = output.get();
  startup.StartupInfo.hStdError = output.get();
  startup.StartupInfo.hStdInput = input.get();
  startup.lpAttributeList = attributes_list;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(), &startup.StartupInfo, &process)) {
    throw std::runtime_error("Cannot start " + tool.string());
  }
  std::unique_ptr<void, decltype(close)> process_handle(process.hProcess, close);
  std::unique_ptr<void, decltype(close)> thread_handle(process.hThread, close);
  if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) throw std::runtime_error("Tool wait failed");
  DWORD code = 1;
  if (!GetExitCodeProcess(process_handle.get(), &code) || code != 0) throw std::runtime_error("Tool failed; see " + log.string());
}

}  // namespace opennr
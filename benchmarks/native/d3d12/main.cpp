#include <Windows.h>

#include <bcrypt.h>
#include <d3d12.h>
#include <DbgHelp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cupti_telemetry.hpp"
#include "neural_rendering_direct.hpp"

using Microsoft::WRL::ComPtr;
namespace direct = renodx::utils::dlss::neural_rendering::direct;
namespace nr = renodx::utils::dlss::neural_rendering;

namespace {

using InitializeNgxCore = NVSDK_NGX_Result(NVSDK_CONV*)(
    uint64_t,
    const wchar_t*,
    ID3D12Device*,
    NVSDK_NGX_Version);

constexpr uint32_t WIDTH = 1920u;
constexpr uint32_t HEIGHT = 1080u;
constexpr uint32_t DEFAULT_EVALUATIONS = 8u;
constexpr DWORD GPU_TIMEOUT_MS = 120000u;
constexpr uintptr_t DLSSNR_310_8_LOG_CALLBACK_RVA = 0x1142750u;

std::filesystem::path log_path;
std::filesystem::path dump_path;
std::filesystem::path snippet_log_path;
uint64_t log_sequence = 0u;

void Log(std::string_view message) {
  if (log_path.empty()) return;
  HANDLE file = CreateFileW(
      log_path.c_str(),
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const std::string line = std::format(
      "{:08} tick={} pid={} tid={} {}\r\n",
      ++log_sequence,
      GetTickCount64(),
      GetCurrentProcessId(),
      GetCurrentThreadId(),
      message);
  DWORD written = 0u;
  (void)WriteFile(
      file,
      line.data(),
      static_cast<DWORD>(line.size()),
      &written,
      nullptr);
  (void)FlushFileBuffers(file);
  CloseHandle(file);
}

void NVSDK_CONV SnippetLog(
    const char* message,
    NVSDK_NGX_Logging_Level logging_level,
    NVSDK_NGX_Feature source_component) {
  if (snippet_log_path.empty()) return;
  HANDLE file = CreateFileW(
      snippet_log_path.c_str(),
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const std::string line = std::format(
      "level={} source={} {}\r\n",
      static_cast<uint32_t>(logging_level),
      static_cast<uint32_t>(source_component),
      message == nullptr ? "<null>" : message);
  DWORD written = 0u;
  (void)WriteFile(
      file,
      line.data(),
      static_cast<DWORD>(line.size()),
      &written,
      nullptr);
  (void)FlushFileBuffers(file);
  CloseHandle(file);
}

bool EnableSnippetLogging() {
  const HMODULE module = direct::runtime.api.module;
  if (module == nullptr) {
    Log("snippet.logging.module missing");
    return false;
  }
  auto* const image = reinterpret_cast<std::byte*>(module);
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    Log("snippet.logging.invalid DOS header");
    return false;
  }
  const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
      image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE
      || nt->OptionalHeader.SizeOfImage
             < DLSSNR_310_8_LOG_CALLBACK_RVA + sizeof(NVSDK_NGX_AppLogCallback)) {
    Log("snippet.logging.unsupported image");
    return false;
  }

  auto* const callback = reinterpret_cast<NVSDK_NGX_AppLogCallback*>(
      image + DLSSNR_310_8_LOG_CALLBACK_RVA);
  DWORD old_protection = 0u;
  if (VirtualProtect(
          callback,
          sizeof(*callback),
          PAGE_READWRITE,
          &old_protection)
      == FALSE) {
    Log(std::format("snippet.logging.protect failed error={}", GetLastError()));
    return false;
  }
  *callback = SnippetLog;
  DWORD restored_protection = 0u;
  const BOOL restored = VirtualProtect(
      callback,
      sizeof(*callback),
      old_protection,
      &restored_protection);
  Log(std::format(
      "snippet.logging.enabled path={} restore={}",
      snippet_log_path.string(),
      restored != FALSE));
  return true;
}

LONG WINAPI UnhandledException(EXCEPTION_POINTERS* exception) {
  if (exception != nullptr && exception->ExceptionRecord != nullptr) {
    MEMORY_BASIC_INFORMATION memory = {};
    const SIZE_T queried = VirtualQuery(
        exception->ExceptionRecord->ExceptionAddress,
        &memory,
        sizeof(memory));
    std::array<wchar_t, 32768u> module_path = {};
    const DWORD module_path_length = queried != 0u
                                         ? GetModuleFileNameW(
                                               static_cast<HMODULE>(memory.AllocationBase),
                                               module_path.data(),
                                               static_cast<DWORD>(module_path.size()))
                                         : 0u;
    Log(std::format(
        "unhandled_exception code={:#010x} address={} module={} base={} offset={:#x}",
        exception->ExceptionRecord->ExceptionCode,
        exception->ExceptionRecord->ExceptionAddress,
        module_path_length != 0u
            ? std::filesystem::path(module_path.data()).string()
            : "(unresolved)",
        memory.AllocationBase,
        queried != 0u
            ? static_cast<uintptr_t>(
                  static_cast<std::byte*>(
                      exception->ExceptionRecord->ExceptionAddress)
                  - static_cast<std::byte*>(memory.AllocationBase))
            : 0u));
  } else {
    Log("unhandled_exception details=unavailable");
  }
  HANDLE dump = CreateFileW(
      dump_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION information = {
        .ThreadId = GetCurrentThreadId(),
        .ExceptionPointers = exception,
        .ClientPointers = FALSE,
    };
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs
        | MiniDumpWithHandleData
        | MiniDumpWithIndirectlyReferencedMemory
        | MiniDumpWithThreadInfo
        | MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        dump,
        type,
        exception != nullptr ? &information : nullptr,
        nullptr,
        nullptr);
    const DWORD dump_error = written != FALSE ? ERROR_SUCCESS : GetLastError();
    (void)FlushFileBuffers(dump);
    CloseHandle(dump);
    Log(std::format(
        "minidump written={} path={} error={}",
        written != FALSE,
        dump_path.string(),
        dump_error));
  } else {
    Log(std::format("minidump.create_failed error={}", GetLastError()));
  }
  if (GetEnvironmentVariableW(
          L"RENODX_DLSSNR_REPRO_NO_MESSAGE_BOX",
          nullptr,
          0u)
      == 0u) {
    (void)MessageBoxW(
        nullptr,
        L"The DLSS-NR repro crashed. Send dlssnr-repro.log, "
        L"dlssnr-direct-breadcrumbs.log, and dlssnr-repro.dmp to the developer.",
        L"DLSS-NR repro crashed",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::filesystem::path ExecutableDirectory() {
  std::array<wchar_t, 32768u> path = {};
  const DWORD length = GetModuleFileNameW(
      nullptr,
      path.data(),
      static_cast<DWORD>(path.size()));
  if (length == 0u || length >= path.size()) return {};
  return std::filesystem::path(path.data()).parent_path();
}

std::filesystem::path FindNgxCore(
    const std::filesystem::path& executable_directory) {
  const auto local = executable_directory / L"nvngx.dll";
  if (std::filesystem::is_regular_file(local)) return local;
  std::array<wchar_t, 32768u> system_directory = {};
  const UINT length = GetSystemDirectoryW(
      system_directory.data(),
      static_cast<UINT>(system_directory.size()));
  if (length == 0u || length >= system_directory.size()) return {};
  const auto repository = std::filesystem::path(system_directory.data())
                          / L"DriverStore"
                          / L"FileRepository";
  std::error_code error;
  std::filesystem::path selected;
  std::filesystem::file_time_type selected_time = {};
  auto iterator = std::filesystem::directory_iterator(
      repository,
      std::filesystem::directory_options::skip_permission_denied,
      error);
  const auto end = std::filesystem::directory_iterator();
  while (!error && iterator != end) {
    const auto entry = *iterator;
    iterator.increment(error);
    std::error_code candidate_error;
    if (!entry.is_directory(candidate_error)) continue;
    const auto candidate = entry.path() / L"nvngx.dll";
    if (!std::filesystem::is_regular_file(candidate, candidate_error)) continue;
    const auto write_time = std::filesystem::last_write_time(
        candidate,
        candidate_error);
    if (!candidate_error && (selected.empty() || write_time > selected_time)) {
      selected = candidate;
      selected_time = write_time;
    }
  }
  return selected;
}

std::string Sha256(const std::filesystem::path& path) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<std::byte> hash_object;
  std::array<uint8_t, 32u> digest = {};
  std::string result;
  DWORD object_size = 0u;
  DWORD bytes = 0u;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          &algorithm,
          BCRYPT_SHA256_ALGORITHM,
          nullptr,
          0u))
      || !BCRYPT_SUCCESS(BCryptGetProperty(
          algorithm,
          BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_size),
          sizeof(object_size),
          &bytes,
          0u))) {
    goto cleanup;
  }
  hash_object.resize(object_size);
  if (!BCRYPT_SUCCESS(BCryptCreateHash(
          algorithm,
          &hash,
          reinterpret_cast<PUCHAR>(hash_object.data()),
          object_size,
          nullptr,
          0u,
          0u))) {
    goto cleanup;
  }
  file = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) goto cleanup;
  {
    std::vector<uint8_t> buffer(1u << 20u);
    DWORD read = 0u;
    BOOL read_result = FALSE;
    while ((read_result = ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr))
               != FALSE
           && read != 0u) {
      if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0u))) {
        goto cleanup;
      }
    }
    if (read_result == FALSE) goto cleanup;
  }
  if (!BCRYPT_SUCCESS(BCryptFinishHash(
          hash,
          digest.data(),
          static_cast<ULONG>(digest.size()),
          0u))) {
    goto cleanup;
  }
  for (const uint8_t value : digest) result += std::format("{:02X}", value);

cleanup:
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (hash != nullptr) BCryptDestroyHash(hash);
  if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0u);
  return result;
}

void EnableDred() {
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
  const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
  Log(std::format("dred.settings result={:#010x}", static_cast<uint32_t>(result)));
  if (FAILED(result)) return;
  settings->SetAutoBreadcrumbsEnablement(
      D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetBreadcrumbContextEnablement(
      D3D12_DRED_ENABLEMENT_FORCED_ON);
}

void LogDred(ID3D12Device* device) {
  if (device == nullptr) return;
  const HRESULT reason = device->GetDeviceRemovedReason();
  Log(std::format(
      "device.removed_reason={:#010x}",
      static_cast<uint32_t>(reason)));
  if (reason == S_OK) return;
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    Log("dred.interface unavailable");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
  const HRESULT result = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
  Log(std::format(
      "dred.breadcrumbs result={:#010x}",
      static_cast<uint32_t>(result)));
  uint32_t index = 0u;
  for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode;
       SUCCEEDED(result) && node != nullptr && index < 64u;
       node = node->pNext, ++index) {
    const UINT completed = node->pLastBreadcrumbValue != nullptr
                               ? *node->pLastBreadcrumbValue
                               : 0u;
    Log(std::format(
        "dred.breadcrumb[{}] completed={}/{} list={} queue={} list_object={} queue_object={}",
        index,
        completed,
        node->BreadcrumbCount,
        node->pCommandListDebugNameA != nullptr
            ? node->pCommandListDebugNameA
            : "(unnamed)",
        node->pCommandQueueDebugNameA != nullptr
            ? node->pCommandQueueDebugNameA
            : "(unnamed)",
        static_cast<void*>(node->pCommandList),
        static_cast<void*>(node->pCommandQueue)));
    if (node->pCommandHistory != nullptr) {
      UINT begin_events = 0u;
      UINT end_events = 0u;
      UINT resource_barriers = 0u;
      UINT last_begin_index = 0u;
      for (UINT operation = 0u; operation < completed; ++operation) {
        switch (node->pCommandHistory[operation]) {
          case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
            ++begin_events;
            last_begin_index = operation;
            break;
          case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
            ++end_events;
            break;
          case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
            ++resource_barriers;
            break;
          default:
            break;
        }
      }
      Log(std::format(
          "dred.progress breadcrumb={} begin_events={} end_events={} active_event={} last_begin_index={} resource_barriers={}",
          index,
          begin_events,
          end_events,
          begin_events > end_events ? begin_events : 0u,
          last_begin_index,
          resource_barriers));
      const UINT history_begin = completed > 8u ? completed - 8u : 0u;
      const UINT history_end = std::min(
          node->BreadcrumbCount,
          completed + 9u);
      for (UINT operation = history_begin;
           operation < history_end;
           ++operation) {
        Log(std::format(
            "dred.history breadcrumb={} index={} completed={} operation={}",
            index,
            operation,
            operation < completed,
            static_cast<uint32_t>(node->pCommandHistory[operation])));
      }
      for (UINT context = 0u;
           context < node->BreadcrumbContextsCount;
           ++context) {
        const auto& value = node->pBreadcrumbContexts[context];
        if (value.BreadcrumbIndex < history_begin
            || value.BreadcrumbIndex >= history_end) {
          continue;
        }
        Log(std::format(
            "dred.context breadcrumb={} index={} text={}",
            index,
            value.BreadcrumbIndex,
            value.pContextString == nullptr
                ? "(null)"
                : std::filesystem::path(value.pContextString).string()));
      }
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault = {};
  const HRESULT page_fault_result = dred->GetPageFaultAllocationOutput1(
      &page_fault);
  Log(std::format(
      "dred.page_fault result={:#010x} gpu_va={:#x}",
      static_cast<uint32_t>(page_fault_result),
      page_fault.PageFaultVA));
}

ComPtr<ID3D12Resource> CreateTexture(
    ID3D12Device* device,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state,
    const wchar_t* name) {
  const D3D12_HEAP_PROPERTIES heap = {
      .Type = D3D12_HEAP_TYPE_DEFAULT,
      .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
      .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
      .CreationNodeMask = 1u,
      .VisibleNodeMask = 1u,
  };
  const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0u,
      .Width = WIDTH,
      .Height = HEIGHT,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = flags,
  };
  ComPtr<ID3D12Resource> resource;
  const HRESULT result = device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      state,
      nullptr,
      IID_PPV_ARGS(&resource));
  Log(std::format(
      "resource.create name={} format={} flags={:#x} state={:#x} result={:#010x} resource={}",
      std::filesystem::path(name).string(),
      static_cast<uint32_t>(format),
      static_cast<uint32_t>(flags),
      static_cast<uint32_t>(state),
      static_cast<uint32_t>(result),
      static_cast<void*>(resource.Get())));
  if (resource != nullptr) resource->SetName(name);
  return resource;
}

struct TextureTransfer {
  ComPtr<ID3D12Resource> upload;
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  uint32_t row_size = 0u;
  uint32_t row_count = 0u;
};

ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device* device,
    uint64_t size,
    D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES state,
    const wchar_t* name) {
  const D3D12_HEAP_PROPERTIES heap = {
      .Type = heap_type,
      .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
      .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
      .CreationNodeMask = 1u,
      .VisibleNodeMask = 1u,
  };
  const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0u,
      .Width = size,
      .Height = 1u,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_NONE,
  };
  ComPtr<ID3D12Resource> resource;
  const HRESULT result = device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      state,
      nullptr,
      IID_PPV_ARGS(&resource));
  Log(std::format(
      "buffer.create name={} size={} heap={} result={:#010x}",
      std::filesystem::path(name).string(),
      size,
      static_cast<uint32_t>(heap_type),
      static_cast<uint32_t>(result)));
  if (resource != nullptr) resource->SetName(name);
  return resource;
}

TextureTransfer CreateTextureTransfer(
    ID3D12Device* device,
    ID3D12Resource* texture,
    uint32_t row_size,
    uint32_t row_count,
    const wchar_t* name) {
  TextureTransfer transfer = {
      .row_size = row_size,
      .row_count = row_count,
  };
  uint64_t total_size = 0u;
  const auto desc = texture->GetDesc();
  device->GetCopyableFootprints(
      &desc,
      0u,
      1u,
      0u,
      &transfer.footprint,
      nullptr,
      nullptr,
      &total_size);
  transfer.upload = CreateBuffer(
      device,
      total_size,
      D3D12_HEAP_TYPE_UPLOAD,
      D3D12_RESOURCE_STATE_GENERIC_READ,
      std::format(L"{} upload", name).c_str());
  transfer.readback = CreateBuffer(
      device,
      total_size,
      D3D12_HEAP_TYPE_READBACK,
      D3D12_RESOURCE_STATE_COPY_DEST,
      std::format(L"{} readback", name).c_str());
  return transfer;
}

void Transition(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  const D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
          .pResource = resource,
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = before,
          .StateAfter = after,
      },
  };
  command_list->ResourceBarrier(1u, &barrier);
}

bool RecordUpload(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES state,
    const TextureTransfer& transfer,
    const std::vector<std::byte>& tight_data) {
  if (tight_data.size()
      != static_cast<size_t>(transfer.row_size) * transfer.row_count) {
    return false;
  }
  std::byte* mapped = nullptr;
  const D3D12_RANGE no_read = {};
  if (FAILED(transfer.upload->Map(
          0u,
          &no_read,
          reinterpret_cast<void**>(&mapped)))) {
    return false;
  }
  mapped += transfer.footprint.Offset;
  for (uint32_t row = 0u; row < transfer.row_count; ++row) {
    std::memcpy(
        mapped
            + (static_cast<size_t>(row)
               * transfer.footprint.Footprint.RowPitch),
        tight_data.data()
            + (static_cast<size_t>(row) * transfer.row_size),
        transfer.row_size);
  }
  transfer.upload->Unmap(0u, nullptr);

  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = transfer.upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = transfer.footprint;
  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = texture;
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0u;
  Transition(command_list, texture, state, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyTextureRegion(
      &destination,
      0u,
      0u,
      0u,
      &source,
      nullptr);
  Transition(command_list, texture, D3D12_RESOURCE_STATE_COPY_DEST, state);
  return true;
}

void RecordReadback(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES state,
    const TextureTransfer& transfer,
    bool uav_written = false) {
  if (uav_written) {
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .UAV = {.pResource = texture},
    };
    command_list->ResourceBarrier(1u, &barrier);
  }
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = texture;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0u;
  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = transfer.readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = transfer.footprint;
  Transition(command_list, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_list->CopyTextureRegion(
      &destination,
      0u,
      0u,
      0u,
      &source,
      nullptr);
  Transition(command_list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

std::vector<std::byte> ReadTight(const TextureTransfer& transfer) {
  std::vector<std::byte> result(
      static_cast<size_t>(transfer.row_size) * transfer.row_count);
  const uint64_t end = transfer.footprint.Offset
                       + (static_cast<uint64_t>(transfer.footprint.Footprint.RowPitch)
                          * transfer.row_count);
  const D3D12_RANGE read_range = {
      .Begin = static_cast<SIZE_T>(transfer.footprint.Offset),
      .End = static_cast<SIZE_T>(end),
  };
  void* mapped_memory = nullptr;
  if (FAILED(transfer.readback->Map(
          0u,
          &read_range,
          &mapped_memory))) {
    return {};
  }
  const auto* mapped = static_cast<const std::byte*>(mapped_memory);
  mapped += transfer.footprint.Offset;
  for (uint32_t row = 0u; row < transfer.row_count; ++row) {
    std::memcpy(
        result.data() + (static_cast<size_t>(row) * transfer.row_size),
        mapped
            + (static_cast<size_t>(row)
               * transfer.footprint.Footprint.RowPitch),
        transfer.row_size);
  }
  transfer.readback->Unmap(0u, nullptr);
  return result;
}

bool WriteRaw(
    const std::filesystem::path& path,
    const std::vector<std::byte>& data) {
  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0u;
  const BOOL succeeded = WriteFile(
      file,
      data.data(),
      static_cast<DWORD>(data.size()),
      &written,
      nullptr);
  CloseHandle(file);
  return succeeded != FALSE && written == data.size();
}

uint16_t FloatToHalf(float value) {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t sign = (bits >> 16u) & 0x8000u;
  const uint32_t exponent = (bits >> 23u) & 0xffu;
  const uint32_t mantissa = bits & 0x7fffffu;
  if (exponent == 0xffu) {
    return static_cast<uint16_t>(
        sign | 0x7c00u | (mantissa != 0u ? 0x0200u : 0u));
  }
  const int32_t half_exponent = static_cast<int32_t>(exponent) - 127 + 15;
  if (half_exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  if (half_exponent <= 0) {
    if (half_exponent < -10) return static_cast<uint16_t>(sign);
    const uint32_t normalized = mantissa | 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - half_exponent);
    const uint32_t rounded = normalized + ((1u << (shift - 1u)) - 1u)
                             + ((normalized >> shift) & 1u);
    return static_cast<uint16_t>(sign | (rounded >> shift));
  }
  const uint32_t rounded = mantissa + 0x0fffu + ((mantissa >> 13u) & 1u);
  if ((rounded & 0x800000u) != 0u) {
    if (half_exponent == 30) return static_cast<uint16_t>(sign | 0x7c00u);
    return static_cast<uint16_t>(sign | ((half_exponent + 1) << 10u));
  }
  return static_cast<uint16_t>(
      sign | (static_cast<uint32_t>(half_exponent) << 10u)
      | (rounded >> 13u));
}

void StoreHalf4(
    std::vector<std::byte>* data,
    size_t texel,
    float x,
    float y,
    float z,
    float w) {
  const std::array<uint16_t, 4u> values = {
      FloatToHalf(x),
      FloatToHalf(y),
      FloatToHalf(z),
      FloatToHalf(w),
  };
  std::memcpy(
      data->data() + (texel * sizeof(values)),
      values.data(),
      sizeof(values));
}

float HalfToFloat(uint16_t value) {
  const uint32_t exponent = (value >> 10u) & 0x1fu;
  const uint32_t mantissa = value & 0x03ffu;
  float result = 0.f;
  if (exponent == 0u) {
    result = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 0x1fu) {
    result = mantissa == 0u
                 ? std::numeric_limits<float>::infinity()
                 : std::numeric_limits<float>::quiet_NaN();
  } else {
    result = std::ldexp(
        static_cast<float>(0x0400u + mantissa),
        static_cast<int>(exponent) - 25);
  }
  return (value & 0x8000u) != 0u ? -result : result;
}

struct Rgba16fStatistics {
  bool layout_valid = false;
  bool finite = false;
  bool non_black = false;
  bool spatially_varying = false;
  bool reference_compared = false;
  uint64_t texels = 0u;
  uint64_t non_finite_components = 0u;
  uint64_t exact_zero_rgb_texels = 0u;
  uint64_t significant_rgb_texels = 0u;
  std::array<double, 3u> minimum_rgb = {};
  std::array<double, 3u> maximum_rgb = {};
  double minimum_alpha = 0.0;
  double maximum_alpha = 0.0;
  double mean_absolute_rgb = 0.0;
  double mean_absolute_alpha = 0.0;
  double mean_absolute_rgb_difference = 0.0;
  std::array<std::array<double, 3u>, 5u> rgb_samples = {};
};

Rgba16fStatistics AnalyzeRgba16f(
    const std::vector<std::byte>& data,
    const std::vector<std::byte>* reference = nullptr) {
  constexpr size_t texel_size = sizeof(uint16_t) * 4u;
  constexpr float significant_rgb_threshold = 0.0001f;
  Rgba16fStatistics statistics;
  statistics.layout_valid = data.size()
                            == static_cast<size_t>(WIDTH) * HEIGHT * texel_size;
  statistics.reference_compared = reference != nullptr
                                  && reference->size() == data.size();
  if (!statistics.layout_valid) return statistics;

  statistics.texels = data.size() / texel_size;
  statistics.minimum_rgb.fill(std::numeric_limits<double>::infinity());
  statistics.maximum_rgb.fill(-std::numeric_limits<double>::infinity());
  statistics.minimum_alpha = std::numeric_limits<double>::infinity();
  statistics.maximum_alpha = -std::numeric_limits<double>::infinity();
  double absolute_rgb_sum = 0.0;
  double absolute_alpha_sum = 0.0;
  double absolute_difference_sum = 0.0;
  for (size_t texel = 0u; texel < statistics.texels; ++texel) {
    std::array<uint16_t, 4u> packed = {};
    std::memcpy(
        packed.data(),
        data.data() + (texel * texel_size),
        texel_size);
    std::array<uint16_t, 4u> reference_packed = {};
    if (statistics.reference_compared) {
      std::memcpy(
          reference_packed.data(),
          reference->data() + (texel * texel_size),
          texel_size);
    }

    bool exact_zero_rgb = true;
    bool significant_rgb = false;
    for (size_t channel = 0u; channel < packed.size(); ++channel) {
      const float component = HalfToFloat(packed[channel]);
      if (!std::isfinite(component)) {
        ++statistics.non_finite_components;
        continue;
      }
      if (channel >= 3u) {
        statistics.minimum_alpha = std::min(
            statistics.minimum_alpha,
            static_cast<double>(component));
        statistics.maximum_alpha = std::max(
            statistics.maximum_alpha,
            static_cast<double>(component));
        absolute_alpha_sum += std::abs(component);
        continue;
      }
      const float absolute_component = std::abs(component);
      exact_zero_rgb = exact_zero_rgb && absolute_component == 0.f;
      significant_rgb = significant_rgb
                        || absolute_component > significant_rgb_threshold;
      statistics.minimum_rgb[channel] = std::min(
          statistics.minimum_rgb[channel],
          static_cast<double>(component));
      statistics.maximum_rgb[channel] = std::max(
          statistics.maximum_rgb[channel],
          static_cast<double>(component));
      absolute_rgb_sum += absolute_component;
      if (statistics.reference_compared) {
        const float reference_component = HalfToFloat(
            reference_packed[channel]);
        if (std::isfinite(reference_component)) {
          absolute_difference_sum += std::abs(
              component - reference_component);
        } else {
          ++statistics.non_finite_components;
        }
      }
    }
    if (exact_zero_rgb) ++statistics.exact_zero_rgb_texels;
    if (significant_rgb) ++statistics.significant_rgb_texels;
  }

  statistics.finite = statistics.non_finite_components == 0u;
  const double rgb_component_count = static_cast<double>(statistics.texels) * 3.0;
  statistics.mean_absolute_rgb = absolute_rgb_sum / rgb_component_count;
  statistics.mean_absolute_alpha = absolute_alpha_sum
                                   / static_cast<double>(statistics.texels);
  if (statistics.reference_compared) {
    statistics.mean_absolute_rgb_difference =
        absolute_difference_sum / rgb_component_count;
  }
  const uint64_t minimum_significant_texels = std::max<uint64_t>(
      1u,
      statistics.texels / 100u);
  statistics.non_black = statistics.finite
                         && statistics.significant_rgb_texels >= minimum_significant_texels;
  for (size_t channel = 0u; channel < 3u; ++channel) {
    if (!std::isfinite(statistics.minimum_rgb[channel])
        || !std::isfinite(statistics.maximum_rgb[channel])) {
      statistics.minimum_rgb[channel] = 0.0;
      statistics.maximum_rgb[channel] = 0.0;
      continue;
    }
    statistics.spatially_varying = statistics.spatially_varying
                                   || statistics.maximum_rgb[channel] - statistics.minimum_rgb[channel]
                                          > significant_rgb_threshold;
  }
  if (!std::isfinite(statistics.minimum_alpha)
      || !std::isfinite(statistics.maximum_alpha)) {
    statistics.minimum_alpha = 0.0;
    statistics.maximum_alpha = 0.0;
  }
  const std::array<size_t, 5u> sample_texels = {
      0u,
      WIDTH - 1u,
      (static_cast<size_t>(HEIGHT / 2u) * WIDTH) + (WIDTH / 2u),
      (static_cast<size_t>(HEIGHT - 1u) * WIDTH),
      (static_cast<size_t>(HEIGHT) * WIDTH) - 1u,
  };
  for (size_t sample = 0u; sample < sample_texels.size(); ++sample) {
    std::array<uint16_t, 4u> packed = {};
    std::memcpy(
        packed.data(),
        data.data() + (sample_texels[sample] * texel_size),
        texel_size);
    for (size_t channel = 0u; channel < 3u; ++channel) {
      statistics.rgb_samples[sample][channel] = HalfToFloat(packed[channel]);
    }
  }
  return statistics;
}

bool IsValidRgba16fOutput(const Rgba16fStatistics& statistics) {
  return statistics.layout_valid
         && statistics.finite
         && statistics.non_black
         && statistics.spatially_varying;
}

std::string FormatRgba16fStatistics(const Rgba16fStatistics& statistics) {
  return std::format(
      "layout_valid={} finite={} non_black={} spatially_varying={} "
      "texels={} non_finite_components={} exact_zero_rgb_texels={} "
      "significant_rgb_texels={} rgb_min=[{:.9g},{:.9g},{:.9g}] "
      "rgb_max=[{:.9g},{:.9g},{:.9g}] mean_absolute_rgb={:.9g} "
      "alpha_min={:.9g} alpha_max={:.9g} mean_absolute_alpha={:.9g} "
      "mean_absolute_rgb_difference={:.9g} "
      "rgb_samples_0_0_w1_0_center_0_h1_w1_h1="
      "[[{:.9g},{:.9g},{:.9g}],[{:.9g},{:.9g},{:.9g}],"
      "[{:.9g},{:.9g},{:.9g}],[{:.9g},{:.9g},{:.9g}],"
      "[{:.9g},{:.9g},{:.9g}]]",
      statistics.layout_valid,
      statistics.finite,
      statistics.non_black,
      statistics.spatially_varying,
      statistics.texels,
      statistics.non_finite_components,
      statistics.exact_zero_rgb_texels,
      statistics.significant_rgb_texels,
      statistics.minimum_rgb[0u],
      statistics.minimum_rgb[1u],
      statistics.minimum_rgb[2u],
      statistics.maximum_rgb[0u],
      statistics.maximum_rgb[1u],
      statistics.maximum_rgb[2u],
      statistics.mean_absolute_rgb,
      statistics.minimum_alpha,
      statistics.maximum_alpha,
      statistics.mean_absolute_alpha,
      statistics.mean_absolute_rgb_difference,
      statistics.rgb_samples[0u][0u],
      statistics.rgb_samples[0u][1u],
      statistics.rgb_samples[0u][2u],
      statistics.rgb_samples[1u][0u],
      statistics.rgb_samples[1u][1u],
      statistics.rgb_samples[1u][2u],
      statistics.rgb_samples[2u][0u],
      statistics.rgb_samples[2u][1u],
      statistics.rgb_samples[2u][2u],
      statistics.rgb_samples[3u][0u],
      statistics.rgb_samples[3u][1u],
      statistics.rgb_samples[3u][2u],
      statistics.rgb_samples[4u][0u],
      statistics.rgb_samples[4u][1u],
      statistics.rgb_samples[4u][2u]);
}

bool WaitForGpu(
    ID3D12CommandQueue* queue,
    ID3D12Fence* fence,
    uint64_t value,
    ID3D12Device* device) {
  Log(std::format("gpu.signal.begin value={}", value));
  const HRESULT signal_result = queue->Signal(fence, value);
  Log(std::format(
      "gpu.signal.end value={} result={:#010x}",
      value,
      static_cast<uint32_t>(signal_result)));
  if (FAILED(signal_result)) {
    LogDred(device);
    return false;
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) return false;
  const HRESULT event_result = fence->SetEventOnCompletion(value, event);
  Log(std::format(
      "gpu.wait.begin value={} set_event={:#010x}",
      value,
      static_cast<uint32_t>(event_result)));
  const DWORD wait_result = SUCCEEDED(event_result)
                                ? WaitForSingleObject(event, GPU_TIMEOUT_MS)
                                : WAIT_FAILED;
  CloseHandle(event);
  Log(std::format(
      "gpu.wait.end value={} wait={:#010x} completed={}",
      value,
      wait_result,
      fence->GetCompletedValue()));
  LogDred(device);
  return wait_result == WAIT_OBJECT_0
         && device->GetDeviceRemovedReason() == S_OK
         && fence->GetCompletedValue() >= value;
}

}  // namespace

int Run(int argc, wchar_t** argv) {
  const auto executable_directory = ExecutableDirectory();
  if (executable_directory.empty()) return 1;
  log_path = executable_directory / L"dlssnr-repro.log";
  dump_path = executable_directory / L"dlssnr-repro.dmp";
  snippet_log_path = executable_directory / L"dlssnr-snippet.log";
  (void)DeleteFileW(dump_path.c_str());
  (void)DeleteFileW(snippet_log_path.c_str());
  HANDLE empty_log = CreateFileW(
      log_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (empty_log != INVALID_HANDLE_VALUE) CloseHandle(empty_log);
  SetUnhandledExceptionFilter(&UnhandledException);

  if (argc == 2
      && std::wstring_view(argv[1]) == L"--validate-black-output") {
    const auto statistics = AnalyzeRgba16f(std::vector<std::byte>(
        static_cast<size_t>(WIDTH) * HEIGHT * sizeof(uint16_t) * 4u));
    const bool incorrectly_accepted = IsValidRgba16fOutput(statistics);
    Log(std::format(
        "validation-only classification=unit-fixture "
        "expected=reject-all-black incorrectly_accepted={} {}",
        incorrectly_accepted,
        FormatRgba16fStatistics(statistics)));
    return incorrectly_accepted ? 0 : 14;
  }

  cupti_telemetry::Session telemetry(
      executable_directory / L"dlssnr-kernels.csv");
  Log(std::format(
      "kernel.telemetry.started={} cupti={} nvapi_hook={}",
      telemetry.Started(),
      telemetry.CuptiStarted(),
      telemetry.NvapiHookStarted()));

  const std::filesystem::path snippet_path = argc >= 2
                                                 ? std::filesystem::absolute(argv[1])
                                                 : executable_directory / L"nvngx_dlssnr.dll";
  const std::filesystem::path core_path = argc >= 3
                                              ? std::filesystem::absolute(argv[2])
                                              : FindNgxCore(executable_directory);
  uint32_t evaluations = DEFAULT_EVALUATIONS;
  const bool profile_mode = GetEnvironmentVariableW(
                                L"RENODX_DLSSNR_REPRO_PROFILE",
                                nullptr,
                                0u)
                            != 0u;
  wchar_t environment_evaluations[32] = {};
  const DWORD environment_evaluation_length = GetEnvironmentVariableW(
      L"RENODX_DLSSNR_REPRO_EVALUATIONS",
      environment_evaluations,
      static_cast<DWORD>(std::size(environment_evaluations)));
  if (argc >= 4 || environment_evaluation_length != 0u) {
    try {
      evaluations = static_cast<uint32_t>(std::stoul(
          argc >= 4 ? argv[3] : environment_evaluations));
    } catch (...) {
      Log("arguments.invalid evaluation count");
      return 1;
    }
  }
  if (!std::filesystem::is_regular_file(snippet_path)) {
    Log(std::format("snippet.missing path={}", snippet_path.string()));
    return 1;
  }
  if (!std::filesystem::is_regular_file(core_path)) {
    Log(std::format("core.missing path={}", core_path.string()));
    return 1;
  }
  Log(std::format(
      "repro.start snippet={} core={} evaluations={} size={}x{} profile={}",
      snippet_path.string(),
      core_path.string(),
      evaluations,
      WIDTH,
      HEIGHT,
      profile_mode));
  Log(std::format(
      "snippet.sha256={} size={}",
      Sha256(snippet_path),
      std::filesystem::file_size(snippet_path)));
  Log(std::format("core.sha256={}", Sha256(core_path)));

  EnableDred();
  const HMODULE core = LoadLibraryExW(
      core_path.c_str(),
      nullptr,
      LOAD_WITH_ALTERED_SEARCH_PATH);
  Log(std::format(
      "core.load module={} error={}",
      static_cast<void*>(core),
      core != nullptr ? 0u : GetLastError()));
  if (core == nullptr) return 2;

  ComPtr<IDXGIFactory6> factory;
  HRESULT result = CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory));
  Log(std::format("dxgi.factory result={:#010x}", static_cast<uint32_t>(result)));
  if (FAILED(result)) return 3;
  ComPtr<IDXGIAdapter1> adapter;
  for (uint32_t index = 0u;
       factory->EnumAdapterByGpuPreference(
           index,
           DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
           IID_PPV_ARGS(&adapter))
       != DXGI_ERROR_NOT_FOUND;
       ++index) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0u
        && SUCCEEDED(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_12_0,
            __uuidof(ID3D12Device),
            nullptr))) {
      Log(std::format(
          "adapter.selected name={} vendor={:#06x} device={:#06x} luid={:#010x}:{:#010x} vram={}",
          std::filesystem::path(desc.Description).string(),
          desc.VendorId,
          desc.DeviceId,
          static_cast<uint32_t>(desc.AdapterLuid.HighPart),
          desc.AdapterLuid.LowPart,
          desc.DedicatedVideoMemory));
      break;
    }
    adapter.Reset();
  }
  if (adapter == nullptr) {
    Log("adapter.select failed");
    return 4;
  }

  ComPtr<ID3D12Device> device;
  result = D3D12CreateDevice(
      adapter.Get(),
      D3D_FEATURE_LEVEL_12_0,
      IID_PPV_ARGS(&device));
  Log(std::format("device.create result={:#010x}", static_cast<uint32_t>(result)));
  if (FAILED(result)) return 5;
  device->SetName(L"RenoDX DLSS-NR repro device");

  const D3D12_COMMAND_QUEUE_DESC queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
      .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
      .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
      .NodeMask = 0u,
  };
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> command_list;
  ComPtr<ID3D12Fence> fence;
  result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(result)) {
    result = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator));
  }
  if (SUCCEEDED(result)) {
    result = device->CreateCommandList(
        0u,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&command_list));
  }
  if (SUCCEEDED(result)) {
    result = device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  }
  Log(std::format("commands.create result={:#010x}", static_cast<uint32_t>(result)));
  if (FAILED(result)) return 6;
  queue->SetName(L"RenoDX DLSS-NR repro queue");
  allocator->SetName(L"RenoDX DLSS-NR repro allocator");
  command_list->SetName(L"RenoDX DLSS-NR repro command list");
  if (profile_mode && !telemetry.ConfigureGpuTimestamps(queue.Get())) {
    Log("kernel.telemetry.gpu_timestamps failed");
    return 6;
  }
  Log(std::format(
      "kernel.telemetry.gpu_timestamps={}",
      telemetry.GpuTimestampsStarted()));

  auto input = CreateTexture(
      device.Get(),
      DXGI_FORMAT_R16G16B16A16_FLOAT,
      D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      L"DLSS-NR repro input");
  auto output = CreateTexture(
      device.Get(),
      DXGI_FORMAT_R16G16B16A16_FLOAT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      L"DLSS-NR repro output");
  auto motion = CreateTexture(
      device.Get(),
      DXGI_FORMAT_R16G16B16A16_FLOAT,
      D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      L"DLSS-NR repro motion");
  auto depth = CreateTexture(
      device.Get(),
      DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      L"DLSS-NR repro depth");
  if (input == nullptr || output == nullptr || motion == nullptr
      || depth == nullptr) {
    return 7;
  }

  const auto capture_directory = executable_directory / L"dlssnr-capture";
  std::error_code capture_error;
  std::filesystem::remove_all(capture_directory, capture_error);
  capture_error.clear();
  std::filesystem::create_directories(capture_directory, capture_error);
  if (capture_error) {
    Log(std::format(
        "capture.directory.failed path={} error={}",
        capture_directory.string(),
        capture_error.message()));
    return 8;
  }

  constexpr uint32_t rgba16f_row_size = WIDTH * 8u;
  constexpr uint32_t r32f_row_size = WIDTH * 4u;
  std::vector<std::byte> input_fixture(
      static_cast<size_t>(rgba16f_row_size) * HEIGHT);
  std::vector<std::byte> motion_fixture(
      static_cast<size_t>(rgba16f_row_size) * HEIGHT);
  std::vector<std::byte> depth_fixture(
      static_cast<size_t>(r32f_row_size) * HEIGHT);
  std::vector<std::byte> output_sentinel(
      static_cast<size_t>(rgba16f_row_size) * HEIGHT);
  for (uint32_t y = 0u; y < HEIGHT; ++y) {
    for (uint32_t x = 0u; x < WIDTH; ++x) {
      const size_t texel = (static_cast<size_t>(y) * WIDTH) + x;
      const float u = (static_cast<float>(x) + 0.5f) / WIDTH;
      const float v = (static_cast<float>(y) + 0.5f) / HEIGHT;
      const bool checker = (((x / 64u) ^ (y / 64u)) & 1u) != 0u;
      const bool highlight = x > WIDTH * 3u / 8u
                             && x < WIDTH * 5u / 8u
                             && y > HEIGHT * 3u / 8u
                             && y < HEIGHT * 5u / 8u;
      float blue = checker ? 0.75f : 0.075f;
      if (highlight) blue = 6.f;
      StoreHalf4(
          &input_fixture,
          texel,
          0.025f + (u * 1.75f),
          0.025f + (v * 1.25f),
          blue,
          1.f);
      StoreHalf4(
          &motion_fixture,
          texel,
          checker ? 0.75f : -0.5f,
          highlight ? -0.375f : 0.25f,
          0.f,
          0.f);
      const float depth_value = highlight ? 0.2f : 0.05f + (v * 0.9f);
      std::memcpy(
          depth_fixture.data() + (texel * sizeof(depth_value)),
          &depth_value,
          sizeof(depth_value));
      StoreHalf4(&output_sentinel, texel, -4.f, -4.f, -4.f, -4.f);
    }
  }

  auto input_transfer = CreateTextureTransfer(
      device.Get(),
      input.Get(),
      rgba16f_row_size,
      HEIGHT,
      L"input");
  auto motion_transfer = CreateTextureTransfer(
      device.Get(),
      motion.Get(),
      rgba16f_row_size,
      HEIGHT,
      L"motion");
  auto depth_transfer = CreateTextureTransfer(
      device.Get(),
      depth.Get(),
      r32f_row_size,
      HEIGHT,
      L"depth");
  auto output_initial_transfer = CreateTextureTransfer(
      device.Get(),
      output.Get(),
      rgba16f_row_size,
      HEIGHT,
      L"output initial");
  auto output_transfer = CreateTextureTransfer(
      device.Get(),
      output.Get(),
      rgba16f_row_size,
      HEIGHT,
      L"output evaluation");
  if (input_transfer.upload == nullptr || input_transfer.readback == nullptr
      || motion_transfer.upload == nullptr
      || motion_transfer.readback == nullptr
      || depth_transfer.upload == nullptr || depth_transfer.readback == nullptr
      || output_initial_transfer.upload == nullptr
      || output_initial_transfer.readback == nullptr
      || output_transfer.readback == nullptr) {
    return 8;
  }
  if (!telemetry.ConfigureStageCapture(output_transfer.readback.Get())) {
    Log("kernel.telemetry.stage_capture failed");
    return 8;
  }
  if (!RecordUpload(
          command_list.Get(),
          input.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          input_transfer,
          input_fixture)
      || !RecordUpload(
          command_list.Get(),
          motion.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          motion_transfer,
          motion_fixture)
      || !RecordUpload(
          command_list.Get(),
          depth.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          depth_transfer,
          depth_fixture)
      || !RecordUpload(
          command_list.Get(),
          output.Get(),
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          output_initial_transfer,
          output_sentinel)) {
    Log("capture.upload.failed");
    return 8;
  }
  RecordReadback(
      command_list.Get(),
      input.Get(),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      input_transfer);
  RecordReadback(
      command_list.Get(),
      motion.Get(),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      motion_transfer);
  RecordReadback(
      command_list.Get(),
      depth.Get(),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      depth_transfer);
  RecordReadback(
      command_list.Get(),
      output.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      output_initial_transfer);
  result = command_list->Close();
  if (FAILED(result)) return 8;
  ID3D12CommandList* initialization_lists[] = {command_list.Get()};
  queue->ExecuteCommandLists(1u, initialization_lists);
  if (!WaitForGpu(queue.Get(), fence.Get(), 1u, device.Get())) return 8;

  const auto input_readback = ReadTight(input_transfer);
  const auto motion_readback = ReadTight(motion_transfer);
  const auto depth_readback = ReadTight(depth_transfer);
  const auto output_initial_readback = ReadTight(output_initial_transfer);
  const auto input_statistics = AnalyzeRgba16f(input_readback);
  const auto motion_statistics = AnalyzeRgba16f(motion_readback);
  const auto output_initial_statistics = AnalyzeRgba16f(
      output_initial_readback);
  const auto black_statistics = AnalyzeRgba16f(
      std::vector<std::byte>(input_readback.size()));
  const bool black_validation_self_test_passed =
      !IsValidRgba16fOutput(black_statistics)
      && black_statistics.exact_zero_rgb_texels == black_statistics.texels;
  const bool inputs_verified = input_readback == input_fixture
                               && motion_readback == motion_fixture
                               && depth_readback == depth_fixture
                               && output_initial_readback == output_sentinel
                               && IsValidRgba16fOutput(input_statistics)
                               && motion_statistics.layout_valid
                               && motion_statistics.finite
                               && black_validation_self_test_passed;
  bool capture_valid = inputs_verified;
  const std::array<std::pair<std::filesystem::path, const std::vector<std::byte>*>, 4u>
      initial_artifacts = {{
          {capture_directory / L"input.rgba16f.raw", &input_readback},
          {capture_directory / L"motion.rgba16f.raw", &motion_readback},
          {capture_directory / L"depth.r32f.raw", &depth_readback},
          {capture_directory / L"output-initial.rgba16f.raw", &output_initial_readback},
      }};
  for (const auto& [path, bytes] : initial_artifacts) {
    if (!profile_mode) {
      capture_valid = WriteRaw(path, *bytes) && capture_valid;
      Log(std::format(
          "capture.write path={} bytes={} sha256={}",
          path.string(),
          bytes->size(),
          Sha256(path)));
    }
  }
  if (profile_mode) Log("capture.raw_artifacts_written=false");
  Log(std::format("capture.inputs.verified={}", inputs_verified));
  Log(std::format(
      "capture.statistics resource=input {}",
      FormatRgba16fStatistics(input_statistics)));
  Log(std::format(
      "capture.statistics resource=motion {}",
      FormatRgba16fStatistics(motion_statistics)));
  Log(std::format(
      "capture.statistics resource=output-initial {}",
      FormatRgba16fStatistics(output_initial_statistics)));
  Log(std::format(
      "capture.validation.black_fixture_rejected={} {}",
      black_validation_self_test_passed,
      FormatRgba16fStatistics(black_statistics)));

  auto* const initialize_core = reinterpret_cast<InitializeNgxCore>(
      GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
  if (initialize_core == nullptr) {
    Log("core.initialize_export missing");
    return 9;
  }
  std::error_code app_data_error;
  auto ngx_app_data = std::filesystem::temp_directory_path(app_data_error)
                      / L"RenoDX"
                      / L"dlssnr-repro";
  if (!app_data_error) {
    std::filesystem::create_directories(ngx_app_data, app_data_error);
  }
  if (app_data_error) ngx_app_data = executable_directory;
  Log(std::format(
      "core.initialize.begin app_data={} local={}",
      ngx_app_data.string(),
      !ngx_app_data.is_relative()
          && !ngx_app_data.native().starts_with(L"\\\\")));
  const NVSDK_NGX_Result core_initialize_result = initialize_core(
      0x876232cULL,
      ngx_app_data.c_str(),
      device.Get(),
      NVSDK_NGX_Version_API);
  Log(std::format(
      "core.initialize.end result={:#010x}",
      static_cast<uint32_t>(core_initialize_result)));
  if (NVSDK_NGX_FAILED(core_initialize_result)) return 10;

  Log("direct.attach.begin");
  if (!direct::Attach(snippet_path)) {
    Log("direct.attach.failed");
    return 11;
  }
  Log("direct.attach.end");
  (void)EnableSnippetLogging();
  Log("direct.bind_device.begin");
  if (!direct::BindDevice(device.Get())) {
    Log("direct.bind_device.failed");
    (void)direct::Detach();
    return 12;
  }
  Log("direct.bind_device.end");
  nr::Options options;
  options.mode = nr::Mode::ON;
  Log("direct.set_options.begin");
  if (!direct::SetOptions(options)) {
    Log("direct.set_options.failed");
    (void)direct::Detach();
    return 13;
  }
  Log("direct.set_options.end");
  telemetry.Flush();

  std::vector<std::filesystem::path> output_paths;
  std::vector<Rgba16fStatistics> output_statistics;
  uint32_t completed_evaluations = 0u;
  bool all_outputs_changed = true;
  bool all_outputs_content_valid = true;
  for (uint32_t evaluation = 1u; evaluation <= evaluations; ++evaluation) {
    telemetry.SetPhase(std::format("evaluation-{:03}", evaluation));
    const auto evaluation_started = std::chrono::steady_clock::now();
    result = allocator->Reset();
    if (SUCCEEDED(result)) result = command_list->Reset(allocator.Get(), nullptr);
    Log(std::format(
        "commands.reset evaluation={} result={:#010x}",
        evaluation,
        static_cast<uint32_t>(result)));
    if (FAILED(result)) break;
    Log(std::format("direct.evaluate.begin evaluation={}", evaluation));
    const bool evaluated = direct::Evaluate({
        .command_list = command_list.Get(),
        .input = input.Get(),
        .output = output.Get(),
        .motion = motion.Get(),
        .depth = depth.Get(),
        .output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        .width = WIDTH,
        .height = HEIGHT,
        .motion_scale_x = 1.f,
        .motion_scale_y = 1.f,
        .depth_inverted = false,
        .reset = evaluation == 1u,
    });
    Log(std::format(
        "direct.evaluate.end evaluation={} succeeded={}",
        evaluation,
        evaluated));
    if (!evaluated) break;
    if (profile_mode) telemetry.ResolveGpuTimestamps(command_list.Get());
    RecordReadback(
        command_list.Get(),
        output.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        output_transfer,
        true);
    result = command_list->Close();
    Log(std::format(
        "commands.close evaluation={} result={:#010x}",
        evaluation,
        static_cast<uint32_t>(result)));
    if (FAILED(result)) break;
    ID3D12CommandList* lists[] = {command_list.Get()};
    Log(std::format("commands.execute.begin evaluation={}", evaluation));
    queue->ExecuteCommandLists(1u, lists);
    Log(std::format("commands.execute.end evaluation={}", evaluation));
    if (!WaitForGpu(
            queue.Get(),
            fence.Get(),
            static_cast<uint64_t>(evaluation) + 1u,
            device.Get())) {
      break;
    }
    Log(std::format(
        "evaluation.duration_us evaluation={} duration={}",
        evaluation,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - evaluation_started)
            .count()));
    telemetry.Flush();
    const auto output_readback = ReadTight(output_transfer);
    const auto output_path = capture_directory
                             / std::format(L"output-eval-{:03}.rgba16f.raw", evaluation);
    const bool output_written = profile_mode
                                || WriteRaw(output_path, output_readback);
    const bool output_changed = output_readback.size() == output_sentinel.size()
                                && output_readback != output_sentinel;
    const auto statistics = AnalyzeRgba16f(output_readback, &input_readback);
    const bool output_content_valid = IsValidRgba16fOutput(statistics);
    all_outputs_changed = all_outputs_changed && output_changed;
    all_outputs_content_valid = all_outputs_content_valid
                                && output_content_valid;
    capture_valid = capture_valid
                    && output_written
                    && output_changed
                    && output_content_valid;
    if (output_written) {
      if (!profile_mode) output_paths.push_back(output_path);
      output_statistics.push_back(statistics);
    }
    Log(std::format(
        "capture.output evaluation={} changed={} content_valid={} bytes={} raw_written={} sha256={}",
        evaluation,
        output_changed,
        output_content_valid,
        output_readback.size(),
        !profile_mode,
        profile_mode ? "not-written" : Sha256(output_path)));
    Log(std::format(
        "capture.statistics resource=output evaluation={} {}",
        evaluation,
        FormatRgba16fStatistics(statistics)));
    ++completed_evaluations;
  }

  Log("direct.detach.begin");
  const bool detached = direct::Detach();
  Log(std::format("direct.detach.end succeeded={}", detached));
  const bool outputs_verified = all_outputs_changed
                                && all_outputs_content_valid
                                && completed_evaluations == evaluations
                                && (profile_mode || output_paths.size() == evaluations)
                                && output_statistics.size() == evaluations;
  const bool test_passed = detached && capture_valid && outputs_verified;
  const auto manifest_path = capture_directory / L"manifest.json";
  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  manifest << std::format(
      "{{\n"
      "  \"schema\": 3,\n"
      "  \"classification\": \"D3D12 E2E runtime test\",\n"
      "  \"captureMode\": \"{}\",\n"
      "  \"rawArtifactsWritten\": {},\n"
      "  \"width\": {},\n"
      "  \"height\": {},\n"
      "  \"jitterPixels\": [0.0, 0.0],\n"
      "  \"jitterBinding\": \"producer metadata; no recovered DLSS-NR jitter parameter\",\n"
      "  \"motionVectorScale\": [1.0, 1.0],\n"
      "  \"depthInverted\": false,\n"
      "  \"validation\": {{\"blackFixtureRejected\": {}, \"requiresFiniteComponents\": true, \"significantRgbAbsoluteThreshold\": 0.0001, \"minimumSignificantRgbFraction\": 0.01, \"requiresSpatialVariation\": true}},\n"
      "  \"inputsVerified\": {},\n"
      "  \"outputsVerified\": {},\n"
      "  \"testPassed\": {},\n"
      "  \"completedEvaluations\": {},\n"
      "  \"requestedEvaluations\": {},\n"
      "  \"resources\": [",
      profile_mode ? "profile" : "raw",
      profile_mode ? "false" : "true",
      WIDTH,
      HEIGHT,
      black_validation_self_test_passed ? "true" : "false",
      inputs_verified ? "true" : "false",
      outputs_verified ? "true" : "false",
      test_passed ? "true" : "false",
      completed_evaluations,
      evaluations);
  if (!profile_mode) {
    manifest << std::format(
        "\n"
        "    {{\"file\": \"input.rgba16f.raw\", \"format\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", \"rowBytes\": {}, \"sha256\": \"{}\"}},\n"
        "    {{\"file\": \"motion.rgba16f.raw\", \"format\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", \"rowBytes\": {}, \"sha256\": \"{}\"}},\n"
        "    {{\"file\": \"depth.r32f.raw\", \"format\": \"DXGI_FORMAT_R32_FLOAT\", \"rowBytes\": {}, \"sha256\": \"{}\"}},\n"
        "    {{\"file\": \"output-initial.rgba16f.raw\", \"format\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", \"rowBytes\": {}, \"sha256\": \"{}\"}}",
        rgba16f_row_size,
        Sha256(initial_artifacts[0u].first),
        rgba16f_row_size,
        Sha256(initial_artifacts[1u].first),
        r32f_row_size,
        Sha256(initial_artifacts[2u].first),
        rgba16f_row_size,
        Sha256(initial_artifacts[3u].first));
  }
  for (const auto& output_path : output_paths) {
    manifest << std::format(
        ",\n    {{\"file\": \"{}\", \"format\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", \"rowBytes\": {}, \"sha256\": \"{}\"}}",
        output_path.filename().string(),
        rgba16f_row_size,
        Sha256(output_path));
  }
  manifest << std::format(
      "\n  ],\n"
      "  \"inputStatistics\": {{\"finite\": {}, \"nonBlack\": {}, \"spatiallyVarying\": {}, \"nonFiniteComponents\": {}, \"exactZeroRgbTexels\": {}, \"significantRgbTexels\": {}, \"rgbMinimum\": [{:.9g}, {:.9g}, {:.9g}], \"rgbMaximum\": [{:.9g}, {:.9g}, {:.9g}], \"meanAbsoluteRgb\": {:.9g}, \"alphaMinimum\": {:.9g}, \"alphaMaximum\": {:.9g}, \"meanAbsoluteAlpha\": {:.9g}}},\n"
      "  \"outputStatistics\": [",
      input_statistics.finite,
      input_statistics.non_black,
      input_statistics.spatially_varying,
      input_statistics.non_finite_components,
      input_statistics.exact_zero_rgb_texels,
      input_statistics.significant_rgb_texels,
      input_statistics.minimum_rgb[0u],
      input_statistics.minimum_rgb[1u],
      input_statistics.minimum_rgb[2u],
      input_statistics.maximum_rgb[0u],
      input_statistics.maximum_rgb[1u],
      input_statistics.maximum_rgb[2u],
      input_statistics.mean_absolute_rgb,
      input_statistics.minimum_alpha,
      input_statistics.maximum_alpha,
      input_statistics.mean_absolute_alpha);
  for (size_t index = 0u; index < output_statistics.size(); ++index) {
    const auto& statistics = output_statistics[index];
    manifest << std::format(
        "{}\n    {{\"evaluation\": {}, \"valid\": {}, \"finite\": {}, \"nonBlack\": {}, \"spatiallyVarying\": {}, \"nonFiniteComponents\": {}, \"exactZeroRgbTexels\": {}, \"significantRgbTexels\": {}, \"rgbMinimum\": [{:.9g}, {:.9g}, {:.9g}], \"rgbMaximum\": [{:.9g}, {:.9g}, {:.9g}], \"meanAbsoluteRgb\": {:.9g}, \"alphaMinimum\": {:.9g}, \"alphaMaximum\": {:.9g}, \"meanAbsoluteAlpha\": {:.9g}, \"meanAbsoluteRgbDifferenceFromInput\": {:.9g}}}",
        index == 0u ? "" : ",",
        index + 1u,
        IsValidRgba16fOutput(statistics),
        statistics.finite,
        statistics.non_black,
        statistics.spatially_varying,
        statistics.non_finite_components,
        statistics.exact_zero_rgb_texels,
        statistics.significant_rgb_texels,
        statistics.minimum_rgb[0u],
        statistics.minimum_rgb[1u],
        statistics.minimum_rgb[2u],
        statistics.maximum_rgb[0u],
        statistics.maximum_rgb[1u],
        statistics.maximum_rgb[2u],
        statistics.mean_absolute_rgb,
        statistics.minimum_alpha,
        statistics.maximum_alpha,
        statistics.mean_absolute_alpha,
        statistics.mean_absolute_rgb_difference);
  }
  manifest << "\n  ]\n}\n";
  manifest.close();
  capture_valid = test_passed
                  && std::filesystem::file_size(manifest_path) != 0u;
  Log(std::format(
      "capture.manifest path={} valid={} sha256={}",
      manifest_path.string(),
      capture_valid,
      Sha256(manifest_path)));
  Log("core.shutdown skipped; one-shot process exit owns driver-core teardown");
  Log("repro.end");
  return detached && capture_valid ? 0 : 14;
}

int wmain(int argc, wchar_t** argv) {
  const int result = Run(argc, argv);
  if (GetEnvironmentVariableW(
          L"RENODX_DLSSNR_REPRO_NO_MESSAGE_BOX",
          nullptr,
          0u)
      != 0u) {
    return result;
  }
  const std::wstring message = result == 0
                                   ? L"DLSS-NR completed successfully. Send dlssnr-repro.log and "
                                     L"dlssnr-direct-breadcrumbs.log to the developer."
                                   : std::format(
                                         L"DLSS-NR failed with exit code {}. Send dlssnr-repro.log and "
                                         L"dlssnr-direct-breadcrumbs.log to the developer.",
                                         result);
  (void)MessageBoxW(
      nullptr,
      message.c_str(),
      result == 0 ? L"DLSS-NR repro succeeded" : L"DLSS-NR repro failed",
      MB_OK
          | (result == 0 ? MB_ICONINFORMATION : MB_ICONERROR)
          | MB_SETFOREGROUND);
  return result;
}

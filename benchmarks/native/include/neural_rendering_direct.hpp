/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <include/reshade.hpp>

#include "neural_rendering.hpp"

namespace renodx::utils::dlss::neural_rendering::direct {

using GetModuleFileNameWFunction = decltype(&GetModuleFileNameW);
using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using InitializeParameterProviderD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
    uint64_t,
    const wchar_t*,
    ID3D12Device*,
    NVSDK_NGX_Version);
using ShutdownParameterProviderD3D12 =
    NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using InitializeD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
    uint64_t,
    const wchar_t*,
    ID3D12Device*,
    NVSDK_NGX_Version,
    const NVSDK_NGX_Parameter*);
using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*,
    NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

struct ResourceSubrect {
  uint32_t base_x = 0u;
  uint32_t base_y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
};

enum class UiCorrectionMode : uint8_t {
  INHERIT,
  DISABLED,
  ENABLED,
};

struct ParameterProviderConfig {
  std::filesystem::path module_path;
  std::filesystem::path application_data_path;
  bool load_if_missing = false;
};

struct Submission {
  ID3D12GraphicsCommandList* command_list = nullptr;
  uint32_t feature_slot = 0u;
  ID3D12Resource* input = nullptr;
  ID3D12Resource* output = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* ui = nullptr;
  ID3D12Resource* ui_alpha = nullptr;
  D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t width = 0u;
  uint32_t height = 0u;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  bool depth_inverted = false;
  bool reset = false;
  ResourceSubrect input_subrect = {};
  ResourceSubrect output_subrect = {};
  ResourceSubrect motion_subrect = {};
  ResourceSubrect depth_subrect = {};
  UiCorrectionMode ui_correction = UiCorrectionMode::INHERIT;
};

struct PrivateOutputSubmission {
  ID3D12GraphicsCommandList* command_list = nullptr;
  ID3D12Device* device = nullptr;
  uint64_t command_list_generation = 0u;
  uint32_t feature_slot = 0u;
  ID3D12Resource* input = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t width = 0u;
  uint32_t height = 0u;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  bool depth_inverted = false;
  bool reset = false;
  ResourceSubrect input_subrect = {};
  ResourceSubrect motion_subrect = {};
  ResourceSubrect depth_subrect = {};
  UiCorrectionMode ui_correction = UiCorrectionMode::INHERIT;
  D3D12_RESOURCE_STATES output_state_after_evaluate = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

struct ReplacementSubmission {
  ID3D12GraphicsCommandList* command_list = nullptr;
  uint64_t command_list_generation = 0u;
  ID3D12Resource* game_output = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  ResourceSubrect output_subrect = {};
  ResourceSubrect motion_subrect = {};
  ResourceSubrect depth_subrect = {};
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  bool depth_inverted = false;
  bool reset = false;
};

struct Status {
  std::filesystem::path snippet_path;
  NVSDK_NGX_Result last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
  uint64_t evaluation_count = 0u;
  uint64_t options_revision = 0u;
  bool attached = false;
  bool device_bound = false;
  bool feature_created = false;
  bool failed = false;
  bool reset_pending = true;
  bool ui_correction_requested = false;
  bool ui_correction_active = false;
  size_t private_output_count = 0u;
  size_t retained_feature_count = 0u;
  uint64_t replacement_count = 0u;
  std::filesystem::path parameter_provider_path;
  bool parameter_provider_loaded = false;
  bool parameter_provider_force_loaded = false;
  bool parameter_provider_initialized = false;
  bool parameter_provider_force_requested = false;
  bool parameter_provider_load_attempted = false;
};

struct Api {
  HMODULE module = nullptr;
  InitializeD3D12 initialize = nullptr;
  CreateFeature create_feature = nullptr;
  EvaluateFeature evaluate_feature = nullptr;
  ReleaseFeature release_feature = nullptr;
  ShutdownD3D12 shutdown = nullptr;
};

struct PrivateOutput {
  ID3D12Resource* input = nullptr;
  ID3D12Resource* resource = nullptr;
  D3D12_RESOURCE_DESC desc = {};
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

inline constexpr size_t MAX_RETAINED_FEATURES = 16u;
inline constexpr uint32_t MAX_FEATURE_SLOTS = 10u;
inline thread_local bool releasing_private_outputs = false;

struct FeatureState {
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t width = 0u;
  uint32_t height = 0u;
  PerformanceMode performance_mode = PerformanceMode::VALUE_3;
  uint32_t render_preset = 1u;
  uint64_t evaluated_options_revision = 0u;
  bool reset_pending = true;
  bool ui_correction_active = false;
  bool reusable = true;
};

struct Runtime {
  std::shared_mutex mutex;
  std::recursive_mutex private_outputs_mutex;
  Api api;
  std::filesystem::path snippet_path;
#ifndef NDEBUG
  std::filesystem::path breadcrumb_path;
  uint64_t breadcrumb_sequence = 0u;
#endif
  HMODULE caller_module = nullptr;
  void** get_module_file_name_iat = nullptr;
  void* original_get_module_file_name = nullptr;
  GetModuleFileNameWFunction real_get_module_file_name = nullptr;
  ID3D12Device* device = nullptr;
  HMODULE parameter_module = nullptr;
  AllocateParameters allocate_parameters = nullptr;
  DestroyParameters destroy_parameters = nullptr;
  InitializeParameterProviderD3D12 initialize_parameter_provider = nullptr;
  ShutdownParameterProviderD3D12 shutdown_parameter_provider = nullptr;
  ParameterProviderConfig parameter_provider_config;
  std::filesystem::path parameter_provider_path;
  std::filesystem::path discovered_parameter_provider_path;
  bool parameter_provider_load_attempted = false;
  bool parameter_provider_force_loaded = false;
  bool parameter_provider_initialization_requested = false;
  bool parameter_provider_initialized = false;
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  Options options;
  uint32_t feature_width = 0u;
  uint32_t feature_height = 0u;
  PerformanceMode feature_performance_mode = PerformanceMode::VALUE_3;
  uint32_t feature_render_preset = 1u;
  NVSDK_NGX_Result last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
  uint64_t options_revision = 0u;
  uint64_t evaluated_options_revision = 0u;
  uint64_t recording_sequence = 0u;
  uint64_t evaluation_count = 0u;
  uint64_t replacement_count = 0u;
  std::vector<PrivateOutput> private_outputs;
  std::vector<FeatureState> pass_features;
  std::vector<FeatureState> retained_features;
  // Called under mutex. Callbacks must not reenter the direct runtime. Without
  // a retirement proof, retain features until the existing GPU-idle teardown.
  void (*record_feature)(NVSDK_NGX_Handle*, ID3D12GraphicsCommandList*) = nullptr;
  bool (*can_retire_feature)(NVSDK_NGX_Handle*) = nullptr;
  void (*forget_feature)(NVSDK_NGX_Handle*) = nullptr;
  bool feature_cache_exhaustion_reported = false;
  bool reset_pending = true;
  bool failed = false;
  bool ui_correction_requested = false;
  bool ui_correction_active = false;
  bool reconstruction_contract_logged = false;
  bool second_device_warning_logged = false;
};

inline Runtime runtime;

inline void SwapActiveFeatureLocked(FeatureState* state) {
  std::swap(runtime.parameters, state->parameters);
  std::swap(runtime.feature, state->feature);
  std::swap(runtime.feature_width, state->width);
  std::swap(runtime.feature_height, state->height);
  std::swap(runtime.feature_performance_mode, state->performance_mode);
  std::swap(runtime.feature_render_preset, state->render_preset);
  std::swap(
      runtime.evaluated_options_revision,
      state->evaluated_options_revision);
  std::swap(runtime.reset_pending, state->reset_pending);
  std::swap(runtime.ui_correction_active, state->ui_correction_active);
}

struct ScopedFeatureSlot {
  FeatureState* parked_feature = nullptr;

  explicit ScopedFeatureSlot(uint32_t feature_slot) {
    if (feature_slot == 0u) return;
    if (runtime.pass_features.size() < feature_slot) {
      runtime.pass_features.resize(feature_slot);
    }
    parked_feature = &runtime.pass_features[feature_slot - 1u];
    SwapActiveFeatureLocked(parked_feature);
  }

  ~ScopedFeatureSlot() {
    if (parked_feature != nullptr) {
      SwapActiveFeatureLocked(parked_feature);
    }
  }

  ScopedFeatureSlot(const ScopedFeatureSlot&) = delete;
  ScopedFeatureSlot& operator=(const ScopedFeatureSlot&) = delete;
};

#ifndef NDEBUG
inline void WriteBreadcrumbLocked(std::string_view message) {
  if (runtime.breadcrumb_path.empty()) return;
  HANDLE file = CreateFileW(
      runtime.breadcrumb_path.c_str(),
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const std::string line = std::format(
      "{:08} tick={} pid={} tid={} {}\r\n",
      ++runtime.breadcrumb_sequence,
      GetTickCount64(),
      GetCurrentProcessId(),
      GetCurrentThreadId(),
      message);
  DWORD bytes_written = 0u;
  (void)WriteFile(
      file,
      line.data(),
      static_cast<DWORD>(line.size()),
      &bytes_written,
      nullptr);
  (void)FlushFileBuffers(file);
  CloseHandle(file);
}

inline void ResetBreadcrumbLogLocked() {
  runtime.breadcrumb_sequence = 0u;
  HANDLE file = CreateFileW(
      runtime.breadcrumb_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
#endif

inline void Log(reshade::log::level level, const std::string& message) {
  if (reshade::internal::get_reshade_module_handle() == nullptr) return;
  reshade::log::message(
      level,
      std::format("[RenoDX] DLSS-NR direct: {}", message).c_str());
}

inline void Breadcrumb(std::string_view message) {
#ifndef NDEBUG
  std::scoped_lock lock(runtime.mutex);
  WriteBreadcrumbLocked(message);
#else
  (void)message;
#endif
}

inline NVSDK_NGX_Result NVSDK_CONV ComputeScalingRatio(
    NVSDK_NGX_Parameter* parameters) {
  if (parameters == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  try {
    parameters->Set("DLSSNR.ScalingRatio", 1.f);
    return NVSDK_NGX_Result_Success;
  } catch (...) {
    runtime.failed = true;
    return NVSDK_NGX_Result_FAIL_PlatformError;
  }
}

inline void** FindImportAddress(HMODULE module, const char* import_name) {
  if (module == nullptr || import_name == nullptr) return nullptr;
  auto* const image = reinterpret_cast<std::byte*>(module);
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
  const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (imports.VirtualAddress == 0u
      || imports.VirtualAddress >= image_size
      || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    return nullptr;
  }

  auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
      image + imports.VirtualAddress);
  const auto* const descriptor_end = reinterpret_cast<const std::byte*>(
                                         descriptor)
                                     + imports.Size;
  for (; reinterpret_cast<const std::byte*>(descriptor + 1u) <= descriptor_end
         && descriptor->Name != 0u;
       ++descriptor) {
    if (descriptor->OriginalFirstThunk == 0u
        || descriptor->OriginalFirstThunk >= image_size
        || descriptor->FirstThunk == 0u
        || descriptor->FirstThunk >= image_size) {
      continue;
    }
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
        image + descriptor->OriginalFirstThunk);
    auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
        image + descriptor->FirstThunk);
    while (reinterpret_cast<std::byte*>(names + 1u) <= image + image_size
           && reinterpret_cast<std::byte*>(addresses + 1u) <= image + image_size
           && names->u1.AddressOfData != 0u) {
      if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
          && names->u1.AddressOfData < image_size) {
        const auto* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
            image + names->u1.AddressOfData);
        if (std::strcmp(
                reinterpret_cast<const char*>(import->Name),
                import_name)
            == 0) {
          return reinterpret_cast<void**>(&addresses->u1.Function);
        }
      }
      ++names;
      ++addresses;
    }
  }
  return nullptr;
}

inline DWORD WINAPI CallerGetModuleFileNameW(
    HMODULE module,
    LPWSTR filename,
    DWORD size) {
  if (module == runtime.caller_module && filename != nullptr && size != 0u) {
    constexpr wchar_t identity[] = L"nvngx.dll";
    constexpr DWORD length = static_cast<DWORD>(std::size(identity) - 1u);
    if (length >= size) {
      filename[0] = L'\0';
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return size;
    }
    std::memcpy(filename, identity, sizeof(identity));
    return length;
  }
  return runtime.real_get_module_file_name != nullptr
             ? runtime.real_get_module_file_name(module, filename, size)
             : 0u;
}

inline bool InstallCallerIdentityLocked() {
  if (runtime.get_module_file_name_iat != nullptr) return true;
  runtime.get_module_file_name_iat = FindImportAddress(
      runtime.api.module,
      "GetModuleFileNameW");
  if (runtime.get_module_file_name_iat == nullptr
      || GetModuleHandleExW(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             reinterpret_cast<LPCWSTR>(&CallerGetModuleFileNameW),
             &runtime.caller_module)
             == FALSE) {
    runtime.get_module_file_name_iat = nullptr;
    runtime.caller_module = nullptr;
    return false;
  }

  DWORD old_protection = 0u;
  if (VirtualProtect(
          runtime.get_module_file_name_iat,
          sizeof(*runtime.get_module_file_name_iat),
          PAGE_READWRITE,
          &old_protection)
      == FALSE) {
    runtime.get_module_file_name_iat = nullptr;
    runtime.caller_module = nullptr;
    return false;
  }
  runtime.original_get_module_file_name = *runtime.get_module_file_name_iat;
  runtime.real_get_module_file_name = reinterpret_cast<
      GetModuleFileNameWFunction>(runtime.original_get_module_file_name);
  *runtime.get_module_file_name_iat = reinterpret_cast<void*>(
      &CallerGetModuleFileNameW);
  FlushInstructionCache(
      GetCurrentProcess(),
      runtime.get_module_file_name_iat,
      sizeof(*runtime.get_module_file_name_iat));
  DWORD ignored = 0u;
  (void)VirtualProtect(
      runtime.get_module_file_name_iat,
      sizeof(*runtime.get_module_file_name_iat),
      old_protection,
      &ignored);
  return true;
}

inline bool RestoreCallerIdentityLocked() {
  if (runtime.get_module_file_name_iat == nullptr) return true;
  DWORD old_protection = 0u;
  if (VirtualProtect(
          runtime.get_module_file_name_iat,
          sizeof(*runtime.get_module_file_name_iat),
          PAGE_READWRITE,
          &old_protection)
      == FALSE) {
    return false;
  }
  *runtime.get_module_file_name_iat = runtime.original_get_module_file_name;
  FlushInstructionCache(
      GetCurrentProcess(),
      runtime.get_module_file_name_iat,
      sizeof(*runtime.get_module_file_name_iat));
  DWORD ignored = 0u;
  const bool restored = VirtualProtect(
                            runtime.get_module_file_name_iat,
                            sizeof(*runtime.get_module_file_name_iat),
                            old_protection,
                            &ignored)
                        != FALSE;
  runtime.get_module_file_name_iat = nullptr;
  runtime.original_get_module_file_name = nullptr;
  runtime.real_get_module_file_name = nullptr;
  runtime.caller_module = nullptr;
  return restored;
}

inline std::filesystem::path GetLoadedModulePath(HMODULE module) {
  if (module == nullptr) return {};
  std::array<wchar_t, 32768u> path = {};
  const DWORD length = GetModuleFileNameW(
      module,
      path.data(),
      static_cast<DWORD>(path.size()));
  return length == 0u || length >= path.size()
             ? std::filesystem::path{}
             : std::filesystem::path(path.data(), path.data() + length);
}

inline std::filesystem::path FindInstalledParameterProviderLocked() {
  if (!runtime.parameter_provider_config.module_path.empty()) {
    std::error_code error;
    const auto path = std::filesystem::canonical(
        runtime.parameter_provider_config.module_path,
        error);
    return !error && std::filesystem::is_regular_file(path, error) && !error
               ? path
               : std::filesystem::path{};
  }

  std::array<wchar_t, 32768u> system_directory = {};
  const UINT length = GetSystemDirectoryW(
      system_directory.data(),
      static_cast<UINT>(system_directory.size()));
  if (length == 0u || length >= system_directory.size()) return {};
  const std::filesystem::path system_path(
      system_directory.data(),
      system_directory.data() + length);
  for (const wchar_t* filename : {L"nvngx.dll", L"_nvngx.dll"}) {
    std::error_code error;
    const auto candidate = system_path / filename;
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      return candidate;
    }
  }

  const auto repository = system_path / L"DriverStore" / L"FileRepository";
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
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) continue;
    for (const wchar_t* filename : {L"nvngx.dll", L"_nvngx.dll"}) {
      const auto candidate = entry.path() / filename;
      if (!std::filesystem::is_regular_file(candidate, entry_error)) continue;
      const auto write_time = std::filesystem::last_write_time(
          candidate,
          entry_error);
      if (!entry_error && (selected.empty() || write_time > selected_time)) {
        selected = candidate;
        selected_time = write_time;
      }
    }
  }
  return selected;
}

inline bool AdoptParameterProviderLocked(
    HMODULE module,
    const std::filesystem::path& path,
    bool force_loaded,
    bool initialize_provider) {
  if (module == nullptr) return false;
  auto* const allocate = reinterpret_cast<AllocateParameters>(
      GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
  auto* const destroy = reinterpret_cast<DestroyParameters>(
      GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
  auto* const initialize = reinterpret_cast<InitializeParameterProviderD3D12>(
      GetProcAddress(module, "NVSDK_NGX_D3D12_Init"));
  auto* const shutdown = reinterpret_cast<ShutdownParameterProviderD3D12>(
      GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
  if (allocate == nullptr
      || destroy == nullptr
      || (initialize_provider
          && (initialize == nullptr || shutdown == nullptr))) {
    return false;
  }
  runtime.parameter_module = module;
  runtime.allocate_parameters = allocate;
  runtime.destroy_parameters = destroy;
  runtime.initialize_parameter_provider = initialize;
  runtime.shutdown_parameter_provider = shutdown;
  runtime.parameter_provider_path = path.empty()
                                        ? GetLoadedModulePath(module)
                                        : path;
  runtime.parameter_provider_force_loaded = force_loaded;
  runtime.parameter_provider_initialization_requested = initialize_provider;
  runtime.parameter_provider_initialized = false;
  Log(
      reshade::log::level::info,
      std::format(
          "using {} NVIDIA parameter provider {}",
          force_loaded ? "force-loaded" : "already-loaded",
          runtime.parameter_provider_path.string()));
  return true;
}

inline void ReleaseParameterProviderModuleLocked(
    bool reset_load_attempt = true) {
  HMODULE module = runtime.parameter_module;
  runtime.parameter_module = nullptr;
  runtime.allocate_parameters = nullptr;
  runtime.destroy_parameters = nullptr;
  runtime.initialize_parameter_provider = nullptr;
  runtime.shutdown_parameter_provider = nullptr;
  runtime.parameter_provider_path.clear();
  runtime.parameter_provider_force_loaded = false;
  runtime.parameter_provider_initialization_requested = false;
  runtime.parameter_provider_initialized = false;
  if (reset_load_attempt) runtime.parameter_provider_load_attempted = false;
  if (module != nullptr) FreeLibrary(module);
}

inline bool InitializeParameterProviderLocked(ID3D12Device* device) {
  if (!runtime.parameter_provider_initialization_requested
      || runtime.parameter_provider_initialized) {
    return true;
  }
  if (device == nullptr || runtime.initialize_parameter_provider == nullptr) {
    return false;
  }
  std::filesystem::path application_data_path =
      runtime.parameter_provider_config.application_data_path;
  std::error_code error;
  if (application_data_path.empty()) {
    application_data_path = std::filesystem::temp_directory_path(error)
                            / L"RenoDX"
                            / L"dlssnr";
  }
  if (!error) {
    std::filesystem::create_directories(application_data_path, error);
  }
  if (error || application_data_path.empty()) {
    Log(
        reshade::log::level::error,
        std::format(
            "could not prepare NGX application-data directory ({})",
            error.message()));
    return false;
  }
  NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_PlatformError;
  try {
    Log(
        reshade::log::level::info,
        std::format(
            "NGX lifecycle: backend D3D12 Init begin device={} module={} force_loaded={} app_id=0x876232c",
            static_cast<void*>(device),
            static_cast<void*>(runtime.parameter_module),
            runtime.parameter_provider_force_loaded));
    result = runtime.initialize_parameter_provider(
        0x876232cULL,
        application_data_path.c_str(),
        device,
        NVSDK_NGX_Version_API);
  } catch (...) {
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  Log(
      reshade::log::level::info,
      std::format(
          "NGX lifecycle: backend D3D12 Init end device={} module={} result={:#010x}",
          static_cast<void*>(device),
          static_cast<void*>(runtime.parameter_module),
          static_cast<uint32_t>(result)));
  if (NVSDK_NGX_FAILED(result)) {
    Log(
        reshade::log::level::error,
        std::format(
            "force-loaded NVIDIA NGX core initialization failed with {:#010x}",
            static_cast<uint32_t>(result)));
    return false;
  }
  runtime.parameter_provider_initialized = true;
  Log(
      reshade::log::level::info,
      "backend-owned NVIDIA NGX core initialization succeeded for the D3D12 device");
  return true;
}

inline bool ShutdownParameterProviderLocked(ID3D12Device* device) {
  if (!runtime.parameter_provider_initialized) return true;
  Log(
      reshade::log::level::info,
      std::format(
          "NGX lifecycle: backend D3D12 Shutdown1 begin device={} module={}",
          static_cast<void*>(device),
          static_cast<void*>(runtime.parameter_module)));
  NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_PlatformError;
  try {
    result = runtime.shutdown_parameter_provider == nullptr
                 ? NVSDK_NGX_Result_FAIL_NotInitialized
                 : runtime.shutdown_parameter_provider(device);
  } catch (...) {
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  runtime.parameter_provider_initialized = false;
  Log(
      reshade::log::level::info,
      std::format(
          "NGX lifecycle: backend D3D12 Shutdown1 end device={} module={} result={:#010x}",
          static_cast<void*>(device),
          static_cast<void*>(runtime.parameter_module),
          static_cast<uint32_t>(result)));
  Log(
      NVSDK_NGX_SUCCEED(result)
          ? reshade::log::level::info
          : reshade::log::level::error,
      std::format(
          "backend-owned NVIDIA NGX core shutdown returned {:#010x}",
          static_cast<uint32_t>(result)));
  return NVSDK_NGX_SUCCEED(result);
}

inline bool ResolveParameterApiLocked() {
  if (runtime.allocate_parameters != nullptr
      && runtime.destroy_parameters != nullptr) {
    return true;
  }
  constexpr const wchar_t* core_module_names[] = {
      L"_nvngx.dll",
      L"nvngx.dll",
  };
  constexpr const wchar_t* feature_module_names[] = {
      L"nvngx_dlss.dll",
      L"nvngx_dlssd.dll",
  };
  for (const wchar_t* module_name : core_module_names) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(0u, module_name, &module) == FALSE) continue;
    if (AdoptParameterProviderLocked(
            module,
            {},
            false,
            runtime.parameter_provider_config.load_if_missing)) {
      return true;
    }
    FreeLibrary(module);
  }
  if (!runtime.parameter_provider_config.load_if_missing) {
    for (const wchar_t* module_name : feature_module_names) {
      HMODULE module = nullptr;
      if (GetModuleHandleExW(0u, module_name, &module) == FALSE) continue;
      if (AdoptParameterProviderLocked(module, {}, false, false)) return true;
      FreeLibrary(module);
    }
  }
  if (runtime.parameter_provider_config.load_if_missing
      && !runtime.parameter_provider_load_attempted) {
    runtime.parameter_provider_load_attempted = true;
    runtime.discovered_parameter_provider_path =
        FindInstalledParameterProviderLocked();
    if (!runtime.discovered_parameter_provider_path.empty()) {
      HMODULE module = LoadLibraryExW(
          runtime.discovered_parameter_provider_path.c_str(),
          nullptr,
          LOAD_WITH_ALTERED_SEARCH_PATH);
      if (module != nullptr) {
        if (AdoptParameterProviderLocked(
                module,
                runtime.discovered_parameter_provider_path,
                true,
                true)) {
          return true;
        }
        FreeLibrary(module);
      }
      Log(
          reshade::log::level::error,
          std::format(
              "could not force-load a complete NVIDIA parameter provider from {}",
              runtime.discovered_parameter_provider_path.string()));
    } else {
      Log(
          reshade::log::level::error,
          "could not locate an installed NVIDIA NGX core to force-load");
    }
  }
  Log(
      reshade::log::level::error,
      "no loaded NVIDIA NGX core exports a complete D3D12 parameter allocator");
  return false;
}

inline void ConfigureParameterProvider(
    const ParameterProviderConfig& config) {
  std::scoped_lock lock(runtime.mutex);
  const bool initialization_enabled =
      !runtime.parameter_provider_config.load_if_missing
      && config.load_if_missing;
  if (runtime.parameter_provider_config.module_path != config.module_path
      || runtime.parameter_provider_config.application_data_path
             != config.application_data_path
      || runtime.parameter_provider_config.load_if_missing
             != config.load_if_missing) {
    runtime.parameter_provider_load_attempted = false;
    runtime.discovered_parameter_provider_path.clear();
  }
  runtime.parameter_provider_config = config;
  // Force mode must make the provider available before transport reaches
  // BindDevice; device-specific NGX initialization remains deferred.
  if (config.load_if_missing && runtime.parameter_module == nullptr) {
    (void)ResolveParameterApiLocked();
  }
  if (initialization_enabled
      && runtime.parameter_module != nullptr
      && runtime.device != nullptr
      && !runtime.parameter_provider_initialized) {
    const auto filename = runtime.parameter_provider_path.filename().wstring();
    if (_wcsicmp(filename.c_str(), L"_nvngx.dll") == 0
        || _wcsicmp(filename.c_str(), L"nvngx.dll") == 0) {
      runtime.parameter_provider_initialization_requested = true;
      (void)InitializeParameterProviderLocked(runtime.device);
    } else {
      Log(
          reshade::log::level::warning,
          "Force NGX Core will apply at the next D3D12 device boundary because the active parameter provider is not the driver core");
    }
  }
}

[[nodiscard]] inline bool IsParameterProviderAvailable() {
  const std::scoped_lock lock(runtime.mutex);
  return runtime.allocate_parameters != nullptr
         && runtime.destroy_parameters != nullptr;
}

inline bool ReleaseFeatureStateLocked(
    NVSDK_NGX_Handle** feature,
    NVSDK_NGX_Parameter** parameters,
    NVSDK_NGX_Result* last_result = &runtime.last_result) {
  *last_result = NVSDK_NGX_Result_Success;
  bool succeeded = true;
  if (*feature != nullptr) {
    try {
      *last_result = runtime.api.release_feature(*feature);
      succeeded = NVSDK_NGX_SUCCEED(*last_result);
    } catch (...) {
      *last_result = NVSDK_NGX_Result_FAIL_PlatformError;
      runtime.failed = true;
      succeeded = false;
    }
    if (!succeeded) {
      Log(
          reshade::log::level::error,
          std::format(
              "ReleaseFeature failed with {:#010x}",
              static_cast<uint32_t>(*last_result)));
    }
    if (runtime.forget_feature != nullptr) runtime.forget_feature(*feature);
    *feature = nullptr;
  }
  if (*parameters != nullptr) {
    if (runtime.destroy_parameters == nullptr) {
      *last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
      succeeded = false;
      Log(
          reshade::log::level::error,
          "cannot destroy feature parameters because the NVIDIA allocator is unavailable");
    } else {
      const auto result = runtime.destroy_parameters(*parameters);
      if (NVSDK_NGX_FAILED(result)) {
        *last_result = result;
        succeeded = false;
        Log(
            reshade::log::level::error,
            std::format(
                "DestroyParameters failed with {:#010x}",
                static_cast<uint32_t>(result)));
      }
    }
    *parameters = nullptr;
  }
  return succeeded;
}

inline bool ReleaseResourcesLocked() {
  bool succeeded = ReleaseFeatureStateLocked(
      &runtime.feature,
      &runtime.parameters);
  for (auto& state : runtime.pass_features) {
    succeeded = ReleaseFeatureStateLocked(
                    &state.feature,
                    &state.parameters)
                && succeeded;
  }
  runtime.pass_features.clear();
  for (auto& state : runtime.retained_features) {
    succeeded = ReleaseFeatureStateLocked(
                    &state.feature,
                    &state.parameters)
                && succeeded;
  }
  runtime.retained_features.clear();
  runtime.feature_cache_exhaustion_reported = false;
  runtime.feature_width = 0u;
  runtime.feature_height = 0u;
  runtime.evaluated_options_revision = 0u;
  runtime.reset_pending = true;
  runtime.ui_correction_active = false;
  return succeeded;
}

inline void ReleasePrivateOutputsLocked() {
  const std::scoped_lock private_outputs_lock(runtime.private_outputs_mutex);
  const bool was_releasing_private_outputs = releasing_private_outputs;
  releasing_private_outputs = true;
  for (auto& output : runtime.private_outputs) {
    if (output.resource != nullptr) output.resource->Release();
  }
  releasing_private_outputs = was_releasing_private_outputs;
  runtime.private_outputs.clear();
  runtime.reconstruction_contract_logged = false;
}

inline bool ShutdownDeviceLocked() {
  const bool resources_released = ReleaseResourcesLocked();
  ReleasePrivateOutputsLocked();
  bool succeeded = resources_released;
  if (runtime.device != nullptr) {
    auto* const device = runtime.device;
#ifndef NDEBUG
    WriteBreadcrumbLocked("shutdown.begin");
#endif
    try {
      runtime.last_result = runtime.api.shutdown(device);
      succeeded = NVSDK_NGX_SUCCEED(runtime.last_result) && succeeded;
    } catch (...) {
      runtime.last_result = NVSDK_NGX_Result_FAIL_PlatformError;
      runtime.failed = true;
      succeeded = false;
    }
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "shutdown.end result={:#010x}",
        static_cast<uint32_t>(runtime.last_result)));
#endif
    Log(
        NVSDK_NGX_SUCCEED(runtime.last_result)
            ? reshade::log::level::info
            : reshade::log::level::error,
        std::format(
            "Shutdown1 returned {:#010x}",
            static_cast<uint32_t>(runtime.last_result)));
    succeeded = ShutdownParameterProviderLocked(device) && succeeded;
    device->Release();
    runtime.device = nullptr;
    runtime.second_device_warning_logged = false;
  }
  ReleaseParameterProviderModuleLocked();
  return succeeded;
}

inline bool Attach(const std::filesystem::path& snippet_path) {
  std::scoped_lock lock(runtime.mutex);
  std::error_code error;
  const auto canonical_path = std::filesystem::canonical(snippet_path, error);
  if (error || !std::filesystem::is_regular_file(canonical_path, error) || error) {
    return false;
  }
  if (runtime.api.module != nullptr) {
    return canonical_path == runtime.snippet_path;
  }

#ifndef NDEBUG
  if (char enabled[2] = {};
      GetEnvironmentVariableA("RENODX_NR_BREADCRUMBS", enabled, sizeof(enabled)) == 1u && enabled[0] == '1') {
    runtime.breadcrumb_path = canonical_path.parent_path() / "dlssnr-direct-breadcrumbs.log";
    ResetBreadcrumbLogLocked();
  }
#endif
  runtime.api.module = LoadLibraryW(canonical_path.c_str());
  if (runtime.api.module == nullptr) {
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "attach.load_library.failed error={}",
        GetLastError()));
#endif
    Log(
        reshade::log::level::error,
        std::format("LoadLibraryW failed for {} ({})", canonical_path.string(), GetLastError()));
    return false;
  }
#ifndef NDEBUG
  WriteBreadcrumbLocked(std::format(
      "attach.load_library.succeeded module={}",
      static_cast<void*>(runtime.api.module)));
#endif
  const auto resolve = [](const char* name) {
    return GetProcAddress(runtime.api.module, name);
  };
  runtime.api.initialize = reinterpret_cast<InitializeD3D12>(
      resolve("NVSDK_NGX_D3D12_Init_Ext"));
  runtime.api.create_feature = reinterpret_cast<CreateFeature>(
      resolve("NVSDK_NGX_D3D12_CreateFeature"));
  runtime.api.evaluate_feature = reinterpret_cast<EvaluateFeature>(
      resolve("NVSDK_NGX_D3D12_EvaluateFeature"));
  runtime.api.release_feature = reinterpret_cast<ReleaseFeature>(
      resolve("NVSDK_NGX_D3D12_ReleaseFeature"));
  runtime.api.shutdown = reinterpret_cast<ShutdownD3D12>(
      resolve("NVSDK_NGX_D3D12_Shutdown1"));
  if (runtime.api.initialize == nullptr
      || runtime.api.create_feature == nullptr
      || runtime.api.evaluate_feature == nullptr
      || runtime.api.release_feature == nullptr
      || runtime.api.shutdown == nullptr
      || !InstallCallerIdentityLocked()) {
#ifndef NDEBUG
    WriteBreadcrumbLocked("attach.exports_or_identity.failed");
#endif
    Log(
        reshade::log::level::error,
        "snippet is missing a required lifecycle export or caller-identity hook");
    FreeLibrary(runtime.api.module);
    runtime.api = {};
    return false;
  }
  runtime.snippet_path = canonical_path;
  runtime.last_result = NVSDK_NGX_Result_Success;
  runtime.failed = false;
#ifndef NDEBUG
  WriteBreadcrumbLocked("attach.succeeded");
#endif
  Log(
      reshade::log::level::info,
      std::format("attached snippet {}", canonical_path.string()));
  return true;
}

inline bool IsSameComObject(IUnknown* lhs, IUnknown* rhs) {
  if (lhs == rhs) return true;
  if (lhs == nullptr || rhs == nullptr) return false;
  IUnknown* lhs_identity = nullptr;
  IUnknown* rhs_identity = nullptr;
  const HRESULT lhs_result = lhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&lhs_identity));
  const HRESULT rhs_result = rhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&rhs_identity));
  const bool same = SUCCEEDED(lhs_result) && SUCCEEDED(rhs_result) && lhs_identity == rhs_identity;
  if (lhs_identity != nullptr) lhs_identity->Release();
  if (rhs_identity != nullptr) rhs_identity->Release();
  return same;
}

inline bool BindDeviceLocked(ID3D12Device* device) {
  if (device == nullptr) return false;
  if (runtime.api.module == nullptr || runtime.failed) {
    Log(reshade::log::level::error, "BindDevice rejected unavailable runtime state");
    return false;
  }
  if (IsSameComObject(runtime.device, device)) return true;
  if (runtime.device != nullptr) {
    if (!runtime.second_device_warning_logged) {
      runtime.second_device_warning_logged = true;
      Log(
          reshade::log::level::warning,
          std::format(
              "BindDevice rejected a distinct live D3D12 device: bound={} requested={}; the process-global Neural "
              "Rendering runtime remains bound to its original device",
              static_cast<void*>(runtime.device),
              static_cast<void*>(device)));
    }
    return false;
  }
  if (!ResolveParameterApiLocked()) return false;
  if (!InitializeParameterProviderLocked(device)) {
    ReleaseParameterProviderModuleLocked(false);
    return false;
  }

#ifndef NDEBUG
  WriteBreadcrumbLocked(std::format(
      "initialize.begin device={}",
      static_cast<void*>(device)));
#endif
  try {
    runtime.last_result = runtime.api.initialize(
        0x876232cULL,
        runtime.snippet_path.parent_path().c_str(),
        device,
        NVSDK_NGX_Version_API,
        nullptr);
  } catch (...) {
    runtime.last_result = NVSDK_NGX_Result_FAIL_PlatformError;
    runtime.failed = true;
  }
#ifndef NDEBUG
  WriteBreadcrumbLocked(std::format(
      "initialize.end result={:#010x}",
      static_cast<uint32_t>(runtime.last_result)));
#endif
  if (NVSDK_NGX_FAILED(runtime.last_result)) {
    Log(
        reshade::log::level::error,
        std::format(
            "Init_Ext failed with {:#010x}",
            static_cast<uint32_t>(runtime.last_result)));
    (void)ShutdownParameterProviderLocked(device);
    ReleaseParameterProviderModuleLocked();
    return false;
  }
  runtime.device = device;
  runtime.device->AddRef();
  runtime.second_device_warning_logged = false;
  runtime.reset_pending = true;
  Log(
      reshade::log::level::info,
      std::format("Init_Ext succeeded for device {}", static_cast<void*>(device)));
  return true;
}

inline bool BindCommandListDeviceLocked(
    ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return false;
  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device)))
      || device == nullptr) {
    return false;
  }
  const bool bound = BindDeviceLocked(device);
  device->Release();
  return bound;
}

inline bool BindDevice(ID3D12Device* device) {
  if (device == nullptr) return false;
  std::scoped_lock lock(runtime.mutex);
  return BindDeviceLocked(device);
}

inline bool CreateFeatureInstance(ID3D12GraphicsCommandList* command_list,
                                  uint32_t width, uint32_t height,
                                  NVSDK_NGX_Handle** feature,
                                  NVSDK_NGX_Parameter** parameters,
                                  NVSDK_NGX_Result* last_result = nullptr) {
  NVSDK_NGX_Result ignored_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (last_result == nullptr) last_result = &ignored_result;
  *last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (command_list == nullptr || width == 0u || height == 0u
      || feature == nullptr || parameters == nullptr) return false;
  *feature = nullptr;
  *parameters = nullptr;

  std::scoped_lock lock(runtime.mutex);
  *last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
  if (!BindCommandListDeviceLocked(command_list) || !ResolveParameterApiLocked()) return false;
  *last_result = runtime.allocate_parameters(parameters);
  if (NVSDK_NGX_FAILED(*last_result)) return false;
  if (*parameters == nullptr) {
    *last_result = NVSDK_NGX_Result_FAIL_PlatformError;
    return false;
  }

  (*parameters)->Set("CreationNodeMask", 1u);
  (*parameters)->Set("VisibilityNodeMask", 1u);
  (*parameters)->Set("Width", width);
  (*parameters)->Set("Height", height);
  (*parameters)->Set("OutWidth", width);
  (*parameters)->Set("OutHeight", height);
  (*parameters)->Set("DLSSNR.Width", width);
  (*parameters)->Set("DLSSNR.Height", height);
  (*parameters)->Set("DLSSNR.InputWidth", width);
  (*parameters)->Set("DLSSNR.InputHeight", height);
  (*parameters)->Set("DLSSNR.OutputWidth", width);
  (*parameters)->Set("DLSSNR.OutputHeight", height);
  (*parameters)->Set("DLSSNR.Output.Width", width);
  (*parameters)->Set("DLSSNR.Output.Height", height);
  (*parameters)->Set("DLSSNR.Hint.Render.Preset", runtime.options.render_preset);
  (*parameters)->Set("PerfQualityValue", static_cast<int32_t>(runtime.options.performance_mode) - 1);
  (*parameters)->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ComputeScalingRatio));
  (*parameters)->Set("DLSSNR.ScalingRatio", 1.f);
  (*parameters)->Set("DLSSNR.Scale", 1.f);
  (*parameters)->Set("DLSSNR.Upscaling", 0);

  *last_result = runtime.api.create_feature(
      command_list, NVSDK_NGX_Feature_Reserved18, *parameters, feature);
  if (NVSDK_NGX_SUCCEED(*last_result) && *feature != nullptr) return true;
  if (NVSDK_NGX_SUCCEED(*last_result)) {
    *last_result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  (void)runtime.destroy_parameters(*parameters);
  *parameters = nullptr;
  *feature = nullptr;
  return false;
}

inline bool ReleaseFeatureInstance(NVSDK_NGX_Handle** feature,
                                   NVSDK_NGX_Parameter** parameters,
                                   NVSDK_NGX_Result* last_result = nullptr) {
  NVSDK_NGX_Result ignored_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (last_result == nullptr) last_result = &ignored_result;
  *last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (feature == nullptr || parameters == nullptr) return false;
  std::scoped_lock lock(runtime.mutex);
  return ReleaseFeatureStateLocked(feature, parameters, last_result);
}

inline bool ReleaseDevice(ID3D12Device* device) {
  if (device == nullptr) return true;
  std::scoped_lock lock(runtime.mutex);
  return !IsSameComObject(runtime.device, device) || ShutdownDeviceLocked();
}

inline bool SetOptions(const Options& options) {
  std::scoped_lock lock(runtime.mutex);
  if (runtime.failed && options.mode != Mode::OFF) return false;
  const bool changed = runtime.options.mode != options.mode
                       || runtime.options.intensity != options.intensity
                       || runtime.options.local_tone_strength != options.local_tone_strength
                       || runtime.options.local_structure_strength
                              != options.local_structure_strength
                       || runtime.options.global_tone_strength != options.global_tone_strength
                       || runtime.options.style != options.style
                       || runtime.options.auto_mask != options.auto_mask
                       || runtime.options.skin_structure_strength
                              != options.skin_structure_strength
                       || runtime.options.performance_mode != options.performance_mode
                       || runtime.options.render_preset != options.render_preset;
  runtime.options = options;
  runtime.reset_pending = runtime.reset_pending || changed;
  if (changed) {
    for (auto& state : runtime.pass_features) {
      state.reset_pending = true;
    }
    ++runtime.options_revision;
#ifndef NDEBUG
    Log(
        reshade::log::level::debug,
        std::format(
            "options revision {}: mode={} intensity={:.3f} local_tone={:.3f} "
            "local_structure={:.3f} global_tone={:.3f} style={} auto_mask={} "
            "skin_structure={:.3f} performance={} preset={}",
            runtime.options_revision,
            static_cast<uint32_t>(options.mode),
            options.intensity,
            options.local_tone_strength,
            options.local_structure_strength,
            options.global_tone_strength,
            options.style,
            options.auto_mask == sl::Boolean::eTrue,
            options.skin_structure_strength,
            static_cast<uint32_t>(options.performance_mode),
            options.render_preset));
#endif
  }
  return runtime.api.module != nullptr;
}

inline void SetUiCorrection(bool enabled) {
  std::scoped_lock lock(runtime.mutex);
  if (runtime.ui_correction_requested == enabled) return;
  runtime.ui_correction_requested = enabled;
  runtime.ui_correction_active = false;
  runtime.reset_pending = true;
  for (auto& state : runtime.pass_features) {
    state.ui_correction_active = false;
    state.reset_pending = true;
  }
  ++runtime.options_revision;
}

inline ID3D12Resource* GetPrivateOutputLocked(
    ID3D12Resource* input, uint32_t width, uint32_t height,
    D3D12_RESOURCE_STATES* state = nullptr) {
  if (input == nullptr || runtime.device == nullptr) return nullptr;
  const std::scoped_lock private_outputs_lock(runtime.private_outputs_mutex);
  auto desc = input->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || desc.Format == DXGI_FORMAT_UNKNOWN
      || desc.SampleDesc.Count != 1u
      || width > desc.Width
      || height > desc.Height) {
    return nullptr;
  }
  desc.Alignment = 0u;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.SampleDesc = {1u, 0u};
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
      (static_cast<uint32_t>(desc.Flags)
       & ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
      | static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
  const auto same_desc = [&desc](const D3D12_RESOURCE_DESC& candidate) {
    return candidate.Dimension == desc.Dimension
           && candidate.Alignment == desc.Alignment
           && candidate.Width == desc.Width
           && candidate.Height == desc.Height
           && candidate.DepthOrArraySize == desc.DepthOrArraySize
           && candidate.MipLevels == desc.MipLevels
           && candidate.Format == desc.Format
           && candidate.SampleDesc.Count == desc.SampleDesc.Count
           && candidate.SampleDesc.Quality == desc.SampleDesc.Quality
           && candidate.Layout == desc.Layout
           && candidate.Flags == desc.Flags;
  };
  const auto existing = std::ranges::find_if(
      runtime.private_outputs,
      [input, &same_desc](
          const PrivateOutput& output) {
        return output.input == input
               && same_desc(output.desc);
      });
  if (existing != runtime.private_outputs.end()) {
    if (state != nullptr) *state = existing->state;
    return existing->resource;
  }

  const D3D12_HEAP_PROPERTIES heap_properties = {
      .Type = D3D12_HEAP_TYPE_DEFAULT,
      .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
      .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
      .CreationNodeMask = 1u,
      .VisibleNodeMask = 1u,
  };
  ID3D12Resource* output = nullptr;
  const HRESULT result = runtime.device->CreateCommittedResource(
      &heap_properties,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr,
      IID_PPV_ARGS(&output));
  if (FAILED(result) || output == nullptr) {
    runtime.failed = true;
    runtime.last_result = NVSDK_NGX_Result_FAIL_PlatformError;
    Log(
        reshade::log::level::error,
        std::format(
            "private output creation failed: hr={:#010x} size={}x{} format={} flags={:#x}",
            static_cast<uint32_t>(result),
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(desc.Flags)));
    return nullptr;
  }
  output->SetName(L"RenoDX DLSS-NR private output");
  runtime.private_outputs.push_back({
      .input = input,
      .resource = output,
      .desc = desc,
      .state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
  });
  if (state != nullptr) *state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Log(
      reshade::log::level::info,
      std::format(
          "created private output {}: resource={} input={} size={}x{} format={} flags={:#x}",
          runtime.private_outputs.size(),
          static_cast<void*>(output),
          static_cast<void*>(input),
          width,
          height,
          static_cast<uint32_t>(desc.Format),
          static_cast<uint32_t>(desc.Flags)));
  return output;
}

inline void SetPrivateOutputStateLocked(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
  if (resource == nullptr) return;
  const std::scoped_lock private_outputs_lock(runtime.private_outputs_mutex);
  const auto existing = std::ranges::find(runtime.private_outputs, resource, &PrivateOutput::resource);
  if (existing != runtime.private_outputs.end()) existing->state = state;
}

inline ResourceSubrect ResolveSubrect(
    const ResourceSubrect& subrect,
    uint32_t fallback_width,
    uint32_t fallback_height) {
  return {
      .base_x = subrect.base_x,
      .base_y = subrect.base_y,
      .width = subrect.width == 0u ? fallback_width : subrect.width,
      .height = subrect.height == 0u ? fallback_height : subrect.height,
  };
}

inline bool IsValidSubrect(
    ID3D12Resource* resource,
    const ResourceSubrect& subrect) {
  if (resource == nullptr || subrect.width == 0u || subrect.height == 0u) {
    return false;
  }
  const auto desc = resource->GetDesc();
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
         && desc.SampleDesc.Count == 1u
         && subrect.base_x <= desc.Width
         && subrect.base_y <= desc.Height
         && subrect.width <= desc.Width - subrect.base_x
         && subrect.height <= desc.Height - subrect.base_y;
}

// Cache pressure is not permission to release GPU-live or replayable work.
inline void RetireCompletedFeaturesLocked() {
  if (runtime.can_retire_feature == nullptr) return;
  for (auto it = runtime.retained_features.begin(); it != runtime.retained_features.end();) {
    if (it->feature != nullptr && runtime.can_retire_feature(it->feature)) {
      (void)ReleaseFeatureStateLocked(&it->feature, &it->parameters);
      it = runtime.retained_features.erase(it);
    } else {
      ++it;
    }
  }
}

inline bool EnsureFeatureLocked(
    ID3D12GraphicsCommandList* command_list,
    uint32_t width,
    uint32_t height) {
  // Retired resolutions must drain even when the active resolution stays fixed.
  // The callback still excludes GPU-live, replayable and untracked feature uses.
  RetireCompletedFeaturesLocked();
  if (runtime.feature != nullptr
      && runtime.feature_width == width
      && runtime.feature_height == height
      && runtime.feature_performance_mode == runtime.options.performance_mode
      && runtime.feature_render_preset == runtime.options.render_preset) {
    return true;
  }

  const auto retained = std::ranges::find_if(
      runtime.retained_features,
      [width, height](const FeatureState& state) {
        return state.reusable
               && state.width == width
               && state.height == height
               && state.performance_mode == runtime.options.performance_mode
               && state.render_preset == runtime.options.render_preset;
      });
  if (retained != runtime.retained_features.end()) {
    const FeatureState next = *retained;
    runtime.retained_features.erase(retained);
    if (runtime.feature != nullptr || runtime.parameters != nullptr) {
      runtime.retained_features.push_back({
          .parameters = runtime.parameters,
          .feature = runtime.feature,
          .width = runtime.feature_width,
          .height = runtime.feature_height,
          .performance_mode = runtime.feature_performance_mode,
          .render_preset = runtime.feature_render_preset,
          .evaluated_options_revision =
              runtime.evaluated_options_revision,
          .reset_pending = runtime.reset_pending,
          .ui_correction_active = runtime.ui_correction_active,
      });
    }
    runtime.parameters = next.parameters;
    runtime.feature = next.feature;
    runtime.feature_width = next.width;
    runtime.feature_height = next.height;
    runtime.feature_performance_mode = next.performance_mode;
    runtime.feature_render_preset = next.render_preset;
    runtime.evaluated_options_revision =
        next.evaluated_options_revision;
    runtime.ui_correction_active = next.ui_correction_active;
    runtime.reset_pending = true;
    Log(
        reshade::log::level::info,
        std::format(
            "reused retained feature: handle={} size={}x{} performance={} preset={}",
            static_cast<void*>(runtime.feature),
            width,
            height,
            static_cast<uint32_t>(runtime.feature_performance_mode),
            runtime.feature_render_preset));
    return true;
  }

  if (runtime.retained_features.size() >= MAX_RETAINED_FEATURES) {
    if (!runtime.feature_cache_exhaustion_reported) Log(
        reshade::log::level::warning,
        std::format(
            "retained feature cache exhausted at {} entries; preserving the native path",
            MAX_RETAINED_FEATURES));
    runtime.feature_cache_exhaustion_reported = true;
    return false;
  }
  runtime.feature_cache_exhaustion_reported = false;

  if (runtime.feature != nullptr || runtime.parameters != nullptr) {
    // ReleaseFeature is forbidden while a command list that evaluated this
    // feature may still be in flight. Keep the complete state reusable until
    // an explicit, GPU-idle release boundary.
    runtime.retained_features.push_back({
        .parameters = runtime.parameters,
        .feature = runtime.feature,
        .width = runtime.feature_width,
        .height = runtime.feature_height,
        .performance_mode = runtime.feature_performance_mode,
        .render_preset = runtime.feature_render_preset,
        .evaluated_options_revision =
            runtime.evaluated_options_revision,
        .reset_pending = runtime.reset_pending,
        .ui_correction_active = runtime.ui_correction_active,
    });
    Log(
        reshade::log::level::debug,
        std::format(
            "retained feature {} for safe reuse; retained count={}",
            static_cast<void*>(runtime.feature),
            runtime.retained_features.size()));
    runtime.parameters = nullptr;
    runtime.feature = nullptr;
    runtime.feature_width = 0u;
    runtime.feature_height = 0u;
    runtime.evaluated_options_revision = 0u;
    runtime.reset_pending = true;
    runtime.ui_correction_active = false;
  }
  try {
    if (!ResolveParameterApiLocked()) return false;
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "create.allocate_parameters.begin size={}x{} performance={} preset={}",
        width,
        height,
        static_cast<uint32_t>(runtime.options.performance_mode),
        runtime.options.render_preset));
#endif
    runtime.last_result = runtime.allocate_parameters(&runtime.parameters);
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "create.allocate_parameters.end result={:#010x} parameters={}",
        static_cast<uint32_t>(runtime.last_result),
        static_cast<void*>(runtime.parameters)));
#endif
    if (NVSDK_NGX_FAILED(runtime.last_result) || runtime.parameters == nullptr) {
      Log(
          reshade::log::level::error,
          std::format(
              "AllocateParameters failed with {:#010x}",
              static_cast<uint32_t>(runtime.last_result)));
      runtime.parameters = nullptr;
      return false;
    }

#ifndef NDEBUG
    WriteBreadcrumbLocked("create.set_parameters.begin");
#endif
    runtime.parameters->Set("CreationNodeMask", 1u);
    runtime.parameters->Set("VisibilityNodeMask", 1u);
    runtime.parameters->Set("Width", width);
    runtime.parameters->Set("Height", height);
    runtime.parameters->Set("OutWidth", width);
    runtime.parameters->Set("OutHeight", height);
    runtime.parameters->Set("DLSSNR.Width", width);
    runtime.parameters->Set("DLSSNR.Height", height);
    runtime.parameters->Set("DLSSNR.InputWidth", width);
    runtime.parameters->Set("DLSSNR.InputHeight", height);
    runtime.parameters->Set("DLSSNR.OutputWidth", width);
    runtime.parameters->Set("DLSSNR.OutputHeight", height);
    runtime.parameters->Set("DLSSNR.Output.Width", width);
    runtime.parameters->Set("DLSSNR.Output.Height", height);
    runtime.parameters->Set(
        "DLSSNR.Hint.Render.Preset",
        runtime.options.render_preset);
    runtime.parameters->Set(
        "PerfQualityValue",
        static_cast<int32_t>(runtime.options.performance_mode) - 1);
    runtime.parameters->Set(
        "DLSSNRComputeScalingRatioCallback",
        reinterpret_cast<void*>(&ComputeScalingRatio));
    runtime.parameters->Set("DLSSNR.ScalingRatio", 1.f);
    runtime.parameters->Set("DLSSNR.Scale", 1.f);
    runtime.parameters->Set("DLSSNR.Upscaling", 0);
#ifndef NDEBUG
    WriteBreadcrumbLocked("create.set_parameters.end");
    WriteBreadcrumbLocked(std::format(
        "create_feature.begin command_list={}",
        static_cast<void*>(command_list)));
#endif
    runtime.last_result = runtime.api.create_feature(
        command_list,
        NVSDK_NGX_Feature_Reserved18,
        runtime.parameters,
        &runtime.feature);
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "create_feature.end result={:#010x} handle={}",
        static_cast<uint32_t>(runtime.last_result),
        static_cast<void*>(runtime.feature)));
#endif
  } catch (...) {
    runtime.last_result = NVSDK_NGX_Result_FAIL_PlatformError;
    runtime.failed = true;
    Log(
        reshade::log::level::error,
        "signed snippet threw during feature creation");
  }
  if (runtime.feature != nullptr && runtime.record_feature != nullptr) {
    runtime.record_feature(runtime.feature, command_list);
  }
  if (NVSDK_NGX_FAILED(runtime.last_result) || runtime.feature == nullptr) {
    Log(
        reshade::log::level::error,
        std::format(
            "CreateFeature(Reserved18) failed with {:#010x}, handle={}",
            static_cast<uint32_t>(runtime.last_result),
            static_cast<void*>(runtime.feature)));
    if (runtime.feature != nullptr) {
      runtime.retained_features.push_back({
          .parameters = runtime.parameters,
          .feature = runtime.feature,
          .width = width,
          .height = height,
          .performance_mode = runtime.options.performance_mode,
          .render_preset = runtime.options.render_preset,
          .evaluated_options_revision =
              runtime.evaluated_options_revision,
          .reset_pending = runtime.reset_pending,
          .ui_correction_active = runtime.ui_correction_active,
          .reusable = false,
      });
    } else if (runtime.parameters != nullptr
               && runtime.destroy_parameters != nullptr) {
      (void)runtime.destroy_parameters(runtime.parameters);
    }
    runtime.feature = nullptr;
    runtime.parameters = nullptr;
    runtime.evaluated_options_revision = 0u;
    runtime.reset_pending = true;
    runtime.ui_correction_active = false;
    return false;
  }
  runtime.feature_width = width;
  runtime.feature_height = height;
  runtime.feature_performance_mode = runtime.options.performance_mode;
  runtime.feature_render_preset = runtime.options.render_preset;
  runtime.reset_pending = true;
  Log(
      reshade::log::level::info,
      std::format(
          "CreateFeature(Reserved18) succeeded: handle={} size={}x{} performance={} preset={}",
          static_cast<void*>(runtime.feature),
          width,
          height,
          static_cast<uint32_t>(runtime.options.performance_mode),
          runtime.options.render_preset));
  return true;
}

inline bool EvaluateLocked(
    const Submission& submission,
    uint64_t* recording_sequence = nullptr) {
  if (recording_sequence != nullptr) *recording_sequence = 0u;
  if (submission.command_list == nullptr
      || submission.input == nullptr
      || submission.output == nullptr
      || submission.motion == nullptr
      || submission.depth == nullptr
      || submission.width == 0u
      || submission.height == 0u) {
    return false;
  }
  const ResourceSubrect input_subrect = ResolveSubrect(
      submission.input_subrect,
      submission.width,
      submission.height);
  const ResourceSubrect output_subrect = ResolveSubrect(
      submission.output_subrect,
      submission.width,
      submission.height);
  const ResourceSubrect motion_subrect = ResolveSubrect(
      submission.motion_subrect,
      submission.width,
      submission.height);
  const ResourceSubrect depth_subrect = ResolveSubrect(
      submission.depth_subrect,
      submission.width,
      submission.height);
  if (!IsValidSubrect(submission.input, input_subrect)
      || !IsValidSubrect(submission.output, output_subrect)
      || !IsValidSubrect(submission.motion, motion_subrect)
      || !IsValidSubrect(submission.depth, depth_subrect)
      || submission.feature_slot >= MAX_FEATURE_SLOTS) {
    return false;
  }
  ScopedFeatureSlot feature_scope(submission.feature_slot);
  if (runtime.device == nullptr
      || runtime.failed
      || runtime.options.mode == Mode::OFF
      || !EnsureFeatureLocked(
          submission.command_list,
          submission.width,
          submission.height)) {
    return false;
  }

  const bool transition_output = submission.output_state
                                 != D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const bool use_ui_correction =
      submission.ui_correction == UiCorrectionMode::ENABLED
      || (submission.ui_correction == UiCorrectionMode::INHERIT
          && runtime.ui_correction_requested);
  const bool use_ui_resources = submission.ui != nullptr
                                && submission.ui_alpha != nullptr;
  if (runtime.ui_correction_active != use_ui_correction) {
    runtime.reset_pending = true;
    ++runtime.options_revision;
  }
  if (transition_output) {
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = submission.output,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = submission.output_state,
            .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        },
    };
    submission.command_list->ResourceBarrier(1u, &barrier);
  }

  try {
#ifndef NDEBUG
    WriteBreadcrumbLocked(std::format(
        "evaluate.begin next={} size={}x{} reset={} input={} output={} motion={} depth={}",
        runtime.evaluation_count + 1u,
        submission.width,
        submission.height,
        runtime.reset_pending || submission.reset,
        static_cast<void*>(submission.input),
        static_cast<void*>(submission.output),
        static_cast<void*>(submission.motion),
        static_cast<void*>(submission.depth)));
    WriteBreadcrumbLocked("evaluate.set_parameters.begin");
#endif
    runtime.parameters->Set("Width", submission.width);
    runtime.parameters->Set("Height", submission.height);
    runtime.parameters->Set("OutWidth", submission.width);
    runtime.parameters->Set("OutHeight", submission.height);
    runtime.parameters->Set("DLSSNR.Width", submission.width);
    runtime.parameters->Set("DLSSNR.Height", submission.height);
    runtime.parameters->Set("DLSSNR.InputWidth", input_subrect.width);
    runtime.parameters->Set("DLSSNR.InputHeight", input_subrect.height);
    runtime.parameters->Set("DLSSNR.OutputWidth", output_subrect.width);
    runtime.parameters->Set("DLSSNR.OutputHeight", output_subrect.height);
    runtime.parameters->Set("DLSSNR.Output.Width", output_subrect.width);
    runtime.parameters->Set("DLSSNR.Output.Height", output_subrect.height);
    runtime.parameters->Set("DLSSNR.Color", submission.input);
    runtime.parameters->Set("DLSSNR.Output", submission.output);
    runtime.parameters->Set("DLSSNR.MVec", submission.motion);
    runtime.parameters->Set("DLSSNR.Depth", submission.depth);
    if (use_ui_resources) {
      runtime.parameters->Set("DLSSNR.UI", submission.ui);
      runtime.parameters->Set("DLSSNR.UIAlpha", submission.ui_alpha);
      for (const char* prefix : {"UI", "UIAlpha"}) {
        const std::string base = std::string("DLSSNR.") + prefix + "Subrect";
        runtime.parameters->Set((base + "BaseX").c_str(), 0u);
        runtime.parameters->Set((base + "BaseY").c_str(), 0u);
        runtime.parameters->Set((base + "Width").c_str(), submission.width);
        runtime.parameters->Set((base + "Height").c_str(), submission.height);
      }
    } else {
      runtime.parameters->Set(
          "DLSSNR.UI",
          static_cast<ID3D12Resource*>(nullptr));
      runtime.parameters->Set(
          "DLSSNR.UIAlpha",
          static_cast<ID3D12Resource*>(nullptr));
    }
    const auto set_subrect = [](
                                 const char* prefix,
                                 const ResourceSubrect& subrect) {
      const std::string base = std::string("DLSSNR.") + prefix + "Subrect";
      runtime.parameters->Set((base + "BaseX").c_str(), subrect.base_x);
      runtime.parameters->Set((base + "BaseY").c_str(), subrect.base_y);
      runtime.parameters->Set((base + "Width").c_str(), subrect.width);
      runtime.parameters->Set((base + "Height").c_str(), subrect.height);
    };
    set_subrect("Color", input_subrect);
    set_subrect("MVec", motion_subrect);
    set_subrect("Depth", depth_subrect);
    set_subrect("Output", output_subrect);
    runtime.parameters->Set("Jitter.Offset.X", submission.jitter_x);
    runtime.parameters->Set("Jitter.Offset.Y", submission.jitter_y);
    runtime.parameters->Set("DLSSNR.JitterOffsetX", submission.jitter_x);
    runtime.parameters->Set("DLSSNR.JitterOffsetY", submission.jitter_y);
    runtime.parameters->Set("DLSSNR.MVecScaleX", submission.motion_scale_x);
    runtime.parameters->Set("DLSSNR.MVecScaleY", submission.motion_scale_y);
    runtime.parameters->Set(
        "DLSSNR.DepthInverted",
        submission.depth_inverted ? 1 : 0);
    runtime.parameters->Set("DLSSNR.Enabled", 1);
    runtime.parameters->Set(
        "DLSSNR.Reset",
        runtime.reset_pending || submission.reset ? 1 : 0);
    runtime.parameters->Set("DLSSNR.Intensity", runtime.options.intensity);
    runtime.parameters->Set(
        "DLSSNR.LocalToneStrength",
        runtime.options.local_tone_strength);
    runtime.parameters->Set(
        "DLSSNR.LocalStructureStrength",
        runtime.options.local_structure_strength);
    runtime.parameters->Set(
        "DLSSNR.GlobalToneStrength",
        runtime.options.global_tone_strength);
    runtime.parameters->Set(
        "DLSSNR.UseAutoMask",
        runtime.options.auto_mask == sl::Boolean::eTrue ? 1 : 0);
    runtime.parameters->Set(
        "DLSSNR.SkinStructureStrength",
        runtime.options.skin_structure_strength);
    runtime.parameters->Set("DLSSNR.Style", runtime.options.style);
    runtime.parameters->Set(
        "DLSSNR.UICorrection",
        use_ui_correction ? 1 : 0);
    runtime.ui_correction_active = use_ui_correction;
    runtime.parameters->Set("DLSS.Indicator.Invert.X.Axis", 0);
    runtime.parameters->Set("DLSS.Indicator.Invert.Y.Axis", 0);
#ifndef NDEBUG
    WriteBreadcrumbLocked("evaluate.set_parameters.end");
#endif
#ifndef NDEBUG
    if (runtime.evaluated_options_revision != runtime.options_revision) {
      float intensity = 0.f;
      float local_tone = 0.f;
      float local_structure = 0.f;
      float skin_structure = 0.f;
      unsigned int style = 0u;
      int auto_mask = 0;
      const auto intensity_result = runtime.parameters->Get(
          "DLSSNR.Intensity", &intensity);
      const auto local_tone_result = runtime.parameters->Get(
          "DLSSNR.LocalToneStrength", &local_tone);
      const auto local_structure_result = runtime.parameters->Get(
          "DLSSNR.LocalStructureStrength", &local_structure);
      const auto skin_structure_result = runtime.parameters->Get(
          "DLSSNR.SkinStructureStrength", &skin_structure);
      const auto style_result = runtime.parameters->Get("DLSSNR.Style", &style);
      const auto auto_mask_result = runtime.parameters->Get(
          "DLSSNR.UseAutoMask", &auto_mask);
      Log(
          reshade::log::level::debug,
          std::format(
              "parameter round-trip revision {}: intensity={:.3f}/{:#010x} "
              "local_tone={:.3f}/{:#010x} local_structure={:.3f}/{:#010x} "
              "skin_structure={:.3f}/{:#010x} style={}/{:#010x} "
              "auto_mask={}/{:#010x}",
              runtime.options_revision,
              intensity,
              static_cast<uint32_t>(intensity_result),
              local_tone,
              static_cast<uint32_t>(local_tone_result),
              local_structure,
              static_cast<uint32_t>(local_structure_result),
              skin_structure,
              static_cast<uint32_t>(skin_structure_result),
              style,
              static_cast<uint32_t>(style_result),
              auto_mask,
              static_cast<uint32_t>(auto_mask_result)));
    }
#endif
#ifndef NDEBUG
    constexpr char marker[] = "RenoDX DLSS-NR EvaluateFeature begin";
    submission.command_list->SetMarker(0u, marker, sizeof(marker) - 1u);
    WriteBreadcrumbLocked("evaluate_feature.begin");
#endif
    if (recording_sequence != nullptr) {
      *recording_sequence = ++runtime.recording_sequence;
    }
    if (runtime.record_feature != nullptr) runtime.record_feature(runtime.feature, submission.command_list);
    runtime.last_result = runtime.api.evaluate_feature(
        submission.command_list,
        runtime.feature,
        runtime.parameters,
        nullptr);
#ifndef NDEBUG
    constexpr char completed_marker[] = "RenoDX DLSS-NR EvaluateFeature end";
    submission.command_list->SetMarker(
        0u,
        completed_marker,
        sizeof(completed_marker) - 1u);
    WriteBreadcrumbLocked(std::format(
        "evaluate_feature.end result={:#010x}",
        static_cast<uint32_t>(runtime.last_result)));
#endif
  } catch (...) {
    runtime.last_result = NVSDK_NGX_Result_FAIL_PlatformError;
    runtime.failed = true;
    Log(
        reshade::log::level::error,
        "signed snippet threw during feature evaluation");
  }
  if (transition_output) {
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = submission.output,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            .StateAfter = submission.output_state,
        },
    };
    submission.command_list->ResourceBarrier(1u, &barrier);
  }
  const bool succeeded = NVSDK_NGX_SUCCEED(runtime.last_result);
  runtime.reset_pending = !succeeded;
#ifndef NDEBUG
  WriteBreadcrumbLocked(std::format(
      "evaluate.end succeeded={} result={:#010x}",
      succeeded,
      static_cast<uint32_t>(runtime.last_result)));
#endif
  if (succeeded) {
    ++runtime.evaluation_count;
    if (runtime.evaluation_count == 1u
        || runtime.evaluated_options_revision != runtime.options_revision) {
      runtime.evaluated_options_revision = runtime.options_revision;
      Log(
          reshade::log::level::info,
          std::format(
              "EvaluateFeature succeeded: evaluation={} options_revision={} result={:#010x}",
              runtime.evaluation_count,
              runtime.options_revision,
              static_cast<uint32_t>(runtime.last_result)));
    }
  } else {
    Log(
        reshade::log::level::error,
        std::format(
            "EvaluateFeature failed with {:#010x} after {} successful evaluations",
            static_cast<uint32_t>(runtime.last_result),
            runtime.evaluation_count));
  }
  return succeeded;
}

inline bool Evaluate(const Submission& submission) {
  std::scoped_lock lock(runtime.mutex);
  if (!BindCommandListDeviceLocked(submission.command_list)) return false;
  return EvaluateLocked(submission);
}

inline bool EvaluateToPrivateOutput(
    const PrivateOutputSubmission& submission,
    ID3D12Resource** evaluated_output = nullptr,
    uint64_t* recording_sequence = nullptr) {
  if (evaluated_output != nullptr) *evaluated_output = nullptr;
  if (recording_sequence != nullptr) *recording_sequence = 0u;
  if (submission.command_list == nullptr
      || submission.input == nullptr
      || submission.motion == nullptr
      || submission.depth == nullptr
      || submission.width == 0u
      || submission.height == 0u
      || submission.feature_slot >= MAX_FEATURE_SLOTS) {
    return false;
  }
  std::scoped_lock lock(runtime.mutex);
  if (submission.device != nullptr) {
    if (!BindDeviceLocked(submission.device)) return false;
  } else if (!BindCommandListDeviceLocked(submission.command_list)) {
    return false;
  }
  D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* const output = GetPrivateOutputLocked(
      submission.input,
      submission.width,
      submission.height,
      &output_state);
  if (output == nullptr) return false;
  const bool evaluated = EvaluateLocked({
                                            .command_list = submission.command_list,
                                            .feature_slot = submission.feature_slot,
                                            .input = submission.input,
                                            .output = output,
                                            .motion = submission.motion,
                                            .depth = submission.depth,
                                            .output_state = output_state,
                                            .width = submission.width,
                                            .height = submission.height,
                                            .jitter_x = submission.jitter_x,
                                            .jitter_y = submission.jitter_y,
                                            .motion_scale_x = submission.motion_scale_x,
                                            .motion_scale_y = submission.motion_scale_y,
                                            .depth_inverted = submission.depth_inverted,
                                            .reset = submission.reset,
                                            .input_subrect = submission.input_subrect,
                                            .output_subrect = {
                                                .width = submission.width,
                                                .height = submission.height,
                                            },
                                            .motion_subrect = submission.motion_subrect,
                                            .depth_subrect = submission.depth_subrect,
                                            .ui_correction = submission.ui_correction,
                                        },
                                        recording_sequence);
  if (evaluated && output_state != submission.output_state_after_evaluate) {
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = output,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = output_state,
            .StateAfter = submission.output_state_after_evaluate,
        },
    };
    submission.command_list->ResourceBarrier(1u, &barrier);
    output_state = submission.output_state_after_evaluate;
  }
  SetPrivateOutputStateLocked(output, output_state);
  if (evaluated && evaluated_output != nullptr) *evaluated_output = output;
  return evaluated;
}

inline bool EvaluateReconstructedOutput(const ReplacementSubmission& submission, bool replace_output = true) {
  if (submission.command_list == nullptr
      || submission.game_output == nullptr
      || submission.motion == nullptr
      || submission.depth == nullptr
      || submission.output_subrect.width == 0u
      || submission.output_subrect.height == 0u) {
    return false;
  }
  std::scoped_lock lock(runtime.mutex);
  if (!BindCommandListDeviceLocked(submission.command_list)) return false;
  if (!IsValidSubrect(submission.game_output, submission.output_subrect)
      || !IsValidSubrect(submission.motion, submission.motion_subrect)
      || !IsValidSubrect(submission.depth, submission.depth_subrect)) {
    return false;
  }
  D3D12_RESOURCE_STATES private_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* const private_output = GetPrivateOutputLocked(
      submission.game_output,
      submission.output_subrect.width,
      submission.output_subrect.height,
      &private_output_state);
  if (private_output == nullptr) return false;

  if (!runtime.reconstruction_contract_logged) {
    const auto game_desc = submission.game_output->GetDesc();
    runtime.reconstruction_contract_logged = true;
    Log(
        reshade::log::level::info,
        std::format(
            "post-reconstruction contract: game_output={} resource={}x{} subrect={},{} {}x{} format={} motion={}x{} depth={}x{}",
            static_cast<void*>(submission.game_output),
            game_desc.Width,
            game_desc.Height,
            submission.output_subrect.base_x,
            submission.output_subrect.base_y,
            submission.output_subrect.width,
            submission.output_subrect.height,
            static_cast<uint32_t>(game_desc.Format),
            submission.motion_subrect.width,
            submission.motion_subrect.height,
            submission.depth_subrect.width,
            submission.depth_subrect.height));
  }

  const D3D12_RESOURCE_BARRIER input_barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
          .pResource = submission.game_output,
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          .StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      },
  };
  submission.command_list->ResourceBarrier(1u, &input_barrier);
  const bool evaluated = EvaluateLocked({
      .command_list = submission.command_list,
      .input = submission.game_output,
      .output = private_output,
      .motion = submission.motion,
      .depth = submission.depth,
      .output_state = private_output_state,
      .width = submission.output_subrect.width,
      .height = submission.output_subrect.height,
      .jitter_x = submission.jitter_x,
      .jitter_y = submission.jitter_y,
      .motion_scale_x = submission.motion_scale_x,
      .motion_scale_y = submission.motion_scale_y,
      .depth_inverted = submission.depth_inverted,
      .reset = submission.reset,
      .input_subrect = submission.output_subrect,
      .output_subrect = {
          .width = submission.output_subrect.width,
          .height = submission.output_subrect.height,
      },
      .motion_subrect = submission.motion_subrect,
      .depth_subrect = submission.depth_subrect,
  });
  if (!evaluated || !replace_output) {
    const D3D12_RESOURCE_BARRIER restore_barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = submission.game_output,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        },
    };
    submission.command_list->ResourceBarrier(1u, &restore_barrier);
    return evaluated;
  }

  const std::array copy_barriers = {
      D3D12_RESOURCE_BARRIER{
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
              .pResource = private_output,
              .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              .StateBefore = private_output_state,
              .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
          },
      },
      D3D12_RESOURCE_BARRIER{
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
              .pResource = submission.game_output,
              .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              .StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
          },
      },
  };
  submission.command_list->ResourceBarrier(
      static_cast<uint32_t>(copy_barriers.size()),
      copy_barriers.data());
  const D3D12_TEXTURE_COPY_LOCATION source = {
      .pResource = private_output,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = 0u,
  };
  const D3D12_TEXTURE_COPY_LOCATION destination = {
      .pResource = submission.game_output,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = 0u,
  };
  submission.command_list->CopyTextureRegion(
      &destination,
      submission.output_subrect.base_x,
      submission.output_subrect.base_y,
      0u,
      &source,
      nullptr);
  const std::array restore_barriers = {
      D3D12_RESOURCE_BARRIER{
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
              .pResource = private_output,
              .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
              .StateAfter = private_output_state,
          },
      },
      D3D12_RESOURCE_BARRIER{
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
              .pResource = submission.game_output,
              .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
              .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          },
      },
  };
  submission.command_list->ResourceBarrier(
      static_cast<uint32_t>(restore_barriers.size()),
      restore_barriers.data());
  SetPrivateOutputStateLocked(private_output, private_output_state);
  ++runtime.replacement_count;
  if (runtime.replacement_count == 1u) {
    Log(
        reshade::log::level::info,
        "first game reconstruction output replacement recorded successfully");
  }
  return true;
}

inline bool ReleaseResources() {
  std::scoped_lock lock(runtime.mutex);
  return ReleaseResourcesLocked();
}

inline size_t ReleasePrivateOutputsForInput(ID3D12Resource* input) {
  if (input == nullptr || releasing_private_outputs) return 0u;
  std::vector<ID3D12Resource*> resources;
  size_t remaining = 0u;
  {
    const std::scoped_lock lock(runtime.private_outputs_mutex);
    std::erase_if(
        runtime.private_outputs,
        [input, &resources](const PrivateOutput& output) {
          if (output.input != input) return false;
          resources.push_back(output.resource);
          return true;
        });
    remaining = runtime.private_outputs.size();
  }
  for (auto* resource : resources) {
    resource->Release();
  }
  if (!resources.empty()) {
    Log(
        reshade::log::level::info,
        std::format(
            "retired {} private output(s) for input {}; remaining={}",
            resources.size(),
            static_cast<void*>(input),
            remaining));
  }
  return resources.size();
}

inline bool IsFailed() {
  std::scoped_lock lock(runtime.mutex);
  return runtime.failed;
}

inline Status CopyStatusLocked() {
  const std::scoped_lock private_outputs_lock(runtime.private_outputs_mutex);
  return {
      .snippet_path = runtime.snippet_path,
      .last_result = runtime.last_result,
      .evaluation_count = runtime.evaluation_count,
      .options_revision = runtime.options_revision,
      .attached = runtime.api.module != nullptr,
      .device_bound = runtime.device != nullptr,
      .feature_created = runtime.feature != nullptr,
      .failed = runtime.failed,
      .reset_pending = runtime.reset_pending,
      .ui_correction_requested = runtime.ui_correction_requested,
      .ui_correction_active = runtime.ui_correction_active,
      .private_output_count = runtime.private_outputs.size(),
      .retained_feature_count = runtime.retained_features.size(),
      .replacement_count = runtime.replacement_count,
      .parameter_provider_path = runtime.parameter_provider_path,
      .parameter_provider_loaded = runtime.parameter_module != nullptr,
      .parameter_provider_force_loaded =
          runtime.parameter_provider_force_loaded,
      .parameter_provider_initialized =
          runtime.parameter_provider_initialized,
      .parameter_provider_force_requested =
          runtime.parameter_provider_config.load_if_missing,
      .parameter_provider_load_attempted =
          runtime.parameter_provider_load_attempted,
  };
}

inline Status CopyStatus() {
  std::shared_lock lock(runtime.mutex);
  return CopyStatusLocked();
}

inline bool TryCopyStatus(Status* status) {
  if (status == nullptr) return false;
  std::shared_lock lock(runtime.mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  *status = CopyStatusLocked();
  return true;
}

inline bool Reset() {
  std::scoped_lock lock(runtime.mutex);
  runtime.failed = false;
  runtime.ui_correction_active = false;
  for (auto& state : runtime.pass_features) {
    state.ui_correction_active = false;
    state.reset_pending = true;
  }
  runtime.last_result = runtime.api.module != nullptr
                            ? NVSDK_NGX_Result_Success
                            : NVSDK_NGX_Result_FAIL_NotInitialized;
  runtime.reset_pending = true;
  return runtime.api.module != nullptr;
}

inline bool Detach() {
  std::scoped_lock lock(runtime.mutex);
  if (runtime.api.module == nullptr) {
    ReleaseParameterProviderModuleLocked();
    return true;
  }
  const bool shutdown = ShutdownDeviceLocked();
  if (!RestoreCallerIdentityLocked()) return false;
  FreeLibrary(runtime.api.module);
  runtime.api = {};
  runtime.snippet_path.clear();
#ifndef NDEBUG
  WriteBreadcrumbLocked("detach.module_unloaded");
  runtime.breadcrumb_path.clear();
  runtime.breadcrumb_sequence = 0u;
#endif
  ReleaseParameterProviderModuleLocked();
  runtime.options = {};
  runtime.last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
  const uint64_t evaluation_count = runtime.evaluation_count;
  runtime.options_revision = 0u;
  runtime.evaluated_options_revision = 0u;
  runtime.evaluation_count = 0u;
  runtime.failed = false;
  runtime.ui_correction_requested = false;
  runtime.ui_correction_active = false;
  Log(
      reshade::log::level::info,
      std::format(
          "detached signed snippet after {} successful evaluations",
          evaluation_count));
  return shutdown;
}

}  // namespace renodx::utils::dlss::neural_rendering::direct

#include "cupti_telemetry.hpp"

#include <Windows.h>

#include <d3d12.h>

#include <cupti.h>
#include <cupti_activity.h>
#include <MinHook.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace cupti_telemetry {
namespace {

constexpr size_t BUFFER_SIZE = 8u * 1024u * 1024u;
constexpr uint32_t MAX_TIMESTAMPED_LAUNCHES = 32768u;
constexpr uint32_t TIMESTAMP_QUERY_COUNT = MAX_TIMESTAMPED_LAUNCHES * 2u;
constexpr uint32_t UNKNOWN_RUNTIME_MODULE = std::numeric_limits<uint32_t>::max();

using Microsoft::WRL::ComPtr;

struct Metrics {
  uint64_t activity_count = 0u;
  uint64_t nvapi_launch_count = 0u;
  uint64_t total_ns = 0u;
  uint64_t minimum_ns = std::numeric_limits<uint64_t>::max();
  uint64_t maximum_ns = 0u;
  uint64_t gpu_timestamp_count = 0u;
  uint64_t gpu_total_ns = 0u;
  uint64_t gpu_minimum_ns = std::numeric_limits<uint64_t>::max();
  uint64_t gpu_maximum_ns = 0u;
};

struct LaunchConfiguration {
  uint32_t grid_x = 0u;
  uint32_t grid_y = 0u;
  uint32_t grid_z = 0u;
  uint32_t block_x = 0u;
  uint32_t block_y = 0u;
  uint32_t block_z = 0u;
  uint32_t dynamic_shared_memory_bytes = 0u;
  uint32_t parameter_size = 0u;
  int32_t parameter_tail_0 = 0;
  int32_t parameter_tail_1 = 0;
  uint32_t chain_index = 0u;
  uint32_t chain_count = 0u;
  bool extended = false;
  uint32_t runtime_module = UNKNOWN_RUNTIME_MODULE;
  std::array<uint64_t, 33u> parameter_qwords = {};

  auto operator<=>(const LaunchConfiguration&) const = default;
};

using MetricKey = std::tuple<std::string, std::string, LaunchConfiguration>;

struct TimestampRecord {
  MetricKey key;
  uint32_t begin_query = 0u;
  uint32_t end_query = 0u;
  bool successful = false;
  bool collected = false;
  uint64_t start_tick = 0u;
  uint64_t end_tick = 0u;
};

struct FunctionInfo {
  std::string name;
  uint32_t runtime_module = UNKNOWN_RUNTIME_MODULE;
};

std::mutex state_mutex;
std::filesystem::path trace_path;
std::string current_phase = "unclassified";
std::map<
    MetricKey,
    Metrics>
    metrics;
uint64_t dropped_records = 0u;
bool active = false;
CUpti_SubscriberHandle subscriber = nullptr;
bool minhook_owned = false;
ComPtr<ID3D12QueryHeap> timestamp_query_heap;
ComPtr<ID3D12Resource> timestamp_readback;
uint64_t timestamp_frequency = 0u;
uint32_t next_timestamp_query = 0u;
size_t resolved_timestamp_records = 0u;
size_t collected_timestamp_records = 0u;
std::vector<TimestampRecord> timestamp_records;
std::vector<MetricKey> launch_records;
bool launch_breadcrumbs_enabled = false;
uint64_t next_launch_call = 0u;
std::string stage_capture_kernel;
bool stage_capture_recorded = false;
uint32_t stage_capture_pointer_qword = 31u;
uint64_t stage_capture_pointer_value = 0u;
uint64_t stage_capture_bytes = 1024u * 1024u;
std::string pointer_override_kernel;
uint32_t pointer_override_qword = 0u;
int64_t pointer_override_delta = 0;
uint64_t pointer_override_value = 0u;
std::string pointer_override_kernel_2;
uint32_t pointer_override_qword_2 = 0u;
int64_t pointer_override_delta_2 = 0;
uint64_t pointer_override_value_2 = 0u;
bool stage_capture_scheduled = false;
bool stage_capture_written = false;
uint64_t stage_capture_gpu_address = 0u;
uint64_t stage_capture_resource_offset = 0u;
ComPtr<ID3D12Resource> stage_capture_readback;

struct TrackedResource {
  ComPtr<ID3D12Resource> resource;
  uint64_t size = 0u;
};

using GetGpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(
    ID3D12Resource*);

GetGpuVirtualAddress original_get_gpu_virtual_address = nullptr;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, TrackedResource> tracked_resources;

using NvapiStatus = int32_t;
using NvapiQueryInterface = void*(__cdecl*)(uint32_t);
using NvapiCreateCuFunction = NvapiStatus(__cdecl*)(
    ID3D12Device*, void*, const char*, void**);

struct NvapiDim3 {
  uint32_t x;
  uint32_t y;
  uint32_t z;
};

struct NvapiCuKernelLaunchParams {
  void* function;
  NvapiDim3 grid;
  NvapiDim3 block;
  uint32_t dynamic_shared_memory_bytes;
  const void* parameters;
  uint32_t parameter_size;
};

struct NvapiCuKernelLaunchParamsEx {
  void* function;
  NvapiDim3 grid;
  NvapiDim3 block;
  uint32_t dynamic_shared_memory_bytes;
  const void* parameters;
  uint32_t parameter_size;
  void** kernel_parameters;
};

using NvapiLaunchCuKernelChain = NvapiStatus(__cdecl*)(
    ID3D12GraphicsCommandList*, const NvapiCuKernelLaunchParams*, uint32_t);
using NvapiLaunchCuKernelChainEx = NvapiStatus(__cdecl*)(
    ID3D12GraphicsCommandList*, const NvapiCuKernelLaunchParamsEx*, uint32_t);
using NvapiDestroyCuFunction = NvapiStatus(__cdecl*)(ID3D12Device*, void*);

constexpr uint32_t NVAPI_CREATE_CU_FUNCTION_ID = 0xe2436e22u;
constexpr uint32_t NVAPI_LAUNCH_CU_KERNEL_CHAIN_ID = 0x24973538u;
constexpr uint32_t NVAPI_LAUNCH_CU_KERNEL_CHAIN_EX_ID = 0x846a9bf0u;
constexpr uint32_t NVAPI_DESTROY_CU_FUNCTION_ID = 0xdf295ea6u;

NvapiQueryInterface original_query_interface = nullptr;
NvapiCreateCuFunction original_create_cu_function = nullptr;
NvapiLaunchCuKernelChain original_launch_cu_kernel_chain = nullptr;
NvapiLaunchCuKernelChainEx original_launch_cu_kernel_chain_ex = nullptr;
NvapiDestroyCuFunction original_destroy_cu_function = nullptr;
std::unordered_map<void*, FunctionInfo> functions;
std::unordered_map<void*, uint32_t> runtime_modules;
uint32_t next_runtime_module = 0u;

LaunchConfiguration MakeLaunchConfiguration(
    void* function,
    NvapiDim3 grid,
    NvapiDim3 block,
    uint32_t dynamic_shared_memory_bytes,
    const void* parameters,
    uint32_t parameter_size,
    uint32_t chain_index,
    uint32_t chain_count,
    bool extended) {
  LaunchConfiguration configuration = {
      .grid_x = grid.x,
      .grid_y = grid.y,
      .grid_z = grid.z,
      .block_x = block.x,
      .block_y = block.y,
      .block_z = block.z,
      .dynamic_shared_memory_bytes = dynamic_shared_memory_bytes,
      .parameter_size = parameter_size,
      .chain_index = chain_index,
      .chain_count = chain_count,
      .extended = extended,
  };
  if (parameters != nullptr && parameter_size >= sizeof(int32_t) * 2u) {
    const auto* tail = static_cast<const std::byte*>(parameters)
                       + parameter_size - sizeof(int32_t) * 2u;
    std::memcpy(&configuration.parameter_tail_0, tail, sizeof(int32_t));
    std::memcpy(
        &configuration.parameter_tail_1,
        tail + sizeof(int32_t),
        sizeof(int32_t));
  }
  if (parameters != nullptr) {
    std::memcpy(
        configuration.parameter_qwords.data(),
        parameters,
        std::min<size_t>(
            parameter_size,
            sizeof(configuration.parameter_qwords)));
  }
  const auto iterator = functions.find(function);
  if (iterator != functions.end()) {
    configuration.runtime_module = iterator->second.runtime_module;
  }
  return configuration;
}

MetricKey MakeMetricKey(
    void* function,
    const LaunchConfiguration& configuration) {
  const auto iterator = functions.find(function);
  return {
      current_phase,
      iterator == functions.end()
          ? std::format("unknown-function-{}", function)
          : iterator->second.name,
      configuration,
  };
}

void RecordNvapiLaunch(
    void* function,
    NvapiDim3 grid,
    NvapiDim3 block,
    uint32_t dynamic_shared_memory_bytes,
    const void* parameters,
    uint32_t parameter_size,
    uint32_t chain_index,
    uint32_t chain_count,
    bool extended) {
  std::scoped_lock lock(state_mutex);
  const auto key = MakeMetricKey(
      function,
      MakeLaunchConfiguration(
          function,
          grid,
          block,
          dynamic_shared_memory_bytes,
          parameters,
          parameter_size,
          chain_index,
          chain_count,
          extended));
  ++metrics[key].nvapi_launch_count;
  launch_records.push_back(key);
}

void CaptureStageParameters(
    void* function,
    const void* parameters,
    uint32_t parameter_size) {
  if (stage_capture_kernel.empty()
      || stage_capture_recorded
      || parameters == nullptr
      || parameter_size == 0u) {
    return;
  }
  const auto iterator = functions.find(function);
  if (iterator == functions.end() || iterator->second.name != stage_capture_kernel) {
    return;
  }
  if (parameter_size < (stage_capture_pointer_qword + 1u) * sizeof(uint64_t)) {
    return;
  }
  uint64_t address = 0u;
  std::memcpy(
      &address,
      static_cast<const std::byte*>(parameters)
          + stage_capture_pointer_qword * sizeof(uint64_t),
      sizeof(address));
  if (stage_capture_pointer_value != 0u
      && address != stage_capture_pointer_value) {
    return;
  }

  auto path = trace_path;
  path.replace_filename(L"dlssnr-stage-parameters.bin");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return;
  stream.write(static_cast<const char*>(parameters), parameter_size);
  if (!stream) return;

  auto manifest_path = trace_path;
  manifest_path.replace_filename(L"dlssnr-stage-parameters.json");
  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest) return;
  manifest
      << "{\n"
      << "  \"schema\": 1,\n"
      << "  \"classification\": \"NVAPI launch parameter capture\",\n"
      << "  \"kernel\": \"" << iterator->second.name << "\",\n"
      << "  \"parameterSize\": " << parameter_size << ",\n"
      << "  \"artifact\": \"dlssnr-stage-parameters.bin\"\n"
      << "}\n";
  stage_capture_recorded = true;
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE HookGetGpuVirtualAddress(
    ID3D12Resource* resource) {
  const D3D12_GPU_VIRTUAL_ADDRESS address =
      original_get_gpu_virtual_address(resource);
  if (address != 0u) {
    const D3D12_RESOURCE_DESC description = resource->GetDesc();
    if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      std::scoped_lock lock(state_mutex);
      tracked_resources[address] = {
          .resource = resource,
          .size = description.Width,
      };
    }
  }
  return address;
}

template <typename Kernel>
void ScheduleStageCapture(
    ID3D12GraphicsCommandList* command_list,
    const Kernel* kernels,
    uint32_t kernel_count) {
  if (stage_capture_kernel.empty()
      || stage_capture_scheduled
      || command_list == nullptr
      || kernels == nullptr) {
    return;
  }
  for (uint32_t index = 0u; index < kernel_count; ++index) {
    const auto function_iterator = functions.find(kernels[index].function);
    if (function_iterator == functions.end()
        || function_iterator->second.name != stage_capture_kernel
        || kernels[index].parameters == nullptr
        || kernels[index].parameter_size
               < (stage_capture_pointer_qword + 1u) * sizeof(uint64_t)) {
      continue;
    }

    uint64_t address = 0u;
    std::memcpy(
        &address,
        static_cast<const std::byte*>(kernels[index].parameters)
            + stage_capture_pointer_qword * sizeof(uint64_t),
        sizeof(address));
    if (stage_capture_pointer_value != 0u
        && address != stage_capture_pointer_value) {
      continue;
    }
    const auto resource_iterator = tracked_resources.upper_bound(address);
    if (resource_iterator == tracked_resources.begin()) return;
    const auto owner = std::prev(resource_iterator);
    const uint64_t offset = address - owner->first;
    if (offset >= owner->second.size) return;
    const uint64_t copy_bytes = std::min(
        stage_capture_bytes,
        owner->second.size - offset);

    ComPtr<ID3D12Device> device;
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device)))) return;
    const D3D12_HEAP_PROPERTIES heap_properties = {
        .Type = D3D12_HEAP_TYPE_READBACK,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1u,
        .VisibleNodeMask = 1u,
    };
    const D3D12_RESOURCE_DESC resource_description = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0u,
        .Width = copy_bytes,
        .Height = 1u,
        .DepthOrArraySize = 1u,
        .MipLevels = 1u,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1u, .Quality = 0u},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &resource_description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readback)))) {
      return;
    }
    const D3D12_RESOURCE_BARRIER to_copy = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = owner->second.resource.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
        },
    };
    const D3D12_RESOURCE_BARRIER from_copy = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = owner->second.resource.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
            .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        },
    };
    command_list->ResourceBarrier(1u, &to_copy);
    command_list->CopyBufferRegion(
        readback.Get(),
        0u,
        owner->second.resource.Get(),
        offset,
        copy_bytes);
    command_list->ResourceBarrier(1u, &from_copy);
    stage_capture_readback = std::move(readback);
    stage_capture_gpu_address = address;
    stage_capture_resource_offset = offset;
    stage_capture_bytes = copy_bytes;
    stage_capture_scheduled = true;
    return;
  }
}

void WriteStageCapture() {
  std::scoped_lock lock(state_mutex);
  if (!stage_capture_scheduled
      || stage_capture_written
      || stage_capture_readback == nullptr) {
    return;
  }
  const D3D12_RANGE read_range = {
      .Begin = 0u,
      .End = static_cast<SIZE_T>(stage_capture_bytes),
  };
  void* mapped = nullptr;
  if (FAILED(stage_capture_readback->Map(0u, &read_range, &mapped))) return;
  auto path = trace_path;
  path.replace_filename(L"dlssnr-stage-output.bin");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(static_cast<const char*>(mapped), stage_capture_bytes);
  const D3D12_RANGE written_range = {};
  stage_capture_readback->Unmap(0u, &written_range);
  if (!stream) return;

  auto manifest_path = trace_path;
  manifest_path.replace_filename(L"dlssnr-stage-output.json");
  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest) return;
  manifest
      << "{\n"
      << "  \"schema\": 1,\n"
      << "  \"classification\": \"D3D12 GPU buffer stage capture\",\n"
      << "  \"kernel\": \"" << stage_capture_kernel << "\",\n"
      << "  \"pointerQword\": " << stage_capture_pointer_qword << ",\n"
      << "  \"gpuAddress\": " << stage_capture_gpu_address << ",\n"
      << "  \"resourceOffset\": " << stage_capture_resource_offset << ",\n"
      << "  \"bytes\": " << stage_capture_bytes << ",\n"
      << "  \"artifact\": \"dlssnr-stage-output.bin\"\n"
      << "}\n";
  stage_capture_written = true;
}

template <typename Kernel>
bool BeginLaunchBreadcrumb(
    ID3D12GraphicsCommandList* command_list,
    const Kernel* kernels,
    uint32_t kernel_count) {
  if (!launch_breadcrumbs_enabled
      || command_list == nullptr
      || kernels == nullptr
      || kernel_count == 0u) {
    return false;
  }
  std::string label;
  {
    std::scoped_lock lock(state_mutex);
    const auto iterator = functions.find(kernels[0u].function);
    label = std::format(
        "{} launch={} kernel={} count={}",
        current_phase,
        ++next_launch_call,
        iterator == functions.end()
            ? std::format("unknown-function-{}", kernels[0u].function)
            : iterator->second.name,
        kernel_count);
  }
  constexpr UINT WINPIX_EVENT_ANSI_VERSION = 1u;
  command_list->BeginEvent(
      WINPIX_EVENT_ANSI_VERSION,
      label.data(),
      static_cast<UINT>(label.size()));
  return true;
}

template <typename Kernel>
size_t BeginGpuTimestamp(
    ID3D12GraphicsCommandList* command_list,
    const Kernel* kernels,
    uint32_t kernel_count,
    bool extended) {
  if (command_list == nullptr || kernels == nullptr || kernel_count == 0u) {
    return std::numeric_limits<size_t>::max();
  }

  uint32_t begin_query = 0u;
  size_t record_index = 0u;
  MetricKey key;
  {
    std::scoped_lock lock(state_mutex);
    if (timestamp_query_heap == nullptr
        || next_timestamp_query + 2u > TIMESTAMP_QUERY_COUNT) {
      ++dropped_records;
      return std::numeric_limits<size_t>::max();
    }
    begin_query = next_timestamp_query;
    next_timestamp_query += 2u;

    if (kernel_count == 1u) {
      const auto configuration = MakeLaunchConfiguration(
          kernels[0u].function,
          kernels[0u].grid,
          kernels[0u].block,
          kernels[0u].dynamic_shared_memory_bytes,
          kernels[0u].parameters,
          kernels[0u].parameter_size,
          0u,
          1u,
          extended);
      key = MakeMetricKey(kernels[0u].function, configuration);
    } else {
      LaunchConfiguration configuration = MakeLaunchConfiguration(
          kernels[0u].function,
          kernels[0u].grid,
          kernels[0u].block,
          kernels[0u].dynamic_shared_memory_bytes,
          kernels[0u].parameters,
          kernels[0u].parameter_size,
          0u,
          kernel_count,
          extended);
      std::string name = std::format("chain[{}]", kernel_count);
      for (uint32_t index = 0u; index < kernel_count; ++index) {
        const auto iterator = functions.find(kernels[index].function);
        name += index == 0u ? ":" : "+";
        name += iterator == functions.end()
                    ? std::format("unknown-function-{}", kernels[index].function)
                    : iterator->second.name;
        if (iterator == functions.end()
            || iterator->second.runtime_module != configuration.runtime_module) {
          configuration.runtime_module = UNKNOWN_RUNTIME_MODULE;
        }
      }
      key = {current_phase, std::move(name), configuration};
    }
    timestamp_records.push_back({
        .key = std::move(key),
        .begin_query = begin_query,
        .end_query = begin_query + 1u,
    });
    record_index = timestamp_records.size() - 1u;
  }
  command_list->EndQuery(
      timestamp_query_heap.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP,
      begin_query);
  return record_index;
}

void EndGpuTimestamp(
    ID3D12GraphicsCommandList* command_list,
    size_t record_index,
    bool successful) {
  if (record_index == std::numeric_limits<size_t>::max()) return;
  uint32_t end_query = 0u;
  {
    std::scoped_lock lock(state_mutex);
    if (record_index >= timestamp_records.size()) return;
    auto& record = timestamp_records[record_index];
    record.successful = successful;
    end_query = record.end_query;
  }
  command_list->EndQuery(
      timestamp_query_heap.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP,
      end_query);
}

NvapiStatus __cdecl HookCreateCuFunction(
    ID3D12Device* device,
    void* module,
    const char* name,
    void** function) {
  const NvapiStatus status =
      original_create_cu_function(device, module, name, function);
  if (status == 0 && function != nullptr && *function != nullptr) {
    std::scoped_lock lock(state_mutex);
    const auto [module_iterator, inserted] = runtime_modules.try_emplace(
        module,
        next_runtime_module);
    if (inserted) ++next_runtime_module;
    functions[*function] = {
        .name = name == nullptr ? "unnamed-function" : name,
        .runtime_module = module_iterator->second,
    };
  }
  return status;
}

template <typename LaunchParams>
[[nodiscard]] std::vector<std::pair<uint64_t*, uint64_t>> ApplyPointerOverrides(
    const LaunchParams* kernels,
    uint32_t kernel_count) {
  std::vector<std::pair<uint64_t*, uint64_t>> overrides;
  if (kernels == nullptr) return overrides;
  const auto apply_rule = [&]<typename String>(
                              const String& kernel_selector,
                              uint32_t qword,
                              int64_t delta,
                              uint64_t value) {
    if (kernel_selector.empty() || delta == 0) return;
    for (uint32_t index = 0u; index < kernel_count; ++index) {
      const auto function = functions.find(kernels[index].function);
      bool name_matches = false;
      size_t selector_begin = 0u;
      while (function != functions.end() && selector_begin <= kernel_selector.size()) {
        const size_t selector_end = kernel_selector.find(';', selector_begin);
        const std::string_view selector{
            kernel_selector.data() + selector_begin,
            (selector_end == std::string::npos ? kernel_selector.size() : selector_end) - selector_begin};
        const bool wildcard = selector.ends_with('*');
        name_matches = wildcard
                           ? function->second.name.starts_with(selector.substr(0u, selector.size() - 1u))
                           : function->second.name == selector;
        if (name_matches || selector_end == std::string::npos) break;
        selector_begin = selector_end + 1u;
      }
      if (function == functions.end()
          || !name_matches
          || kernels[index].parameters == nullptr
          || kernels[index].parameter_size < (qword + 1u) * sizeof(uint64_t)) {
        continue;
      }
      auto* pointer = const_cast<uint64_t*>(
                          static_cast<const uint64_t*>(kernels[index].parameters))
                      + qword;
      if (value != 0u && *pointer != value) continue;
      overrides.emplace_back(pointer, *pointer);
      *pointer = static_cast<uint64_t>(static_cast<int64_t>(*pointer) + delta);
    }
  };
  apply_rule(pointer_override_kernel, pointer_override_qword, pointer_override_delta, pointer_override_value);
  apply_rule(pointer_override_kernel_2, pointer_override_qword_2, pointer_override_delta_2, pointer_override_value_2);
  return overrides;
}

NvapiStatus __cdecl HookLaunchCuKernelChain(
    ID3D12GraphicsCommandList* command_list,
    const NvapiCuKernelLaunchParams* kernels,
    uint32_t kernel_count) {
  if (kernels != nullptr) {
    std::scoped_lock lock(state_mutex);
    for (uint32_t index = 0u; index < kernel_count; ++index) {
      CaptureStageParameters(
          kernels[index].function,
          kernels[index].parameters,
          kernels[index].parameter_size);
    }
  }
  const bool breadcrumb_started = BeginLaunchBreadcrumb(
      command_list,
      kernels,
      kernel_count);
  const size_t timestamp_record = BeginGpuTimestamp(
      command_list,
      kernels,
      kernel_count,
      false);
  const auto pointer_overrides = ApplyPointerOverrides(kernels, kernel_count);
  const NvapiStatus status =
      original_launch_cu_kernel_chain(command_list, kernels, kernel_count);
  for (const auto& [pointer, value] : pointer_overrides) {
    *pointer = value;
  }
  if (status == 0) {
    std::scoped_lock lock(state_mutex);
    ScheduleStageCapture(command_list, kernels, kernel_count);
  }
  EndGpuTimestamp(command_list, timestamp_record, status == 0);
  if (breadcrumb_started) command_list->EndEvent();
  if (status == 0 && kernels != nullptr) {
    for (uint32_t index = 0u; index < kernel_count; ++index) {
      RecordNvapiLaunch(
          kernels[index].function,
          kernels[index].grid,
          kernels[index].block,
          kernels[index].dynamic_shared_memory_bytes,
          kernels[index].parameters,
          kernels[index].parameter_size,
          index,
          kernel_count,
          false);
    }
  }
  return status;
}

NvapiStatus __cdecl HookLaunchCuKernelChainEx(
    ID3D12GraphicsCommandList* command_list,
    const NvapiCuKernelLaunchParamsEx* kernels,
    uint32_t kernel_count) {
  if (kernels != nullptr) {
    std::scoped_lock lock(state_mutex);
    for (uint32_t index = 0u; index < kernel_count; ++index) {
      CaptureStageParameters(
          kernels[index].function,
          kernels[index].parameters,
          kernels[index].parameter_size);
    }
  }
  const bool breadcrumb_started = BeginLaunchBreadcrumb(
      command_list,
      kernels,
      kernel_count);
  const size_t timestamp_record = BeginGpuTimestamp(
      command_list,
      kernels,
      kernel_count,
      true);
  const auto pointer_overrides = ApplyPointerOverrides(kernels, kernel_count);
  const NvapiStatus status =
      original_launch_cu_kernel_chain_ex(command_list, kernels, kernel_count);
  for (const auto& [pointer, value] : pointer_overrides) {
    *pointer = value;
  }
  if (status == 0) {
    std::scoped_lock lock(state_mutex);
    ScheduleStageCapture(command_list, kernels, kernel_count);
  }
  EndGpuTimestamp(command_list, timestamp_record, status == 0);
  if (breadcrumb_started) command_list->EndEvent();
  if (status == 0 && kernels != nullptr) {
    for (uint32_t index = 0u; index < kernel_count; ++index) {
      RecordNvapiLaunch(
          kernels[index].function,
          kernels[index].grid,
          kernels[index].block,
          kernels[index].dynamic_shared_memory_bytes,
          kernels[index].parameters,
          kernels[index].parameter_size,
          index,
          kernel_count,
          true);
    }
  }
  return status;
}

NvapiStatus __cdecl HookDestroyCuFunction(ID3D12Device* device, void* function) {
  const NvapiStatus status = original_destroy_cu_function(device, function);
  if (status == 0) {
    std::scoped_lock lock(state_mutex);
    functions.erase(function);
  }
  return status;
}

void* __cdecl HookQueryInterface(uint32_t interface_id) {
  void* function = original_query_interface(interface_id);
  switch (interface_id) {
    case NVAPI_CREATE_CU_FUNCTION_ID:
      original_create_cu_function =
          reinterpret_cast<NvapiCreateCuFunction>(function);
      return function == nullptr
                 ? nullptr
                 : reinterpret_cast<void*>(&HookCreateCuFunction);
    case NVAPI_LAUNCH_CU_KERNEL_CHAIN_ID:
      original_launch_cu_kernel_chain =
          reinterpret_cast<NvapiLaunchCuKernelChain>(function);
      return function == nullptr
                 ? nullptr
                 : reinterpret_cast<void*>(&HookLaunchCuKernelChain);
    case NVAPI_LAUNCH_CU_KERNEL_CHAIN_EX_ID:
      original_launch_cu_kernel_chain_ex =
          reinterpret_cast<NvapiLaunchCuKernelChainEx>(function);
      return function == nullptr
                 ? nullptr
                 : reinterpret_cast<void*>(&HookLaunchCuKernelChainEx);
    case NVAPI_DESTROY_CU_FUNCTION_ID:
      original_destroy_cu_function =
          reinterpret_cast<NvapiDestroyCuFunction>(function);
      return function == nullptr
                 ? nullptr
                 : reinterpret_cast<void*>(&HookDestroyCuFunction);
    default:
      return function;
  }
}

bool StartNvapiHook() {
  const HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
  if (nvapi == nullptr) return false;
  void* query_interface = reinterpret_cast<void*>(
      GetProcAddress(nvapi, "nvapi_QueryInterface"));
  if (query_interface == nullptr) return false;
  const MH_STATUS initialize_status = MH_Initialize();
  if (initialize_status != MH_OK
      && initialize_status != MH_ERROR_ALREADY_INITIALIZED) {
    return false;
  }
  minhook_owned = initialize_status == MH_OK;
  if (MH_CreateHook(
          query_interface,
          reinterpret_cast<void*>(&HookQueryInterface),
          reinterpret_cast<void**>(&original_query_interface))
          != MH_OK
      || MH_EnableHook(query_interface) != MH_OK) {
    if (minhook_owned) (void)MH_Uninitialize();
    minhook_owned = false;
    return false;
  }
  return true;
}

void CollectGpuTimestamps() {
  std::scoped_lock lock(state_mutex);
  if (timestamp_readback == nullptr
      || timestamp_frequency == 0u
      || collected_timestamp_records >= resolved_timestamp_records) {
    return;
  }

  const D3D12_RANGE read_range = {
      .Begin = 0u,
      .End = static_cast<SIZE_T>(next_timestamp_query) * sizeof(uint64_t),
  };
  void* mapped_data = nullptr;
  if (FAILED(timestamp_readback->Map(0u, &read_range, &mapped_data))) {
    return;
  }
  const auto* query_values = static_cast<const uint64_t*>(mapped_data);
  for (size_t index = collected_timestamp_records;
       index < resolved_timestamp_records;
       ++index) {
    auto& record = timestamp_records[index];
    record.start_tick = query_values[record.begin_query];
    record.end_tick = query_values[record.end_query];
    record.collected = true;
    if (!record.successful || record.end_tick < record.start_tick) continue;
    const uint64_t duration_ns = static_cast<uint64_t>(
        static_cast<long double>(record.end_tick - record.start_tick)
        * 1000000000.0L / timestamp_frequency);
    auto& value = metrics[record.key];
    ++value.gpu_timestamp_count;
    value.gpu_total_ns += duration_ns;
    value.gpu_minimum_ns = std::min(value.gpu_minimum_ns, duration_ns);
    value.gpu_maximum_ns = std::max(value.gpu_maximum_ns, duration_ns);
  }
  const D3D12_RANGE written_range = {};
  timestamp_readback->Unmap(0u, &written_range);
  collected_timestamp_records = resolved_timestamp_records;
}

void WriteSnapshot() {
  std::scoped_lock lock(state_mutex);
  if (trace_path.empty()) return;
  const auto quote = [](std::string text) {
    size_t position = 0u;
    while ((position = text.find('"', position)) != std::string::npos) {
      text.insert(position, 1u, '"');
      position += 2u;
    }
    return '"' + text + '"';
  };
  std::ofstream stream(trace_path, std::ios::binary | std::ios::trunc);
  if (!stream) return;
  stream
      << "phase,kernel,activity_count,nvapi_launch_count,total_us,mean_us,"
         "minimum_us,maximum_us,gpu_timestamp_count,gpu_total_us,gpu_mean_us,"
         "gpu_minimum_us,gpu_maximum_us,runtime_module,grid_x,grid_y,grid_z,"
         "block_x,block_y,block_z,"
         "dynamic_shared_memory_bytes,parameter_size,parameter_tail_0,"
         "parameter_tail_1,chain_index,chain_count,extended";
  for (size_t index = 0u; index < LaunchConfiguration{}.parameter_qwords.size(); ++index) {
    stream << ",parameter_qword_" << index;
  }
  stream << ",fp8\n";
  for (const auto& [key, value] : metrics) {
    const auto& [phase, name, configuration] = key;
    stream
        << quote(phase) << ','
        << quote(name) << ','
        << value.activity_count << ','
        << value.nvapi_launch_count << ','
        << static_cast<double>(value.total_ns) / 1000.0 << ','
        << (value.activity_count == 0u
                ? 0.0
                : static_cast<double>(value.total_ns)
                      / value.activity_count / 1000.0)
        << ','
        << (value.activity_count == 0u
                ? 0.0
                : static_cast<double>(value.minimum_ns) / 1000.0)
        << ','
        << static_cast<double>(value.maximum_ns) / 1000.0 << ','
        << value.gpu_timestamp_count << ','
        << static_cast<double>(value.gpu_total_ns) / 1000.0 << ','
        << (value.gpu_timestamp_count == 0u
                ? 0.0
                : static_cast<double>(value.gpu_total_ns)
                      / value.gpu_timestamp_count / 1000.0)
        << ','
        << (value.gpu_timestamp_count == 0u
                ? 0.0
                : static_cast<double>(value.gpu_minimum_ns) / 1000.0)
        << ','
        << static_cast<double>(value.gpu_maximum_ns) / 1000.0 << ','
        << (configuration.runtime_module == UNKNOWN_RUNTIME_MODULE
                ? -1
                : static_cast<int64_t>(configuration.runtime_module))
        << ','
        << configuration.grid_x << ','
        << configuration.grid_y << ','
        << configuration.grid_z << ','
        << configuration.block_x << ','
        << configuration.block_y << ','
        << configuration.block_z << ','
        << configuration.dynamic_shared_memory_bytes << ','
        << configuration.parameter_size << ','
        << configuration.parameter_tail_0 << ','
        << configuration.parameter_tail_1 << ','
        << configuration.chain_index << ','
        << configuration.chain_count << ','
        << (configuration.extended ? "true" : "false");
    for (const uint64_t parameter : configuration.parameter_qwords) {
      stream << ',' << parameter;
    }
    stream << ','
           << (name.find("_fp8") != std::string::npos ? "true" : "false")
           << '\n';
  }
  stream << "\"summary\",\"dropped_records\",0," << dropped_records;
  for (uint32_t index = 0u; index < 22u; ++index) stream << ",0";
  stream << ",false";
  for (size_t index = 0u; index < LaunchConfiguration{}.parameter_qwords.size(); ++index) {
    stream << ",0";
  }
  stream << ",false\n";

  auto launch_path = trace_path;
  launch_path.replace_filename(L"dlssnr-launches.csv");
  std::ofstream launch_stream(
      launch_path,
      std::ios::binary | std::ios::trunc);
  launch_stream
      << "sequence,phase,kernel,runtime_module,grid_x,grid_y,grid_z,"
         "block_x,block_y,block_z,dynamic_shared_memory_bytes,parameter_size,"
         "parameter_tail_0,parameter_tail_1,chain_index,chain_count,extended";
  for (size_t index = 0u; index < LaunchConfiguration{}.parameter_qwords.size(); ++index) {
    launch_stream << ",parameter_qword_" << index;
  }
  launch_stream << ",fp8\n";
  for (size_t sequence = 0u; sequence < launch_records.size(); ++sequence) {
    const auto& [phase, name, configuration] = launch_records[sequence];
    launch_stream
        << sequence + 1u << ','
        << quote(phase) << ','
        << quote(name) << ','
        << (configuration.runtime_module == UNKNOWN_RUNTIME_MODULE
                ? -1
                : static_cast<int64_t>(configuration.runtime_module))
        << ','
        << configuration.grid_x << ','
        << configuration.grid_y << ','
        << configuration.grid_z << ','
        << configuration.block_x << ','
        << configuration.block_y << ','
        << configuration.block_z << ','
        << configuration.dynamic_shared_memory_bytes << ','
        << configuration.parameter_size << ','
        << configuration.parameter_tail_0 << ','
        << configuration.parameter_tail_1 << ','
        << configuration.chain_index << ','
        << configuration.chain_count << ','
        << (configuration.extended ? "true" : "false");
    for (const uint64_t parameter : configuration.parameter_qwords) {
      launch_stream << ',' << parameter;
    }
    launch_stream << ','
                  << (name.find("_fp8") != std::string::npos ? "true" : "false")
                  << '\n';
  }

  std::map<std::pair<uint32_t, std::string>, uint64_t> folded;
  for (const auto& [key, value] : metrics) {
    const auto& [phase, name, configuration] = key;
    if (!phase.starts_with("evaluation-")
        || phase == "evaluation-001"
        || value.gpu_timestamp_count == 0u) {
      continue;
    }
    folded[{configuration.runtime_module, name}] += value.gpu_total_ns;
  }
  auto folded_path = trace_path;
  folded_path.replace_filename(L"dlssnr-kernels.folded");
  std::ofstream folded_stream(
      folded_path,
      std::ios::binary | std::ios::trunc);
  for (const auto& [key, total_ns] : folded) {
    const auto& [runtime_module, name] = key;
    folded_stream
        << "DLSS-NR;"
        << (runtime_module == UNKNOWN_RUNTIME_MODULE
                ? "runtime-module-unknown"
                : std::format("runtime-module-{}", runtime_module))
        << ';' << name << ' ' << total_ns << '\n';
  }

  const auto json_escape = [](std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
      switch (character) {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result += character; break;
      }
    }
    return result;
  };
  uint64_t first_tick = std::numeric_limits<uint64_t>::max();
  for (const auto& record : timestamp_records) {
    if (record.collected && record.successful) {
      first_tick = std::min(first_tick, record.start_tick);
    }
  }
  auto trace_json_path = trace_path;
  trace_json_path.replace_filename(L"dlssnr-trace.json");
  std::ofstream trace_stream(
      trace_json_path,
      std::ios::binary | std::ios::trunc);
  trace_stream
      << std::setprecision(std::numeric_limits<double>::max_digits10)
      << "{\n  \"displayTimeUnit\": \"ns\",\n  \"traceEvents\": [";
  bool first_event = true;
  std::map<std::string, uint32_t> phase_threads;
  for (const auto& record : timestamp_records) {
    if (!record.collected
        || !record.successful
        || record.end_tick < record.start_tick
        || first_tick == std::numeric_limits<uint64_t>::max()) {
      continue;
    }
    const auto& [phase, name, configuration] = record.key;
    const auto [thread, inserted] = phase_threads.try_emplace(
        phase,
        static_cast<uint32_t>(phase_threads.size() + 1u));
    static_cast<void>(inserted);
    const long double start_us = static_cast<long double>(
                                     record.start_tick - first_tick)
                                 * 1000000.0L / timestamp_frequency;
    const long double duration_us = static_cast<long double>(
                                        record.end_tick - record.start_tick)
                                    * 1000000.0L / timestamp_frequency;
    trace_stream
        << (first_event ? "\n" : ",\n")
        << "    {\"name\":\"" << json_escape(name)
        << "\",\"cat\":\"runtime-module-"
        << (configuration.runtime_module == UNKNOWN_RUNTIME_MODULE
                ? "unknown"
                : std::to_string(configuration.runtime_module))
        << "\",\"ph\":\"X\",\"ts\":" << static_cast<double>(start_us)
        << ",\"dur\":" << static_cast<double>(duration_us)
        << ",\"pid\":1,\"tid\":" << thread->second
        << ",\"args\":{\"phase\":\"" << json_escape(phase)
        << "\",\"grid\":\"" << configuration.grid_x << 'x'
        << configuration.grid_y << 'x' << configuration.grid_z
        << "\",\"block\":\"" << configuration.block_x << 'x'
        << configuration.block_y << 'x' << configuration.block_z << "\"}}";
    first_event = false;
  }
  trace_stream << "\n  ]\n}\n";
}

void CUPTIAPI SubscriberCallback(
    void*, CUpti_CallbackDomain, CUpti_CallbackId, const void*) {}

void CUPTIAPI BufferRequested(
    uint8_t** buffer,
    size_t* size,
    size_t* maximum_records,
    CUpti_BufferCallbackRequestInfo*) {
  *buffer = static_cast<uint8_t*>(std::malloc(BUFFER_SIZE));
  *size = *buffer == nullptr ? 0u : BUFFER_SIZE;
  *maximum_records = 0u;
}

void CUPTIAPI BufferCompleted(
    uint8_t* buffer,
    size_t,
    size_t valid_size,
    CUpti_BufferCallbackCompleteInfo*) {
  if (buffer == nullptr) return;
  CUpti_Activity* record = nullptr;
  while (cuptiActivityGetNextRecord(buffer, valid_size, &record)
         == CUPTI_SUCCESS) {
    if (record->kind != CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) continue;
    const auto* kernel = reinterpret_cast<const CUpti_ActivityKernel12*>(record);
    if (kernel->name == nullptr || kernel->end < kernel->start) continue;
    std::scoped_lock lock(state_mutex);
    auto& value = metrics[{current_phase, kernel->name, {}}];
    const uint64_t duration = kernel->end - kernel->start;
    ++value.activity_count;
    value.total_ns += duration;
    value.minimum_ns = std::min(value.minimum_ns, duration);
    value.maximum_ns = std::max(value.maximum_ns, duration);
  }
  std::free(buffer);
}

}  // namespace

Session::Session(const std::filesystem::path& output_path) {
  {
    std::scoped_lock lock(state_mutex);
    if (active) return;
    trace_path = output_path;
    current_phase = "initialization";
    metrics.clear();
    dropped_records = 0u;
    functions.clear();
    runtime_modules.clear();
    next_runtime_module = 0u;
    timestamp_query_heap.Reset();
    timestamp_readback.Reset();
    timestamp_frequency = 0u;
    next_timestamp_query = 0u;
    resolved_timestamp_records = 0u;
    collected_timestamp_records = 0u;
    timestamp_records.clear();
    launch_records.clear();
    launch_breadcrumbs_enabled = GetEnvironmentVariableW(
                                     L"RENODX_DLSSNR_REPRO_LAUNCH_BREADCRUMBS",
                                     nullptr,
                                     0u)
                                 != 0u;
    next_launch_call = 0u;
    stage_capture_kernel.clear();
    std::array<char, 256u> stage_kernel = {};
    const DWORD stage_kernel_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_STAGE_KERNEL",
        stage_kernel.data(),
        static_cast<DWORD>(stage_kernel.size()));
    if (stage_kernel_length != 0u && stage_kernel_length < stage_kernel.size()) {
      stage_capture_kernel.assign(stage_kernel.data(), stage_kernel_length);
    }
    stage_capture_recorded = false;
    stage_capture_scheduled = false;
    stage_capture_written = false;
    stage_capture_gpu_address = 0u;
    stage_capture_resource_offset = 0u;
    stage_capture_readback.Reset();
    tracked_resources.clear();
    std::array<char, 32u> stage_qword = {};
    const DWORD stage_qword_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_STAGE_POINTER_QWORD",
        stage_qword.data(),
        static_cast<DWORD>(stage_qword.size()));
    if (stage_qword_length != 0u && stage_qword_length < stage_qword.size()) {
      stage_capture_pointer_qword = static_cast<uint32_t>(
          std::stoul(stage_qword.data()));
    }
    stage_capture_pointer_value = 0u;
    std::array<char, 32u> stage_pointer_value = {};
    const DWORD stage_pointer_value_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_STAGE_POINTER_VALUE",
        stage_pointer_value.data(),
        static_cast<DWORD>(stage_pointer_value.size()));
    if (stage_pointer_value_length != 0u
        && stage_pointer_value_length < stage_pointer_value.size()) {
      stage_capture_pointer_value = std::stoull(stage_pointer_value.data());
    }
    std::array<char, 32u> stage_bytes = {};
    const DWORD stage_bytes_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_STAGE_BYTES",
        stage_bytes.data(),
        static_cast<DWORD>(stage_bytes.size()));
    if (stage_bytes_length != 0u && stage_bytes_length < stage_bytes.size()) {
      stage_capture_bytes = std::stoull(stage_bytes.data());
    }
    pointer_override_kernel.clear();
    pointer_override_qword = 0u;
    pointer_override_delta = 0;
    pointer_override_value = 0u;
    std::array<char, 256u> override_kernel = {};
    const DWORD override_kernel_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_KERNEL",
        override_kernel.data(),
        static_cast<DWORD>(override_kernel.size()));
    if (override_kernel_length != 0u && override_kernel_length < override_kernel.size()) {
      pointer_override_kernel.assign(override_kernel.data(), override_kernel_length);
    }
    std::array<char, 32u> override_qword = {};
    const DWORD override_qword_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_QWORD",
        override_qword.data(),
        static_cast<DWORD>(override_qword.size()));
    if (override_qword_length != 0u && override_qword_length < override_qword.size()) {
      pointer_override_qword = static_cast<uint32_t>(std::stoul(override_qword.data()));
    }
    std::array<char, 32u> override_delta = {};
    const DWORD override_delta_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_DELTA",
        override_delta.data(),
        static_cast<DWORD>(override_delta.size()));
    if (override_delta_length != 0u && override_delta_length < override_delta.size()) {
      pointer_override_delta = std::stoll(override_delta.data());
    }
    std::array<char, 32u> override_value = {};
    const DWORD override_value_length = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_VALUE",
        override_value.data(),
        static_cast<DWORD>(override_value.size()));
    if (override_value_length != 0u
        && override_value_length < override_value.size()) {
      pointer_override_value = std::stoull(override_value.data());
    }
    pointer_override_kernel_2.clear();
    pointer_override_qword_2 = 0u;
    pointer_override_delta_2 = 0;
    pointer_override_value_2 = 0u;
    std::array<char, 256u> override_kernel_2 = {};
    const DWORD override_kernel_length_2 = GetEnvironmentVariableA(
        "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_KERNEL_2",
        override_kernel_2.data(),
        static_cast<DWORD>(override_kernel_2.size()));
    if (override_kernel_length_2 != 0u
        && override_kernel_length_2 < override_kernel_2.size()) {
      pointer_override_kernel_2.assign(
          override_kernel_2.data(), override_kernel_length_2);
    }
    std::array<char, 32u> override_qword_2 = {};
    if (const DWORD length = GetEnvironmentVariableA(
            "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_QWORD_2",
            override_qword_2.data(),
            static_cast<DWORD>(override_qword_2.size()));
        length != 0u && length < override_qword_2.size()) {
      pointer_override_qword_2 = static_cast<uint32_t>(
          std::stoul(override_qword_2.data()));
    }
    std::array<char, 32u> override_delta_2 = {};
    if (const DWORD length = GetEnvironmentVariableA(
            "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_DELTA_2",
            override_delta_2.data(),
            static_cast<DWORD>(override_delta_2.size()));
        length != 0u && length < override_delta_2.size()) {
      pointer_override_delta_2 = std::stoll(override_delta_2.data());
    }
    std::array<char, 32u> override_value_2 = {};
    if (const DWORD length = GetEnvironmentVariableA(
            "RENODX_DLSSNR_REPRO_POINTER_OVERRIDE_VALUE_2",
            override_value_2.data(),
            static_cast<DWORD>(override_value_2.size()));
        length != 0u && length < override_value_2.size()) {
      pointer_override_value_2 = std::stoull(override_value_2.data());
    }
  }
  nvapi_hook_started_ = StartNvapiHook();
  CUpti_SubscriberParams subscriber_parameters = {
      .structSize = sizeof(CUpti_SubscriberParams),
  };
  if (cuptiSubscribe_v2(
          &subscriber,
          SubscriberCallback,
          nullptr,
          &subscriber_parameters)
          != CUPTI_SUCCESS
      || cuptiActivityRegisterCallbacks_v2(
             subscriber,
             BufferRequested,
             BufferCompleted)
             != CUPTI_SUCCESS
      || cuptiActivityEnable_v2(
             subscriber,
             CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL,
             nullptr)
             != CUPTI_SUCCESS) {
    WriteSnapshot();
    started_ = nvapi_hook_started_;
    return;
  }
  {
    std::scoped_lock lock(state_mutex);
    active = true;
  }
  cupti_started_ = true;
  started_ = true;
  WriteSnapshot();
}

Session::~Session() {
  if (!started_) return;
  Flush();
  if (cupti_started_) {
    (void)cuptiActivityDisable_v2(
        subscriber,
        CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL,
        nullptr);
    (void)cuptiUnsubscribe(subscriber);
    subscriber = nullptr;
  }
  if (nvapi_hook_started_) {
    const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi != nullptr) {
      if (void* query_interface = reinterpret_cast<void*>(
              GetProcAddress(nvapi, "nvapi_QueryInterface"));
          query_interface != nullptr) {
        (void)MH_DisableHook(query_interface);
        (void)MH_RemoveHook(query_interface);
      }
    }
    if (minhook_owned) (void)MH_Uninitialize();
    minhook_owned = false;
  }
  std::scoped_lock lock(state_mutex);
  timestamp_query_heap.Reset();
  timestamp_readback.Reset();
  timestamp_frequency = 0u;
  active = false;
}

bool Session::Started() const {
  return started_;
}

bool Session::CuptiStarted() const {
  return cupti_started_;
}

bool Session::NvapiHookStarted() const {
  return nvapi_hook_started_;
}

bool Session::GpuTimestampsStarted() const {
  return gpu_timestamps_started_;
}

bool Session::ConfigureGpuTimestamps(ID3D12CommandQueue* command_queue) {
  if (!started_ || command_queue == nullptr) return false;

  ComPtr<ID3D12Device> device;
  uint64_t frequency = 0u;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device)))
      || FAILED(command_queue->GetTimestampFrequency(&frequency))
      || frequency == 0u) {
    return false;
  }
  const D3D12_QUERY_HEAP_DESC query_desc = {
      .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
      .Count = TIMESTAMP_QUERY_COUNT,
      .NodeMask = 0u,
  };
  ComPtr<ID3D12QueryHeap> query_heap;
  if (FAILED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query_heap)))) {
    return false;
  }
  const D3D12_HEAP_PROPERTIES heap_properties = {
      .Type = D3D12_HEAP_TYPE_READBACK,
      .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
      .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
      .CreationNodeMask = 1u,
      .VisibleNodeMask = 1u,
  };
  const D3D12_RESOURCE_DESC resource_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0u,
      .Width = static_cast<uint64_t>(TIMESTAMP_QUERY_COUNT) * sizeof(uint64_t),
      .Height = 1u,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_NONE,
  };
  ComPtr<ID3D12Resource> readback;
  if (FAILED(device->CreateCommittedResource(
          &heap_properties,
          D3D12_HEAP_FLAG_NONE,
          &resource_desc,
          D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,
          IID_PPV_ARGS(&readback)))) {
    return false;
  }
  query_heap->SetName(L"RenoDX DLSS-NR kernel timestamp queries");
  readback->SetName(L"RenoDX DLSS-NR kernel timestamp readback");

  {
    std::scoped_lock lock(state_mutex);
    timestamp_query_heap = std::move(query_heap);
    timestamp_readback = std::move(readback);
    timestamp_frequency = frequency;
  }
  gpu_timestamps_started_ = true;
  return true;
}

bool Session::ConfigureStageCapture(ID3D12Resource* resource) {
  if (!started_ || stage_capture_kernel.empty() || resource == nullptr) {
    return stage_capture_kernel.empty();
  }
  void** virtual_table = *reinterpret_cast<void***>(resource);
  void* get_gpu_virtual_address = virtual_table[11u];
  if (MH_CreateHook(
          get_gpu_virtual_address,
          reinterpret_cast<void*>(&HookGetGpuVirtualAddress),
          reinterpret_cast<void**>(&original_get_gpu_virtual_address))
          != MH_OK
      || MH_EnableHook(get_gpu_virtual_address) != MH_OK) {
    return false;
  }
  (void)resource->GetGPUVirtualAddress();
  return true;
}

void Session::ResolveGpuTimestamps(
    ID3D12GraphicsCommandList* command_list) const {
  if (!gpu_timestamps_started_ || command_list == nullptr) return;

  uint32_t first_query = 0u;
  uint32_t query_count = 0u;
  {
    std::scoped_lock lock(state_mutex);
    if (resolved_timestamp_records >= timestamp_records.size()) return;
    first_query = timestamp_records[resolved_timestamp_records].begin_query;
    query_count = timestamp_records.back().end_query - first_query + 1u;
    resolved_timestamp_records = timestamp_records.size();
  }
  command_list->ResolveQueryData(
      timestamp_query_heap.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP,
      first_query,
      query_count,
      timestamp_readback.Get(),
      static_cast<uint64_t>(first_query) * sizeof(uint64_t));
}

void Session::SetPhase(std::string_view phase) const {
  if (!started_) return;
  Flush();
  std::scoped_lock lock(state_mutex);
  current_phase = phase;
}

void Session::Flush() const {
  if (!started_) return;
  if (cupti_started_) {
    (void)cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);
  }
  if (gpu_timestamps_started_) CollectGpuTimestamps();
  WriteStageCapture();
  WriteSnapshot();
}

}  // namespace cupti_telemetry
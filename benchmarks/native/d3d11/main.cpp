#include <Windows.h>

#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

#include "neural_rendering_direct.hpp"

using Microsoft::WRL::ComPtr;
namespace direct = renodx::utils::dlss::neural_rendering::direct;

namespace {

constexpr uint32_t WIDTH = 1920u;
constexpr uint32_t HEIGHT = 1080u;

using InitializeCore = NVSDK_NGX_Result(NVSDK_CONV*)(
    uint64_t, const wchar_t*, ID3D11Device*, NVSDK_NGX_Version);
using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using InitializeSnippet = NVSDK_NGX_Result(NVSDK_CONV*)(
    uint64_t, const wchar_t*, ID3D11Device*, NVSDK_NGX_Version,
    const NVSDK_NGX_Parameter*);
using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D11DeviceContext*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownSnippet = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11Device*);

void PrintResult(const char* boundary, NVSDK_NGX_Result result) {
  std::cout << boundary << "=0x" << std::hex
            << static_cast<uint32_t>(result) << std::dec << '\n';
}

ComPtr<ID3D11Texture2D> CreateTexture(
    ID3D11Device* device,
    DXGI_FORMAT format,
    uint32_t bytes_per_pixel,
    UINT bind_flags,
    const std::vector<uint8_t>& initial_data) {
  const D3D11_TEXTURE2D_DESC desc = {
      .Width = WIDTH,
      .Height = HEIGHT,
      .MipLevels = 1u,
      .ArraySize = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = bind_flags,
      .CPUAccessFlags = 0u,
      .MiscFlags = 0u,
  };
  const D3D11_SUBRESOURCE_DATA data = {
      .pSysMem = initial_data.data(),
      .SysMemPitch = WIDTH * bytes_per_pixel,
      .SysMemSlicePitch = 0u,
  };
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return nullptr;
  return texture;
}

bool OutputChanged(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* output,
    const std::vector<uint8_t>& sentinel) {
  D3D11_TEXTURE2D_DESC desc = {};
  output->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0u;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
  context->CopyResource(staging.Get(), output);
  context->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped))) {
    return false;
  }
  bool changed = false;
  constexpr size_t row_bytes = static_cast<size_t>(WIDTH) * 8u;
  for (uint32_t y = 0u; y < HEIGHT && !changed; ++y) {
    const auto* actual = static_cast<const uint8_t*>(mapped.pData)
                         + static_cast<size_t>(mapped.RowPitch) * y;
    const auto* expected = sentinel.data() + row_bytes * y;
    changed = std::memcmp(actual, expected, row_bytes) != 0;
  }
  context->Unmap(staging.Get(), 0u);
  return changed;
}

int Run(const std::filesystem::path& snippet_path,
        const std::filesystem::path& core_path) {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0u;
       factory->EnumAdapterByGpuPreference(
           index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
           IID_PPV_ARGS(&adapter))
       != DXGI_ERROR_NOT_FOUND;
       ++index) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    if (desc.VendorId == 0x10de) break;
    adapter.Reset();
  }
  if (adapter == nullptr) return 3;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level = {};
  const HRESULT device_result = D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0u, nullptr, 0u,
      D3D11_SDK_VERSION, &device, &feature_level, &context);
  std::cout << "d3d11_device=0x" << std::hex
            << static_cast<uint32_t>(device_result)
            << " feature_level=0x" << static_cast<uint32_t>(feature_level)
            << std::dec << '\n';
  if (FAILED(device_result)) return 4;

  const HMODULE core = LoadLibraryExW(
      core_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (core == nullptr) return 5;
  auto* const initialize_core = reinterpret_cast<InitializeCore>(
      GetProcAddress(core, "NVSDK_NGX_D3D11_Init"));
  auto* const allocate_parameters = reinterpret_cast<AllocateParameters>(
      GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
  auto* const destroy_parameters = reinterpret_cast<DestroyParameters>(
      GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters"));
  if (initialize_core == nullptr || allocate_parameters == nullptr
      || destroy_parameters == nullptr) {
    return 6;
  }

  const auto app_data = std::filesystem::temp_directory_path()
                        / L"RenoDX" / L"dlssnr-d3d11-repro";
  std::filesystem::create_directories(app_data);
  const auto core_result = initialize_core(
      0x876232cULL, app_data.c_str(), device.Get(), NVSDK_NGX_Version_API);
  PrintResult("core_init", core_result);
  if (NVSDK_NGX_FAILED(core_result)) return 7;

  if (!direct::Attach(snippet_path)) return 8;
  const HMODULE snippet = direct::runtime.api.module;
  auto* const initialize = reinterpret_cast<InitializeSnippet>(
      GetProcAddress(snippet, "NVSDK_NGX_D3D11_Init_Ext"));
  auto* const create = reinterpret_cast<CreateFeature>(
      GetProcAddress(snippet, "NVSDK_NGX_D3D11_CreateFeature"));
  auto* const evaluate = reinterpret_cast<EvaluateFeature>(
      GetProcAddress(snippet, "NVSDK_NGX_D3D11_EvaluateFeature"));
  auto* const release = reinterpret_cast<ReleaseFeature>(
      GetProcAddress(snippet, "NVSDK_NGX_D3D11_ReleaseFeature"));
  auto* const shutdown = reinterpret_cast<ShutdownSnippet>(
      GetProcAddress(snippet, "NVSDK_NGX_D3D11_Shutdown1"));
  if (initialize == nullptr || create == nullptr || evaluate == nullptr
      || release == nullptr || shutdown == nullptr) {
    return 9;
  }

  const auto init_result = initialize(
      0x876232cULL, snippet_path.parent_path().c_str(), device.Get(),
      NVSDK_NGX_Version_API, nullptr);
  PrintResult("snippet_init", init_result);
  if (NVSDK_NGX_FAILED(init_result)) return 10;

  NVSDK_NGX_Parameter* parameters = nullptr;
  const auto allocate_result = allocate_parameters(&parameters);
  PrintResult("allocate", allocate_result);
  if (NVSDK_NGX_FAILED(allocate_result) || parameters == nullptr) return 11;

  parameters->Set("CreationNodeMask", 1u);
  parameters->Set("VisibilityNodeMask", 1u);
  parameters->Set("Width", WIDTH);
  parameters->Set("Height", HEIGHT);
  parameters->Set("OutWidth", WIDTH);
  parameters->Set("OutHeight", HEIGHT);
  parameters->Set("DLSSNR.Width", WIDTH);
  parameters->Set("DLSSNR.Height", HEIGHT);
  parameters->Set("DLSSNR.InputWidth", WIDTH);
  parameters->Set("DLSSNR.InputHeight", HEIGHT);
  parameters->Set("DLSSNR.OutputWidth", WIDTH);
  parameters->Set("DLSSNR.OutputHeight", HEIGHT);
  parameters->Set("DLSSNR.Output.Width", WIDTH);
  parameters->Set("DLSSNR.Output.Height", HEIGHT);
  parameters->Set("DLSSNR.Hint.Render.Preset", 1u);
  parameters->Set("PerfQualityValue", 5);
  parameters->Set("DLSSNRComputeScalingRatioCallback",
                  reinterpret_cast<void*>(&direct::ComputeScalingRatio));
  parameters->Set("DLSSNR.ScalingRatio", 1.f);
  parameters->Set("DLSSNR.Scale", 1.f);
  parameters->Set("DLSSNR.Upscaling", 0);

  NVSDK_NGX_Handle* feature = nullptr;
  const auto create_result = create(
      context.Get(), NVSDK_NGX_Feature_Reserved18, parameters, &feature);
  PrintResult("create_reserved18", create_result);
  if (NVSDK_NGX_FAILED(create_result) || feature == nullptr) return 12;

  const size_t texels = static_cast<size_t>(WIDTH) * HEIGHT;
  std::vector<uint8_t> color(texels * 8u);
  std::vector<uint8_t> motion(texels * 8u, 0u);
  std::vector<uint8_t> depth(texels * 4u);
  std::vector<uint8_t> sentinel(texels * 8u);
  for (size_t index = 0u; index < texels; ++index) {
    auto* color_half = reinterpret_cast<uint16_t*>(color.data() + index * 8u);
    color_half[0] = DirectX::PackedVector::XMConvertFloatToHalf(
        0.05f + static_cast<float>(index % WIDTH) / WIDTH);
    color_half[1] = DirectX::PackedVector::XMConvertFloatToHalf(
        0.05f + static_cast<float>(index / WIDTH) / HEIGHT);
    color_half[2] = DirectX::PackedVector::XMConvertFloatToHalf(0.25f);
    color_half[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
    const float depth_value = 0.5f;
    std::memcpy(depth.data() + index * 4u, &depth_value, sizeof(depth_value));
    auto* output_half = reinterpret_cast<uint16_t*>(
        sentinel.data() + index * 8u);
    for (uint32_t component = 0u; component < 4u; ++component) {
      output_half[component] = DirectX::PackedVector::XMConvertFloatToHalf(-4.f);
    }
  }

  auto input_texture = CreateTexture(
      device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8u,
      D3D11_BIND_SHADER_RESOURCE, color);
  auto motion_texture = CreateTexture(
      device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8u,
      D3D11_BIND_SHADER_RESOURCE, motion);
  auto depth_texture = CreateTexture(
      device.Get(), DXGI_FORMAT_R32_FLOAT, 4u,
      D3D11_BIND_SHADER_RESOURCE, depth);
  auto output_texture = CreateTexture(
      device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8u,
      D3D11_BIND_UNORDERED_ACCESS, sentinel);
  if (input_texture == nullptr || motion_texture == nullptr
      || depth_texture == nullptr || output_texture == nullptr) {
    return 13;
  }

  parameters->Set("DLSSNR.Color", input_texture.Get());
  parameters->Set("DLSSNR.Output", output_texture.Get());
  parameters->Set("DLSSNR.MVec", motion_texture.Get());
  parameters->Set("DLSSNR.Depth", depth_texture.Get());
  parameters->Set("DLSSNR.UI", static_cast<ID3D11Resource*>(nullptr));
  parameters->Set("DLSSNR.UIAlpha", static_cast<ID3D11Resource*>(nullptr));
  for (const char* prefix : {"Color", "Output", "MVec", "Depth"}) {
    const std::string base = std::string("DLSSNR.") + prefix + "Subrect";
    parameters->Set((base + "BaseX").c_str(), 0u);
    parameters->Set((base + "BaseY").c_str(), 0u);
    parameters->Set((base + "Width").c_str(), WIDTH);
    parameters->Set((base + "Height").c_str(), HEIGHT);
  }
  parameters->Set("Jitter.Offset.X", 0.f);
  parameters->Set("Jitter.Offset.Y", 0.f);
  parameters->Set("DLSSNR.JitterOffsetX", 0.f);
  parameters->Set("DLSSNR.JitterOffsetY", 0.f);
  parameters->Set("DLSSNR.MVecScaleX", 1.f);
  parameters->Set("DLSSNR.MVecScaleY", 1.f);
  parameters->Set("DLSSNR.DepthInverted", 0);
  parameters->Set("DLSSNR.Enabled", 1);
  parameters->Set("DLSSNR.Reset", 1);
  parameters->Set("DLSSNR.Intensity", 1.f);
  parameters->Set("DLSSNR.LocalToneStrength", 1.f);
  parameters->Set("DLSSNR.LocalStructureStrength", 1.f);
  parameters->Set("DLSSNR.GlobalToneStrength", 1.f);
  parameters->Set("DLSSNR.UseAutoMask", 0);
  parameters->Set("DLSSNR.SkinStructureStrength", 1.f);
  parameters->Set("DLSSNR.Style", 0u);
  parameters->Set("DLSSNR.UICorrection", 0);
  parameters->Set("DLSS.Indicator.Invert.X.Axis", 0);
  parameters->Set("DLSS.Indicator.Invert.Y.Axis", 0);

  const auto evaluate_result = evaluate(
      context.Get(), feature, parameters, nullptr);
  PrintResult("evaluate", evaluate_result);
  const bool changed = NVSDK_NGX_SUCCEED(evaluate_result)
                       && OutputChanged(device.Get(), context.Get(),
                                        output_texture.Get(), sentinel);
  std::cout << "output_changed=" << changed << '\n';

  PrintResult("release", release(feature));
  PrintResult("snippet_shutdown", shutdown(device.Get()));
  PrintResult("destroy_parameters", destroy_parameters(parameters));
  (void)direct::Detach();
  return changed ? 0 : 14;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::cerr << "usage: dlssnr-d3d11-repro <nvngx_dlssnr.dll> <nvngx.dll>\n";
    return 1;
  }
  return Run(argv[1], argv[2]);
}

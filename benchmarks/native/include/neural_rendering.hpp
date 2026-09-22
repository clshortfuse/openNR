/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <sl.h>

namespace renodx::utils::dlss::neural_rendering {

constexpr sl::Feature FEATURE = 1004u;
constexpr sl::BufferType BUFFER_TYPE_UPLIFT_INPUT_COLOR = 70u;
constexpr sl::BufferType BUFFER_TYPE_UPLIFT_OUTPUT_COLOR = 71u;
constexpr sl::BufferType BUFFER_TYPE_UPLIFT_CONTROL_MASK = 72u;

enum class IntegrationMode : uint8_t {
  DIRECT_SNIPPET = 0u,
  NGX_CORE_STREAMLINE = 1u,
};

constexpr IntegrationMode DEFAULT_INTEGRATION_MODE =
    IntegrationMode::DIRECT_SNIPPET;

struct DeploymentPaths {
  std::filesystem::path plugin;
  std::filesystem::path ngx_snippet;

  bool HasPlugin() const { return std::filesystem::is_regular_file(plugin); }
  bool HasSnippet() const { return std::filesystem::is_regular_file(ngx_snippet); }
  bool IsComplete() const { return HasPlugin() && HasSnippet(); }
};

inline DeploymentPaths ResolveDeploymentPaths(
    const std::filesystem::path& plugin_directory) {
  return {
      .plugin = plugin_directory / L"sl.dlss_nr.dll",
      .ngx_snippet = plugin_directory / L"nvngx_dlssnr.dll",
  };
}

enum class Mode : uint32_t {
  OFF = 0u,
  ON = 1u,
};

enum class PerformanceMode : uint32_t {
  VALUE_1 = 1u,
  VALUE_2 = 2u,
  VALUE_3 = 3u,
  VALUE_6 = 6u,
};

enum class SignalEncoding : uint8_t {
  SRGB = 0u,
  SCRGB_NL_BT709_GAMUT_COMPRESSION_N2_RESTORE = 1u,
  HDR_SRGB_BT709_N2_RESTORE =
      SCRGB_NL_BT709_GAMUT_COMPRESSION_N2_RESTORE,
  SCRGB_LINEAR_BT709_GAMUT_COMPRESSION_N2_RESTORE = 2u,
  HDR_LINEAR_BT709_N2_RESTORE = 3u,
  HDR_PQ_BT2020_N2_RESTORE = 4u,
};

// Recovered from the supplied prerelease Streamline 2.13 sl.dlss_nr.dll.
// {29DFDFE0-273A-4E72-B492-2DC823D5B1AD}
struct Options : sl::BaseStructure {
  constexpr static sl::StructType s_structType = sl::StructType({
      0x29dfdfe0,
      0x273a,
      0x4e72,
      {0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad},
  });

  Options() : sl::BaseStructure(s_structType, sl::kStructVersion3) {}

  Mode mode = Mode::OFF;
  float intensity = 1.f;
  float local_tone_strength = 1.f;
  float local_structure_strength = 1.f;
  float global_tone_strength = 1.f;
  uint32_t style = 0u;
  uint32_t render_preset = 1u;
  sl::Boolean auto_mask = sl::Boolean::eFalse;
  float skin_structure_strength = -1.f;
  PerformanceMode performance_mode = PerformanceMode::VALUE_3;
};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(Options) == 0x48u);
static_assert(offsetof(Options, mode) == 0x20u);
static_assert(offsetof(Options, intensity) == 0x24u);
static_assert(offsetof(Options, local_tone_strength) == 0x28u);
static_assert(offsetof(Options, local_structure_strength) == 0x2cu);
static_assert(offsetof(Options, global_tone_strength) == 0x30u);
static_assert(offsetof(Options, style) == 0x34u);
static_assert(offsetof(Options, render_preset) == 0x38u);
static_assert(offsetof(Options, auto_mask) == 0x3cu);
static_assert(offsetof(Options, skin_structure_strength) == 0x40u);
static_assert(offsetof(Options, performance_mode) == 0x44u);
#else
static_assert(sizeof(Options) == 0x40u);
static_assert(offsetof(Options, mode) == 0x18u);
static_assert(offsetof(Options, intensity) == 0x1cu);
static_assert(offsetof(Options, local_tone_strength) == 0x20u);
static_assert(offsetof(Options, local_structure_strength) == 0x24u);
static_assert(offsetof(Options, global_tone_strength) == 0x28u);
static_assert(offsetof(Options, style) == 0x2cu);
static_assert(offsetof(Options, render_preset) == 0x30u);
static_assert(offsetof(Options, auto_mask) == 0x34u);
static_assert(offsetof(Options, skin_structure_strength) == 0x38u);
static_assert(offsetof(Options, performance_mode) == 0x3cu);
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using PFun_slDLSSNRSetOptions = sl::Result(
    const sl::ViewportHandle& viewport,
    const Options& options);

}  // namespace renodx::utils::dlss::neural_rendering

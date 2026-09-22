#pragma once

#include <filesystem>
#include <string_view>

struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace cupti_telemetry {

class Session final {
 public:
  explicit Session(const std::filesystem::path& output_path);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] bool Started() const;
  [[nodiscard]] bool CuptiStarted() const;
  [[nodiscard]] bool NvapiHookStarted() const;
  [[nodiscard]] bool GpuTimestampsStarted() const;
  bool ConfigureGpuTimestamps(ID3D12CommandQueue* command_queue);
  bool ConfigureStageCapture(ID3D12Resource* resource);
  void ResolveGpuTimestamps(ID3D12GraphicsCommandList* command_list) const;
  void SetPhase(std::string_view phase) const;
  void Flush() const;

 private:
  bool started_ = false;
  bool cupti_started_ = false;
  bool nvapi_hook_started_ = false;
  bool gpu_timestamps_started_ = false;
};

}  // namespace cupti_telemetry
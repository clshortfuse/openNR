#include <cuda_runtime.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr unsigned int kWarpSize = 32;
constexpr unsigned int kWarpCount = 256;
constexpr unsigned int kConversionCount = 65536;

__device__ uint32_t DecodeE4M3Pair(uint16_t packed) {
  uint32_t result;
  asm volatile("cvt.rn.f16x2.e4m3x2 %0, %1;" : "=r"(result) : "h"(packed));
  return result;
}

__device__ __forceinline__ uint16_t DecodeE4M3Compact(uint8_t value) {
  const uint32_t magnitude_code = value & 0x7Fu;
  const uint32_t normal = (magnitude_code + 64u) << 7;
  uint32_t subnormal;
  asm volatile(
      "prmt.b32 %0, %1, %2, %3;"
      : "=r"(subnormal)
      : "r"(0x1E1C1800u), "r"(0x23222120u), "r"(value & 7u));
  const uint32_t magnitude = magnitude_code == 127u
      ? 0x7FFFu
      : ((value & 0x78u) == 0u ? subnormal << 8 : normal)
          | ((value & 0x80u) << 8);
  return static_cast<uint16_t>(magnitude);
}

__device__ __forceinline__ uint32_t DecodeE4M3PairCompact(uint16_t packed) {
  return DecodeE4M3Compact(static_cast<uint8_t>(packed))
      | (static_cast<uint32_t>(DecodeE4M3Compact(static_cast<uint8_t>(packed >> 8))) << 16);
}

__device__ __forceinline__ uint8_t EncodeE4M3Compact(uint16_t value) {
  const uint32_t sign = (value >> 8) & 0x80u;
  const uint32_t magnitude = value & 0x7FFFu;
  const uint32_t exponent = magnitude >> 10;
  const uint32_t normal_lsb = (magnitude >> 7) & 1u;
  const uint32_t normal = min(
    ((magnitude + 63u + normal_lsb) >> 7) - 64u,
    126u);
  uint32_t subnormal = 0;
  if (exponent >= 5u && exponent < 9u) {
  const uint32_t shift = 16u - exponent;
  const uint32_t significand = 1024u + (magnitude & 1023u);
  const uint32_t truncated = significand >> shift;
  subnormal = (significand + ((1u << (shift - 1u)) - 1u)
    + (truncated & 1u)) >> shift;
  }
  if (magnitude > 0x7C00u) return 0x7Fu;
  return static_cast<uint8_t>(
    sign | (exponent >= 9u ? normal : subnormal));
}

__global__ void CaptureConversions(
    uint32_t* decoded,
    uint32_t* compact_decoded,
  uint16_t* encoded,
  uint16_t* compact_encoded) {
  const unsigned int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= kConversionCount) return;
  decoded[index] = DecodeE4M3Pair(static_cast<uint16_t>(index));
  compact_decoded[index] = DecodeE4M3PairCompact(static_cast<uint16_t>(index));
  const uint32_t duplicated = index | (index << 16);
  uint16_t result;
  asm volatile(
      "cvt.rn.satfinite.e4m3x2.f16x2 %0, %1;"
      : "=h"(result)
      : "r"(duplicated));
  encoded[index] = result;
  const uint16_t compact = EncodeE4M3Compact(static_cast<uint16_t>(index));
  compact_encoded[index] = compact | (compact << 8);
}

uint16_t DecodeE4M3(uint8_t value) {
  const uint16_t sign = static_cast<uint16_t>(value & 0x80u) << 8;
  const uint16_t exponent = (value >> 3) & 0xFu;
  const uint16_t mantissa = value & 0x7u;
  uint16_t magnitude;
  if (exponent == 0) {
    if (mantissa == 0) {
      magnitude = 0;
    } else {
      unsigned int position = 0;
      for (unsigned int bits = mantissa; bits >>= 1; ++position) {}
      const uint16_t base = 1u << position;
      magnitude = static_cast<uint16_t>(
          ((position + 6u) << 10)
          | ((mantissa - base) << (10u - position)));
    }
  } else if (exponent == 0xFu && mantissa == 0x7u) {
    return 0x7FFFu;
  } else {
    magnitude = static_cast<uint16_t>(
        ((exponent + 8u) << 10) | (mantissa << 7));
  }
  return sign | magnitude;
}

uint8_t EncodeE4M3(uint16_t value) {
  const uint8_t sign = static_cast<uint8_t>((value >> 8) & 0x80u);
  const uint16_t magnitude = value & 0x7FFFu;
  const unsigned int exponent = magnitude >> 10;
  const unsigned int mantissa = magnitude & 0x3FFu;
  unsigned int code = 0;
  if (magnitude > 0x7C00u) {
    return 0x7Fu;
  } else if (exponent >= 9) {
    unsigned int quantized = mantissa >> 7;
    const unsigned int remainder = mantissa & 0x7Fu;
    quantized += remainder > 0x40u
        || (remainder == 0x40u && (quantized & 1u));
    const unsigned int carry = quantized >> 3;
    quantized &= 0x7u;
    code = ((exponent - 8u + carry) << 3) | quantized;
    if (code > 0x7Eu) code = 0x7Eu;
  } else if (exponent >= 5) {
    const unsigned int shift = 16u - exponent;
    const unsigned int significand = 0x400u + mantissa;
    unsigned int quantized = significand >> shift;
    const unsigned int remainder = significand & ((1u << shift) - 1u);
    const unsigned int halfway = 1u << (shift - 1u);
    quantized += remainder > halfway
        || (remainder == halfway && (quantized & 1u));
    code = quantized;
  }
  return sign | static_cast<uint8_t>(code);
}

__global__ void CompareMma(
    const uint32_t* matrix_a,
    const uint32_t* matrix_b,
    uint32_t* native_result,
  uint32_t* split_k8_result,
  uint32_t* split_k16_result) {
  const unsigned int thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t* const a = matrix_a + thread * 4;
  const uint32_t* const b = matrix_b + thread * 2;

  uint32_t native_0;
  uint32_t native_1;
  asm volatile(
      "mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 "
      "{%0, %1}, {%2, %3, %4, %5}, {%6, %7}, {%8, %9};"
      : "=r"(native_0), "=r"(native_1)
      : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]),
        "r"(b[0]), "r"(b[1]), "r"(0u), "r"(0u));

  uint32_t split_k8_0 = 0;
  uint32_t split_k8_1 = 0;
  const unsigned int lane = threadIdx.x & 31u;
  const unsigned int thread_in_group = lane & 3u;
#pragma unroll
  for (unsigned int slice = 0; slice != 4; ++slice) {
    const unsigned int source_lane =
        (lane & ~3u) | ((slice & 1u) * 2u + (thread_in_group >> 1));
    const unsigned int pair_shift = (thread_in_group & 1u) * 16u;
    const unsigned int a_register = (slice >> 1) * 2u;
    const uint32_t a_top_packed = __shfl_sync(0xFFFFFFFFu, a[a_register], source_lane);
    const uint32_t a_bottom_packed = __shfl_sync(0xFFFFFFFFu, a[a_register + 1], source_lane);
    const uint32_t b_packed = __shfl_sync(0xFFFFFFFFu, b[slice >> 1], source_lane);
    const uint32_t a_top = DecodeE4M3Pair(static_cast<uint16_t>(a_top_packed >> pair_shift));
    const uint32_t a_bottom = DecodeE4M3Pair(static_cast<uint16_t>(a_bottom_packed >> pair_shift));
    const uint32_t b_slice = DecodeE4M3Pair(static_cast<uint16_t>(b_packed >> pair_shift));
    uint32_t next_0;
    uint32_t next_1;
    asm volatile(
        "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
        "{%0, %1}, {%2, %3}, {%4}, {%5, %6};"
        : "=r"(next_0), "=r"(next_1)
        : "r"(a_top), "r"(a_bottom), "r"(b_slice),
          "r"(split_k8_0), "r"(split_k8_1));
    split_k8_0 = next_0;
    split_k8_1 = next_1;
  }

  uint32_t split_k16_0 = 0;
  uint32_t split_k16_1 = 0;
#pragma unroll
  for (unsigned int half = 0; half != 2; ++half) {
    const unsigned int source_lane_0 =
        (lane & ~3u) | (thread_in_group >> 1);
    const unsigned int source_lane_1 = source_lane_0 + 2u;
    const unsigned int pair_shift = (thread_in_group & 1u) * 16u;
    const unsigned int a_register = half * 2u;
    const uint32_t a_top_0 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register], source_lane_0) >> pair_shift));
    const uint32_t a_bottom_0 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register + 1], source_lane_0) >> pair_shift));
    const uint32_t a_top_1 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register], source_lane_1) >> pair_shift));
    const uint32_t a_bottom_1 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register + 1], source_lane_1) >> pair_shift));
    const uint32_t b_0 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, b[half], source_lane_0) >> pair_shift));
    const uint32_t b_1 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, b[half], source_lane_1) >> pair_shift));
    uint32_t next_0;
    uint32_t next_1;
    asm volatile(
        "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 "
        "{%0, %1}, {%2, %3, %4, %5}, {%6, %7}, {%8, %9};"
        : "=r"(next_0), "=r"(next_1)
        : "r"(a_top_0), "r"(a_bottom_0), "r"(a_top_1), "r"(a_bottom_1),
          "r"(b_0), "r"(b_1), "r"(split_k16_0), "r"(split_k16_1));
    split_k16_0 = next_0;
    split_k16_1 = next_1;
  }

  native_result[thread * 2] = native_0;
  native_result[thread * 2 + 1] = native_1;
  split_k8_result[thread * 2] = split_k8_0;
  split_k8_result[thread * 2 + 1] = split_k8_1;
  split_k16_result[thread * 2] = split_k16_0;
  split_k16_result[thread * 2 + 1] = split_k16_1;
}

__global__ void CompareMmaPair(
    const uint32_t* matrix_a,
    const uint32_t* matrix_b_0,
    const uint32_t* matrix_b_1,
    uint32_t* native_result,
    uint32_t* paired_result) {
  const unsigned int thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t* const a = matrix_a + thread * 4;
  const uint32_t* const b_0 = matrix_b_0 + thread * 2;
  const uint32_t* const b_1 = matrix_b_1 + thread * 2;

  uint32_t native_0_0;
  uint32_t native_0_1;
  uint32_t native_1_0;
  uint32_t native_1_1;
  asm volatile(
      "mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 "
      "{%0, %1}, {%2, %3, %4, %5}, {%6, %7}, {%8, %9};"
      : "=r"(native_0_0), "=r"(native_0_1)
      : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]),
        "r"(b_0[0]), "r"(b_0[1]), "r"(0u), "r"(0u));
  asm volatile(
      "mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 "
      "{%0, %1}, {%2, %3, %4, %5}, {%6, %7}, {%8, %9};"
      : "=r"(native_1_0), "=r"(native_1_1)
      : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]),
        "r"(b_1[0]), "r"(b_1[1]), "r"(0u), "r"(0u));

  uint32_t paired_0_0 = 0;
  uint32_t paired_0_1 = 0;
  uint32_t paired_1_0 = 0;
  uint32_t paired_1_1 = 0;
  const unsigned int lane = threadIdx.x & 31u;
  const unsigned int thread_in_group = lane & 3u;
#pragma unroll
  for (unsigned int slice = 0; slice != 4; ++slice) {
    const unsigned int source_lane =
        (lane & ~3u) | ((slice & 1u) * 2u + (thread_in_group >> 1));
    const unsigned int pair_shift = (thread_in_group & 1u) * 16u;
    const unsigned int a_register = (slice >> 1) * 2u;
    const uint32_t a_top = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register], source_lane) >> pair_shift));
    const uint32_t a_bottom = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, a[a_register + 1], source_lane) >> pair_shift));
    const uint32_t b_slice_0 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, b_0[slice >> 1], source_lane) >> pair_shift));
    const uint32_t b_slice_1 = DecodeE4M3PairCompact(static_cast<uint16_t>(
        __shfl_sync(0xFFFFFFFFu, b_1[slice >> 1], source_lane) >> pair_shift));
    uint32_t next_0_0;
    uint32_t next_0_1;
    uint32_t next_1_0;
    uint32_t next_1_1;
    asm volatile(
        "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
        "{%0, %1}, {%2, %3}, {%4}, {%5, %6};"
        : "=r"(next_0_0), "=r"(next_0_1)
        : "r"(a_top), "r"(a_bottom), "r"(b_slice_0),
          "r"(paired_0_0), "r"(paired_0_1));
    asm volatile(
        "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
        "{%0, %1}, {%2, %3}, {%4}, {%5, %6};"
        : "=r"(next_1_0), "=r"(next_1_1)
        : "r"(a_top), "r"(a_bottom), "r"(b_slice_1),
          "r"(paired_1_0), "r"(paired_1_1));
    paired_0_0 = next_0_0;
    paired_0_1 = next_0_1;
    paired_1_0 = next_1_0;
    paired_1_1 = next_1_1;
  }

  native_result[thread * 4] = native_0_0;
  native_result[thread * 4 + 1] = native_0_1;
  native_result[thread * 4 + 2] = native_1_0;
  native_result[thread * 4 + 3] = native_1_1;
  paired_result[thread * 4] = paired_0_0;
  paired_result[thread * 4 + 1] = paired_0_1;
  paired_result[thread * 4 + 2] = paired_1_0;
  paired_result[thread * 4 + 3] = paired_1_1;
}

void Check(cudaError_t result, const char* operation) {
  if (result == cudaSuccess) return;
  std::fprintf(stderr, "%s failed: %s\n", operation, cudaGetErrorString(result));
  std::exit(1);
}

}  // namespace

int main() {
  std::vector<uint32_t> native_decoded(kConversionCount);
  std::vector<uint32_t> compact_decoded(kConversionCount);
  std::vector<uint16_t> native_encoded(kConversionCount);
  std::vector<uint16_t> compact_encoded(kConversionCount);
  uint32_t* device_decoded = nullptr;
  uint32_t* device_compact_decoded = nullptr;
  uint16_t* device_encoded = nullptr;
  uint16_t* device_compact_encoded = nullptr;
  Check(cudaMalloc(&device_decoded, native_decoded.size() * sizeof(uint32_t)), "cudaMalloc decoded");
  Check(cudaMalloc(&device_compact_decoded, compact_decoded.size() * sizeof(uint32_t)), "cudaMalloc compact decoded");
  Check(cudaMalloc(&device_encoded, native_encoded.size() * sizeof(uint16_t)), "cudaMalloc encoded");
    Check(cudaMalloc(&device_compact_encoded, compact_encoded.size() * sizeof(uint16_t)), "cudaMalloc compact encoded");
  CaptureConversions<<<kConversionCount / 256, 256>>>(
      device_decoded, device_compact_decoded, device_encoded,
      device_compact_encoded);
  Check(cudaGetLastError(), "CaptureConversions launch");
  Check(cudaDeviceSynchronize(), "CaptureConversions execution");
  Check(cudaMemcpy(native_decoded.data(), device_decoded, native_decoded.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost), "copy decoded");
  Check(cudaMemcpy(compact_decoded.data(), device_compact_decoded, compact_decoded.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost), "copy compact decoded");
  Check(cudaMemcpy(native_encoded.data(), device_encoded, native_encoded.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost), "copy encoded");
  Check(cudaMemcpy(compact_encoded.data(), device_compact_encoded, compact_encoded.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost), "copy compact encoded");
  size_t decode_mismatches = 0;
  size_t compact_decode_mismatches = 0;
  size_t encode_mismatches = 0;
  size_t compact_encode_mismatches = 0;
  for (unsigned int value = 0; value != kConversionCount; ++value) {
    const uint32_t decoded = DecodeE4M3(static_cast<uint8_t>(value))
        | (static_cast<uint32_t>(DecodeE4M3(static_cast<uint8_t>(value >> 8))) << 16);
    if (decoded != native_decoded[value]) {
      if (decode_mismatches < 8) {
        std::printf("decode mismatch[%04x] native=%08x emulated=%08x\n", value, native_decoded[value], decoded);
      }
      ++decode_mismatches;
    }
    if (compact_decoded[value] != native_decoded[value]) {
      if (compact_decode_mismatches < 8) {
        std::printf("compact decode mismatch[%04x] native=%08x compact=%08x\n",
            value, native_decoded[value], compact_decoded[value]);
      }
      ++compact_decode_mismatches;
    }
    const uint16_t encoded = EncodeE4M3(static_cast<uint16_t>(value))
        | (static_cast<uint16_t>(EncodeE4M3(static_cast<uint16_t>(value))) << 8);
    if (encoded != native_encoded[value]) {
      if (encode_mismatches < 8) {
        std::printf("encode mismatch[%04x] native=%04x emulated=%04x\n", value, native_encoded[value], encoded);
      }
      ++encode_mismatches;
    }
    if (compact_encoded[value] != native_encoded[value]) {
      if (compact_encode_mismatches < 8) {
        std::printf("compact encode mismatch[%04x] native=%04x compact=%04x\n",
            value, native_encoded[value], compact_encoded[value]);
      }
      ++compact_encode_mismatches;
    }
  }
  std::printf(
      "decode_cases=%u mismatches=%zu compact_mismatches=%zu "
      "encode_cases=%u mismatches=%zu compact_mismatches=%zu\n",
      kConversionCount, decode_mismatches, compact_decode_mismatches,
      kConversionCount, encode_mismatches, compact_encode_mismatches);

  constexpr size_t thread_count = kWarpSize * kWarpCount;
  std::vector<uint32_t> matrix_a(thread_count * 4);
  std::vector<uint32_t> matrix_b(thread_count * 2);
  std::vector<uint32_t> matrix_b_pair(thread_count * 2);
  std::vector<uint32_t> native_result(thread_count * 2);
  std::vector<uint32_t> split_k8_result(thread_count * 2);
  std::vector<uint32_t> split_k16_result(thread_count * 2);
  std::vector<uint32_t> native_pair_result(thread_count * 4);
  std::vector<uint32_t> paired_result(thread_count * 4);
  constexpr std::array<uint8_t, 5> values = {0x00, 0x30, 0x38, 0xB0, 0xB8};
  std::mt19937 random(0xD1554E52u);
  auto fill = [&](std::vector<uint32_t>* words) {
    for (uint32_t& word : *words) {
      word = 0;
      for (unsigned int byte = 0; byte != 4; ++byte) {
        word |= static_cast<uint32_t>(values[random() % values.size()]) << (byte * 8);
      }
    }
  };
  fill(&matrix_a);
  fill(&matrix_b);
  fill(&matrix_b_pair);

  uint32_t* device_a = nullptr;
  uint32_t* device_b = nullptr;
  uint32_t* device_b_pair = nullptr;
  uint32_t* device_native = nullptr;
  uint32_t* device_split_k8 = nullptr;
  uint32_t* device_split_k16 = nullptr;
  uint32_t* device_native_pair = nullptr;
  uint32_t* device_paired = nullptr;
  Check(cudaMalloc(&device_a, matrix_a.size() * sizeof(uint32_t)), "cudaMalloc A");
  Check(cudaMalloc(&device_b, matrix_b.size() * sizeof(uint32_t)), "cudaMalloc B");
  Check(cudaMalloc(&device_b_pair, matrix_b_pair.size() * sizeof(uint32_t)), "cudaMalloc paired B");
  Check(cudaMalloc(&device_native, native_result.size() * sizeof(uint32_t)), "cudaMalloc native");
  Check(cudaMalloc(&device_split_k8, split_k8_result.size() * sizeof(uint32_t)), "cudaMalloc split k8");
  Check(cudaMalloc(&device_split_k16, split_k16_result.size() * sizeof(uint32_t)), "cudaMalloc split k16");
  Check(cudaMalloc(&device_native_pair, native_pair_result.size() * sizeof(uint32_t)), "cudaMalloc native pair");
  Check(cudaMalloc(&device_paired, paired_result.size() * sizeof(uint32_t)), "cudaMalloc paired");
  Check(cudaMemcpy(device_a, matrix_a.data(), matrix_a.size() * sizeof(uint32_t), cudaMemcpyHostToDevice), "copy A");
  Check(cudaMemcpy(device_b, matrix_b.data(), matrix_b.size() * sizeof(uint32_t), cudaMemcpyHostToDevice), "copy B");
  Check(cudaMemcpy(device_b_pair, matrix_b_pair.data(), matrix_b_pair.size() * sizeof(uint32_t), cudaMemcpyHostToDevice), "copy paired B");
  CompareMma<<<kWarpCount, kWarpSize>>>(
      device_a, device_b, device_native, device_split_k8, device_split_k16);
  Check(cudaGetLastError(), "CompareMma launch");
  Check(cudaDeviceSynchronize(), "CompareMma execution");
  Check(cudaMemcpy(native_result.data(), device_native, native_result.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost), "copy native result");
  Check(cudaMemcpy(split_k8_result.data(), device_split_k8, split_k8_result.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost), "copy split k8 result");
  Check(cudaMemcpy(split_k16_result.data(), device_split_k16, split_k16_result.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost), "copy split k16 result");
    CompareMmaPair<<<kWarpCount, kWarpSize>>>(
      device_a, device_b, device_b_pair, device_native_pair, device_paired);
    Check(cudaGetLastError(), "CompareMmaPair launch");
    Check(cudaDeviceSynchronize(), "CompareMmaPair execution");
    Check(cudaMemcpy(native_pair_result.data(), device_native_pair,
      native_pair_result.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost),
      "copy native pair result");
    Check(cudaMemcpy(paired_result.data(), device_paired,
      paired_result.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost),
      "copy paired result");

  size_t k8_mismatches = 0;
  size_t k16_mismatches = 0;
  for (size_t index = 0; index != native_result.size(); ++index) {
    if (native_result[index] != split_k8_result[index]) {
      if (k8_mismatches < 8) {
        std::printf("k8 mismatch[%zu] native=%08x split=%08x\n",
            index, native_result[index], split_k8_result[index]);
      }
      ++k8_mismatches;
    }
    if (native_result[index] != split_k16_result[index]) {
      if (k16_mismatches < 8) {
        std::printf("k16 mismatch[%zu] native=%08x split=%08x\n",
            index, native_result[index], split_k16_result[index]);
      }
      ++k16_mismatches;
    }
  }
  std::printf("words=%zu k8_mismatches=%zu k16_mismatches=%zu\n",
      native_result.size(), k8_mismatches, k16_mismatches);
  size_t pair_mismatches = 0;
  for (size_t index = 0; index != native_pair_result.size(); ++index) {
    if (native_pair_result[index] != paired_result[index]) {
      if (pair_mismatches < 8) {
        std::printf("pair mismatch[%zu] native=%08x paired=%08x\n",
            index, native_pair_result[index], paired_result[index]);
      }
      ++pair_mismatches;
    }
  }
  std::printf("paired_words=%zu pair_mismatches=%zu\n",
      native_pair_result.size(), pair_mismatches);
  return decode_mismatches == 0 && compact_decode_mismatches == 0
      && encode_mismatches == 0 && compact_encode_mismatches == 0
      && k8_mismatches == 0 && k16_mismatches == 0
      && pair_mismatches == 0 ? 0 : 2;
}

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

__global__ void mma_m16n8k16(const uint32_t* a, const uint32_t* b,
                             const uint32_t* c, uint32_t* d) {
  const unsigned int index = blockIdx.x * 32 + threadIdx.x;
  const uint32_t* lane_a = a + index * 4;
  const uint32_t* lane_b = b + index * 2;
  const uint32_t* lane_c = c + index * 2;
  uint32_t d0;
  uint32_t d1;
  asm volatile(
      "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 "
      "{%0, %1}, {%2, %3, %4, %5}, {%6, %7}, {%8, %9};"
      : "=r"(d0), "=r"(d1)
      : "r"(lane_a[0]), "r"(lane_a[1]), "r"(lane_a[2]), "r"(lane_a[3]),
        "r"(lane_b[0]), "r"(lane_b[1]), "r"(lane_c[0]), "r"(lane_c[1]));
  d[index * 2] = d0;
  d[index * 2 + 1] = d1;
}

__global__ void mma_two_m16n8k8(const uint32_t* a, const uint32_t* b,
                                const uint32_t* c, uint32_t* d) {
  const unsigned int index = blockIdx.x * 32 + threadIdx.x;
  const uint32_t* lane_a = a + index * 4;
  const uint32_t* lane_b = b + index * 2;
  const uint32_t* lane_c = c + index * 2;
  uint32_t partial0;
  uint32_t partial1;
  asm volatile(
      "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
      "{%0, %1}, {%2, %3}, {%4}, {%5, %6};"
      : "=r"(partial0), "=r"(partial1)
      : "r"(lane_a[0]), "r"(lane_a[1]), "r"(lane_b[0]), "r"(lane_c[0]),
        "r"(lane_c[1]));
  uint32_t d0;
  uint32_t d1;
  asm volatile(
      "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
      "{%0, %1}, {%2, %3}, {%4}, {%5, %6};"
      : "=r"(d0), "=r"(d1)
      : "r"(lane_a[2]), "r"(lane_a[3]), "r"(lane_b[1]), "r"(partial0),
        "r"(partial1));
  d[index * 2] = d0;
  d[index * 2 + 1] = d1;
}

uint32_t pack_half2(float low, float high) {
  const __half low_half = __float2half(low);
  const __half high_half = __float2half(high);
  uint16_t low_bits;
  uint16_t high_bits;
  std::memcpy(&low_bits, &low_half, sizeof(low_bits));
  std::memcpy(&high_bits, &high_half, sizeof(high_bits));
  return low_bits | (static_cast<uint32_t>(high_bits) << 16);
}

int main() {
  constexpr unsigned int cases = 256;
  constexpr unsigned int lanes = cases * 32;
  std::vector<uint32_t> a(lanes * 4);
  std::vector<uint32_t> b(lanes * 2);
  std::vector<uint32_t> c(lanes * 2);
  for (unsigned int i = 0; i < a.size(); ++i) {
    a[i] = pack_half2(((i * 17 + 3) % 127 + 1) / 128.f,
                      ((i * 29 + 5) % 127 + 1) / 128.f);
  }
  for (unsigned int i = 0; i < b.size(); ++i) {
    b[i] = pack_half2(((i * 37 + 7) % 127 + 1) / 128.f,
                      ((i * 43 + 11) % 127 + 1) / 128.f);
    c[i] = pack_half2(((i * 13 + 2) % 31 + 1) / 256.f,
                      ((i * 19 + 4) % 31 + 1) / 256.f);
  }

  uint32_t* device_a;
  uint32_t* device_b;
  uint32_t* device_c;
  uint32_t* device_reference;
  uint32_t* device_split;
  cudaMalloc(&device_a, a.size() * sizeof(uint32_t));
  cudaMalloc(&device_b, b.size() * sizeof(uint32_t));
  cudaMalloc(&device_c, c.size() * sizeof(uint32_t));
  cudaMalloc(&device_reference, c.size() * sizeof(uint32_t));
  cudaMalloc(&device_split, c.size() * sizeof(uint32_t));
  cudaMemcpy(device_a, a.data(), a.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);
  cudaMemcpy(device_b, b.data(), b.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);
  cudaMemcpy(device_c, c.data(), c.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);

  mma_m16n8k16<<<cases, 32>>>(device_a, device_b, device_c, device_reference);
  mma_two_m16n8k8<<<cases, 32>>>(device_a, device_b, device_c, device_split);
  const cudaError_t launch_error = cudaDeviceSynchronize();
  if (launch_error != cudaSuccess) {
    std::fprintf(stderr, "CUDA failure: %s\n", cudaGetErrorString(launch_error));
    return 1;
  }

  std::vector<uint32_t> reference(c.size());
  std::vector<uint32_t> split(c.size());
  cudaMemcpy(reference.data(), device_reference, reference.size() * sizeof(uint32_t),
             cudaMemcpyDeviceToHost);
  cudaMemcpy(split.data(), device_split, split.size() * sizeof(uint32_t),
             cudaMemcpyDeviceToHost);

  unsigned int differing_half_values = 0;
  unsigned int max_positive_ulp_difference = 0;
  for (unsigned int i = 0; i < reference.size(); ++i) {
    for (unsigned int shift : {0u, 16u}) {
      const uint16_t expected = (reference[i] >> shift) & 0xffff;
      const uint16_t actual = (split[i] >> shift) & 0xffff;
      if (expected != actual) {
        ++differing_half_values;
        max_positive_ulp_difference = std::max(
            max_positive_ulp_difference,
            static_cast<unsigned int>(expected > actual ? expected - actual
                                                        : actual - expected));
      }
    }
  }

  std::printf("half-values=%zu differing=%u max-positive-ulp=%u\n",
              reference.size() * 2, differing_half_values,
              max_positive_ulp_difference);
  return max_positive_ulp_difference > 1 ? 2 : 0;
}

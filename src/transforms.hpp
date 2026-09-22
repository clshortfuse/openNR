#pragma once

#include <array>
#include <format>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "architectures.hpp"
#include "text.hpp"

namespace opennr {

namespace internal {

enum class PackedFp8Conversion {
  ENCODE_F16X2_TO_E4M3X2,
  DECODE_E4M3X2_TO_F16X2,
};

struct PackedFp8ConversionSignature {
  PackedFp8Conversion operation;
  std::string_view opcode;
  std::string_view destination_register_bank;
  std::string_view source_register_bank;
};

inline constexpr std::array PACKED_FP8_CONVERSIONS{
    PackedFp8ConversionSignature{
        .operation = PackedFp8Conversion::ENCODE_F16X2_TO_E4M3X2,
        .opcode = "cvt.rn.satfinite.e4m3x2.f16x2",
        .destination_register_bank = "%rs",
        .source_register_bank = "%r",
    },
    PackedFp8ConversionSignature{
        .operation = PackedFp8Conversion::DECODE_E4M3X2_TO_F16X2,
        .opcode = "cvt.rn.f16x2.e4m3x2",
        .destination_register_bank = "%r",
        .source_register_bank = "%rs",
    },
};

inline std::string LowerPackedFp8Conversions(std::string_view ptx) {
  // Regex locates a complete packed-FP8 conversion instruction. The operation
  // table below determines which opcode and typed operand signature is legal.
  auto lowered_ptx = Rewrite(ptx, R"(^\s*(\S*e4m3x2\S*)\s+([^;]+);\s*$)", [](const std::cmatch& match) {
    const auto opcode = StringViewFromCsubMatch(match[1]);
    const PackedFp8ConversionSignature* signature = nullptr;
    for (const auto& known_conversion : PACKED_FP8_CONVERSIONS) {
      if (known_conversion.opcode == opcode) {
        signature = &known_conversion;
        break;
      }
    }
    if (signature == nullptr) {
      throw std::runtime_error(std::format("Unsupported packed FP8 conversion opcode: {}", opcode));
    }

    const auto operands = SplitRegisters<2>(StringViewFromCsubMatch(match[2]));
    const auto destination = operands[0];
    const auto source = operands[1];
    if (!IsNumberedRegisterInBank(destination, signature->destination_register_bank) || !IsNumberedRegisterInBank(source, signature->source_register_bank)) {
      throw std::runtime_error(std::format("Invalid operands for {}: expected {} destination and {} source", opcode, signature->destination_register_bank, signature->source_register_bank));
    }

    switch (signature->operation) {
      case PackedFp8Conversion::ENCODE_F16X2_TO_E4M3X2:
        return std::format(R"(and.b32 %ax0, {1}, 0x7FFF7FFF;
set.nan.f16x2.f16x2 %ax1, %ax0, %ax0;
min.f16x2 %ax2, %ax0, %ax16;
max.f16x2 %ax2, %ax2, %ax17;
shr.u32 %ax3, %ax2, 7;
and.b32 %ax3, %ax3, 0x00010001;
add.s32 %ax4, %ax2, 0xE03EE03F;
add.s32 %ax4, %ax4, %ax3;
shr.u32 %ax4, %ax4, 7;
add.rn.f16x2 %ax5, %ax0, %ax18;
and.b32 %ax5, %ax5, 0x000F000F;
set.ge.f16x2.f16x2 %ax6, %ax0, %ax17;
shr.u32 %ax6, %ax6, 10;
mul.lo.s32 %ax6, %ax6, 0x11;
lop3.b32 %ax7, %ax4, %ax5, %ax6, 0xE4;
shr.u32 %ax8, %ax1, 10;
mul.lo.s32 %ax8, %ax8, 0x11;
lop3.b32 %ax7, %ax7, 0x007F007F, %ax8, 0xF8;
prmt.b32 %ax9, %ax7, 0, 0x4420;
prmt.b32 %ax10, {1}, 0, 0x4431;
lop3.b32 %ax9, %ax9, %ax10, 0x80808080, 0xF8;
cvt.u16.u32 {0}, %ax9;
)",
                           destination, source);
      case PackedFp8Conversion::DECODE_E4M3X2_TO_F16X2:
        return std::format(R"(cvt.u32.u16 %ax0, {1};
and.b32 %ax1, %ax0, 0x7F;
shl.b32 %ax1, %ax1, 7;
and.b32 %ax2, %ax0, 0x80;
shl.b32 %ax2, %ax2, 8;
and.b32 %ax3, %ax0, 0x7F00;
shl.b32 %ax3, %ax3, 15;
and.b32 %ax4, %ax0, 0x8000;
shl.b32 %ax4, %ax4, 16;
or.b32 %ax1, %ax1, %ax2;
or.b32 %ax3, %ax3, %ax4;
or.b32 %ax1, %ax1, %ax3;
mov.b32 %ax5, 0x5C005C00;
mul.rn.f16x2 {0}, %ax1, %ax5;
)",
                           destination, source);
    }
    throw std::runtime_error("Unknown packed FP8 conversion");
  });
  for (const auto& conversion : PACKED_FP8_CONVERSIONS) {
    if (lowered_ptx.find(conversion.opcode) != std::string::npos) {
      throw std::runtime_error(
          std::format("Unsupported operand shape for {}", conversion.opcode));
    }
  }
  return lowered_ptx;
}

}  // namespace internal

inline std::string GenerateApproximatePtx(const std::string& original_ptx, std::span<const RegisterCapOverride> register_caps = {}) {
  auto normalized_ptx = Replace(original_ptx, "\r\n", "\n");
  normalized_ptx = internal::RetargetPtx(normalized_ptx, "sm_120", "sm_86");
  static const std::regex ENTRIES(R"((?:^|\n)\.visible \.entry (\w+)\()");
  struct EntryLocation {
    size_t offset;
    std::string_view name;
  };
  std::vector<EntryLocation> entry_locations;
  for (auto it = std::cregex_iterator(normalized_ptx.data(), normalized_ptx.data() + normalized_ptx.size(), ENTRIES); it != std::cregex_iterator(); ++it) {
    const auto position = static_cast<size_t>(it->position());
    const auto entry_offset = position + (normalized_ptx[position] == '\n' ? 1 : 0);
    entry_locations.push_back({.offset = entry_offset, .name = StringViewFromCsubMatch((*it)[1])});
  }
  if (entry_locations.empty()) throw std::runtime_error("No PTX entries");
  std::string approximate_ptx = normalized_ptx.substr(0, entry_locations.front().offset);
  size_t bulk_copy_index = 0;
  for (size_t entry_index = 0; entry_index < entry_locations.size(); ++entry_index) {
    const auto entry_start = entry_locations[entry_index].offset;
    const auto entry_end = (entry_index + 1 < entry_locations.size() ? entry_locations[entry_index + 1].offset : normalized_ptx.size());
    const auto original_entry = std::string_view(normalized_ptx).substr(entry_start, entry_end - entry_start);
    static const std::regex RESERVED_REGISTER_BANK(R"(%(?:ax|ah|af|ap|aq|hx)(?:\d+|<))");
    if (std::regex_search(original_entry.begin(), original_entry.end(), RESERVED_REGISTER_BANK)) {
      throw std::runtime_error(std::format("Entry {} uses a register bank reserved by approximate lowering", entry_locations[entry_index].name));
    }
    std::map<std::string_view, std::array<std::string, 2>> unpacked_operands;
    size_t next_unpack_register = 0;
    size_t previous_mma_end = 0;
    // Replace each FP8 MMA with two FP16 MMAs. Reuse unpacked operands only
    // within the current label-bounded block while numbering registers entry-wide.
    auto rewritten_entry = Rewrite(original_entry, R"(mma\.sync\.aligned\.m16n8k32\.row\.col\.f16\.e4m3\.e4m3\.f16\s*\{([^}]+)\},\s*\{([^}]+)\},\s*\{([^}]+)\},\s*\{([^}]+)\};)", [&](const std::cmatch& match) {
      const auto preceding_ptx = original_entry.substr(previous_mma_end, match.position() - previous_mma_end);
      static const std::regex LABEL(R"((?:^|\n)\s*[\w$]+:)");
      if (std::regex_search(preceding_ptx.data(), preceding_ptx.data() + preceding_ptx.size(), LABEL)) {
        unpacked_operands.clear();
      }
      previous_mma_end = match.position() + match.length();
      const auto destination = SplitRegisters<2>(StringViewFromCsubMatch(match[1]));
      const auto matrix_a = SplitRegisters<4>(StringViewFromCsubMatch(match[2]));
      const auto matrix_b = SplitRegisters<2>(StringViewFromCsubMatch(match[3]));
      const auto accumulator = SplitRegisters<2>(StringViewFromCsubMatch(match[4]));
      std::string replacement;
      const std::array operands{matrix_a[0], matrix_a[1], matrix_a[2], matrix_a[3], matrix_b[0], matrix_b[1]};
      for (const auto& operand : operands) {
        if (unpacked_operands.contains(operand)) continue;
        const auto register_index = next_unpack_register;
        next_unpack_register += 6;
        unpacked_operands[operand] = {std::format("%hx{}", register_index), std::format("%hx{}", register_index + 1)};
        replacement += std::format(
            "and.b32 %hx{2}, {0}, 0x007F007F;\nshl.b32 %hx{2}, %hx{2}, 7;\n"
            "and.b32 %hx{3}, {0}, 0x00800080;\nshl.b32 %hx{3}, %hx{3}, 8;\n"
            "or.b32 %hx{1}, %hx{2}, %hx{3};\nmul.rn.f16x2 %hx{1}, %hx{1}, %ax23;\n"
            "and.b32 %hx{4}, {0}, 0x7F007F00;\nshr.u32 %hx{4}, %hx{4}, 1;\n"
            "and.b32 %hx{5}, {0}, 0x80008000;\nor.b32 %hx{6}, %hx{4}, %hx{5};\n"
            "mul.rn.f16x2 %hx{6}, %hx{6}, %ax23;\n",
            operand, register_index, register_index + 2, register_index + 3, register_index + 4, register_index + 5, register_index + 1);
      }
      for (size_t half_index = 0; half_index < 2; ++half_index) {
        const auto& input_accumulator = (half_index == 0 ? accumulator : destination);
        replacement += std::format("mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {{{}, {}}}, {{{}, {}, {}, {}}}, {{{}, {}}}, {{{}, {}}};\n",
                                   destination[0], destination[1], unpacked_operands[matrix_a[2 * half_index]][0], unpacked_operands[matrix_a[2 * half_index + 1]][0],
                                   unpacked_operands[matrix_a[2 * half_index]][1], unpacked_operands[matrix_a[2 * half_index + 1]][1], unpacked_operands[matrix_b[half_index]][0],
                                   unpacked_operands[matrix_b[half_index]][1], input_accumulator[0], input_accumulator[1]);
      }
      return replacement;
    });
    // Preserve the packed one-byte FP8 storage ABI while replacing conversion
    // instructions unavailable on SM86.
    rewritten_entry = internal::LowerPackedFp8Conversions(rewritten_entry);
    std::string prologue = R"(
.reg .b32 %ax<24>;
.reg .b16 %ah<8>;
.reg .f32 %af<8>;
.reg .pred %ap<8>;
.reg .b64 %aq<4>;
mov.b32 %ax16, 0x5F005F00;
mov.b32 %ax17, 0x24002400;
mov.b32 %ax18, 0x40004000;
)";
    if (next_unpack_register != 0) {
      prologue += std::format(".reg .b32 %hx<{}>;\nmov.b32 %ax23, 0x5C005C00;\n", next_unpack_register);
    }
    rewritten_entry.insert(rewritten_entry.find('{') + 1, prologue);
    // Lower SM120-only coordination, bulk-copy, vector-reduction and fused
    // arithmetic instructions to the accepted SM86 equivalents.
    rewritten_entry = internal::LowerWarpLeaderElection(rewritten_entry, internal::CompatibilityTarget::AMPERE_APPROXIMATION);
    rewritten_entry = Rewrite(rewritten_entry, R"(cp\.async\.bulk\.shared::cta\.global\.mbarrier::complete_tx::bytes \[(%r\d+)\], \[(%rd\d+)\], (%r\d+), \[(%r\d+)\];)", [&](const std::cmatch& match) {
      return std::format(R"(mov.u32 %ax20, %laneid;
shl.b32 %ax20, %ax20, 4;
$L__amb_{4}:
setp.ge.u32 %ap7, %ax20, {2};
@%ap7 bra $L__ambd_{4};
add.s32 %ax21, {0}, %ax20;
cvt.u64.u32 %aq0, %ax20;
add.s64 %aq1, {1}, %aq0;
cp.async.cg.shared.global [%ax21], [%aq1], 16;
add.s32 %ax20, %ax20, 512;
bra $L__amb_{4};
$L__ambd_{4}:
cp.async.mbarrier.arrive.shared.b64 [{3}];
)",
                         StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]), StringViewFromCsubMatch(match[3]), StringViewFromCsubMatch(match[4]), bulk_copy_index++);
    });
    rewritten_entry = internal::RemoveBulkCopyTransactionExpectation(rewritten_entry);
    rewritten_entry = internal::LowerBarrierArrival(rewritten_entry);
    rewritten_entry = internal::LowerBarrierWaitForAmpere(rewritten_entry);
    rewritten_entry = internal::LowerVectorHalfReduction(rewritten_entry, internal::CompatibilityTarget::AMPERE_APPROXIMATION);
    rewritten_entry = internal::AddAcquireSemanticsToGpuReleaseFence(rewritten_entry);
    rewritten_entry = internal::LowerSignedMinimumWithRelu(rewritten_entry);
    approximate_ptx += rewritten_entry;
  }
  while (!approximate_ptx.empty() && approximate_ptx.back() == '\0') {
    approximate_ptx.pop_back();
  }
  internal::ApplyRegisterCapOverrides(&approximate_ptx, register_caps);
  approximate_ptx.push_back('\0');
  return approximate_ptx;
}

}  // namespace opennr
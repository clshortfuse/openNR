#pragma once

#include <array>
#include <format>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "text.hpp"

namespace opennr {

struct RegisterCapOverride {
  std::string entry_name;
  int expected;
  int replacement;
};

// Older architectures implement the acquire half through a stronger acq_rel
// fence at the same GPU scope. Storage and synchronization scope are preserved.
inline std::string StrengthenGpuAcquireFences(std::string_view ptx) {
  return Replace(std::string(ptx), "fence.acquire.gpu;", "fence.acq_rel.gpu;");
}

namespace internal {

inline std::string RewriteSinglePtxDirective(std::string_view ptx, std::string_view directive, std::string_view replacement) {
  size_t matches = 0;
  const std::regex expression(std::format(R"((?:^|\n)\.{}[^\r\n]*)", directive));
  auto rewritten_ptx = Rewrite(ptx, expression, [&](const std::cmatch& match) {
    ++matches;
    const auto matched_directive = StringViewFromCsubMatch(match[0]);
    return std::format("{}{}", matched_directive.starts_with('\n') ? "\n" : "", replacement);
  });
  if (matches != 1) {
    throw std::runtime_error(std::format("Expected exactly one PTX {} directive, found {}", directive, matches));
  }
  return rewritten_ptx;
}

inline std::string RetargetPtx(std::string_view ptx, std::string_view source_architecture, std::string_view target_architecture) {
  const std::regex target_expression(R"((?:^|\n)\.target\s+([^,\s]+)[^\r\n]*)");
  std::cmatch target_match;
  if (!std::regex_search(ptx.data(), ptx.data() + ptx.size(), target_match, target_expression)) {
    throw std::runtime_error("Missing PTX target directive");
  }
  if (StringViewFromCsubMatch(target_match[1]) != source_architecture) {
    throw std::runtime_error(std::format("Expected PTX target {}, found {}", source_architecture, StringViewFromCsubMatch(target_match[1])));
  }
  auto retargeted_ptx = RewriteSinglePtxDirective(ptx, "target", std::format(".target {}", target_architecture));
  return RewriteSinglePtxDirective(retargeted_ptx, "version", ".version 9.3");
}

inline void ApplyRegisterCapOverrides(std::string* ptx, std::span<const RegisterCapOverride> register_caps) {
  for (const auto& register_cap : register_caps) {
    const auto entry_signature = ".visible .entry " + register_cap.entry_name + "(";
    const auto entry_start = ptx->find(entry_signature);
    if (entry_start == std::string::npos
        || ptx->find(entry_signature, entry_start + entry_signature.size())
               != std::string::npos) {
      throw std::runtime_error(
          std::format("Expected exactly one PTX entry named {}", register_cap.entry_name));
    }
    const auto entry_end = ptx->find(".visible .entry ", entry_start + entry_signature.size());
    const auto cap_position = ptx->find(".maxnreg ", entry_start);
    const auto expected_cap = std::format(".maxnreg {}", register_cap.expected);
    if (cap_position == std::string::npos
        || (entry_end != std::string::npos && cap_position >= entry_end)
        || ptx->substr(cap_position, expected_cap.size()) != expected_cap) {
      throw std::runtime_error(std::format("Register cap precondition failed for {}", register_cap.entry_name));
    }
    ptx->replace(cap_position, expected_cap.size(), std::format(".maxnreg {}", register_cap.replacement));
  }
}

enum class TuringMmaLowering {
  SPLIT_M16N8K16_INTO_M16N8K8,
};

struct MmaOperationSignature {
  TuringMmaLowering lowering;
  std::string_view opcode;
  std::array<std::string_view, 1> destination_register_banks;
  std::array<std::string_view, 2> matrix_a_register_banks;
  std::array<std::string_view, 2> matrix_b_register_banks;
  std::array<std::string_view, 1> accumulator_register_banks;
};

inline constexpr std::array TURING_MMA_OPERATIONS{
    MmaOperationSignature{
        .lowering = TuringMmaLowering::SPLIT_M16N8K16_INTO_M16N8K8,
        .opcode = "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
        .destination_register_banks = {"%r"},
        .matrix_a_register_banks = {"%r", "%hx"},
        .matrix_b_register_banks = {"%r", "%hx"},
        .accumulator_register_banks = {"%r"},
    },
};

struct M16n8k16Operands {
  std::array<std::string_view, 2> destination;
  std::array<std::string_view, 4> matrix_a;
  std::array<std::string_view, 2> matrix_b;
  std::array<std::string_view, 2> accumulator;
};

inline M16n8k16Operands ParseM16n8k16Operands(const std::cmatch& match, const MmaOperationSignature& signature) {
  M16n8k16Operands operands{
      .destination = SplitRegisters<2>(StringViewFromCsubMatch(match[2])),
      .matrix_a = SplitRegisters<4>(StringViewFromCsubMatch(match[3])),
      .matrix_b = SplitRegisters<2>(StringViewFromCsubMatch(match[4])),
      .accumulator = SplitRegisters<2>(StringViewFromCsubMatch(match[5])),
  };
  const auto require_register_banks = [&signature](const auto& fragment, std::string_view fragment_name, const auto& allowed_banks) {
    for (const auto register_name : fragment) {
      bool bank_is_allowed = false;
      for (const auto register_bank : allowed_banks) {
        if (IsNumberedRegisterInBank(register_name, register_bank)) {
          bank_is_allowed = true;
          break;
        }
      }
      if (!bank_is_allowed) {
        throw std::runtime_error(std::format("Invalid {} operand for {}", fragment_name, signature.opcode));
      }
    }
  };
  require_register_banks(operands.destination, "destination", signature.destination_register_banks);
  require_register_banks(operands.matrix_a, "matrix A", signature.matrix_a_register_banks);
  require_register_banks(operands.matrix_b, "matrix B", signature.matrix_b_register_banks);
  require_register_banks(operands.accumulator, "accumulator", signature.accumulator_register_banks);
  return operands;
}

inline std::string LowerMmaForTuring(std::string_view ptx) {
  // Match the complete m16n8k16 instruction shape, then select and validate a
  // declared operation signature before lowering its typed operand fragments.
  auto lowered_ptx = Rewrite(ptx, R"((mma\.sync\.aligned\.m16n8k16\.\S+)\s*\{([^}]+)\},\s*\{([^}]+)\},\s*\{([^}]+)\},\s*\{([^}]+)\};)", [](const std::cmatch& match) {
    const auto opcode = StringViewFromCsubMatch(match[1]);
    const MmaOperationSignature* signature = nullptr;
    for (const auto& known_operation : TURING_MMA_OPERATIONS) {
      if (known_operation.opcode == opcode) {
        signature = &known_operation;
        break;
      }
    }
    if (signature == nullptr) {
      throw std::runtime_error(std::format("Unsupported m16n8k16 MMA operation: {}", opcode));
    }

    switch (signature->lowering) {
      case TuringMmaLowering::SPLIT_M16N8K16_INTO_M16N8K8: {
        const auto operands = ParseM16n8k16Operands(match, *signature);
        return std::format(
            "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {{{0}, {1}}}, {{{2}, {3}}}, {{{6}}}, {{{8}, {9}}};\n"
            "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {{{0}, {1}}}, {{{4}, {5}}}, {{{7}}}, {{{0}, {1}}};",
            operands.destination[0], operands.destination[1], operands.matrix_a[0], operands.matrix_a[1], operands.matrix_a[2], operands.matrix_a[3], operands.matrix_b[0], operands.matrix_b[1], operands.accumulator[0], operands.accumulator[1]);
      }
    }
    throw std::runtime_error("Unknown Turing MMA lowering");
  });
  if (lowered_ptx.find("mma.sync.aligned.m16n8k16") != std::string::npos) {
    throw std::runtime_error("Unsupported m16n8k16 MMA instruction shape");
  }
  return lowered_ptx;
}

enum class RemovedTuringSynchronization {
  COPY_BARRIER_ARRIVAL,
  COPY_GROUP_COMMIT,
  COPY_GROUP_WAIT,
  BARRIER_INITIALIZATION,
};

struct RemovedTuringSynchronizationSignature {
  RemovedTuringSynchronization operation;
  std::string_view opcode;
};

inline constexpr std::array REMOVED_TURING_SYNCHRONIZATION_OPERATIONS{
    RemovedTuringSynchronizationSignature{
        .operation = RemovedTuringSynchronization::COPY_BARRIER_ARRIVAL,
        .opcode = "cp.async.mbarrier.arrive.shared.b64",
    },
    RemovedTuringSynchronizationSignature{
        .operation = RemovedTuringSynchronization::COPY_GROUP_COMMIT,
        .opcode = "cp.async.commit_group",
    },
    RemovedTuringSynchronizationSignature{
        .operation = RemovedTuringSynchronization::COPY_GROUP_WAIT,
        .opcode = "cp.async.wait_group",
    },
    RemovedTuringSynchronizationSignature{
        .operation = RemovedTuringSynchronization::BARRIER_INITIALIZATION,
        .opcode = "mbarrier.init.shared.b64",
    },
};

inline std::string_view TrimPtxOperand(std::string_view operand) {
  const auto first = operand.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return operand.substr(first, operand.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string_view ParseSharedRegisterAddress(std::string_view operand, std::string_view opcode) {
  operand = TrimPtxOperand(operand);
  if (operand.size() < 3 || operand.front() != '[' || operand.back() != ']') {
    throw std::runtime_error(std::format("Invalid shared address operand for {}", opcode));
  }
  const auto register_name = TrimPtxOperand(operand.substr(1, operand.size() - 2));
  if (!IsNumberedRegisterInBank(register_name, "%r")) {
    throw std::runtime_error(std::format("Invalid shared address register for {}", opcode));
  }
  return register_name;
}

inline void ValidateRemovedTuringSynchronizationOperands(RemovedTuringSynchronization operation, std::string_view opcode, std::string_view operands) {
  operands = TrimPtxOperand(operands);
  switch (operation) {
    case RemovedTuringSynchronization::COPY_BARRIER_ARRIVAL:
      ParseSharedRegisterAddress(operands, opcode);
      return;
    case RemovedTuringSynchronization::COPY_GROUP_COMMIT:
      if (operands.empty()) {
        return;
      }
      break;
    case RemovedTuringSynchronization::COPY_GROUP_WAIT:
      if (operands == "0") {
        return;
      }
      break;
    case RemovedTuringSynchronization::BARRIER_INITIALIZATION: {
      const auto separator = operands.find(',');
      if (separator != std::string_view::npos && operands.find(',', separator + 1) == std::string_view::npos) {
        ParseSharedRegisterAddress(operands.substr(0, separator), opcode);
        const auto arrival_count = TrimPtxOperand(operands.substr(separator + 1));
        if (IsNumberedRegisterInBank(arrival_count, "%r")) {
          return;
        }
      }
      break;
    }
  }
  throw std::runtime_error(std::format("Invalid operands for removed Turing synchronization operation {}", opcode));
}

inline std::string RemoveTuringSynchronization(std::string_view ptx) {
  return Rewrite(ptx, R"(((?:cp\.async|mbarrier)\.[^\s;]+)(?:\s+([^;]*))?;)", [](const std::cmatch& match) {
    const auto opcode = StringViewFromCsubMatch(match[1]);
    for (const auto& signature : REMOVED_TURING_SYNCHRONIZATION_OPERATIONS) {
      if (signature.opcode == opcode) {
        ValidateRemovedTuringSynchronizationOperands(signature.operation, opcode, StringViewFromCsubMatch(match[2]));
        return std::string{};
      }
    }
    return std::string(StringViewFromCsubMatch(match[0]));
  });
}

enum class PackedMinMaxOperation {
  MINIMUM,
  MAXIMUM,
};

struct PackedMinMaxSignature {
  PackedMinMaxOperation operation;
  std::string_view opcode;
  std::string_view comparison;
  std::array<std::string_view, 2> register_banks;
};

inline constexpr std::array TURING_PACKED_MINMAX_OPERATIONS{
    PackedMinMaxSignature{
        .operation = PackedMinMaxOperation::MINIMUM,
        .opcode = "min.f16x2",
        .comparison = "lt",
        .register_banks = {"%r", "%ax"},
    },
    PackedMinMaxSignature{
        .operation = PackedMinMaxOperation::MAXIMUM,
        .opcode = "max.f16x2",
        .comparison = "gt",
        .register_banks = {"%r", "%ax"},
    },
};

inline std::string LowerPackedMinMaxInstruction(const std::cmatch& match) {
  const auto opcode = StringViewFromCsubMatch(match[1]);
  const PackedMinMaxSignature* signature = nullptr;
  for (const auto& known_operation : TURING_PACKED_MINMAX_OPERATIONS) {
    if (known_operation.opcode == opcode) {
      signature = &known_operation;
      break;
    }
  }
  if (signature == nullptr) {
    throw std::runtime_error(std::format("Unsupported packed min/max operation: {}", opcode));
  }

  const auto destination = StringViewFromCsubMatch(match[2]);
  const auto source_a = StringViewFromCsubMatch(match[3]);
  const auto source_b = StringViewFromCsubMatch(match[4]);
  for (const auto operand : {destination, source_a, source_b}) {
    bool register_bank_is_supported = false;
    for (const auto register_bank : signature->register_banks) {
      if (IsNumberedRegisterInBank(operand, register_bank)) {
        register_bank_is_supported = true;
        break;
      }
    }
    if (!register_bank_is_supported) {
      throw std::runtime_error(std::format("Invalid operand register bank for {}", opcode));
    }
  }

  const std::array operations{
      std::string("{"),
      std::string("  .reg .pred TURING_LO_SELECT, TURING_HI_SELECT;"),
      std::string("  .reg .f16 TURING_A_LO, TURING_A_HI, TURING_B_LO, TURING_B_HI;"),
      std::string("  .reg .f16 TURING_D_LO, TURING_D_HI;"),
      std::format("  mov.b32 {{TURING_A_LO, TURING_A_HI}}, {};", source_a),
      std::format("  mov.b32 {{TURING_B_LO, TURING_B_HI}}, {};", source_b),
      std::format("  setp.{}.f16 TURING_LO_SELECT, TURING_A_LO, TURING_B_LO;", signature->comparison),
      std::format("  setp.{}.f16 TURING_HI_SELECT, TURING_A_HI, TURING_B_HI;", signature->comparison),
      std::string("  selp.b16 TURING_D_LO, TURING_A_LO, TURING_B_LO, TURING_LO_SELECT;"),
      std::string("  selp.b16 TURING_D_HI, TURING_A_HI, TURING_B_HI, TURING_HI_SELECT;"),
      std::format("  mov.b32 {}, {{TURING_D_LO, TURING_D_HI}};", destination),
      std::string("}"),
  };
  std::string lowered_operations;
  for (const auto& operation : operations) {
    if (!lowered_operations.empty()) {
      lowered_operations += '\n';
    }
    lowered_operations += operation;
  }
  return lowered_operations;
}

inline std::string LowerPackedMinMaxForTuring(std::string_view ptx) {
  const std::string instruction_pattern = R"(((?:min|max)\.f16x2)\s+(%[A-Za-z]+\d+)\s*,\s*(%[A-Za-z]+\d+)\s*,\s*(%[A-Za-z]+\d+)\s*;)";
  auto lowered_ptx = Rewrite(ptx, "\\{\\s*" + instruction_pattern + "\\s*\\}", LowerPackedMinMaxInstruction);
  return Rewrite(lowered_ptx, "^\\s*" + instruction_pattern + "\\s*$", LowerPackedMinMaxInstruction);
}

enum class CompatibilityTarget {
  AMPERE_APPROXIMATION,
  ADA,
};

inline std::string RequireOperationFullyLowered(std::string lowered_ptx, std::string_view source_opcode) {
  if (lowered_ptx.find(source_opcode) != std::string::npos) {
    throw std::runtime_error(std::format("Unsupported operand shape for {}", source_opcode));
  }
  return lowered_ptx;
}

inline std::string LowerWarpLeaderElection(std::string_view ptx, CompatibilityTarget target) {
  auto lowered_ptx = Rewrite(ptx, R"(elect\.sync _\|P_OUT, (%r\d+);)", [target](const std::cmatch& match) {
    if (target == CompatibilityTarget::AMPERE_APPROXIMATION) {
      return std::format("setp.eq.u32 P_OUT, {0}, {0};", StringViewFromCsubMatch(match[1]));
    }
    return std::string("mov.pred P_OUT, -1;");
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "elect.sync");
}

inline std::string RemoveBulkCopyTransactionExpectation(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(mbarrier\.expect_tx\.relaxed\.cta\.shared::cta\.b64 \[(%r\d+)\], (%r\d+);)", [](const std::cmatch&) { return std::string{}; });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.expect_tx.relaxed.cta.shared::cta.b64");
}

inline std::string LowerBarrierArrival(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(mbarrier\.arrive\.shared::cta\.b64 (%rd\d+), \[(%r\d+)\], (%r\d+);)", [](const std::cmatch& match) {
    return std::format("mbarrier.arrive.shared.b64 {}, [{}];", StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]));
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.arrive.shared::cta.b64");
}

inline std::string LowerBarrierWaitForAmpere(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(mbarrier\.try_wait\.shared::cta\.b64 (P_[A-Za-z0-9_]+), \[(%r\d+)\], (%rd\d+);)", [](const std::cmatch& match) {
    return std::format("mbarrier.test_wait.shared.b64 {}, [{}], {};", StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]), StringViewFromCsubMatch(match[3]));
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.try_wait.shared::cta.b64");
}

inline std::string LowerBarrierWaitForAda(std::string_view ptx) {
  int wait_label_index = 0;
  auto lowered_ptx = Rewrite(ptx, R"(mbarrier\.try_wait\.shared::cta\.b64 (P_[A-Za-z0-9_]+), \[(%r\d+)\], (%rd\d+);)", [&](const std::cmatch& match) {
    return std::format("$L__ADA_WAIT_{0}:\nmbarrier.test_wait.shared.b64 {1}, [{2}], {3};\n@!{1} bra $L__ADA_WAIT_{0};", ++wait_label_index, StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]), StringViewFromCsubMatch(match[3]));
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.try_wait.shared::cta.b64");
}

inline std::string LowerVectorHalfReduction(std::string_view ptx, CompatibilityTarget target) {
  auto lowered_ptx = Rewrite(ptx, R"(red\.global\.v4\.f16x2\.add\.noftz \[(%rd\d+)\], \{([^}]+)\};)", [target](const std::cmatch& match) {
    const auto values = SplitRegisters<4>(StringViewFromCsubMatch(match[2]));
    std::string lowered_reductions;
    for (size_t i = 0; i < values.size(); ++i) {
      if (target == CompatibilityTarget::AMPERE_APPROXIMATION) {
        lowered_reductions += std::format("red.global.add.noftz.f16x2 [{}+{}], {};\n", StringViewFromCsubMatch(match[1]), i * 4, values[i]);
      } else {
        if (i != 0) {
          lowered_reductions += '\n';
        }
        const auto offset = (i == 0 ? std::string{} : std::format("+{}", i * 4));
        lowered_reductions += std::format("red.global.f16x2.add.noftz [{}{}], {};", StringViewFromCsubMatch(match[1]), offset, values[i]);
      }
    }
    return lowered_reductions;
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "red.global.v4.f16x2.add.noftz");
}

inline std::string AddAcquireSemanticsToGpuReleaseFence(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(fence\.release\.gpu;)", [](const std::cmatch&) { return "fence.acq_rel.gpu;"; });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "fence.release.gpu");
}

inline std::string LowerSignedMinimumWithRelu(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(min\.relu\.s32 (%r\d+), (%r\d+), (%r\d+);)", [](const std::cmatch& match) {
    return std::format("min.s32 {0}, {1}, {2};\nmax.s32 {0}, {0}, 0;", StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]), StringViewFromCsubMatch(match[3]));
  });
  return RequireOperationFullyLowered(std::move(lowered_ptx), "min.relu.s32");
}

inline std::string LowerAsynchronousCopyForTuring(std::string_view ptx) {
  auto lowered_ptx = Rewrite(ptx, R"(cp\.async\.cg\.shared\.global \[(%[A-Za-z]+\d+)\], \[(%[A-Za-z]+\d+)\], 16;)", [](const std::cmatch& match) {
    return std::format(
        "{{\n.reg .b32 TURING_COPY_WORD_0, TURING_COPY_WORD_1, TURING_COPY_WORD_2, TURING_COPY_WORD_3;\n"
        "ld.global.cg.v4.u32 {{TURING_COPY_WORD_0, TURING_COPY_WORD_1, TURING_COPY_WORD_2, TURING_COPY_WORD_3}}, [{1}];\n"
        "st.shared.v4.u32 [{0}], {{TURING_COPY_WORD_0, TURING_COPY_WORD_1, TURING_COPY_WORD_2, TURING_COPY_WORD_3}};\n}}",
        StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]));
  });
  lowered_ptx = Rewrite(lowered_ptx, R"(cp\.async\.ca\.shared\.global \[(%[A-Za-z]+\d+)\], \[(%[A-Za-z]+\d+)\], 4;)", [](const std::cmatch& match) {
    return std::format("{{\n.reg .b32 TURING_COPY_WORD;\nld.global.ca.u32 TURING_COPY_WORD, [{}];\nst.shared.u32 [{}], TURING_COPY_WORD;\n}}", StringViewFromCsubMatch(match[2]), StringViewFromCsubMatch(match[1]));
  });
  lowered_ptx = RemoveTuringSynchronization(lowered_ptx);
  lowered_ptx = Rewrite(lowered_ptx, R"(mbarrier\.arrive\.shared\.b64 (%[A-Za-z]+\d+), \[(%[A-Za-z]+\d+)\];)", [](const std::cmatch& match) {
    return std::format("bar.sync 0;\nmov.b64 {}, 0;", StringViewFromCsubMatch(match[1]));
  });
  lowered_ptx = Rewrite(lowered_ptx, R"(mbarrier\.test_wait\.shared\.b64 P_OUT, \[(%[A-Za-z]+\d+)\], (%[A-Za-z]+\d+);)", [](const std::cmatch&) { return "mov.pred P_OUT, -1;"; });
  lowered_ptx = RequireOperationFullyLowered(std::move(lowered_ptx), "cp.async.cg.shared.global");
  lowered_ptx = RequireOperationFullyLowered(std::move(lowered_ptx), "cp.async.ca.shared.global");
  lowered_ptx = RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.arrive.shared.b64");
  return RequireOperationFullyLowered(std::move(lowered_ptx), "mbarrier.test_wait.shared.b64");
}

}  // namespace internal

inline std::string LowerApproximatePtx(std::string approximate_ptx, int architecture, std::span<const RegisterCapOverride> register_caps = {}) {
  while (!approximate_ptx.empty() && approximate_ptx.back() == '\0') {
    approximate_ptx.pop_back();
  }
  approximate_ptx = internal::RetargetPtx(approximate_ptx, "sm_86", std::format("sm_{}", architecture));
  if (approximate_ptx.empty() || approximate_ptx.back() != '\n') {
    approximate_ptx += '\n';
  }
  if (architecture == 75) {
    // Split Ampere m16n8k16 MMA operations into Turing's two m16n8k8 steps
    // and replace asynchronous-copy synchronization with synchronous copies.
    approximate_ptx = internal::LowerMmaForTuring(approximate_ptx);
    approximate_ptx = internal::LowerPackedMinMaxForTuring(approximate_ptx);
    approximate_ptx = internal::LowerAsynchronousCopyForTuring(approximate_ptx);
  }
  internal::ApplyRegisterCapOverrides(&approximate_ptx, register_caps);
  return approximate_ptx;
}

inline std::string LowerOriginalPtxForAda(std::string original_ptx) {
  auto lowered_ptx = Replace(original_ptx, "\r\n", "\n");
  lowered_ptx = internal::RetargetPtx(lowered_ptx, "sm_120", "sm_89");
  lowered_ptx = internal::LowerWarpLeaderElection(lowered_ptx, internal::CompatibilityTarget::ADA);
  constexpr int WARP_LANES = 32;
  constexpr int BYTES_PER_LANE_COPY = 16;
  constexpr int BYTES_PER_WARP_COPY = WARP_LANES * BYTES_PER_LANE_COPY;
  // Infer each cooperative copy's byte count from its entry-local assignment,
  // then distribute synchronous lane copies across the warp.
  const std::string_view bulk_copy_source_snapshot = lowered_ptx;
  lowered_ptx = Rewrite(lowered_ptx, R"(cp\.async\.bulk\.shared::cta\.global\.mbarrier::complete_tx::bytes \[(%r\d+)\], \[(%rd\d+)\], (%r\d+), \[(%r\d+)\];)", [&](const std::cmatch& match) {
    const auto entry_start = bulk_copy_source_snapshot.rfind(".visible .entry ", match.position());
    const auto preceding_entry_ptx = bulk_copy_source_snapshot.substr(entry_start, match.position() - entry_start);
    const std::regex size_assignments(std::format("(?:^|\\n)mov\\.b32 {}, ([1-9][0-9]*);(?=\\n|$)", StringViewFromCsubMatch(match[3])));
    int copy_size_bytes = 0;
    for (auto it = std::cregex_iterator(preceding_entry_ptx.data(), preceding_entry_ptx.data() + preceding_entry_ptx.size(), size_assignments); it != std::cregex_iterator(); ++it) {
      copy_size_bytes = std::stoi(std::string(StringViewFromCsubMatch((*it)[1])));
    }
    if (copy_size_bytes == 0 || copy_size_bytes % BYTES_PER_WARP_COPY != 0) {
      throw std::runtime_error(std::format("Ada copy size must be a positive multiple of {} bytes", BYTES_PER_WARP_COPY));
    }
    std::string replacement = "{\n.reg .b32 ADA_LANE, ADA_OFFSET, ADA_DESTINATION;\n.reg .b64 ADA_OFFSET_64, ADA_SOURCE;\nmov.u32 ADA_LANE, %laneid;\nshl.b32 ADA_OFFSET, ADA_LANE, 4;\n";
    for (int copy_offset = 0; copy_offset < copy_size_bytes; copy_offset += BYTES_PER_WARP_COPY) {
      replacement += std::format("add.u32 ADA_DESTINATION, {}, ADA_OFFSET;\ncvt.u64.u32 ADA_OFFSET_64, ADA_OFFSET;\nadd.u64 ADA_SOURCE, {}, ADA_OFFSET_64;\ncp.async.ca.shared.global [ADA_DESTINATION], [ADA_SOURCE], 16;", StringViewFromCsubMatch(match[1]), StringViewFromCsubMatch(match[2]));
      if (copy_offset + BYTES_PER_WARP_COPY < copy_size_bytes) {
        replacement += std::format("\nadd.u32 ADA_OFFSET, ADA_OFFSET, {};\n", BYTES_PER_WARP_COPY);
      }
    }
    return replacement + "\ncp.async.commit_group;\ncp.async.wait_group 0;\n}";
  });
  lowered_ptx = internal::RemoveBulkCopyTransactionExpectation(lowered_ptx);
  lowered_ptx = internal::LowerBarrierArrival(lowered_ptx);
  lowered_ptx = internal::LowerBarrierWaitForAda(lowered_ptx);
  lowered_ptx = internal::LowerVectorHalfReduction(lowered_ptx, internal::CompatibilityTarget::ADA);
  lowered_ptx = internal::AddAcquireSemanticsToGpuReleaseFence(lowered_ptx);
  lowered_ptx = internal::LowerSignedMinimumWithRelu(lowered_ptx);
  return lowered_ptx;
}

}  // namespace opennr
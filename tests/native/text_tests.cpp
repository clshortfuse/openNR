#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

#include "architectures.hpp"
#include "binary.hpp"
#include "io.hpp"
#include "text.hpp"
#include "transforms.hpp"

int main() {
  using namespace opennr;
  try {
    const auto check = [](bool condition, const char* message) {
      if (!condition) throw std::runtime_error(message);
    };
    const std::string input = "outside:%r1, %r2:outside";
    const auto slice = std::string_view(input).substr(8, 8);
    const auto registers = SplitRegisters<2>(slice);
    check(registers[0].data() == input.data() + 8, "Register copied instead of borrowed");
    check(registers[1].data() == input.data() + 13, "Second register view has wrong offset");
    const std::regex register_expression(R"((%r\d+)(unused)?)");
    size_t captures = 0;
    const auto output = Rewrite(slice, register_expression, [&](const std::cmatch& match) {
      const auto capture = StringViewFromCsubMatch(match[1]);
      check(capture.data() == registers[captures++].data(), "Capture copied instead of borrowed");
      check(StringViewFromCsubMatch(match[2]).empty(), "Unmatched capture not empty");
      return capture;
    });
    check(output == slice && captures == 2, "Rewrite escaped slice or changed bytes");
    check(Rewrite(std::string_view{}, std::regex("^$"), [](const std::cmatch&) { return "empty"; }) == "empty", "Null empty view handling");
    check(Rewrite("ab", std::regex(""), [](const std::cmatch&) { return "|"; }) == "|a|b|", "Zero-length matches");
    check(Rewrite("first\nvalue\nlast", std::string(R"(^value$)"), [](const std::cmatch&) { return "changed"; }) == "first\nchanged\nlast", "Multiline replacement");
    check(Rewrite("value\nvalue", std::string(R"(^value$)"), [](const std::cmatch&) { return "changed"; }) == "changed\nchanged", "Multiline boundary replacement");
    const std::string nul_input("a\0b", 3);
    check(Rewrite(nul_input, std::regex("b"), [](const std::cmatch&) { return "c"; }) == std::string("a\0c", 3), "Embedded NUL truncated");
    check(Replace("aaaa", "aa", "b") == "bb", "Replacement overlap");
    check(Replace("abc", "x", "y") == "abc", "Unmatched replacement");
    bool rejected = false;
    try {
      Replace("a", "", "b");
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "Empty literal pattern accepted");
    for (const auto malformed : {"%r0", "%r0,%r1,%r2", "%r0,", "%r0,not_a_register", "%1,%r1", "%r-0,%r1", "%r.0,%r1"}) {
      rejected = false;
      try {
        SplitRegisters<2>(malformed);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      check(rejected, "Malformed register fragment accepted");
    }
    const auto lowered_conversions = internal::LowerPackedFp8Conversions(
        "cvt.rn.satfinite.e4m3x2.f16x2 %rs2, %r53;\n"
        "cvt.rn.f16x2.e4m3x2 %r715, %rs41;");
    check(lowered_conversions.find("cvt.u16.u32 %rs2, %ax9;") != std::string::npos, "Packed FP8 encode was not lowered");
    check(lowered_conversions.find("mul.rn.f16x2 %r715, %ax1, %ax5;") != std::string::npos, "Packed FP8 decode was not lowered");
    check(
        internal::LowerPackedFp8Conversions(
            "  cvt.rn.satfinite.e4m3x2.f16x2 %rs2, %r53;")
                .find("cvt.u16.u32 %rs2, %ax9;")
            != std::string::npos,
        "Indented packed FP8 conversion was not lowered");
    for (const auto malformed_conversion : {
             "cvt.rn.satfinite.e4m3x2.f16x2 %r2, %r53;",
             "cvt.rn.satfinite.e4m3x2.f16x2 %rs2, %rs53;",
             "cvt.rn.satfinite.e4m3x2.f16x2 %rs2, %r53, %r54;",
             "cvt.rn.e4m3x2.f16x2 %rs2, %r53;",
         }) {
      rejected = false;
      try {
        internal::LowerPackedFp8Conversions(malformed_conversion);
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "Unknown packed FP8 conversion signature accepted");
    }
    const auto lowered_mma = internal::LowerMmaForTuring(
        "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 "
        "{%r0, %r1}, {%hx2, %hx3, %hx4, %hx5}, {%hx6, %hx7}, {%r8, %r9};");
    check(lowered_mma.find("{%r8, %r9};") != std::string::npos, "First Turing MMA did not use the original accumulator");
    check(lowered_mma.ends_with("{%r0, %r1};"), "Second Turing MMA did not accumulate into its destination");
    for (const auto malformed_mma : {
             "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%r0, %r1}, {%r2, %r3, %r4, %r5}, {%r6, %r7}, {%r8, %r9};",
             "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%r0}, {%r2, %r3, %r4, %r5}, {%r6, %r7}, {%r8, %r9};",
             "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%rs0, %rs1}, {%r2, %r3, %r4, %r5}, {%r6, %r7}, {%r8, %r9};",
             "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%r0, %r1}, {%r2, %r3, %r4, %r5}, {%r6, %r7};",
         }) {
      rejected = false;
      try {
        internal::LowerMmaForTuring(malformed_mma);
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "Unknown Turing MMA signature accepted");
    }
    const auto lowered_minmax = internal::LowerPackedMinMaxForTuring(
        "min.f16x2 %ax0, %ax1, %ax2;\n"
        "{ max.f16x2 %ax3, %ax4, %ax5; }");
    check(lowered_minmax.find("setp.lt.f16 TURING_LO_SELECT") != std::string::npos, "Packed minimum did not select the less-than comparison");
    check(lowered_minmax.find("setp.gt.f16 TURING_LO_SELECT") != std::string::npos, "Packed maximum did not select the greater-than comparison");
    rejected = false;
    try {
      internal::LowerPackedMinMaxForTuring("min.f16x2 %rs0, %rs1, %rs2;");
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "Unsupported packed min/max register bank accepted");
    const auto synchronized_copy = internal::RemoveTuringSynchronization(
        "mbarrier.init.shared.b64 [%r1], %r2;\n"
        "cp.async.ca.shared.global [%r3], [%rd4], 4;\n"
        "cp.async.mbarrier.arrive.shared.b64 [%r1];\n"
        "cp.async.commit_group;\n"
        "cp.async.wait_group 0;");
    check(synchronized_copy == "\ncp.async.ca.shared.global [%r3], [%rd4], 4;\n\n\n", "Turing synchronization removal changed an unrelated copy operation");
    for (const auto unsupported_synchronization_instruction : {
             "cp.async.mbarrier.arrive.shared.b64 %r1;",
             "cp.async.mbarrier.arrive.shared.b64 [%rd1];",
             "cp.async.commit_group %r1;",
             "cp.async.wait_group 1;",
             "mbarrier.init.shared.b64 [%r1];",
             "mbarrier.init.shared.b64 [%r1], %rd2;",
         }) {
      rejected = false;
      try {
        internal::RemoveTuringSynchronization(unsupported_synchronization_instruction);
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "Invalid removed Turing synchronization operands accepted");
    }
    const auto temporary = fs::temp_directory_path() / std::format("opennr-native-tests-{}", GetCurrentProcessId());
    fs::remove_all(temporary);
    fs::create_directories(temporary);
    const auto cleanup = [&temporary](void*) { fs::remove_all(temporary); };
    const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
    Write(temporary / "same.bin", "expected");
    Write(temporary / "same.bin", "expected");
    rejected = false;
    try {
      Write(temporary / "same.bin", "changed");
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Differing existing output accepted");
    rejected = false;
    try {
      Check("changed", Hash("expected"));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Hash mismatch accepted");
    rejected = false;
    try {
      Run(temporary / "missing.exe", {}, temporary, temporary / "missing.log");
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Missing process accepted");
    std::array<wchar_t, MAX_PATH> command_processor{};
    const auto command_processor_size = GetEnvironmentVariableW(L"ComSpec", command_processor.data(), static_cast<DWORD>(command_processor.size()));
    check(command_processor_size != 0 && command_processor_size < command_processor.size(), "ComSpec unavailable");
    rejected = false;
    try {
      Run(fs::path(command_processor.data()), {L"/d", L"/c", L"exit", L"7"}, temporary, temporary / "failure.log");
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Nonzero process exit accepted");
    const auto unicode_directory = temporary / L"unicode-Эю";
    fs::create_directory(unicode_directory);
    const auto unicode_tool = unicode_directory / L"tool-ж.exe";
    fs::copy_file(fs::path(command_processor.data()).parent_path() / L"where.exe", unicode_tool);
    Run(
        unicode_tool,
        {L"cmd.exe"},
        temporary,
        unicode_directory / L"tool-ж.log");
    check(
        fs::is_regular_file(unicode_directory / L"tool-ж.log"),
        "Unicode process path was not supported");
    const std::string source = ".version 9.4\n.target sm_120\n.visible .entry sample(\n)\n{\nret;\n}\n";
    const auto approximate = GenerateApproximatePtx(source);
    check(approximate.back() == '\0', "Approximate terminator");
    check(LowerApproximatePtx(approximate, 86).find(".target sm_86") != std::string::npos, "SM86 target");
    check(LowerApproximatePtx(approximate, 75).find(".target sm_75") != std::string::npos, "SM75 target");
    check(LowerOriginalPtxForAda(source).find(".target sm_89") != std::string::npos, "Ada target");
    const auto newer_ptx_version = GenerateApproximatePtx(Replace(source, ".version 9.4", ".version 9.7"));
    check(newer_ptx_version.find(".version 9.3") != std::string::npos, "Newer PTX version was not normalized");
    rejected = false;
    try {
      GenerateApproximatePtx(Replace(source, ".target sm_120", ".target sm_121"));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Unexpected source architecture accepted");
    rejected = false;
    try {
      GenerateApproximatePtx(Replace(source, "ret;", ".reg .b32 %ax<1>;\nret;"));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Reserved synthetic register bank accepted");
    const std::string capped_source = Replace(source, "{\nret;", "{\n.maxnreg 168\nret;");
    const std::array register_caps{RegisterCapOverride{.entry_name = "sample", .expected = 168, .replacement = 128}};
    check(GenerateApproximatePtx(capped_source, register_caps).find(".maxnreg 128") != std::string::npos, "Recipe register cap was not applied");
    const std::string adjacent_entries =
        ".version 9.4\n.target sm_120\n.visible .entry first()\n{\nret;\n}\n"
        ".visible .entry second()\n{\n.maxnreg 168\nret;\n}\n";
    rejected = false;
    try {
      GenerateApproximatePtx(
          adjacent_entries,
          std::array{RegisterCapOverride{
              .entry_name = "first", .expected = 168, .replacement = 128}});
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Register cap crossed into the following PTX entry");
    check(internal::AddAcquireSemanticsToGpuReleaseFence("fence.release.gpu;") == "fence.acq_rel.gpu;", "GPU release fence did not gain acquire semantics");
    check(StrengthenGpuAcquireFences("@%p1 fence.acquire.gpu;") == "@%p1 fence.acq_rel.gpu;", "Acquire fence lost its predicate");
    check(StrengthenGpuAcquireFences("fence.acq_rel.gpu;") == "fence.acq_rel.gpu;", "Already lowered fence changed");
    check(StrengthenGpuAcquireFences("fence.acquire.sys;") == "fence.acquire.sys;", "Acquire fence scope changed");
    check(internal::LowerBarrierWaitForAmpere("mbarrier.try_wait.shared::cta.b64 P_OUT, [%r1], %rd2;") == "mbarrier.test_wait.shared.b64 P_OUT, [%r1], %rd2;", "Barrier wait intent lowering failed");
    rejected = false;
    try {
      internal::LowerBarrierWaitForAmpere("mbarrier.try_wait.shared::cta.b64 P_OUT, [%rd1], %rd2;");
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Unsupported barrier wait operands were preserved");
    std::string pe_image(0x204, '\0');
    pe_image.replace(0, 2, "MZ");
    const auto write_pe_value = [&pe_image](size_t offset, const auto& value) { std::memcpy(pe_image.data() + offset, &value, sizeof(value)); };
    write_pe_value(0x3C, uint32_t{0x40});
    pe_image.replace(0x40, 4, std::string("PE\0\0", 4));
    write_pe_value(0x44, uint16_t{0x8664});
    write_pe_value(0x46, uint16_t{1});
    write_pe_value(0x54, uint16_t{112});
    write_pe_value(0x56, uint16_t{0x2000});
    write_pe_value(0x58, uint16_t{0x020B});
    pe_image.replace(0xC8, 5, ".data");
    write_pe_value(0xD8, uint32_t{4});
    write_pe_value(0xDC, uint32_t{0x200});
    pe_image.replace(0x200, 4, "test");
    const auto pe_inventory = InspectPe(pe_image);
    check(pe_inventory.sections.size() == 1 && pe_inventory.sections[0].name == ".data" && pe_inventory.sections[0].sha256 == Hash("test"), "PE section inventory mismatch");
    // Upgrade the synthetic PE optional header to include security directory
    // fields and reserve a second section header without moving raw data.
    pe_image.replace(0xF0, 40, pe_image.substr(0xC8, 40));
    pe_image.replace(0xC8, 40, std::string(40, '\0'));
    write_pe_value(0x54, uint16_t{152});
    write_pe_value(0x58 + 8, uint32_t{4});
    write_pe_value(0x58 + 32, uint32_t{0x1000});
    write_pe_value(0x58 + 36, uint32_t{0x200});
    write_pe_value(0x58 + 56, uint32_t{0x2000});
    write_pe_value(0x58 + 60, uint32_t{0x200});
    write_pe_value(0x58 + 108, uint32_t{5});
    write_pe_value(0xF0 + 8, uint32_t{4});
    write_pe_value(0xF0 + 12, uint32_t{0x1000});
    const auto original_pe = pe_image;
    const auto appended = AppendFatbinSection(&pe_image, "new-fatbin");
    check(appended.rva == 0x2000 && appended.offset == 0x400, "Wrong appended PE location");
    check(pe_image.substr(0x200, 4) == "test" && pe_image.substr(appended.offset, 10) == "new-fatbin", "PE append changed payload bytes");
    check(InspectPe(pe_image).sections.size() == 2 && pe_image.size() == 0x600, "Invalid appended section layout");
    for (int invalid = 0; invalid < 5; ++invalid) {
      pe_image = original_pe;
      if (invalid == 0) pe_image[0x118] = 'X';
      if (invalid == 1) write_pe_value(0x58 + 60, uint32_t{0x118});
      if (invalid == 2) write_pe_value(0x58 + 36, uint32_t{3});
      if (invalid == 3) write_pe_value(0x58 + 56, uint32_t{0x1000});
      if (invalid == 4) write_pe_value(0x58 + 8, uint32_t{0xFFFFFFFF});
      rejected = false;
      try {
        AppendFatbinSection(&pe_image, "new-fatbin");
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      check(rejected, "Malformed PE append layout accepted");
    }
    std::string fatbin(20, '\0');
    const auto write_fatbin_value = [&fatbin](size_t offset, const auto& value) { std::memcpy(fatbin.data() + offset, &value, sizeof(value)); };
    write_fatbin_value(0, uint32_t{0xBA55ED50});
    write_fatbin_value(4, uint16_t{1});
    write_fatbin_value(6, uint16_t{16});
    write_fatbin_value(8, uint64_t{4});
    const auto fatbins = FindFatbins(fatbin, 0, fatbin.size());
    check(fatbins.size() == 1 && fatbins[0].offset == 0 && fatbins[0].size == fatbin.size(), "Fatbin discovery mismatch");
    rejected = false;
    try {
      FindFatbins(fatbin, 1, fatbin.size());
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "Out-of-bounds fatbin scan accepted");
    std::cout << "String-view and lowering checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
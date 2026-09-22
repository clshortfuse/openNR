#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "architectures.hpp"
#include "binary.hpp"
#include "io.hpp"
#include "transforms.hpp"

namespace {

class OutputTransaction {
 public:
  explicit OutputTransaction(const opennr::fs::path& destination)
      : destination_(destination),
        working_(destination.parent_path()
                 / (destination.filename().wstring() + L".incomplete-"
                    + std::to_wstring(GetCurrentProcessId()))) {
    if (opennr::fs::exists(destination_)) {
      throw std::runtime_error("Output must be a new directory: " + destination_.string());
    }
    if (opennr::fs::exists(working_)) {
      throw std::runtime_error("Temporary output already exists: " + working_.string());
    }
    opennr::fs::create_directories(destination_.parent_path());
    opennr::fs::create_directory(working_);
  }

  ~OutputTransaction() {
    if (!committed_) {
      std::error_code error;
      opennr::fs::remove_all(working_, error);
    }
  }

  const opennr::fs::path& Working() const { return working_; }

  void Commit() {
    opennr::fs::rename(working_, destination_);
    committed_ = true;
  }

 private:
  opennr::fs::path destination_;
  opennr::fs::path working_;
  bool committed_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace opennr;
  using Json = nlohmann::json;
  try {
    fs::path root = fs::current_path();
    fs::path output;
    fs::path input;
    fs::path cuda;
    std::string command = "build";
    if (argc > 1) {
      command = argv[1];
    }
    for (int i = 2; i < argc;) {
      const std::string option = argv[i];
      if (!option.starts_with("--") && input.empty() && (command == "inspect" || command == "verify-base")) {
        input = fs::absolute(option);
        ++i;
        continue;
      }
      if (i + 1 == argc) throw std::runtime_error("Missing option value");
      if (option == "--root") {
        root = fs::absolute(argv[i + 1]);
      } else if (option == "--output") {
        output = fs::absolute(argv[i + 1]);
      } else if (option == "--cuda-bin") {
        cuda = fs::absolute(argv[i + 1]);
      } else {
        throw std::runtime_error("Unknown option " + option);
      }
      i += 2;
    }
    if (command == "--help") {
      std::cout << "opennr [build|sources|extract] [--root DIR] [--output NEW_DIR] [--cuda-bin DIR]\n"
                   "opennr [inspect|verify-base] [DLL] [--root DIR] [--output FILE]\n";
      return 0;
    }
    if (command != "build" && command != "verify-base" && command != "sources" && command != "inspect" && command != "extract") throw std::runtime_error("Unknown command");
    const auto profile = Json::parse(Read(root / "profiles/compat-fp16.json"));
    const auto& cuda_toolkit = profile.at("cuda_toolkit");
    if (cuda.empty()) {
      cuda = fs::path("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA")
             / ("v" + cuda_toolkit.at("version").get<std::string>()) / "bin";
    }
    const auto base_path = input.empty() ? root / profile.at("base_filename").get<std::string>() : input;
    auto base = Read(base_path);
    const auto inventory = InspectPe(base);
    const bool recognized_upstream = std::ranges::any_of(profile.at("supported_upstream_sha256"), [&](const Json& hash) { return inventory.sha256 == hash.get<std::string>(); });
    if (command == "inspect") {
      Json report{
          {"schema", 1},
          {"size", inventory.size},
          {"machine", "AMD64"},
          {"sha256", inventory.sha256},
          {"sections", Json::array()},
          {"recognized_upstream", recognized_upstream ? Json(profile.at("name")) : Json(nullptr)},
          {"recognized_reference", inventory.sha256 == profile.at("reference_output_sha256").get<std::string>() ? Json(profile.at("name")) : Json(nullptr)},
          {"build_available", profile.value("build_available", false)},
      };
      for (const auto& section : inventory.sections) {
        report["sections"].push_back({{"name", section.name}, {"offset", section.offset}, {"size", section.size}, {"sha256", section.sha256}});
      }
      const auto text = report.dump(2) + '\n';
      if (output.empty()) {
        std::cout << text;
      } else {
        Write(output, text);
      }
      return 0;
    }
    if (!recognized_upstream) {
      throw std::runtime_error("Unexpected base DLL SHA-256 " + inventory.sha256);
    }
    if (command == "verify-base") {
      std::cout << "Verified original DLL: " << inventory.sha256 << '\n';
      return 0;
    }
    const auto recipe_bytes = Read(root / "profiles/compat-fp16-recipe.json");
    Check(recipe_bytes, profile.at("input_manifest_sha256"));
    const auto recipe = Json::parse(recipe_bytes);
    const auto& data_section = RequireSinglePeSection(inventory, ".data");
    const auto discovered_fatbins = FindFatbins(base, data_section.offset, data_section.size);
    if (discovered_fatbins.size() != recipe.at("modules").size()) {
      throw std::runtime_error(std::format("Expected {} fatbins, discovered {}", recipe.at("modules").size(), discovered_fatbins.size()));
    }
    for (size_t module_index = 0; module_index < discovered_fatbins.size(); ++module_index) {
      const auto& module = recipe.at("modules").at(module_index);
      if (module.at("index").get<size_t>() != module_index || module.at("offset").get<size_t>() != discovered_fatbins[module_index].offset || module.at("size").get<size_t>() != discovered_fatbins[module_index].size) {
        throw std::runtime_error(std::format("Discovered fatbin {} does not match its recipe slot", module_index));
      }
    }
    if (output.empty()) {
      output = root / (command == "extract" ? "build/original-native" : "build/compat-fp16");
    }
    const auto requested_output = output;
    OutputTransaction output_transaction(requested_output);
    output = output_transaction.Working();
    for (const std::string tool : {"cuobjdump", "ptxas", "fatbinary"}) {
      const auto log = output / (tool + "-version.log");
      Run(cuda / (tool + ".exe"), {L"--version"}, output, log);
      const auto version_key = tool == "cuobjdump" ? "extractor_version" : "compiler_version";
      const auto version = cuda_toolkit.at(version_key).get<std::string>();
      if (Read(log).find("V" + version) == std::string::npos) throw std::runtime_error("Unexpected " + tool + " version");
    }
    Json report = {{"profile", "compat-fp16"}, {"implementation", "C++20"}, {"modules", Json::array()}};
    if (command == "extract") {
      Json manifest{{"schema", 1}, {"base_sha256", inventory.sha256}, {"modules", Json::array()}};
      for (size_t index = 0; index < discovered_fatbins.size(); ++index) {
        const auto directory = output / std::format("module_{:03}", index);
        fs::create_directories(directory);
        const auto& location = discovered_fatbins[index];
        const auto fatbin_bytes = base.substr(location.offset, location.size);
        const auto fatbin = directory / "original.fatbin";
        Write(fatbin, fatbin_bytes);
        for (const std::string kind : {"ptx", "elf"}) {
          Run(cuda / "cuobjdump.exe", {L"--extract-" + fs::path(kind).wstring(), L"all", fatbin.wstring()}, directory, directory / ("extract-" + kind + ".log"));
        }
        fs::path ptx;
        fs::path cubin;
        for (const auto& file : fs::directory_iterator(directory)) {
          if (file.path().extension() == ".ptx") {
            if (!ptx.empty()) throw std::runtime_error("Multiple extracted PTX images");
            ptx = file.path();
          } else if (file.path().extension() == ".cubin") {
            if (!cubin.empty()) throw std::runtime_error("Multiple extracted cubins");
            cubin = file.path();
          }
        }
        if (ptx.empty() || cubin.empty()) throw std::runtime_error("Missing extracted SM120 image");
        const auto ptx_bytes = Read(ptx);
        const auto cubin_bytes = Read(cubin);
        const bool targets_sm120 = ptx_bytes.find(".target sm_120") != std::string::npos;
        const bool has_elf_identification = cubin_bytes.size() >= 6;
        const bool has_elf_magic = has_elf_identification && static_cast<unsigned char>(cubin_bytes[0]) == 0x7F && cubin_bytes[1] == 'E' && cubin_bytes[2] == 'L' && cubin_bytes[3] == 'F';
        const bool is_little_endian_elf64 = has_elf_magic && cubin_bytes[4] == 2 && cubin_bytes[5] == 1;
        if (!targets_sm120 || !is_little_endian_elf64) {
          throw std::runtime_error("Unexpected extracted SM120 image format");
        }
        const auto& module = recipe.at("modules").at(index);
        Check(cubin_bytes, module.at("elf120").at("sha256"));
        for (const auto& source : module.at("files")) {
          if (source.at("kind") == "ptx") {
            Check(ptx_bytes, source.at("sha256"));
          }
        }
        manifest["modules"].push_back({
            {"index", index},
            {"offset", location.offset},
            {"size", location.size},
            {"fatbin_sha256", Hash(fatbin_bytes)},
            {"ptx", {{"path", fs::relative(ptx, output).generic_string()}, {"size", ptx_bytes.size()}, {"sha256", Hash(ptx_bytes)}}},
            {"elf", {{"path", fs::relative(cubin, output).generic_string()}, {"size", cubin_bytes.size()}, {"sha256", Hash(cubin_bytes)}}},
        });
      }
      Write(output / "manifest.json", manifest.dump(2) + '\n');
      output_transaction.Commit();
      std::cout << "Extracted " << discovered_fatbins.size() << " original PTX and SM120 cubin images: " << requested_output << '\n';
      return 0;
    }
    std::vector<std::tuple<size_t, std::string, std::string>> replacements;
    for (const auto& module : recipe.at("modules")) {
      const int index = module.at("index");
      const size_t offset = module.at("offset");
      const size_t size = module.at("size");
      if (offset > base.size() || size > base.size() - offset) throw std::runtime_error("Slot outside DLL");
      const auto slot = base.substr(offset, size);
      Check(slot, module.at("base_slot_sha256"));
      const auto directory = output / std::format("module_{:03}", index);
      const auto original_directory = directory / "original";
      fs::create_directories(original_directory);
      const auto fatbin = original_directory / "original.fatbin";
      Write(fatbin, slot);
      for (const std::string kind : {"ptx", "elf"}) {
        Run(cuda / "cuobjdump.exe", {L"--extract-" + fs::path(kind).wstring(), L"all", fatbin.wstring()}, original_directory, directory / ("extract-" + kind + ".log"));
      }
      fs::path original_ptx;
      fs::path original_elf;
      for (const auto& file : fs::directory_iterator(original_directory)) {
        if (file.path().extension() == ".ptx") {
          if (!original_ptx.empty()) throw std::runtime_error("Multiple original PTX images");
          original_ptx = file.path();
        }
        if (file.path().extension() == ".cubin") {
          if (!original_elf.empty()) throw std::runtime_error("Multiple original cubins");
          original_elf = file.path();
        }
      }
      Check(Read(original_elf), module.at("elf120").at("sha256"));
      const auto original_text = Read(original_ptx);
      std::vector<RegisterCapOverride> approximate_register_caps;
      for (const auto& register_cap : module.value("approximate_register_caps", Json::array())) {
        approximate_register_caps.push_back({
            .entry_name = register_cap.at("entry"),
            .expected = register_cap.at("expected"),
            .replacement = register_cap.at("replacement"),
        });
      }
      const auto approximate = GenerateApproximatePtx(original_text, approximate_register_caps);
      Write(directory / "approximate.ptx", approximate);
      std::vector<std::wstring> images;
      for (const auto& source : module.at("compile")) {
        const int sm = source.at("sm");
        std::vector<RegisterCapOverride> register_caps;
        for (const auto& register_cap : source.value("register_caps", Json::array())) {
          register_caps.push_back({
              .entry_name = register_cap.at("entry"),
              .expected = register_cap.at("expected"),
              .replacement = register_cap.at("replacement"),
          });
        }
        const auto generated = LowerApproximatePtx(approximate, sm, register_caps);
        const auto ptx = directory / std::format("module.sm_{}.generated.ptx", sm);
        Write(ptx, generated);
        if (command == "sources") continue;
        const auto cubin = directory / std::format("module.sm_{}.cubin", sm);
        std::cout << "Compiling " << index << " SM" << sm << std::endl;
        Run(cuda / "ptxas.exe", {L"--gpu-name", std::format(L"sm_{}", sm), L"--opt-level", L"3", L"--set-texmode-raw=true", L"--split-compile=0", L"--warn-on-spills", L"--verbose", L"--output-file", cubin.wstring(), ptx.wstring()}, directory, directory / std::format("sm_{}.log", sm));
        Check(Read(cubin), source.at("cubin_sha256"));
        images.insert(images.end(), {L"--image3", L"file=" + cubin.wstring() + L",kind=elf,sm=" + std::to_wstring(sm)});
      }
      const auto ada_ptx = directory / "module.sm_89.generated.ptx";
      Write(ada_ptx, LowerOriginalPtxForAda(original_text));
      if (command == "sources") {
        std::cout << "Generated source module " << index << std::endl;
        continue;
      }
      for (const auto& source : module.at("files")) {
        if (source.at("kind") != "elf") continue;
        const auto cubin = directory / "module.sm_89.cubin";
        std::cout << "Compiling " << index << " SM89" << std::endl;
        Run(cuda / "ptxas.exe", {L"-arch=sm_89", L"--set-texmode-raw=true", L"-split-compile=0", L"-o", cubin.wstring(), ada_ptx.wstring()}, directory, directory / "sm_89.log");
        Check(Read(cubin), source.at("sha256"));
        images.insert(images.end(), {L"--image3", L"file=" + cubin.wstring() + L",kind=elf,sm=89"});
      }
      images.insert(images.end(), {L"--image3", L"file=" + original_elf.wstring() + L",kind=elf,sm=120"});
      for (const auto& source : module.at("files")) {
        if (source.at("kind") != "ptx") continue;
        Check(original_text, source.at("sha256"));
        images.insert(images.end(), {L"--image3", L"file=" + original_ptx.wstring() + L",kind=ptx,sm=120"});
      }
      const auto packed = directory / "module.fatbin";
      images.insert(images.begin(), {L"--64", L"--create", packed.wstring()});
      images.push_back(L"--compress-all");
      Run(cuda / "fatbinary.exe", images, directory, directory / "fatbinary.log");
      auto bytes = Read(packed);
      if (bytes.size() > size) throw std::runtime_error("Fatbin exceeds slot");
      report["modules"].push_back({{"index", index}, {"packed_size", bytes.size()}, {"sha256", Hash(bytes)}});
      bytes.resize(size, '\0');
      replacements.emplace_back(offset, slot, std::move(bytes));
    }
    if (command == "sources") {
      output_transaction.Commit();
      return 0;
    }
    for (const auto& patch : recipe.at("host_patches")) {
      const auto decode = [](const std::string& text) {
        std::string bytes;
        if (text.size() % 2 != 0) throw std::runtime_error("Odd hex length");
        for (size_t i = 0; i < text.size(); i += 2) {
          bytes += static_cast<char>(std::stoul(text.substr(i, 2), nullptr, 16));
        }
        return bytes;
      };
      replacements.emplace_back(patch.at("offset"), decode(patch.at("before")), decode(patch.at("after")));
    }
    std::sort(replacements.begin(), replacements.end());
    size_t end = 0;
    for (const auto& [offset, before, after] : replacements) {
      if (offset < end || offset > base.size() || before.size() > base.size() - offset || before.size() != after.size() || base.substr(offset, before.size()) != before) throw std::runtime_error("Invalid replacement");
      base.replace(offset, before.size(), after);
      end = offset + before.size();
    }
    Check(base, profile.at("reference_output_sha256"));
    Write(output / "nvngx_dlssnr.dll", base);
    report["output_sha256"] = Hash(base);
    report["reference_exact"] = true;
    Write(output / "build-report.json", report.dump(2) + '\n');
    output_transaction.Commit();
    std::cout << "Exact reference match: " << Hash(base) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
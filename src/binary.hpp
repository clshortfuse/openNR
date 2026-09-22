#pragma once

#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "io.hpp"

namespace opennr {

struct PeSection {
  std::string name;
  size_t offset;
  size_t size;
  std::string sha256;
};

struct PeInventory {
  size_t size;
  std::string sha256;
  std::vector<PeSection> sections;
};

struct FatbinLocation {
  size_t offset;
  size_t size;
};

namespace internal {

template <typename Value>
inline Value ReadBinaryValue(std::string_view bytes, size_t offset, std::string_view field) {
  if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
    throw std::runtime_error(std::format("Truncated {}", field));
  }
  Value value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

}  // namespace internal

inline PeInventory InspectPe(std::string_view bytes) {
  constexpr uint16_t AMD64_MACHINE = 0x8664;
  constexpr uint16_t DLL_CHARACTERISTIC = 0x2000;
  constexpr uint16_t PE32_PLUS_MAGIC = 0x020B;
  constexpr size_t DOS_PE_OFFSET = 0x3C;
  constexpr size_t COFF_HEADER_SIZE = 24;
  constexpr size_t SECTION_HEADER_SIZE = 40;
  if (bytes.size() < 64 || bytes.substr(0, 2) != "MZ") {
    throw std::runtime_error("Not a DOS/PE image");
  }
  const auto pe_offset = static_cast<size_t>(internal::ReadBinaryValue<uint32_t>(bytes, DOS_PE_OFFSET, "DOS header"));
  if (pe_offset < 64 || pe_offset > bytes.size() || COFF_HEADER_SIZE > bytes.size() - pe_offset || bytes.substr(pe_offset, 4) != std::string_view("PE\0\0", 4)) {
    throw std::runtime_error("Invalid PE header");
  }
  const auto machine = internal::ReadBinaryValue<uint16_t>(bytes, pe_offset + 4, "COFF machine");
  const auto section_count = internal::ReadBinaryValue<uint16_t>(bytes, pe_offset + 6, "COFF section count");
  const auto optional_header_size = internal::ReadBinaryValue<uint16_t>(bytes, pe_offset + 20, "COFF optional-header size");
  const auto characteristics = internal::ReadBinaryValue<uint16_t>(bytes, pe_offset + 22, "COFF characteristics");
  if (machine != AMD64_MACHINE || (characteristics & DLL_CHARACTERISTIC) == 0) {
    throw std::runtime_error("Expected an AMD64 DLL");
  }
  if (optional_header_size < 112 || internal::ReadBinaryValue<uint16_t>(bytes, pe_offset + COFF_HEADER_SIZE, "optional-header magic") != PE32_PLUS_MAGIC) {
    throw std::runtime_error("Expected PE32+");
  }
  const auto section_table = pe_offset + COFF_HEADER_SIZE + optional_header_size;
  if (section_table > bytes.size() || static_cast<size_t>(section_count) > (bytes.size() - section_table) / SECTION_HEADER_SIZE) {
    throw std::runtime_error("Truncated PE section table");
  }
  PeInventory inventory{.size = bytes.size(), .sha256 = Hash(std::string(bytes))};
  for (size_t index = 0; index < section_count; ++index) {
    const auto header = section_table + index * SECTION_HEADER_SIZE;
    const auto raw_size = static_cast<size_t>(internal::ReadBinaryValue<uint32_t>(bytes, header + 16, "section raw size"));
    const auto raw_offset = static_cast<size_t>(internal::ReadBinaryValue<uint32_t>(bytes, header + 20, "section raw offset"));
    if (raw_size != 0 && (raw_offset < section_table + section_count * SECTION_HEADER_SIZE || raw_offset > bytes.size() || raw_size > bytes.size() - raw_offset)) {
      throw std::runtime_error(std::format("Invalid raw range for PE section {}", index));
    }
    const auto name_bytes = bytes.substr(header, 8);
    const auto terminator = name_bytes.find('\0');
    inventory.sections.push_back({
        .name = std::string(name_bytes.substr(0, terminator)),
        .offset = raw_offset,
        .size = raw_size,
        .sha256 = Hash(std::string(bytes.substr(raw_offset, raw_size))),
    });
  }
  return inventory;
}

inline std::vector<FatbinLocation> FindFatbins(std::string_view bytes, size_t scan_offset, size_t scan_size) {
  constexpr uint32_t FATBIN_MAGIC = 0xBA55ED50;
  constexpr uint16_t FATBIN_VERSION = 1;
  constexpr uint16_t FATBIN_HEADER_SIZE = 16;
  if (scan_offset > bytes.size() || scan_size > bytes.size() - scan_offset) {
    throw std::runtime_error("Invalid fatbin scan range");
  }
  const auto scan_end = scan_offset + scan_size;
  std::vector<FatbinLocation> locations;
  for (size_t cursor = scan_offset; cursor < scan_end;) {
    const auto relative_offset = bytes.substr(cursor, scan_end - cursor).find(std::string_view("\x50\xED\x55\xBA", 4));
    if (relative_offset == std::string_view::npos) {
      break;
    }
    const auto offset = cursor + relative_offset;
    if (offset > scan_end || FATBIN_HEADER_SIZE > scan_end - offset) {
      throw std::runtime_error("Truncated fatbin header");
    }
    const auto magic = internal::ReadBinaryValue<uint32_t>(bytes, offset, "fatbin magic");
    const auto version = internal::ReadBinaryValue<uint16_t>(bytes, offset + 4, "fatbin version");
    const auto header_size = internal::ReadBinaryValue<uint16_t>(bytes, offset + 6, "fatbin header size");
    const auto payload_size = internal::ReadBinaryValue<uint64_t>(bytes, offset + 8, "fatbin payload size");
    if (magic != FATBIN_MAGIC || version != FATBIN_VERSION || header_size != FATBIN_HEADER_SIZE || payload_size == 0 || payload_size > scan_end - offset - header_size) {
      throw std::runtime_error(std::format("Invalid fatbin header at {:#x}", offset));
    }
    const auto size = static_cast<size_t>(header_size + payload_size);
    locations.push_back({.offset = offset, .size = size});
    cursor = offset + size;
  }
  return locations;
}

inline const PeSection& RequireSinglePeSection(const PeInventory& inventory, std::string_view name) {
  const PeSection* found = nullptr;
  for (const auto& section : inventory.sections) {
    if (section.name == name) {
      if (found != nullptr) {
        throw std::runtime_error(std::format("Expected one {} section", name));
      }
      found = &section;
    }
  }
  if (found == nullptr) {
    throw std::runtime_error(std::format("Expected one {} section", name));
  }
  return *found;
}

}  // namespace opennr
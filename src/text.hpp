/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace opennr {

// Adapted from RenoDX src/utils/string_view.hpp (Carlos Lopez, MIT).
// Captures borrow the input; never retain them across input mutation/destruction.
inline std::string_view StringViewFromCsubMatch(const std::csub_match& match) {
  if (!match.matched) return {};
  return {match.first, static_cast<size_t>(match.length())};
}

inline bool IsNumberedRegisterInBank(std::string_view register_name, std::string_view register_bank) {
  if (!register_name.starts_with(register_bank) || register_name.size() == register_bank.size()) {
    return false;
  }
  for (const char digit : register_name.substr(register_bank.size())) {
    if (!std::isdigit(static_cast<unsigned char>(digit))) {
      return false;
    }
  }
  return true;
}

inline std::string Replace(std::string_view text, std::string_view before, std::string_view after) {
  if (before.empty()) throw std::invalid_argument("Empty replacement pattern");
  std::string output;
  output.reserve(text.size());
  size_t end = 0;
  for (auto position = text.find(before); position != std::string_view::npos; position = text.find(before, end)) {
    output.append(text.substr(end, position - end));
    output.append(after);
    end = position + before.size();
  }
  output.append(text.substr(end));
  return output;
}

template <typename Callback>
inline std::string Rewrite(std::string_view text, const std::regex& expression, Callback&& callback) {
  // Default-constructed views can have null data; regex requires a valid range.
  if (text.data() == nullptr) {
    text = "";
  }
  std::string output;
  output.reserve(text.size());
  size_t end = 0;
  for (auto it = std::cregex_iterator(text.data(), text.data() + text.size(), expression); it != std::cregex_iterator(); ++it) {
    const auto& match = *it;
    output.append(text.substr(end, match.position() - end));
    output += callback(match);
    end = match.position() + match.length();
  }
  output.append(text.substr(end));
  return output;
}

template <typename Callback>
inline std::string Rewrite(std::string_view text, const std::string& pattern, Callback&& callback) {
  std::string translated = pattern;
  const bool anchored_at_line_start = translated.starts_with('^');
  if (anchored_at_line_start) {
    translated.replace(0, 1, "(?:^|\\n)");
  }
  if (translated.ends_with('$') && (translated.size() < 2 || translated[translated.size() - 2] != '\\')) {
    translated.replace(translated.size() - 1, 1, "(?=\\n|$)");
  }
  return Rewrite(text, std::regex(translated, std::regex_constants::ECMAScript), [&](const std::cmatch& match) {
    const auto replacement = callback(match);
    if (anchored_at_line_start && match.length() != 0 && *match[0].first == '\n') {
      return std::string("\n") + replacement;
    }
    return std::string(replacement);
  });
}

template <size_t N>
inline std::array<std::string_view, N> SplitRegisters(std::string_view text) {
  std::array<std::string_view, N> result{};
  size_t count = 0;
  size_t begin = 0;
  while (begin < text.size()) {
    const auto comma = text.find(',', begin);
    const auto value_size = (comma == std::string_view::npos ? text.size() - begin : comma - begin);
    const auto value = text.substr(begin, value_size);
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos || value[first] != '%' || count == N) throw std::invalid_argument("Invalid register fragment");
    const auto token = value.substr(first, last - first + 1);
    if (token.size() < 3 || !std::isalpha(static_cast<unsigned char>(token[1]))) throw std::invalid_argument("Invalid register token");
    for (const char character : token.substr(2)) {
      if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') throw std::invalid_argument("Invalid register token");
    }
    result[count++] = token;
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
    if (begin == text.size()) throw std::invalid_argument("Trailing register separator");
  }
  if (count != N) throw std::invalid_argument("Wrong register fragment size");
  return result;
}

}  // namespace opennr
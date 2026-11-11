#pragma once

#include <algorithm>
#include <optional>
#include <string_view>

#include <huxerui/data.h>

namespace folio {

/// Folds ASCII letters for case-insensitive scheme and file-name comparisons.
constexpr char LowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

/// Parses an absolute HTTP or HTTPS URL with a nonempty authority.
inline std::optional<huxerui::Uri> ParseWebUrl(std::string_view source) {
  auto url = huxerui::Uri::Parse(source);
  if (!url || url->Authority().value_or("").empty()) {
    return std::nullopt;
  }
  const auto scheme = url->Scheme();
  if (!std::ranges::equal(scheme, std::string_view("http"), {}, LowerAscii) &&
      !std::ranges::equal(scheme, std::string_view("https"), {}, LowerAscii)) {
    return std::nullopt;
  }
  return url;
}

} // namespace folio

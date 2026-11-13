#include "document_links.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "attachments.h"

using namespace huxerui;
using namespace huxerui::richeditor;

namespace folio {
namespace {

int HexDigit(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
}

std::optional<std::string> DecodeComponent(std::string_view value) {
  std::string decoded;
  for (std::size_t index = 0; index < value.size(); ++index) {
    unsigned char byte = value[index];
    if (byte == '%') {
      if (value.size() - index < 3 || HexDigit(value[index + 1]) < 0 || HexDigit(value[index + 2]) < 0) {
        return std::nullopt;
      }
      byte = static_cast<unsigned char>(HexDigit(value[index + 1]) * 16 + HexDigit(value[index + 2]));
      index += 2;
    }
    if (byte < 32 || byte == 127) {
      return std::nullopt;
    }
    decoded += static_cast<char>(byte);
  }
  try {
    (void)AttributedText(decoded);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  return decoded;
}

std::string HeadingTitle(const BlockData& data, const AttributedText& text) {
  if (data.inline_objects.empty()) {
    return data.text;
  }
  std::string title;
  TextOffset start = 0;
  for (const auto& object : data.inline_objects) {
    title += text.TextInRange({start, object.offset});
    title += object.data.fallback_text.empty() ? object.data.type : object.data.fallback_text;
    start = object.offset + 1;
  }
  title += text.TextInRange({start, text.Length()});
  return title;
}

std::string HeadingAnchor(std::string_view title) {
  constexpr std::string_view whitespace = " \t\r\n";
  const auto start = title.find_first_not_of(whitespace);
  if (start == title.npos) {
    return {};
  }
  title = title.substr(start, title.find_last_not_of(whitespace) - start + 1);
  std::string anchor;
  for (std::size_t index = 0; index < title.size();) {
    const auto first = static_cast<unsigned char>(title[index]);
    if (first < 128) {
      ++index;
      if (first >= 'A' && first <= 'Z') {
        anchor += static_cast<char>(first - 'A' + 'a');
      } else if ((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9') || first == '-' || first == '_') {
        anchor += static_cast<char>(first);
      } else if (whitespace.find(static_cast<char>(first)) != whitespace.npos) {
        anchor += '-';
      }
      continue;
    }
    // Titles are already valid UTF-8. Preserve letters across scripts and skip common Unicode punctuation.
    const std::size_t width = first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
    unsigned codepoint = first & (0x7F >> width);
    for (std::size_t offset = 1; offset < width; ++offset) {
      codepoint = (codepoint << 6) | (static_cast<unsigned char>(title[index + offset]) & 0x3F);
    }
    if (codepoint == 0xA0 || codepoint == 0x3000 || (codepoint >= 0x2000 && codepoint <= 0x200A)) {
      anchor += '-';
    } else if (!((codepoint >= 0x2010 && codepoint <= 0x206F) || (codepoint >= 0x3001 && codepoint <= 0x3003) ||
                 (codepoint >= 0x3008 && codepoint <= 0x301F) || codepoint == 0x3030 ||
                 (codepoint >= 0xFF01 && codepoint <= 0xFF0F) || (codepoint >= 0xFF1A && codepoint <= 0xFF20) ||
                 (codepoint >= 0xFF3B && codepoint <= 0xFF40) || (codepoint >= 0xFF5B && codepoint <= 0xFF65))) {
      anchor += title.substr(index, width);
    }
    index += width;
  }
  return anchor;
}

} // namespace

std::optional<DocumentLink> ParseDocumentLink(std::string_view target) {
  const auto hash = target.find('#');
  auto path = target.substr(0, hash);
  if (path.starts_with('/') || path.find_first_of("\\:?") != path.npos) {
    return std::nullopt;
  }
  DocumentLink link;
  if (hash != target.npos) {
    link.anchor = DecodeComponent(target.substr(hash + 1));
    if (!link.anchor || link.anchor->empty()) {
      return std::nullopt;
    }
  }
  if (path.empty()) {
    return link.anchor ? std::optional(std::move(link)) : std::nullopt;
  }
  for (;;) {
    const auto slash = path.find('/');
    auto part = DecodeComponent(path.substr(0, slash));
    if (!part || part->empty() || part->find_first_of("/\\:") != part->npos) {
      return std::nullopt;
    }
    link.path.push_back(std::move(*part));
    if (slash == path.npos) {
      break;
    }
    path.remove_prefix(slash + 1);
  }
  return FormatOf(link.path.back()) ? std::optional(std::move(link)) : std::nullopt;
}

std::optional<std::vector<std::string>> ResolveDocumentLinkPath(
    std::span<const std::string> source, const DocumentLink& link) {
  if (source.empty() || link.path.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> path(source.begin(), source.end() - 1);
  for (const auto& part : link.path) {
    if (part == "..") {
      if (path.empty()) {
        return std::nullopt;
      }
      path.pop_back();
    } else if (part != ".") {
      path.push_back(part);
    }
  }
  return path;
}

std::vector<DocumentHeading> DocumentHeadings(const RichDocument& document) {
  std::vector<DocumentHeading> headings;
  std::unordered_set<std::string> anchors;
  std::unordered_map<std::string, std::size_t> suffixes;
  const auto visit = [&](const auto& self, const Block& block, std::size_t root) -> void {
    const auto& data = block.Data();
    if (data.kind == BlockKind::Heading && data.text.find_first_not_of(" \t\r\n") != data.text.npos) {
      const AttributedText text(data.text);
      auto title = HeadingTitle(data, text);
      auto base = HeadingAnchor(title);
      auto anchor = base;
      if (!base.empty() && !anchors.insert(anchor).second) {
        auto& suffix = suffixes[base];
        do {
          anchor = base + "-" + std::to_string(++suffix);
        } while (!anchors.insert(anchor).second);
      }
      headings.push_back({block, root, text.Length(), std::move(title), std::move(anchor)});
    }
    for (const auto& child : data.children) {
      self(self, child, root);
    }
  };
  const auto blocks = document.Blocks();
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    visit(visit, blocks[index], index);
  }
  return headings;
}

} // namespace folio

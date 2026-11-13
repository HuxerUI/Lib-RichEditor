#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/richeditor/document.h>

namespace folio {

struct DocumentLink {
  /// Decoded relative path. An empty path identifies the current document.
  std::vector<std::string> path;
  std::optional<std::string> anchor;
};

std::optional<DocumentLink> ParseDocumentLink(std::string_view target);

/// Resolves a link against the source document's path within its authorized folder.
/// The returned path is relative to that same folder.
std::optional<std::vector<std::string>> ResolveDocumentLinkPath(
    std::span<const std::string> source, const DocumentLink& link);

struct DocumentHeading {
  huxerui::richeditor::Block block;
  std::size_t root_index;
  huxerui::TextOffset length;
  std::string title;
  std::string anchor;
};

/// Lists headings in reading order, assigning unique anchors across the whole document.
std::vector<DocumentHeading> DocumentHeadings(const huxerui::richeditor::RichDocument& document);

} // namespace folio

#include "reading_position.h"

#include <algorithm>
#include <string_view>

#include <huxerui/richeditor.h>

using namespace huxerui;
using namespace huxerui::richeditor;

namespace folio {
namespace {

const Block* LeadingTextBlock(const Block& block) {
  const auto& data = block.Data();
  if (!data.text.empty()) {
    return &block;
  }
  for (const auto& child : data.children) {
    if (const auto* text = LeadingTextBlock(child)) {
      return text;
    }
  }
  return nullptr;
}

std::uint64_t TextFingerprint(const Block& block) {
  const auto* text = LeadingTextBlock(block);
  if (!text) {
    return 0;
  }
  // Use a stable hash of the first 96 UTF-8 bytes so edits later in a long paragraph still match.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : std::string_view(text->Data().text).substr(0, 96)) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

std::size_t FindReadingBlock(std::span<const Block> blocks, ReadingPosition position) {
  const auto index = static_cast<std::size_t>(std::min<std::uint64_t>(position.block_index, blocks.size() - 1));
  if (position.block_index == 0) {
    return 0;
  }
  if (position.text_fingerprint != 0) {
    constexpr std::size_t search_radius = 64;
    const auto matches = [&](std::size_t candidate) {
      return TextFingerprint(blocks[candidate]) == position.text_fingerprint;
    };
    if (matches(index)) {
      return index;
    }
    for (std::size_t distance = 1; distance <= search_radius; ++distance) {
      if (distance <= index && matches(index - distance)) {
        return index - distance;
      }
      if (distance < blocks.size() - index && matches(index + distance)) {
        return index + distance;
      }
    }
  }
  std::size_t nearest_text = blocks.size();
  for (std::size_t candidate = index + 1; candidate > 0;) {
    const auto& block = blocks[--candidate];
    if (block.Data().kind == BlockKind::Heading && !block.Data().text.empty()) {
      return candidate;
    }
    if (nearest_text == blocks.size() && LeadingTextBlock(block)) {
      nearest_text = candidate;
    }
  }
  return nearest_text == blocks.size() ? 0 : nearest_text;
}

} // namespace

std::optional<ReadingPosition> CaptureReadingPosition(const RichDocument& document, std::size_t block_index) {
  const auto blocks = document.Blocks();
  if (block_index == 0 || block_index >= blocks.size()) {
    return std::nullopt;
  }
  return ReadingPosition{block_index, TextFingerprint(blocks[block_index])};
}

void RestoreReadingPosition(const RichEditorController& editor, ReadingPosition position) {
  const auto document = editor.Document();
  const auto blocks = document.Blocks();
  if (blocks.empty()) {
    return;
  }
  const auto index = FindReadingBlock(blocks, position);
  if (index == 0) {
    return;
  }
  if (const auto* text = LeadingTextBlock(blocks[index])) {
    const auto first_byte = static_cast<unsigned char>(text->Data().text.front());
    const TextOffset first_scalar_length = first_byte >= 0xF0 ? 2 : 1;
    editor.RevealTextRange(*text, {0, first_scalar_length}, ScrollAlignment::Start);
  }
}

} // namespace folio

#pragma once

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor::detail {

using BlockId = std::uint64_t;
using BlockPath = std::vector<std::size_t>;

inline constexpr std::size_t max_nesting = 128;
inline constexpr std::size_t max_table_cells = 4096;
inline constexpr Mark all_marks = Mark::Bold | Mark::Italic | Mark::Strike | Mark::Code | Mark::Underline | Mark::Highlight;

inline bool IsTextBlock(BlockKind kind) {
  return kind == BlockKind::Paragraph || kind == BlockKind::Heading || kind == BlockKind::CodeBlock;
}

inline bool IsAtomicBlock(BlockKind kind) {
  return kind == BlockKind::Image || kind == BlockKind::Custom;
}

inline bool IsLeaf(BlockKind kind) {
  return IsTextBlock(kind) || IsAtomicBlock(kind);
}

inline bool ValidTableSize(std::size_t rows, std::size_t columns) {
  return columns != 0 && rows != 0 && rows <= max_table_cells / columns;
}

inline bool Has(Mark marks, Mark mark) {
  return (marks & mark) != Mark::None;
}

inline Mark Without(Mark marks, Mark mark) {
  return static_cast<Mark>(static_cast<unsigned>(marks) & ~static_cast<unsigned>(mark));
}

struct Leaf {
  Block block;
  BlockPath path;
  /// Start in the flattened UTF-16 text, which places one LF between successive leaves.
  TextOffset start;
};

struct TableCellLocation {
  std::size_t table;
  std::size_t row;
  std::size_t column;
};

struct DocumentStorage {
  std::vector<Block> blocks;
  std::vector<Leaf> leaves;
  std::unordered_map<BlockId, std::size_t> indexes;
  TextOffset length = 0;
  BlockId max_id = 0;
};

struct DocumentAccess {
  static const DocumentStorage& Get(const RichDocument& document) { return *document.storage_; }
  static std::shared_ptr<const DocumentStorage> Storage(const RichDocument& document) { return document.storage_; }
  static bool Same(const RichDocument& first, const RichDocument& second) { return first.storage_ == second.storage_; }
  static BlockId Id(const Block& block) { return block.id_; }
  static const AttributedText& Body(const Block& block);
  static Block WithData(const Block& block, BlockData data);
  static RichDocument Build(std::vector<Block> blocks, BlockId& last_id, bool fresh = false,
                            std::vector<BlockId>* added_text_blocks = nullptr);
};

inline const Leaf* FindTextLeaf(const DocumentStorage& document, BlockId id) {
  const auto found = document.indexes.find(id);
  if (found != document.indexes.end() && IsTextBlock(document.leaves[found->second].block.Data().kind)) {
    return &document.leaves[found->second];
  }
  return nullptr;
}

/// A document position anchored to a leaf identity. The offset is UTF-16 relative to that leaf.
struct Position {
  BlockId block = 0;
  TextOffset offset = 0;
  TextAffinity affinity = TextAffinity::Downstream;
  bool operator==(const Position&) const = default;
};

struct Selection {
  Position anchor;
  Position active;
  bool operator==(const Selection&) const = default;
};

const Block& BlockAt(const RichDocument& document, std::span<const std::size_t> path);
std::optional<TableCellLocation> TableCellAt(const RichDocument& document, const Leaf& leaf);
std::size_t LeafIndex(const RichDocument& document, TextOffset absolute);
Position PositionAt(const RichDocument& document, TextOffset absolute, TextAffinity affinity = TextAffinity::Downstream);
TextOffset Absolute(const RichDocument& document, Position position);
std::string TextSlice(const RichDocument& document, TextRange range);
bool SameText(const RichDocument& first, const RichDocument& second);
std::string NormalizeLineEndings(std::string_view text);
void AddMark(std::vector<MarkRange>& marks, TextRange range, Mark value);
bool ValidImageSource(std::string_view source);
bool ValidImageWidth(std::optional<float> width);
bool ValidImageData(const ImageBlockData& image);
bool ValidCustomData(const CustomBlockData& custom);

} // namespace huxerui::richeditor::detail

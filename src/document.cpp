#include "document_internal.h"

#include <huxerui/richeditor/math.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace huxerui::richeditor {

struct Block::Storage {
  BlockData data;
  AttributedText body;
};

Block::Block(BlockData data)
    : storage_(std::make_shared<const Storage>(Storage{std::move(data), {}})) {}

const BlockData& Block::Data() const noexcept { return storage_->data; }

bool Block::operator==(const Block& other) const {
  return storage_ == other.storage_ || Data() == other.Data();
}

RichDocument::RichDocument() : RichDocument(Create({})) {}

RichDocument::RichDocument(std::shared_ptr<const detail::DocumentStorage> storage) : storage_(std::move(storage)) {}

RichDocument RichDocument::Create(std::vector<Block> blocks) {
  detail::BlockId last_id = 0;
  return detail::DocumentAccess::Build(std::move(blocks), last_id, true);
}

std::span<const Block> RichDocument::Blocks() const noexcept { return storage_->blocks; }

bool RichDocument::operator==(const RichDocument& other) const {
  return storage_ == other.storage_ || storage_->blocks == other.storage_->blocks;
}

CustomBlockData LatexBlock(std::string source) {
  source = detail::NormalizeLineEndings(source);
  return {std::string(latex_block_type), 1, source, source};
}

namespace detail {
namespace {

bool HasInvalidControl(std::string_view text) {
  return text.find('\0') != text.npos || text.find('\r') != text.npos;
}

AttributedText BlockBody(const BlockData& data) {
  // Project each atomic block as one UTF-16 object replacement character.
  return IsAtomicBlock(data.kind) ? AttributedText("\xEF\xBF\xBC") : AttributedText(data.text);
}

void ValidateNesting(const BlockData& data, BlockKind parent, bool root) {
  const bool leaf = IsLeaf(data.kind);
  if ((!root && parent == BlockKind::List) != (data.kind == BlockKind::ListItem) ||
      (!root && parent == BlockKind::Table) != (data.kind == BlockKind::TableRow) ||
      (!root && parent == BlockKind::TableRow) != (data.kind == BlockKind::TableCell) ||
      (parent == BlockKind::TableCell && data.kind != BlockKind::Paragraph) ||
      (data.kind == BlockKind::TableCell && data.children.size() != 1) ||
      ((data.kind == BlockKind::Table || IsAtomicBlock(data.kind)) && !root) ||
      (leaf && !data.children.empty()) || (!leaf && data.children.empty())) {
    throw std::invalid_argument("Invalid block nesting");
  }
  if (data.kind == BlockKind::Table) {
    const auto columns = data.children.front().Data().children.size();
    if (!ValidTableSize(data.children.size(), columns) ||
        !std::ranges::all_of(data.children, [columns](const Block& row) {
          return row.Data().children.size() == columns;
        })) {
      throw std::invalid_argument("Invalid table dimensions");
    }
  }
}

void ValidateBlock(const BlockData& data, const AttributedText& body) {
  const bool text_block = IsTextBlock(data.kind);
  if (data.kind < BlockKind::Paragraph || data.kind > BlockKind::Custom ||
      (data.kind == BlockKind::Heading ? data.heading_level < 1 || data.heading_level > 6 : data.heading_level != 0) ||
      (data.kind != BlockKind::List && (data.ordered || data.list_start != 1 || !data.tight)) ||
      (!data.ordered && data.list_start != 1) ||
      (data.kind != BlockKind::CodeBlock && !data.fence_info.empty()) ||
      (data.kind != BlockKind::Table && data.table_has_header) ||
      (data.kind == BlockKind::Image) != data.image.has_value() ||
      (data.image && !ValidImageData(*data.image)) ||
      (data.kind == BlockKind::Custom) != data.custom.has_value() ||
      (data.custom && !ValidCustomData(*data.custom))) {
    throw std::invalid_argument("Invalid block attributes");
  }
  if (HasInvalidControl(data.text) || HasInvalidControl(data.fence_info) ||
      (data.kind == BlockKind::Heading && data.text.find('\n') != std::string::npos) ||
      (data.kind == BlockKind::CodeBlock && data.fence_info.find('\n') != std::string::npos)) {
    throw std::invalid_argument("Invalid block text or code info");
  }
  if ((!text_block && !data.text.empty()) ||
      ((!text_block || data.kind == BlockKind::CodeBlock) && (!data.marks.empty() || !data.links.empty()))) {
    throw std::invalid_argument("This block cannot contain inline content");
  }
  if (!data.fence_info.empty()) {
    const AttributedText info(data.fence_info);
  }
  TextOffset previous = 0;
  for (const auto& mark : data.marks) {
    if (mark.range.start < previous || mark.range.start >= mark.range.end ||
        mark.marks == Mark::None || Without(mark.marks, all_marks) != Mark::None) {
      throw std::invalid_argument("Invalid mark range");
    }
    (void)body.TextInRange({mark.range.start, mark.range.start});
    (void)body.TextInRange({mark.range.end, mark.range.end});
    previous = mark.range.end;
  }
  previous = 0;
  for (const auto& link : data.links) {
    if (link.range.start < previous || link.range.start >= link.range.end ||
        HasInvalidControl(link.target) || HasInvalidControl(link.title)) {
      throw std::invalid_argument("Invalid link range or attributes");
    }
    (void)body.TextInRange({link.range.start, link.range.start});
    (void)body.TextInRange({link.range.end, link.range.end});
    const AttributedText target(link.target);
    const AttributedText title(link.title);
    previous = link.range.end;
  }
}

} // namespace

bool ValidImageSource(std::string_view source) {
  if (source.empty() || HasInvalidControl(source) || source.find('\n') != source.npos) {
    return false;
  }
  try {
    (void)AttributedText(std::string(source));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool ValidImageWidth(std::optional<float> width) {
  return !width || (std::isfinite(*width) && *width > 0.0F);
}

bool ValidImageData(const ImageBlockData& image) {
  if (!ValidImageSource(image.source) || !ValidImageWidth(image.width) ||
      HasInvalidControl(image.alt_text) || HasInvalidControl(image.title)) {
    return false;
  }
  try {
    (void)AttributedText(image.alt_text);
    (void)AttributedText(image.title);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool ValidCustomData(const CustomBlockData& custom) {
  const auto dot = custom.type.find('.');
  if (custom.version == 0 || dot == std::string::npos || dot == 0 || dot + 1 == custom.type.size() ||
      custom.type.find("..") != std::string::npos ||
      custom.type.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
          std::string::npos ||
      HasInvalidControl(custom.payload) || HasInvalidControl(custom.fallback_text)) {
    return false;
  }
  try {
    (void)AttributedText(custom.payload);
    (void)AttributedText(custom.fallback_text);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

const AttributedText& DocumentAccess::Body(const Block& block) { return block.storage_->body; }

Block DocumentAccess::WithData(const Block& block, BlockData data) {
  const auto& old = *block.storage_;
  const bool same_projection = IsAtomicBlock(data.kind) == IsAtomicBlock(old.data.kind) &&
      data.text == old.data.text;
  const auto body = same_projection ? old.body : BlockBody(data);
  Block changed = block;
  changed.storage_ = std::make_shared<const Block::Storage>(Block::Storage{std::move(data), body});
  return changed;
}

RichDocument DocumentAccess::Build(std::vector<Block> blocks, BlockId& last_id, bool fresh,
                                   std::vector<BlockId>* added_text_blocks) {
  if (blocks.empty()) {
    blocks.emplace_back(BlockData{});
  }
  auto storage = std::make_shared<DocumentStorage>();
  BlockPath path;
  auto visit = [&](auto& self, std::vector<Block>& siblings, BlockKind parent) -> bool {
    if (path.size() >= max_nesting) {
      throw std::invalid_argument("Document nesting limit exceeded");
    }
    bool reassigned = false;
    for (std::size_t i = 0; i < siblings.size(); ++i) {
      Block& block = siblings[i];
      ValidateNesting(block.Data(), parent, path.empty());
      const bool needs_id = fresh || block.id_ == 0;
      if (needs_id) {
        auto data = block.Data();
        const auto body = BlockBody(data);
        ValidateBlock(data, body);
        auto marks = std::move(data.marks);
        data.marks.clear();
        for (const auto& mark : marks) {
          AddMark(data.marks, mark.range, mark.marks);
        }
        block.storage_ = std::make_shared<const Block::Storage>(Block::Storage{std::move(data), body});
        if (last_id == std::numeric_limits<BlockId>::max()) {
          throw std::length_error("Editor block identity limit exceeded");
        }
        block.id_ = ++last_id;
        if (added_text_blocks && IsTextBlock(block.Data().kind)) {
          added_text_blocks->push_back(block.id_);
        }
        reassigned = true;
      }
      path.push_back(i);
      if (IsLeaf(block.Data().kind)) {
        if (!storage->leaves.empty()) {
          ++storage->length;
        }
        storage->indexes.emplace(block.id_, storage->leaves.size());
        storage->leaves.push_back({block, path, storage->length});
        storage->length += Body(block).Length();
      } else {
        auto children = block.Data().children;
        if (self(self, children, block.Data().kind)) {
          auto data = block.Data();
          data.children = std::move(children);
          block = WithData(block, std::move(data));
          reassigned = true;
        }
      }
      path.pop_back();
    }
    return reassigned;
  };
  visit(visit, blocks, BlockKind::Paragraph);
  storage->blocks = std::move(blocks);
  storage->max_id = last_id;
  return RichDocument(std::move(storage));
}

const Block& BlockAt(const RichDocument& document, std::span<const std::size_t> path) {
  auto children = document.Blocks();
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    children = children[path[i]].Data().children;
  }
  return children[path.back()];
}

std::optional<TableCellLocation> TableCellAt(const RichDocument& document, const Leaf& leaf) {
  const auto& path = leaf.path;
  if (path.size() == 4 && document.Blocks()[path.front()].Data().kind == BlockKind::Table) {
    return TableCellLocation{path[0], path[1], path[2]};
  }
  return std::nullopt;
}

std::size_t LeafIndex(const RichDocument& document, TextOffset absolute) {
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto after = std::upper_bound(leaves.begin(), leaves.end(), absolute,
                                      [](TextOffset value, const Leaf& leaf) { return value < leaf.start; });
  return static_cast<std::size_t>(std::distance(leaves.begin(), after) - 1);
}

Position PositionAt(const RichDocument& document, TextOffset absolute, TextAffinity affinity) {
  const auto& storage = DocumentAccess::Get(document);
  if (absolute < 0 || absolute > storage.length ||
      (affinity != TextAffinity::Downstream && affinity != TextAffinity::Upstream)) {
    throw std::invalid_argument("Invalid editor position");
  }
  const auto& leaf = storage.leaves[LeafIndex(document, absolute)];
  const auto local = absolute - leaf.start;
  (void)DocumentAccess::Body(leaf.block).TextInRange({local, local});
  return {DocumentAccess::Id(leaf.block), local, affinity};
}

TextOffset Absolute(const RichDocument& document, Position position) {
  const auto& storage = DocumentAccess::Get(document);
  return storage.leaves[storage.indexes.at(position.block)].start + position.offset;
}

std::string TextSlice(const RichDocument& document, TextRange range) {
  if (!range.IsValid()) {
    throw std::invalid_argument("Invalid editor range");
  }
  (void)PositionAt(document, range.start);
  (void)PositionAt(document, range.end);
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto first = LeafIndex(document, range.start);
  const auto last = LeafIndex(document, range.end);
  std::string result;
  for (std::size_t i = first; i <= last; ++i) {
    const auto& leaf = leaves[i];
    const auto& body = DocumentAccess::Body(leaf.block);
    if (i != first) {
      result += '\n';
    }
    const auto from = i == first ? range.start - leaf.start : 0;
    const auto to = i == last ? range.end - leaf.start : body.Length();
    if (from == 0 && to == body.Length()) {
      result += body.PlainText();
    } else {
      result += body.TextInRange({from, to});
    }
  }
  return result;
}

bool SameText(const RichDocument& first, const RichDocument& second) {
  if (DocumentAccess::Same(first, second)) {
    return true;
  }
  const auto& a = DocumentAccess::Get(first);
  const auto& b = DocumentAccess::Get(second);
  if (a.length != b.length) {
    return false;
  }
  if (a.leaves.size() == b.leaves.size() &&
      std::equal(a.leaves.begin(), a.leaves.end(), b.leaves.begin(), [](const Leaf& x, const Leaf& y) {
        return DocumentAccess::Body(x.block).PlainText() == DocumentAccess::Body(y.block).PlainText();
      })) {
    return true;
  }
  return TextSlice(first, {0, a.length}) == TextSlice(second, {0, b.length});
}

std::string NormalizeLineEndings(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      result += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    } else {
      result += text[i];
    }
  }
  return result;
}

void AddMark(std::vector<MarkRange>& marks, TextRange range, Mark value) {
  if (range.IsCollapsed() || value == Mark::None) {
    return;
  }
  if (!marks.empty() && marks.back().marks == value && marks.back().range.end == range.start) {
    marks.back().range.end = range.end;
  } else {
    marks.push_back({range, value});
  }
}

} // namespace detail
} // namespace huxerui::richeditor

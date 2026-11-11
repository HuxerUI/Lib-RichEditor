#include "document_internal.h"

#include <atomic>
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
  return detail::DocumentAccess::Build(std::move(blocks), last_id);
}

std::span<const Block> RichDocument::Blocks() const noexcept { return storage_->blocks; }

bool RichDocument::operator==(const RichDocument& other) const {
  return storage_ == other.storage_ || storage_->blocks == other.storage_->blocks;
}

namespace detail {
namespace {

bool HasInvalidControl(std::string_view text) {
  return text.find('\0') != text.npos || text.find('\r') != text.npos;
}

AttributedText BlockBody(const BlockData& data) {
  // Project each atomic block as one UTF-16 object replacement character.
  return IsAtomicBlock(data.kind) ? AttributedText(std::string(object_replacement)) : AttributedText(data.text);
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

namespace {

bool ValidCustomContent(std::string_view type, unsigned version, const std::string& payload,
                        const std::string& fallback) {
  const auto dot = type.find('.');
  if (version == 0 || dot == type.npos || dot == 0 || dot + 1 == type.size() ||
      type.find("..") != type.npos ||
      type.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != type.npos ||
      HasInvalidControl(payload) || HasInvalidControl(fallback)) {
    return false;
  }
  try {
    (void)AttributedText(payload);
    (void)AttributedText(fallback);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

} // namespace

bool ValidCustomData(const CustomBlockData& custom) {
  return ValidCustomContent(custom.type, custom.version, custom.payload, custom.fallback_text);
}

bool ValidInlineData(const InlineObjectData& object) {
  if (!ValidCustomContent(object.type, object.version, object.payload, object.fallback_text) ||
      object.fallback_text.find('\n') != std::string::npos) {
    return false;
  }
  std::vector<std::string_view> roles;
  for (const auto& attachment : object.attachments) {
    if (attachment.role.empty() ||
        attachment.role.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
            std::string::npos ||
        !ValidImageSource(attachment.source) || std::ranges::find(roles, attachment.role) != roles.end()) {
      return false;
    }
    roles.push_back(attachment.role);
  }
  return true;
}

void DocumentAccess::PrepareInlineObjects(BlockData& data, const AttributedText& body, bool fresh) {
  if (data.inline_objects.empty()) {
    return;
  }
  if (data.kind != BlockKind::Paragraph && data.kind != BlockKind::Heading) {
    throw std::invalid_argument("Inline objects require a paragraph or heading");
  }
  static std::atomic<std::uint64_t> last_id{0};
  TextOffset previous = -1;
  for (auto& object : data.inline_objects) {
    if (object.offset <= previous || object.offset >= body.Length() ||
        body.TextInRange({object.offset, object.offset + 1}) != object_replacement ||
        !ValidInlineData(object.data)) {
      throw std::invalid_argument("Invalid inline object");
    }
    if (fresh || object.id_ == 0) {
      object.id_ = ++last_id;
    }
    previous = object.offset;
  }
}

const AttributedText& DocumentAccess::Body(const Block& block) { return block.storage_->body; }

Block DocumentAccess::WithData(const Block& block, BlockData data) {
  const auto& old = *block.storage_;
  const bool same_projection = IsAtomicBlock(data.kind) == IsAtomicBlock(old.data.kind) &&
      data.text == old.data.text;
  const auto body = same_projection ? old.body : BlockBody(data);
  PrepareInlineObjects(data, body, false);
  Block changed = block;
  changed.storage_ = std::make_shared<const Block::Storage>(Block::Storage{std::move(data), body});
  return changed;
}

void DocumentAccess::ValidatePlacement(const Block& block, BlockKind parent, bool root) {
  ValidateNesting(block.Data(), parent, root);
}

void DocumentAccess::AssignId(Block& block, BlockId& last_id, std::vector<BlockId>* added_text_blocks, bool fresh) {
  auto data = block.Data();
  const auto body = BlockBody(data);
  ValidateBlock(data, body);
  PrepareInlineObjects(data, body, fresh);
  auto marks = std::move(data.marks);
  data.marks.clear();
  for (const auto& mark : marks) {
    AddMark(data.marks, mark.range, mark.marks);
  }
  if (last_id == std::numeric_limits<BlockId>::max()) {
    throw std::length_error("Editor block identity limit exceeded");
  }
  block.storage_ = std::make_shared<const Block::Storage>(Block::Storage{std::move(data), body});
  block.id_ = ++last_id;
  if (added_text_blocks && IsTextBlock(block.Data().kind)) {
    added_text_blocks->push_back(block.id_);
  }
}

RichDocument DocumentAccess::Build(std::vector<Block> blocks, BlockId& last_id, const RichDocument* previous,
                                   const ChildChanges& changes, std::vector<BlockId>* added_text_blocks) {
  if (blocks.empty()) {
    blocks.emplace_back(BlockData{});
  }
  auto index = DocumentIndex::Build(blocks, last_id, previous ? &Get(*previous).index : nullptr,
                                    previous ? previous->Blocks() : std::span<const Block>{},
                                    changes, added_text_blocks);
  return RichDocument(std::make_shared<const DocumentStorage>(
      DocumentStorage{std::move(blocks), std::move(index), last_id}));
}

const Block& BlockAt(const RichDocument& document, std::span<const std::size_t> path) {
  auto children = document.Blocks();
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    children = children[path[i]].Data().children;
  }
  return children[path.back()];
}

bool ValidTextRange(const Block& block, TextRange range) {
  const auto& body = DocumentAccess::Body(block);
  if (!range.IsValid() || range.IsCollapsed() || range.end > body.Length()) {
    return false;
  }
  try {
    (void)body.TextInRange({range.start, range.start});
    (void)body.TextInRange({range.end, range.end});
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::optional<TableCellLocation> TableCellAt(const RichDocument& document, const Leaf& leaf) {
  const auto& path = leaf.path;
  if (path.size() == 4 && document.Blocks()[path.front()].Data().kind == BlockKind::Table) {
    return TableCellLocation{path[0], path[1], path[2]};
  }
  return std::nullopt;
}

std::size_t LeafIndex(const RichDocument& document, TextOffset absolute) {
  return DocumentAccess::Get(document).index.AtOffset(absolute).ordinal;
}

Position PositionAt(const RichDocument& document, TextOffset absolute, TextAffinity affinity) {
  const auto& index = DocumentAccess::Get(document).index;
  if (absolute < 0 || absolute > index.Length() ||
      (affinity != TextAffinity::Downstream && affinity != TextAffinity::Upstream)) {
    throw std::invalid_argument("Invalid editor position");
  }
  const auto leaf = index.AtOffset(absolute);
  const auto local = absolute - leaf.start;
  (void)DocumentAccess::Body(leaf.block).TextInRange({local, local});
  return {DocumentAccess::Id(leaf.block), local, affinity};
}

TextOffset Absolute(const RichDocument& document, Position position) {
  return DocumentAccess::Get(document).index.StartOf(position.block) + position.offset;
}

static std::string Slice(const RichDocument& document, TextRange range, bool plain) {
  if (!range.IsValid()) {
    throw std::invalid_argument("Invalid editor range");
  }
  (void)PositionAt(document, range.start);
  (void)PositionAt(document, range.end);
  const auto& index = DocumentAccess::Get(document).index;
  const auto first = index.AtOffset(range.start).ordinal;
  const auto last = index.AtOffset(range.end).ordinal;
  std::string result;
  auto leaf = index.Leaves(first);
  for (auto i = first; i <= last; ++i, leaf.Next()) {
    const auto& body = DocumentAccess::Body(leaf->block);
    if (i != first) {
      result += '\n';
    }
    const auto from = i == first ? range.start - leaf->start : 0;
    const auto to = i == last ? range.end - leaf->start : body.Length();
    const auto& data = leaf->block.Data();
    if (plain && from == 0 && to == 1 && IsAtomicBlock(data.kind)) {
      result += data.image ? data.image->alt_text : data.custom->fallback_text;
    } else if (plain && !data.inline_objects.empty()) {
      auto start = from;
      for (const auto& object : data.inline_objects) {
        if (object.offset >= from && object.offset < to) {
          result += body.TextInRange({start, object.offset});
          result += object.data.fallback_text;
          start = object.offset + 1;
        }
      }
      result += body.TextInRange({start, to});
    } else if (from == 0 && to == body.Length()) {
      result += body.PlainText();
    } else {
      result += body.TextInRange({from, to});
    }
  }
  return result;
}

std::string TextSlice(const RichDocument& document, TextRange range) { return Slice(document, range, false); }

std::string PlainTextSlice(const RichDocument& document, TextRange range) { return Slice(document, range, true); }

bool SameText(const RichDocument& first, const RichDocument& second) {
  if (DocumentAccess::Same(first, second)) {
    return true;
  }
  const auto& a = DocumentAccess::Get(first).index;
  const auto& b = DocumentAccess::Get(second).index;
  if (a.Length() != b.Length()) {
    return false;
  }
  if (a.LeafCount() == b.LeafCount()) {
    auto left = a.Leaves();
    auto right = b.Leaves();
    for (; left && DocumentAccess::Body(left->block).PlainText() == DocumentAccess::Body(right->block).PlainText();
         left.Next(), right.Next()) {
    }
    if (!left) {
      return true;
    }
  }
  return TextSlice(first, {0, a.Length()}) == TextSlice(second, {0, b.Length()});
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

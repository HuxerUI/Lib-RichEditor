#include "edit_operations.h"

#include <limits>
#include <stdexcept>

namespace huxerui::richeditor::detail {

void EditChanges::Touch(const Block& block) {
  if (IsLeaf(block.Data().kind)) {
    touched.push_back(DocumentAccess::Id(block));
  }
  for (const auto& child : block.Data().children) {
    Touch(child);
  }
}

RichDocument EditChanges::Build(std::vector<Block> blocks) {
  return DocumentAccess::Build(std::move(blocks), last_id, false, &touched);
}

namespace {

using Replacements = std::unordered_map<BlockId, std::vector<Block>>;

std::vector<Block> Rewrite(std::span<const Block> blocks, const Replacements& changes, bool& changed) {
  std::vector<Block> result;
  for (const auto& block : blocks) {
    const auto found = changes.find(DocumentAccess::Id(block));
    if (found != changes.end()) {
      result.insert(result.end(), found->second.begin(), found->second.end());
      changed = true;
    } else if (IsLeaf(block.Data().kind)) {
      result.push_back(block);
    } else {
      bool child_changed = false;
      auto children = Rewrite(block.Data().children, changes, child_changed);
      changed |= child_changed;
      if (!children.empty()) {
        if (child_changed) {
          auto data = block.Data();
          data.children = std::move(children);
          result.push_back(DocumentAccess::WithData(block, std::move(data)));
        } else {
          result.push_back(block);
        }
      }
    }
  }
  return result;
}

RichDocument Rewrite(const RichDocument& document, const Replacements& changes, EditChanges& edit) {
  bool changed = false;
  auto blocks = Rewrite(document.Blocks(), changes, changed);
  return changed ? edit.Build(std::move(blocks)) : document;
}

TextRange Clip(TextRange range, TextRange within) {
  const auto start = std::max(range.start, within.start);
  return {start, std::max(start, std::min(range.end, within.end))};
}

struct LeafBuilder {
  BlockData data;
  TextOffset length = 0;
  const LinkRange* last_link = nullptr;

  void AppendLink(TextRange range, const LinkRange& link) {
    if (last_link == &link && data.links.back().range.end == range.start) {
      data.links.back().range.end = range.end;
    } else {
      data.links.push_back({range, link.target, link.title});
    }
    last_link = &link;
  }

  void AppendSlice(const Block& block, TextRange range) {
    data.text += DocumentAccess::Body(block).TextInRange(range);
    if (data.kind != BlockKind::CodeBlock) {
      for (const auto& mark : block.Data().marks) {
        const auto part = Clip(mark.range, range);
        if (!part.IsCollapsed()) {
          AddMark(data.marks, {length + part.start - range.start, length + part.end - range.start}, mark.marks);
        }
      }
      for (const auto& link : block.Data().links) {
        const auto part = Clip(link.range, range);
        if (!part.IsCollapsed()) {
          AppendLink({length + part.start - range.start, length + part.end - range.start}, link);
        }
      }
    }
    length += range.Length();
  }

  void AppendText(std::string_view text, TextOffset size, Mark marks, const LinkRange* link) {
    data.text += text;
    if (data.kind != BlockKind::CodeBlock) {
      AddMark(data.marks, {length, length + size}, marks);
      if (link && size != 0) {
        AppendLink({length, length + size}, *link);
      }
    }
    length += size;
  }
};

Mark MarkAt(const Block& block, TextOffset offset) {
  for (const auto& run : block.Data().marks) {
    if (run.range.start <= offset && offset < run.range.end) {
      return run.marks;
    }
  }
  return Mark::None;
}

std::pair<std::size_t, std::size_t> TouchedLeaves(const RichDocument& document, TextRange range) {
  const auto first = LeafIndex(document, range.start);
  auto last = LeafIndex(document, range.end);
  if (last != first && DocumentAccess::Get(document).leaves[last].start == range.end) {
    --last;
  }
  return {first, last};
}

bool TouchesRoot(const RichDocument& document, TextRange range, auto matches) {
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto first = leaves[LeafIndex(document, range.start)].path.front();
  const auto last = leaves[LeafIndex(document, range.end)].path.front();
  return std::ranges::any_of(document.Blocks().subspan(first, last - first + 1), [matches](const Block& block) {
    return matches(block.Data().kind);
  });
}

bool TouchesRootKind(const RichDocument& document, TextRange range, BlockKind kind) {
  return TouchesRoot(document, range, [kind](BlockKind value) { return value == kind; });
}

bool TouchesAtomicBlock(const RichDocument& document, TextRange range) {
  return TouchesRoot(document, range, IsAtomicBlock);
}

TextRange LocalRange(const Leaf& leaf, TextRange range) {
  const auto length = DocumentAccess::Body(leaf.block).Length();
  return {std::clamp(range.start - leaf.start, TextOffset{0}, length),
          std::clamp(range.end - leaf.start, TextOffset{0}, length)};
}

std::vector<MarkRange> ChangeMarks(const Block& block, TextRange range, Mark mark, bool remove) {
  std::vector<MarkRange> result;
  TextOffset cursor = range.start;
  const auto change = [&](TextRange part, Mark old) {
    AddMark(result, part, remove ? Without(old, mark) : old | mark);
  };
  for (const auto& run : block.Data().marks) {
    if (run.range.end <= range.start) {
      AddMark(result, run.range, run.marks);
      continue;
    }
    if (run.range.start >= range.end) {
      change({cursor, range.end}, Mark::None);
      cursor = range.end;
      AddMark(result, run.range, run.marks);
      continue;
    }
    const auto part = Clip(run.range, range);
    if (run.range.start < part.start) {
      AddMark(result, {run.range.start, part.start}, run.marks);
    }
    change({cursor, part.start}, Mark::None);
    change(part, run.marks);
    cursor = part.end;
    if (part.end < run.range.end) {
      AddMark(result, {part.end, run.range.end}, run.marks);
    }
  }
  change({cursor, range.end}, Mark::None);
  return result;
}

const LinkRange* FindLink(const Leaf& leaf, TextRange range) {
  const auto& links = leaf.block.Data().links;
  const auto start = range.start - leaf.start;
  const auto end = range.end - leaf.start;
  const auto found = std::ranges::upper_bound(links, start, {}, [](const LinkRange& link) { return link.range.end; });
  if (found != links.end() && found->range.start <= start && end <= found->range.end) {
    return &*found;
  }
  return nullptr;
}

std::vector<LinkRange> ChangeLink(const Block& block, TextRange range, const SetLink& link) {
  std::vector<LinkRange> links;
  for (auto run : block.Data().links) {
    if (run.range.end <= range.start || run.range.start >= range.end) {
      links.push_back(std::move(run));
      continue;
    }
    if (run.range.start < range.start) {
      links.push_back({{run.range.start, range.start}, run.target, run.title});
    }
    if (run.range.end > range.end) {
      links.push_back({{range.end, run.range.end}, std::move(run.target), std::move(run.title)});
    }
  }
  if (link.target) {
    const auto pos = std::ranges::lower_bound(links, range.start, {}, [](const LinkRange& run) { return run.range.start; });
    links.insert(pos, {range, *link.target, link.title});
  }
  return links;
}

struct TreeRange {
  BlockPath parent;
  std::size_t first;
  std::size_t last;
};

std::span<const Block> ChildrenAt(const RichDocument& document, std::span<const std::size_t> parent) {
  return parent.empty() ? document.Blocks() : std::span<const Block>(BlockAt(document, parent).Data().children);
}

std::vector<Block> BlocksBetween(std::span<const Block> blocks, std::size_t begin, std::size_t end) {
  const auto part = blocks.subspan(begin, end - begin);
  return {part.begin(), part.end()};
}

RichDocument Splice(const RichDocument& document, const TreeRange& range, std::vector<Block> replacement, EditChanges& edit) {
  const auto siblings = ChildrenAt(document, range.parent);
  for (const auto& block : siblings.subspan(range.first, range.last - range.first + 1)) {
    edit.Touch(block);
  }
  auto children = BlocksBetween(siblings, 0, range.first);
  children.insert(children.end(), replacement.begin(), replacement.end());
  children.insert(children.end(), siblings.begin() + static_cast<std::ptrdiff_t>(range.last + 1), siblings.end());
  if (range.parent.empty()) {
    return edit.Build(std::move(children));
  }
  const auto& parent = BlockAt(document, range.parent);
  auto data = parent.Data();
  data.children = std::move(children);
  Replacements changes;
  if (!data.children.empty()) {
    changes[DocumentAccess::Id(parent)] = {DocumentAccess::WithData(parent, std::move(data))};
  } else {
    changes[DocumentAccess::Id(parent)] = {};
  }
  return Rewrite(document, changes, edit);
}

TreeRange Siblings(const RichDocument& document, TextRange range) {
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto& a = leaves[first].path;
  const auto& b = leaves[last].path;
  std::size_t depth = 0;
  while (depth + 1 < a.size() && depth + 1 < b.size() && a[depth] == b[depth]) {
    ++depth;
  }
  BlockPath parent(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(depth));
  if (!parent.empty() && BlockAt(document, parent).Data().kind == BlockKind::List) {
    parent.pop_back();
    --depth;
  }
  return {std::move(parent), a[depth], b[depth]};
}

std::optional<TreeRange> SelectedItems(const RichDocument& document, TextRange range) {
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto& a = leaves[first].path;
  const auto& b = leaves[last].path;
  BlockPath common;
  std::optional<TreeRange> result;
  for (std::size_t depth = 0; depth + 1 < a.size() && depth + 1 < b.size() && a[depth] == b[depth]; ++depth) {
    common.push_back(a[depth]);
    if (BlockAt(document, common).Data().kind == BlockKind::List) {
      result = TreeRange{common, a[depth + 1], b[depth + 1]};
    }
  }
  return result;
}

TreeRange ParentSlot(BlockPath path) {
  const auto index = path.back();
  path.pop_back();
  return {std::move(path), index, index};
}

Block ListSlice(const Block& list, std::size_t begin, std::size_t end) {
  auto data = list.Data();
  data.children = BlocksBetween(data.children, begin, end);
  if (data.ordered) {
    data.list_start = static_cast<unsigned>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(data.list_start) + begin, std::numeric_limits<unsigned>::max()));
  }
  return Block(std::move(data));
}

void AppendBlocks(std::vector<Block>& destination, std::span<const Block> blocks) {
  destination.insert(destination.end(), blocks.begin(), blocks.end());
}

RichDocument ChangeList(const RichDocument& document, TextRange range, bool ordered, EditChanges& edit) {
  if (const auto items = SelectedItems(document, range)) {
    const auto& list = BlockAt(document, items->parent);
    const auto& data = list.Data();
    std::vector<Block> blocks;
    if (items->first != 0) {
      blocks.push_back(ListSlice(list, 0, items->first));
    }
    if (data.ordered != ordered) {
      blocks.emplace_back(BlockData{.kind = BlockKind::List,
                                    .children = BlocksBetween(data.children, items->first, items->last + 1),
                                    .ordered = ordered, .tight = data.tight});
    } else {
      for (std::size_t i = items->first; i <= items->last; ++i) {
        AppendBlocks(blocks, data.children[i].Data().children);
      }
    }
    if (items->last + 1 < data.children.size()) {
      blocks.push_back(ListSlice(list, items->last + 1, data.children.size()));
    }
    return Splice(document, ParentSlot(items->parent), std::move(blocks), edit);
  }

  const auto selected = Siblings(document, range);
  const auto siblings = ChildrenAt(document, selected.parent);
  std::vector<Block> items;
  for (std::size_t i = selected.first; i <= selected.last; ++i) {
    items.emplace_back(BlockData{.kind = BlockKind::ListItem, .children = {siblings[i]}});
  }
  return Splice(document, selected, {Block(BlockData{.kind = BlockKind::List, .children = std::move(items),
                                                   .ordered = ordered})}, edit);
}

RichDocument ChangeQuote(const RichDocument& document, TextRange range, EditChanges& edit) {
  const auto selected = Siblings(document, range);
  const auto siblings = ChildrenAt(document, selected.parent);
  auto blocks = BlocksBetween(siblings, selected.first, selected.last + 1);
  if (!selected.parent.empty() && BlockAt(document, selected.parent).Data().kind == BlockKind::Quote) {
    std::vector<Block> replacement;
    if (selected.first != 0) {
      replacement.emplace_back(BlockData{.kind = BlockKind::Quote, .children = BlocksBetween(siblings, 0, selected.first)});
    }
    AppendBlocks(replacement, blocks);
    if (selected.last + 1 < siblings.size()) {
      replacement.emplace_back(BlockData{.kind = BlockKind::Quote,
                                         .children = BlocksBetween(siblings, selected.last + 1, siblings.size())});
    }
    return Splice(document, ParentSlot(selected.parent), std::move(replacement), edit);
  }
  return Splice(document, selected, {Block(BlockData{.kind = BlockKind::Quote, .children = std::move(blocks)})}, edit);
}

RichDocument Indent(const RichDocument& document, const TreeRange& items, EditChanges& edit) {
  const auto& list = BlockAt(document, items.parent).Data();
  const auto& previous = list.children[items.first - 1];
  auto item = previous.Data();
  auto moving = BlocksBetween(list.children, items.first, items.last + 1);
  auto& tail = item.children.back();
  if (tail.Data().kind == BlockKind::List && tail.Data().ordered == list.ordered) {
    auto nested = tail.Data();
    AppendBlocks(nested.children, moving);
    tail = DocumentAccess::WithData(tail, std::move(nested));
  } else {
    item.children.emplace_back(BlockData{.kind = BlockKind::List, .children = std::move(moving),
                                        .ordered = list.ordered, .tight = list.tight});
  }
  return Splice(document, {items.parent, items.first - 1, items.last},
                {DocumentAccess::WithData(previous, std::move(item))}, edit);
}

RichDocument Outdent(const RichDocument& document, const TreeRange& items, EditChanges& edit) {
  const auto& list = BlockAt(document, items.parent);
  const auto& children = list.Data().children;
  const auto slot = ParentSlot(items.parent);
  const auto before = items.first == 0 ? std::nullopt : std::optional(ListSlice(list, 0, items.first));
  const auto after = items.last + 1 == children.size() ? std::nullopt
                                                      : std::optional(ListSlice(list, items.last + 1, children.size()));
  std::vector<Block> replacement;
  if (!slot.parent.empty() && BlockAt(document, slot.parent).Data().kind == BlockKind::ListItem) {
    const auto& parent = BlockAt(document, slot.parent);
    auto first_item = parent.Data();
    auto trailing = BlocksBetween(first_item.children, slot.first + 1, first_item.children.size());
    first_item.children.erase(first_item.children.begin() + static_cast<std::ptrdiff_t>(slot.first), first_item.children.end());
    if (before) {
      first_item.children.push_back(*before);
    }
    if (!first_item.children.empty()) {
      replacement.push_back(DocumentAccess::WithData(parent, std::move(first_item)));
    }
    AppendBlocks(replacement, std::span(children).subspan(items.first, items.last - items.first + 1));
    auto last_item = replacement.back().Data();
    if (after) {
      last_item.children.push_back(*after);
    }
    AppendBlocks(last_item.children, trailing);
    replacement.back() = DocumentAccess::WithData(replacement.back(), std::move(last_item));
    return Splice(document, ParentSlot(slot.parent), std::move(replacement), edit);
  }

  if (before) {
    replacement.push_back(*before);
  }
  for (std::size_t i = items.first; i <= items.last; ++i) {
    AppendBlocks(replacement, children[i].Data().children);
  }
  if (after) {
    replacement.push_back(*after);
  }
  return Splice(document, slot, std::move(replacement), edit);
}

Block EmptyCell() {
  return Block(BlockData{.kind = BlockKind::TableCell, .children = {Block(BlockData{})}});
}

Block EmptyRow(std::size_t columns) {
  return Block(BlockData{.kind = BlockKind::TableRow, .children = std::vector<Block>(columns, EmptyCell())});
}

void SelectBlock(Selection& selection, const Block& block, bool at_end = false) {
  const Block* leaf = &block;
  while (!IsLeaf(leaf->Data().kind)) {
    const auto& children = leaf->Data().children;
    leaf = at_end ? &children.back() : &children.front();
  }
  const Position caret{DocumentAccess::Id(*leaf), at_end ? DocumentAccess::Body(*leaf).Length() : 0};
  selection = {caret, caret};
}

} // namespace

bool CanEditTextRange(const RichDocument& document, TextRange range) {
  const auto first = LeafIndex(document, range.start);
  const auto last = LeafIndex(document, range.end);
  if (first == last) {
    return true;
  }
  const auto& leaves = DocumentAccess::Get(document).leaves;
  // An atomic block's separator can only be removed together with the block.
  return !TouchesRootKind(document, range, BlockKind::Table) &&
      (!IsAtomicBlock(leaves[first].block.Data().kind) || range.start == leaves[first].start) &&
      (!IsAtomicBlock(leaves[last].block.Data().kind) || range.end > leaves[last].start);
}

EditResult InsertParagraphAtAtomicBlock(const RichDocument& document, TextOffset caret, EditChanges& edit) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, caret)];
  const bool after = caret != leaf.start;
  const Block paragraph(BlockData{});
  auto blocks = after ? std::vector<Block>{leaf.block, paragraph} : std::vector<Block>{paragraph, leaf.block};
  return {Splice(document, ParentSlot(leaf.path), std::move(blocks), edit), caret + after, after, !after};
}

EditResult ReplaceText(const RichDocument& document, TextRange range, std::string_view input,
                       std::optional<Mark> marks, bool hard_break, EditChanges& edit) {
  if (!range.IsValid() || input.find('\0') != input.npos) {
    throw std::invalid_argument("Invalid text replacement");
  }
  const auto start = PositionAt(document, range.start);
  const auto finish = PositionAt(document, range.end);
  if (!CanEditTextRange(document, range)) {
    throw std::invalid_argument("Text replacement crosses a protected block boundary");
  }
  const auto& storage = DocumentAccess::Get(document);
  const auto first = storage.indexes.at(start.block);
  const auto last = storage.indexes.at(finish.block);
  const auto& head = storage.leaves[first].block;
  const auto& tail = storage.leaves[last].block;
  const bool head_atomic = IsAtomicBlock(head.Data().kind);
  const bool tail_atomic = IsAtomicBlock(tail.Data().kind);
  hard_break |= TableCellAt(document, storage.leaves[first]).has_value();
  auto text = NormalizeLineEndings(input);
  const auto inserted_length = AttributedText(text).Length();
  if (head_atomic && range.IsCollapsed()) {
    if (text.empty()) {
      return {document, range.start};
    }
    auto opened = InsertParagraphAtAtomicBlock(document, range.start, edit);
    auto result = ReplaceText(opened.document, {opened.caret, opened.caret}, text, marks, hard_break, edit);
    result.leading_separator = opened.leading_separator;
    result.trailing_separator = opened.trailing_separator;
    return result;
  }
  if (head_atomic && tail_atomic && text.empty()) {
    const auto first_block = storage.leaves[first].path.front();
    const auto last_block = storage.leaves[last].path.front();
    const bool has_next = last_block + 1 < document.Blocks().size();
    const TextOffset leading = !has_next && first_block != 0 ? -1 : 0;
    return {Splice(document, {{}, first_block, last_block}, {}, edit),
            range.start + leading, leading, has_next ? -1 : 0};
  }

  if (!head_atomic && first == last && DocumentAccess::Body(head).TextInRange({start.offset, finish.offset}) == text) {
    return {document, range.start + inserted_length};
  }

  LeafBuilder builder;
  const auto& base = head_atomic ? tail.Data() : head.Data();
  if (IsTextBlock(base.kind)) {
    builder.data.kind = base.kind;
    builder.data.heading_level = base.heading_level;
    builder.data.fence_info = base.fence_info;
  }
  if ((!head_atomic && base.kind == BlockKind::CodeBlock && tail.Data().kind != BlockKind::CodeBlock) ||
      (base.kind == BlockKind::Heading && tail.Data().text.find('\n') != std::string::npos)) {
    builder.data = BlockData{};
  }
  builder.AppendSlice(head, {0, start.offset});

  const Mark inserted_marks = marks.value_or(range.IsCollapsed() ? MarksAt(document, range.start) : MarkAt(head, start.offset));
  const LinkRange* link = nullptr;
  if (first == last) {
    for (const auto& candidate : head.Data().links) {
      if (candidate.range.start < start.offset && finish.offset < candidate.range.end) {
        link = &candidate;
        break;
      }
    }
  }

  std::vector<Block> blocks;
  if (builder.data.kind != BlockKind::CodeBlock && !(hard_break && builder.data.kind == BlockKind::Paragraph)) {
    std::size_t begin = 0;
    while (true) {
      const auto end = text.find('\n', begin);
      const auto part = std::string_view(text).substr(begin, end == text.npos ? text.npos : end - begin);
      builder.AppendText(part, AttributedText(std::string(part)).Length(), inserted_marks, link);
      if (end == text.npos) {
        break;
      }
      blocks.push_back(blocks.empty() ? DocumentAccess::WithData(head, std::move(builder.data))
                                     : Block(std::move(builder.data)));
      builder = {};
      begin = end + 1;
    }
  } else {
    builder.AppendText(text, inserted_length, inserted_marks, link);
  }
  if (!tail_atomic) {
    builder.AppendSlice(tail, {finish.offset, DocumentAccess::Body(tail).Length()});
  }
  blocks.push_back(blocks.empty() ? DocumentAccess::WithData(head, std::move(builder.data))
                                 : Block(std::move(builder.data)));

  edit.Touch(head);
  Replacements replacements{{start.block, std::move(blocks)}};
  for (std::size_t i = first + 1; i <= last; ++i) {
    const auto& block = storage.leaves[i].block;
    edit.Touch(block);
    replacements[DocumentAccess::Id(block)] = {};
  }
  return {Rewrite(document, replacements, edit), range.start + inserted_length};
}

Mark MarksAt(const RichDocument& document, TextOffset position) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, position)];
  const auto local = position - leaf.start;
  return MarkAt(leaf.block, local == 0 ? 0 : local - 1);
}

CommandState MarkStatus(const RichDocument& document, TextRange range, Mark mark, std::optional<Mark> pending) {
  if (!CanEditTextRange(document, range)) {
    return {};
  }
  if (range.IsCollapsed()) {
    const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, range.start)];
    return {IsTextBlock(leaf.block.Data().kind) && leaf.block.Data().kind != BlockKind::CodeBlock,
            Has(pending.value_or(MarksAt(document, range.start)), mark) ? CheckState::On : CheckState::Off};
  }

  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto [first, last] = TouchedLeaves(document, range);
  TextOffset length = 0;
  TextOffset marked = 0;
  for (std::size_t i = first; i <= last; ++i) {
    if (leaves[i].block.Data().kind == BlockKind::CodeBlock || IsAtomicBlock(leaves[i].block.Data().kind)) {
      continue;
    }
    const auto part = LocalRange(leaves[i], range);
    length += part.Length();
    for (const auto& run : leaves[i].block.Data().marks) {
      if (Has(run.marks, mark)) {
        marked += Clip(run.range, part).Length();
      }
    }
  }
  return {length != 0, marked == 0 ? CheckState::Off : marked == length ? CheckState::On : CheckState::Mixed};
}

const LinkRange* LinkAt(const RichDocument& document, TextRange range) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, range.start)];
  return FindLink(leaf, range);
}

bool CanSetLink(const RichDocument& document, TextRange range) {
  return range.IsCollapsed() ? LinkAt(document, range) != nullptr
                            : MarkStatus(document, range, Mark::Bold, {}).enabled;
}

CommandState BlockKindStatus(const RichDocument& document, TextRange range, const SetBlockKind& command) {
  if (!IsTextBlock(command.kind) || TouchesRootKind(document, range, BlockKind::Table) ||
      TouchesAtomicBlock(document, range)) {
    return {};
  }
  const bool heading = command.kind == BlockKind::Heading;
  if (heading && (command.heading_level == 0 || command.heading_level > 6)) {
    return {};
  }
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& leaves = DocumentAccess::Get(document).leaves;
  bool enabled = true;
  std::size_t matching = 0;
  for (std::size_t i = first; i <= last; ++i) {
    const auto& data = leaves[i].block.Data();
    if (heading && data.text.find('\n') != std::string::npos) {
      enabled = false;
    }
    if (data.kind == command.kind && (!heading || data.heading_level == command.heading_level)) {
      ++matching;
    }
  }
  const auto checked = matching == 0 ? CheckState::Off
      : matching == last - first + 1 ? CheckState::On : CheckState::Mixed;
  return {enabled, checked};
}

RichDocument Format(const RichDocument& document, TextRange range, const EditorCommand& command, bool remove_mark,
                    EditChanges& edit) {
  Replacements changes;
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto* link = std::get_if<SetLink>(&command);
  if (link && range.IsCollapsed()) {
    const auto& leaf = leaves[LeafIndex(document, range.start)];
    const auto* current = FindLink(leaf, range);
    if (!current) {
      return document;
    }
    range = {leaf.start + current->range.start, leaf.start + current->range.end};
  }
  const auto [first, last] = TouchedLeaves(document, range);
  for (std::size_t i = first; i <= last; ++i) {
    const auto& leaf = leaves[i];
    if (IsAtomicBlock(leaf.block.Data().kind)) {
      continue;
    }
    auto data = leaf.block.Data();
    const auto local = LocalRange(leaf, range);
    if (const auto* kind = std::get_if<SetBlockKind>(&command)) {
      if (kind->kind == BlockKind::CodeBlock) {
        data.marks.clear();
        data.links.clear();
        data.fence_info = kind->fence_info;
        if (data.fence_info.find_first_of("\r\n") != std::string::npos ||
            data.fence_info.find('\0') != std::string::npos) {
          throw std::invalid_argument("Invalid code fence info");
        }
        const AttributedText info(data.fence_info);
      } else {
        data.fence_info.clear();
      }
      data.kind = kind->kind;
      data.heading_level = kind->kind == BlockKind::Heading ? kind->heading_level : 0;
    } else if (data.kind != BlockKind::CodeBlock && !local.IsCollapsed()) {
      if (const auto* toggle = std::get_if<ToggleMark>(&command)) {
        data.marks = ChangeMarks(leaf.block, local, toggle->mark, remove_mark);
      } else if (link) {
        data.links = ChangeLink(leaf.block, local, *link);
      } else {
        data.marks = ChangeMarks(leaf.block, local, all_marks, true);
        data.links = ChangeLink(leaf.block, local, SetLink{});
      }
    }
    if (data != leaf.block.Data()) {
      edit.Touch(leaf.block);
      changes[DocumentAccess::Id(leaf.block)] = {DocumentAccess::WithData(leaf.block, std::move(data))};
    }
  }
  return Rewrite(document, changes, edit);
}

std::optional<unsigned> ListStartAt(const RichDocument& document, TextRange range) {
  if (const auto items = SelectedItems(document, range)) {
    const auto& data = BlockAt(document, items->parent).Data();
    if (data.ordered) {
      return data.list_start;
    }
  }
  return std::nullopt;
}

CommandState ContainerStatus(const RichDocument& document, TextRange range, const EditorCommand& command) {
  if (TouchesRootKind(document, range, BlockKind::Table) || TouchesAtomicBlock(document, range)) {
    return {};
  }
  if (std::holds_alternative<ToggleQuote>(command)) {
    const auto selected = Siblings(document, range);
    return {true, !selected.parent.empty() && BlockAt(document, selected.parent).Data().kind == BlockKind::Quote
                      ? CheckState::On : CheckState::Off};
  }
  const auto items = SelectedItems(document, range);
  if (const auto* list = std::get_if<ToggleList>(&command)) {
    const bool ordered = list->kind == ListKind::Ordered;
    return {true, items && BlockAt(document, items->parent).Data().ordered == ordered
                      ? CheckState::On : CheckState::Off};
  }
  return {items.has_value() && (!std::holds_alternative<IndentList>(command) || items->first > 0)};
}

RichDocument ChangeContainer(const RichDocument& document, TextRange range, const EditorCommand& command, EditChanges& edit) {
  if (const auto* list = std::get_if<ToggleList>(&command)) {
    return ChangeList(document, range, list->kind == ListKind::Ordered, edit);
  }
  if (std::holds_alternative<ToggleQuote>(command)) {
    return ChangeQuote(document, range, edit);
  }
  const auto items = SelectedItems(document, range);
  if (!items) {
    return document;
  }
  if (const auto* start = std::get_if<SetListStart>(&command)) {
    const auto& list = BlockAt(document, items->parent);
    if (!list.Data().ordered || list.Data().list_start == start->start) {
      return document;
    }
    auto data = list.Data();
    data.list_start = start->start;
    return Splice(document, ParentSlot(items->parent), {DocumentAccess::WithData(list, std::move(data))}, edit);
  }
  if (std::holds_alternative<IndentList>(command)) {
    return items->first == 0 ? document : Indent(document, *items, edit);
  }
  return Outdent(document, *items, edit);
}

RichDocument SplitListItem(const RichDocument& document, TextOffset new_paragraph, EditChanges& edit) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, new_paragraph)];
  const auto slot = ParentSlot(leaf.path);
  if (slot.parent.empty() || slot.first == 0) {
    return document;
  }
  const auto& item = BlockAt(document, slot.parent);
  if (item.Data().kind != BlockKind::ListItem) {
    return document;
  }
  auto data = item.Data();
  auto next = BlocksBetween(data.children, slot.first, data.children.size());
  data.children.erase(data.children.begin() + static_cast<std::ptrdiff_t>(slot.first), data.children.end());
  return Splice(document, ParentSlot(slot.parent),
                {DocumentAccess::WithData(item, std::move(data)),
                 Block(BlockData{.kind = BlockKind::ListItem, .children = std::move(next)})}, edit);
}

RichDocument ExitContainer(const RichDocument& document, TextOffset caret, bool empty_only, EditChanges& edit) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, caret)];
  if (caret != leaf.start || (empty_only && !leaf.block.Data().text.empty())) {
    return document;
  }
  const auto slot = ParentSlot(leaf.path);
  if (!slot.parent.empty() && BlockAt(document, slot.parent).Data().kind == BlockKind::Quote &&
      (empty_only ? slot.last + 1 == ChildrenAt(document, slot.parent).size() : slot.first == 0)) {
    return ChangeQuote(document, {caret, caret}, edit);
  }
  if (const auto items = SelectedItems(document, {caret, caret}); items && slot.first == 0 &&
      !slot.parent.empty() && BlockAt(document, slot.parent).Data().kind == BlockKind::ListItem &&
      (empty_only ? ChildrenAt(document, slot.parent).size() == 1 : items->first == 0)) {
    return Outdent(document, *items, edit);
  }
  return document;
}

EditResult ExitCodeBlock(const RichDocument& document, TextOffset caret, EditChanges& edit) {
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, caret)];
  if (leaf.block.Data().kind != BlockKind::CodeBlock) {
    return {document, caret};
  }
  return {Splice(document, ParentSlot(leaf.path), {leaf.block, Block(BlockData{})}, edit),
          leaf.start + DocumentAccess::Body(leaf.block).Length() + 1};
}

bool IsTableCommand(const EditorCommand& command) {
  return std::holds_alternative<InsertTable>(command) || std::holds_alternative<InsertTableRow>(command) ||
      std::holds_alternative<InsertTableColumn>(command) || std::holds_alternative<DeleteTableRow>(command) ||
      std::holds_alternative<DeleteTableColumn>(command) || std::holds_alternative<ToggleTableHeader>(command) ||
      std::holds_alternative<DeleteTable>(command) || std::holds_alternative<ExitTable>(command);
}

CommandState TableStatus(const RichDocument& document, TextRange range, const EditorCommand& command) {
  const auto& leaves = DocumentAccess::Get(document).leaves;
  const auto first = LeafIndex(document, range.start);
  const auto& leaf = leaves[first];
  if (const auto* insert = std::get_if<InsertTable>(&command)) {
    return {range.IsCollapsed() && leaf.path.size() == 1 && leaf.block.Data().kind == BlockKind::Paragraph &&
            ValidTableSize(insert->rows, insert->columns)};
  }
  const auto cell = TableCellAt(document, leaf);
  if (!cell || first != LeafIndex(document, range.end)) {
    return {};
  }
  const auto& table = document.Blocks()[cell->table].Data();
  const auto rows = table.children.size();
  const auto columns = table.children.front().Data().children.size();
  if (const auto* row = std::get_if<InsertTableRow>(&command)) {
    return {ValidTableSize(rows + 1, columns) && (row->side == TableInsertSide::After ||
            (row->side == TableInsertSide::Before && (!table.table_has_header || cell->row != 0)))};
  }
  if (const auto* column = std::get_if<InsertTableColumn>(&command)) {
    return {ValidTableSize(rows, columns + 1) &&
            (column->side == TableInsertSide::Before || column->side == TableInsertSide::After)};
  }
  if (std::holds_alternative<DeleteTableRow>(command)) {
    return {rows > 1};
  }
  if (std::holds_alternative<DeleteTableColumn>(command)) {
    return {columns > 1};
  }
  if (std::holds_alternative<ToggleTableHeader>(command)) {
    return {true, table.table_has_header ? CheckState::On : CheckState::Off};
  }
  return {true};
}

RichDocument ChangeTable(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit) {
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.leaves[storage.indexes.at(selection.active.block)];
  auto index = leaf.path.front();
  auto blocks = BlocksBetween(document.Blocks(), 0, document.Blocks().size());
  if (const auto* insert = std::get_if<InsertTable>(&command)) {
    Block table(BlockData{.kind = BlockKind::Table,
                          .children = std::vector<Block>(insert->rows, EmptyRow(insert->columns)),
                          .table_has_header = insert->has_header});
    if (leaf.block.Data().text.empty()) {
      edit.Touch(blocks[index]);
      blocks[index] = std::move(table);
    } else {
      blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(++index), std::move(table));
    }
    auto next = edit.Build(std::move(blocks));
    SelectBlock(selection, next.Blocks()[index]);
    return next;
  }

  const auto cell = *TableCellAt(document, leaf);
  if (std::holds_alternative<DeleteTable>(command)) {
    edit.Touch(blocks[index]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(index));
    auto next = edit.Build(std::move(blocks));
    const bool at_end = index >= next.Blocks().size();
    SelectBlock(selection, next.Blocks()[at_end ? index - 1 : index], at_end);
    return next;
  }
  if (std::holds_alternative<ExitTable>(command)) {
    ++index;
    if (index < blocks.size() && blocks[index].Data().kind == BlockKind::Paragraph) {
      SelectBlock(selection, blocks[index]);
      return document;
    }
    blocks.emplace(blocks.begin() + static_cast<std::ptrdiff_t>(index), BlockData{});
    auto next = edit.Build(std::move(blocks));
    SelectBlock(selection, next.Blocks()[index]);
    return next;
  }

  auto data = blocks[index].Data();
  auto& rows = data.children;
  auto row = cell.row;
  auto column = cell.column;
  const auto column_count = rows.front().Data().children.size();
  if (const auto* insert = std::get_if<InsertTableRow>(&command)) {
    const auto at = row + (insert->side == TableInsertSide::After);
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), EmptyRow(column_count));
  } else if (std::holds_alternative<DeleteTableRow>(command)) {
    edit.Touch(rows[row]);
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(row));
    if (row == 0) {
      data.table_has_header = false;
    }
    row = std::min(row, rows.size() - 1);
  } else if (std::holds_alternative<ToggleTableHeader>(command)) {
    data.table_has_header = !data.table_has_header;
  } else {
    const auto* insert = std::get_if<InsertTableColumn>(&command);
    const auto at = static_cast<std::ptrdiff_t>(column + (insert && insert->side == TableInsertSide::After));
    for (auto& block : rows) {
      auto row_data = block.Data();
      auto& cells = row_data.children;
      if (insert) {
        cells.insert(cells.begin() + at, EmptyCell());
      } else {
        edit.Touch(cells[at]);
        cells.erase(cells.begin() + at);
      }
      block = DocumentAccess::WithData(block, std::move(row_data));
    }
    if (!insert) {
      column = std::min(column, column_count - 2);
    }
  }
  blocks[index] = DocumentAccess::WithData(blocks[index], std::move(data));
  auto next = edit.Build(std::move(blocks));
  if (!DocumentAccess::Get(next).indexes.contains(selection.active.block)) {
    SelectBlock(selection, next.Blocks()[index].Data().children[row].Data().children[column]);
  }
  return next;
}

bool IsImageCommand(const EditorCommand& command) {
  return std::holds_alternative<InsertImage>(command) || std::holds_alternative<ReplaceImage>(command) ||
      std::holds_alternative<DeleteImage>(command) || std::holds_alternative<ResizeImage>(command);
}

std::optional<std::size_t> SelectedAtomicIndex(const RichDocument& document, TextRange range) {
  const auto index = LeafIndex(document, range.start);
  const auto& leaf = DocumentAccess::Get(document).leaves[index];
  if (IsAtomicBlock(leaf.block.Data().kind) && range == TextRange{leaf.start, leaf.start + 1}) {
    return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> SelectedImageIndex(const RichDocument& document, TextRange range) {
  const auto index = SelectedAtomicIndex(document, range);
  return index && DocumentAccess::Get(document).leaves[*index].block.Data().image ? index : std::nullopt;
}

bool CanInsertAtomicBlock(const RichDocument& document, TextRange range) {
  if (!range.IsCollapsed()) {
    return SelectedAtomicIndex(document, range).has_value();
  }
  const auto& leaf = DocumentAccess::Get(document).leaves[LeafIndex(document, range.start)];
  const auto kind = leaf.block.Data().kind;
  return leaf.path.size() == 1 && (kind == BlockKind::Paragraph || IsAtomicBlock(kind));
}

CommandState ImageStatus(const RichDocument& document, TextRange range, const EditorCommand& command) {
  if (const auto* insert = std::get_if<InsertImage>(&command)) {
    return {CanInsertAtomicBlock(document, range) && ValidImageData(insert->image)};
  }
  if (!SelectedImageIndex(document, range)) {
    return {};
  }
  if (const auto* replace = std::get_if<ReplaceImage>(&command)) {
    return {ValidImageSource(replace->source)};
  }
  if (const auto* resize = std::get_if<ResizeImage>(&command)) {
    return {ValidImageWidth(resize->width)};
  }
  return {true};
}

RichDocument InsertAtomicBlock(const RichDocument& document, Selection& selection, BlockData data,
                               EditChanges& edit) {
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.leaves[storage.indexes.at(selection.active.block)];
  const auto slot = ParentSlot(leaf.path);
  std::vector<Block> blocks;
  const auto length = DocumentAccess::Body(leaf.block).Length();
  const bool at_atomic = IsAtomicBlock(leaf.block.Data().kind);
  const bool after_selected_block = at_atomic && selection.anchor.offset != selection.active.offset;
  const auto offset = after_selected_block ? length : selection.active.offset;
  if (offset != 0) {
    if (at_atomic || offset == length) {
      blocks.push_back(leaf.block);
    } else {
      LeafBuilder prefix;
      prefix.AppendSlice(leaf.block, {0, offset});
      blocks.push_back(DocumentAccess::WithData(leaf.block, std::move(prefix.data)));
    }
  }
  const auto block_index = slot.first + blocks.size();
  blocks.emplace_back(std::move(data));
  if (offset < length) {
    if (offset == 0) {
      blocks.push_back(leaf.block);
    } else {
      LeafBuilder suffix;
      suffix.AppendSlice(leaf.block, {offset, length});
      blocks.emplace_back(std::move(suffix.data));
    }
  }
  auto next = Splice(document, slot, std::move(blocks), edit);
  if (!at_atomic && offset < length) {
    SelectBlock(selection, next.Blocks()[block_index + 1]);
  } else {
    SelectBlock(selection, next.Blocks()[block_index], true);
  }
  return next;
}

RichDocument ChangeImage(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit) {
  if (const auto* insert = std::get_if<InsertImage>(&command)) {
    return InsertAtomicBlock(document, selection, {.kind = BlockKind::Image, .image = insert->image}, edit);
  }
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.leaves[storage.indexes.at(selection.active.block)];
  const auto slot = ParentSlot(leaf.path);
  if (const auto* replace = std::get_if<ReplaceImage>(&command)) {
    auto data = leaf.block.Data();
    if (data.image->source == replace->source) {
      return document;
    }
    data.image->source = replace->source;
    return Splice(document, slot, {DocumentAccess::WithData(leaf.block, std::move(data))}, edit);
  }
  if (const auto* resize = std::get_if<ResizeImage>(&command)) {
    auto data = leaf.block.Data();
    if (data.image->width == resize->width) {
      return document;
    }
    data.image->width = resize->width;
    return Splice(document, slot, {DocumentAccess::WithData(leaf.block, std::move(data))}, edit);
  }
  auto result = ReplaceText(document, {leaf.start, leaf.start + 1}, {}, {}, false, edit);
  const auto caret = PositionAt(result.document, result.caret);
  selection = {caret, caret};
  return std::move(result.document);
}

RichDocument ReplaceSelectedCustomBlock(const RichDocument& document, const Selection& selection,
                                        const CustomBlockData& replacement, EditChanges& edit) {
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.leaves[storage.indexes.at(selection.active.block)];
  auto data = leaf.block.Data();
  data.custom = replacement;
  return Splice(document, ParentSlot(leaf.path), {DocumentAccess::WithData(leaf.block, std::move(data))}, edit);
}

} // namespace huxerui::richeditor::detail

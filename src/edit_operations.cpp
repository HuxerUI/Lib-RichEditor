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

RichDocument EditChanges::Build(const RichDocument& document, std::vector<Block> blocks, const ChildChanges& changes) {
  return DocumentAccess::Build(std::move(blocks), last_id, &document, changes, &touched);
}

namespace {

using Replacements = std::unordered_map<BlockId, std::vector<Block>>;

std::vector<Block> Rewrite(std::span<const Block> blocks, const Replacements& changes, BlockId parent,
                           ChildChanges& child_changes) {
  std::vector<Block> result;
  result.reserve(blocks.size());
  std::optional<ChildChange> changed;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    const auto id = DocumentAccess::Id(block);
    const auto found = changes.find(id);
    if (found != changes.end()) {
      result.insert(result.end(), found->second.begin(), found->second.end());
    } else if (IsLeaf(block.Data().kind)) {
      result.push_back(block);
      continue;
    } else {
      auto children = Rewrite(block.Data().children, changes, id, child_changes);
      if (!child_changes.contains(id)) {
        result.push_back(block);
        continue;
      }
      if (!children.empty()) {
        auto data = block.Data();
        data.children = std::move(children);
        result.push_back(DocumentAccess::WithData(block, std::move(data)));
      }
    }
    if (!changed) {
      changed = {i, i + 1, result.size()};
    } else {
      changed->old_end = i + 1;
      changed->new_end = result.size();
    }
  }
  if (changed) {
    child_changes.emplace(parent, *changed);
  }
  return result;
}

RichDocument Rewrite(const RichDocument& document, const Replacements& changes, EditChanges& edit,
                     ChildChanges child_changes = {}) {
  auto blocks = Rewrite(document.Blocks(), changes, 0, child_changes);
  return child_changes.contains(0) ? edit.Build(document, std::move(blocks), child_changes) : document;
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

  void AppendSlice(const Block& block, TextRange range, bool fresh_objects = false) {
    data.text += DocumentAccess::Body(block).TextInRange(range);
    const auto& objects = block.Data().inline_objects;
    auto object = std::ranges::lower_bound(objects, range.start, {}, &InlineObject::offset);
    for (; object != objects.end() && object->offset < range.end; ++object) {
      auto copy = fresh_objects ? InlineObject(object->offset, object->data) : *object;
      copy.offset = length + copy.offset - range.start;
      data.inline_objects.push_back(std::move(copy));
    }
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
  if (last != first && DocumentAccess::Get(document).index.At(last).start == range.end) {
    --last;
  }
  return {first, last};
}

bool TouchesRoot(const RichDocument& document, TextRange range, auto matches) {
  const auto& leaves = DocumentAccess::Get(document).index;
  const auto first = leaves.At(LeafIndex(document, range.start)).path.front();
  const auto last = leaves.At(LeafIndex(document, range.end)).path.front();
  return std::ranges::any_of(document.Blocks().subspan(first, last - first + 1), [matches](const Block& block) {
    return matches(block.Data().kind);
  });
}

bool TouchesRootKind(const RichDocument& document, TextRange range, BlockKind kind) {
  return TouchesRoot(document, range, [kind](BlockKind value) { return value == kind; });
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
  const ChildChange change{range.first, range.last + 1, range.first + replacement.size()};
  if (range.parent.empty()) {
    return edit.Build(document, std::move(children), {{0, change}});
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
  return Rewrite(document, changes, edit, {{DocumentAccess::Id(parent), change}});
}

TreeRange Siblings(const RichDocument& document, TextRange range) {
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& leaves = DocumentAccess::Get(document).index;
  const auto& a = leaves.At(first).path;
  const auto& b = leaves.At(last).path;
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

std::optional<TreeRange> CommonListItems(const RichDocument& document, std::span<const std::size_t> first_path,
                                        std::span<const std::size_t> last_path) {
  std::size_t item_depth = 0;
  auto siblings = document.Blocks();
  for (std::size_t depth = 0; depth + 1 < std::min(first_path.size(), last_path.size()) &&
      first_path[depth] == last_path[depth]; ++depth) {
    const auto& data = siblings[first_path[depth]].Data();
    if (data.kind == BlockKind::List) {
      item_depth = depth + 1;
    }
    siblings = data.children;
  }
  if (item_depth == 0) {
    return std::nullopt;
  }
  const auto list_path = first_path.first(item_depth);
  return TreeRange{{list_path.begin(), list_path.end()}, first_path[item_depth], last_path[item_depth]};
}

bool StartsWith(std::span<const std::size_t> path, std::span<const std::size_t> prefix) {
  return path.size() >= prefix.size() && std::ranges::equal(path.first(prefix.size()), prefix);
}

struct ListPosition {
  const BlockData* list;
  /// Index of the item in the path to the selected leaf.
  std::size_t item_depth;

  std::span<const std::size_t> ItemPath(std::span<const std::size_t> path) const {
    return path.first(item_depth + 1);
  }

  /// The marker is placed on the item's first-child path, which can pass through a nested container.
  bool LeadsItem(std::span<const std::size_t> path) const {
    return IsLeadingPath(path.subspan(item_depth + 1));
  }
};

std::optional<ListPosition> ListAt(const RichDocument& document, std::span<const std::size_t> path) {
  std::optional<ListPosition> position;
  auto siblings = document.Blocks();
  for (std::size_t depth = 0; depth + 1 < path.size(); ++depth) {
    const auto& data = siblings[path[depth]].Data();
    if (data.kind == BlockKind::List) {
      position = ListPosition{&data, depth + 1};
    }
    siblings = data.children;
  }
  return position;
}

TreeRange ItemSlot(std::span<const std::size_t> path, std::size_t item_depth) {
  const auto list = path.first(item_depth);
  return {{list.begin(), list.end()}, path[item_depth], path[item_depth]};
}

/// For structural keys, the first direct text block is the editable lead of an item.
std::optional<TreeRange> LeadingListItem(const RichDocument& document, std::span<const std::size_t> path) {
  const auto position = ListAt(document, path);
  if (!position || position->item_depth + 2 != path.size() || !position->LeadsItem(path)) {
    return std::nullopt;
  }
  return ItemSlot(path, position->item_depth);
}

struct ListSelection {
  /// An eligible range of sibling items, or a range of plain blocks to wrap.
  std::optional<TreeRange> items;
  std::optional<TreeRange> blocks;
  bool numbered = false;
  bool bulleted = false;
  bool plain = false;

  CheckState Checked(bool ordered) const {
    if (!(ordered ? numbered : bulleted)) {
      return CheckState::Off;
    }
    return plain || (ordered ? bulleted : numbered) ? CheckState::Mixed : CheckState::On;
  }
};

ListSelection ResolveListSelection(const RichDocument& document, TextRange range) {
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& index = DocumentAccess::Get(document).index;
  auto leaves = index.Leaves(first);
  const auto first_path = leaves->path;
  const auto start = ListAt(document, first_path);
  ListSelection result;
  if (start && start->LeadsItem(first_path)) {
    result.items = ItemSlot(first_path, start->item_depth);
  }
  for (auto ordinal = first; ordinal <= last; ++ordinal, leaves.Next()) {
    const auto path = std::span(leaves->path);
    const auto position = ordinal == first ? start : ListAt(document, path);
    if (position) {
      const auto item_path = position->ItemPath(path);
      // A continuation takes its item's style only when the selection includes the marker.
      const bool includes_marker = !StartsWith(first_path, item_path) || position->LeadsItem(first_path);
      if (includes_marker) {
        result.numbered |= position->list->ordered;
        result.bulleted |= !position->list->ordered;
      } else {
        result.plain = true;
      }
    } else {
      result.plain = true;
    }
    if (result.items) {
      const auto& list = result.items->parent;
      if (position && position->item_depth == list.size() && StartsWith(path, list)) {
        result.items->last = path[position->item_depth];
      } else {
        result.items.reset();
      }
    }
  }
  if (!result.numbered && !result.bulleted) {
    auto blocks = Siblings(document, range);
    if (!start || blocks.parent.size() > start->item_depth) {
      result.blocks = std::move(blocks);
    }
  }
  return result;
}

/// Removing a marker may only expose standalone text and must preserve the parent's leading block.
bool CanRemoveListMarkers(const RichDocument& document, const TreeRange& items) {
  if (items.first == 0) {
    const auto parent = ListAt(document, items.parent);
    if (parent && parent->LeadsItem(items.parent)) {
      return false;
    }
  }
  const auto selected = ChildrenAt(document, items.parent).subspan(items.first, items.last - items.first + 1);
  return std::ranges::all_of(selected, [](const Block& item) {
    const auto& blocks = item.Data().children;
    return blocks.size() == 1 && IsTextBlock(blocks.front().Data().kind);
  });
}

TreeRange ParentSlot(BlockPath path) {
  const auto index = path.back();
  path.pop_back();
  return {std::move(path), index, index};
}

RichDocument EraseLeaves(const RichDocument& document, std::size_t first, std::size_t last, EditChanges& edit) {
  const auto& index = DocumentAccess::Get(document).index;
  auto slot = ParentSlot(index.At(first).path);
  const auto end = ParentSlot(index.At(last).path);
  if (slot.parent == end.parent) {
    slot.last = end.last;
    return Splice(document, slot, {}, edit);
  }
  Replacements removed;
  auto leaf = index.Leaves(first);
  for (auto i = first; i <= last; ++i, leaf.Next()) {
    edit.Touch(leaf->block);
    removed[DocumentAccess::Id(leaf->block)] = {};
  }
  return Rewrite(document, removed, edit);
}

Block ListSlice(const Block& list, std::size_t begin, std::size_t end, std::size_t removed_before = 0) {
  auto data = list.Data();
  data.children = BlocksBetween(data.children, begin, end);
  if (data.ordered) {
    data.list_start = static_cast<unsigned>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(data.list_start) + (begin - removed_before), std::numeric_limits<unsigned>::max()));
  }
  return Block(std::move(data));
}

void AppendBlocks(std::vector<Block>& destination, std::span<const Block> blocks) {
  destination.insert(destination.end(), blocks.begin(), blocks.end());
}

void JoinListItems(const RichDocument& document, const TreeRange& items, Replacements& replacements,
                   ChildChanges& child_changes) {
  const auto siblings = ChildrenAt(document, items.parent);
  const auto& last = siblings[items.last];
  const auto last_id = DocumentAccess::Id(last);
  auto suffix = Rewrite(last.Data().children, replacements, last_id, child_changes);
  replacements[last_id] = {};
  if (suffix.empty()) {
    return;
  }

  // The text replacement joins the boundary leaves. Keep the last item's surviving blocks with that text.
  const auto& first = siblings[items.first];
  const auto first_id = DocumentAccess::Id(first);
  auto data = first.Data();
  data.children = Rewrite(data.children, replacements, first_id, child_changes);
  AppendBlocks(data.children, suffix);
  // Appending the suffix extends the splice through the end of the first item.
  auto& change = child_changes.at(first_id);
  change.old_end = first.Data().children.size();
  change.new_end = data.children.size();
  replacements[first_id] = {DocumentAccess::WithData(first, std::move(data))};
}

RichDocument UnwrapListItems(const RichDocument& document, const TreeRange& items, EditChanges& edit,
                             std::size_t removed_before = 0) {
  const auto& list = BlockAt(document, items.parent);
  const auto& children = list.Data().children;
  std::vector<Block> blocks;
  if (items.first != 0) {
    blocks.push_back(ListSlice(list, 0, items.first));
  }
  for (std::size_t i = items.first; i <= items.last; ++i) {
    AppendBlocks(blocks, children[i].Data().children);
  }
  if (items.last + 1 < children.size()) {
    blocks.push_back(ListSlice(list, items.last + 1, children.size(), removed_before));
  }
  return Splice(document, ParentSlot(items.parent), std::move(blocks), edit);
}

/// Joins only the neighbors that directly touch the list created by this edit.
RichDocument SpliceList(const RichDocument& document, TreeRange range, std::vector<Block> replacement,
                        std::size_t inserted, EditChanges& edit, bool inherit_tight = false) {
  const auto siblings = ChildrenAt(document, range.parent);
  const auto& list = replacement[inserted].Data();
  const auto neighbor = [&](std::size_t index) -> const Block* {
    const auto& block = siblings[index];
    const auto& data = block.Data();
    return data.kind == BlockKind::List && data.ordered == list.ordered && (inherit_tight || data.tight == list.tight)
        ? &block : nullptr;
  };
  const auto* before = inserted == 0 && range.first != 0 ? neighbor(range.first - 1) : nullptr;
  const auto* after = inserted + 1 == replacement.size() && range.last + 1 < siblings.size()
      ? neighbor(range.last + 1) : nullptr;
  if (before && after && before->Data().tight != after->Data().tight) {
    after = nullptr;
  }
  if (after && list.ordered) {
    const auto right_start = after->Data().list_start;
    const auto added = list.children.size();
    // Preserve the first ordinal of the following list when extending or prepending items.
    const bool continues = before
        ? static_cast<std::uint64_t>(before->Data().list_start) + before->Data().children.size() + added == right_start
        : added <= right_start;
    if (!continues) {
      after = nullptr;
    }
  }
  if (!before && !after) {
    return Splice(document, range, std::move(replacement), edit);
  }

  auto joined = before ? before->Data() : list;
  if (before) {
    AppendBlocks(joined.children, list.children);
    --range.first;
  }
  if (after) {
    const auto& following = after->Data();
    if (!before) {
      joined.tight = following.tight;
      if (joined.ordered) {
        joined.list_start = following.list_start - static_cast<unsigned>(list.children.size());
      }
    }
    AppendBlocks(joined.children, following.children);
    ++range.last;
  }
  replacement[inserted] = DocumentAccess::WithData(before ? *before : *after, std::move(joined));
  return Splice(document, range, std::move(replacement), edit);
}

RichDocument ChangeList(const RichDocument& document, const ListSelection& selection, bool ordered, EditChanges& edit) {
  if (const auto& items = selection.items) {
    const auto& list = BlockAt(document, items->parent);
    const auto& data = list.Data();
    if (data.ordered == ordered) {
      return UnwrapListItems(document, *items, edit);
    }
    std::vector<Block> blocks;
    if (items->first != 0) {
      blocks.push_back(ListSlice(list, 0, items->first));
    }
    const auto converted = blocks.size();
    blocks.emplace_back(BlockData{.kind = BlockKind::List,
                                  .children = BlocksBetween(data.children, items->first, items->last + 1),
                                  .ordered = ordered, .tight = data.tight});
    if (items->last + 1 < data.children.size()) {
      blocks.push_back(ListSlice(list, items->last + 1, data.children.size()));
    }
    return SpliceList(document, ParentSlot(items->parent), std::move(blocks), converted, edit);
  }
  if (const auto& selected = selection.blocks) {
    const auto siblings = ChildrenAt(document, selected->parent);
    BlockData list{.kind = BlockKind::List, .ordered = ordered};
    for (std::size_t i = selected->first; i <= selected->last; ++i) {
      list.children.emplace_back(BlockData{.kind = BlockKind::ListItem, .children = {siblings[i]}});
    }
    return SpliceList(document, *selected, {Block(std::move(list))}, 0, edit, true);
  }
  return document;
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
  if (tail.Data().kind == BlockKind::List && tail.Data().ordered == list.ordered && tail.Data().tight == list.tight) {
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

RichDocument Outdent(const RichDocument& document, const TreeRange& items, EditChanges& edit,
                     bool leave_empty_item = false) {
  const auto& list = BlockAt(document, items.parent);
  const auto& children = list.Data().children;
  const auto slot = ParentSlot(items.parent);
  const std::size_t removed_before = leave_empty_item ? 1 : 0;
  if (slot.parent.empty() || BlockAt(document, slot.parent).Data().kind != BlockKind::ListItem) {
    return UnwrapListItems(document, items, edit, removed_before);
  }
  const auto before = items.first == 0 ? std::nullopt : std::optional(ListSlice(list, 0, items.first));
  const auto after = items.last + 1 == children.size() ? std::nullopt :
      std::optional(ListSlice(list, items.last + 1, children.size(), removed_before));
  std::vector<Block> replacement;
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

RichDocument ContinueAfterBlock(const RichDocument& document, std::size_t index, Selection& selection,
                                EditChanges& edit) {
  const auto roots = document.Blocks();
  const auto next = index + 1;
  if (next < roots.size() && roots[next].Data().kind == BlockKind::Paragraph) {
    SelectBlock(selection, roots[next]);
    return document;
  }
  auto blocks = BlocksBetween(roots, 0, roots.size());
  blocks.emplace(blocks.begin() + static_cast<std::ptrdiff_t>(next), BlockData{});
  auto result = edit.Build(document, std::move(blocks), {{0, {next, next, next + 1}}});
  SelectBlock(selection, result.Blocks()[next]);
  return result;
}

} // namespace

bool CanEditTextRange(const RichDocument& document, TextRange range) {
  const auto first = LeafIndex(document, range.start);
  const auto last = LeafIndex(document, range.end);
  if (first == last) {
    return true;
  }
  const auto& leaves = DocumentAccess::Get(document).index;
  // An atomic block's separator can only be removed together with the block.
  return !TouchesRootKind(document, range, BlockKind::Table) &&
      (!IsAtomicBlock(leaves.At(first).block.Data().kind) || range.start == leaves.At(first).start) &&
      (!IsAtomicBlock(leaves.At(last).block.Data().kind) || range.end > leaves.At(last).start);
}

EditResult InsertParagraphAtAtomicBlock(const RichDocument& document, TextOffset caret, EditChanges& edit) {
  const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, caret));
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
  const auto first = storage.index.IndexOf(start.block).value();
  const auto last = storage.index.IndexOf(finish.block).value();
  const auto first_leaf = storage.index.At(first);
  const auto last_leaf = storage.index.At(last);
  const auto& head = first_leaf.block;
  const auto& tail = last_leaf.block;
  const bool head_atomic = IsAtomicBlock(head.Data().kind);
  const bool tail_atomic = IsAtomicBlock(tail.Data().kind);
  hard_break |= TableCellAt(document, first_leaf).has_value();
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
    const bool has_next = last + 1 < storage.index.LeafCount();
    const TextOffset leading = !has_next && first != 0 ? -1 : 0;
    return {EraseLeaves(document, first, last, edit),
            range.start + leading, leading, has_next ? -1 : 0};
  }

  if (!head_atomic && first == last && DocumentAccess::Body(head).TextInRange({start.offset, finish.offset}) == text &&
      std::ranges::none_of(head.Data().inline_objects, [&](const InlineObject& object) {
        return object.offset >= start.offset && object.offset < finish.offset;
      })) {
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
  auto removed = storage.index.Leaves(first + 1);
  for (std::size_t i = first + 1; i <= last; ++i, removed.Next()) {
    const auto& block = removed->block;
    edit.Touch(block);
    replacements[DocumentAccess::Id(block)] = {};
  }
  ChildChanges child_changes;
  if (first != last) {
    const auto items = CommonListItems(document, storage.index.At(first).path, storage.index.At(last).path);
    if (items && items->first != items->last) {
      JoinListItems(document, *items, replacements, child_changes);
    }
  }
  return {Rewrite(document, replacements, edit, std::move(child_changes)), range.start + inserted_length};
}

bool CanInsertInlineObject(const RichDocument& document, TextRange range) {
  const auto& index = DocumentAccess::Get(document).index;
  const auto start = index.AtOffset(range.start);
  return start.ordinal == index.AtOffset(range.end).ordinal &&
      (start.block.Data().kind == BlockKind::Paragraph || start.block.Data().kind == BlockKind::Heading);
}

EditResult InsertInlineContent(const RichDocument& document, TextRange range, const InlineObjectData& object,
                               std::optional<Mark> marks, EditChanges& edit) {
  const auto leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
  const auto local = LocalRange(leaf, range);
  LeafBuilder builder;
  builder.data.kind = leaf.block.Data().kind;
  builder.data.heading_level = leaf.block.Data().heading_level;
  builder.AppendSlice(leaf.block, {0, local.start});
  builder.data.inline_objects.emplace_back(builder.length, object);
  const auto inherited = marks.value_or(MarksAt(document, range.start));
  const auto* link = FindLink(leaf, range);
  builder.AppendText(object_replacement, 1, Without(inherited, Mark::Code), link);
  builder.AppendSlice(leaf.block, {local.end, DocumentAccess::Body(leaf.block).Length()});
  auto block = DocumentAccess::WithData(leaf.block, std::move(builder.data));
  return {Splice(document, ParentSlot(leaf.path), {std::move(block)}, edit), range.start + 1};
}

RichDocument ReplaceInlineContent(const RichDocument& document, const Leaf& leaf, std::size_t index,
                                  const InlineObjectData& object, EditChanges& edit) {
  auto data = leaf.block.Data();
  data.inline_objects[index].data = object;
  return Splice(document, ParentSlot(leaf.path), {DocumentAccess::WithData(leaf.block, std::move(data))}, edit);
}

namespace {

bool InlineText(BlockKind kind) {
  return kind == BlockKind::Paragraph || kind == BlockKind::Heading;
}

LeafBuilder ForTextBlock(const Block& block) {
  LeafBuilder builder;
  builder.data.kind = block.Data().kind;
  builder.data.heading_level = block.Data().heading_level;
  builder.data.fence_info = block.Data().fence_info;
  return builder;
}

Block CopyLeaf(const Leaf& leaf, TextRange range) {
  const auto kind = leaf.block.Data().kind;
  const auto local = LocalRange(leaf, range);
  if (IsAtomicBlock(kind)) {
    return local.IsCollapsed() ? Block(BlockData{}) : leaf.block;
  }
  auto builder = ForTextBlock(leaf.block);
  builder.AppendSlice(leaf.block, local);
  return Block(std::move(builder.data));
}

std::vector<Block> CopyFragmentBlocks(const RichDocument& document, std::span<const Block> blocks, TextRange range,
                                      std::size_t first, std::size_t last, std::size_t& ordinal, std::size_t depth = 0) {
  const auto& index = DocumentAccess::Get(document).index;
  std::vector<Block> result;
  for (const auto& block : blocks) {
    if (ordinal > last) {
      break;
    }
    const auto& data = block.Data();
    if (IsLeaf(data.kind)) {
      if (ordinal >= first) {
        result.push_back(CopyLeaf(index.At(ordinal), range));
      }
      ++ordinal;
    } else if (data.kind == BlockKind::Table) {
      const auto count = data.children.size() * data.children.front().Data().children.size();
      const auto end = ordinal + count;
      if (end > first) {
        const auto first_cell = index.At(ordinal);
        const auto last_cell = index.At(end - 1);
        if (range.start <= first_cell.start && range.end >= last_cell.start + DocumentAccess::Body(last_cell.block).Length()) {
          result.push_back(block);
        } else {
          auto cell = index.Leaves(std::max(first, ordinal));
          for (auto i = std::max(first, ordinal); i < end && i <= last; ++i, cell.Next()) {
            result.push_back(CopyLeaf(*cell, range));
          }
        }
      }
      ordinal = end;
    } else {
      const auto begin = ordinal;
      auto children = CopyFragmentBlocks(document, data.children, range, first, last, ordinal, depth + 1);
      if (!children.empty()) {
        auto copy = data;
        copy.children = std::move(children);
        if (data.kind == BlockKind::List && data.ordered && begin < first) {
          const auto path = index.At(first).path;
          const auto ordinal = static_cast<std::uint64_t>(copy.list_start) + path[depth + 1];
          copy.list_start = static_cast<unsigned>(std::min<std::uint64_t>(ordinal, std::numeric_limits<unsigned>::max()));
        }
        result.emplace_back(std::move(copy));
      }
    }
  }
  return result;
}

Block FreshFragmentBlock(const Block& block) {
  auto data = block.Data();
  for (auto& object : data.inline_objects) {
    object = InlineObject(object.offset, std::move(object.data));
  }
  for (auto& child : data.children) {
    child = FreshFragmentBlock(child);
  }
  return Block(std::move(data));
}

void AppendFreshText(LeafBuilder& builder, const Block& block) {
  builder.AppendSlice(block, {0, DocumentAccess::Body(block).Length()}, true);
}

} // namespace

DocumentFragment CopyFragment(const RichDocument& document, TextRange range) {
  const auto& index = DocumentAccess::Get(document).index;
  const auto first = index.AtOffset(range.start);
  const auto last = index.AtOffset(range.end);
  std::size_t ordinal = 0;
  auto blocks = CopyFragmentBlocks(document, document.Blocks(), range, first.ordinal, last.ordinal, ordinal);
  return {RichDocument::Create(std::move(blocks)), range.start != first.start,
          range.end != last.start + DocumentAccess::Body(last.block).Length()};
}

bool CanInsertFragment(const RichDocument& document, TextRange range, const DocumentFragment& fragment) {
  if (!CanInsertInlineObject(document, range)) {
    return false;
  }
  const auto blocks = fragment.content.Blocks();
  if (blocks.size() == 1 && InlineText(blocks.front().Data().kind)) {
    const auto target = DocumentAccess::Get(document).index.AtOffset(range.start);
    return target.block.Data().kind != BlockKind::Heading || blocks.front().Data().text.find('\n') == std::string::npos;
  }
  return DocumentAccess::Get(document).index.At(LeafIndex(document, range.start)).path.size() == 1;
}

EditResult PasteFragment(const RichDocument& document, TextRange range, const DocumentFragment& fragment,
                         EditChanges& edit) {
  const auto leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
  const auto local = LocalRange(leaf, range);
  const auto length = DocumentAccess::Body(leaf.block).Length();
  const auto content = fragment.content.Blocks();
  std::vector<Block> incoming;
  incoming.reserve(content.size());
  for (const auto& block : content) {
    incoming.push_back(FreshFragmentBlock(block));
  }
  auto prefix = ForTextBlock(leaf.block);
  prefix.AppendSlice(leaf.block, {0, local.start});
  if (incoming.size() == 1 && InlineText(incoming.front().Data().kind)) {
    AppendFreshText(prefix, content.front());
    const auto caret = leaf.start + prefix.length;
    prefix.AppendSlice(leaf.block, {local.end, length});
    if (prefix.data == leaf.block.Data()) {
      return {document, caret};
    }
    auto block = DocumentAccess::WithData(leaf.block, std::move(prefix.data));
    return {Splice(document, ParentSlot(leaf.path), {std::move(block)}, edit), caret};
  }

  const auto root = leaf.path.front();
  std::vector<Block> replacement;
  if (fragment.leading_partial && InlineText(incoming.front().Data().kind) &&
      (prefix.data.kind != BlockKind::Heading || incoming.front().Data().text.find('\n') == std::string::npos)) {
    AppendFreshText(prefix, content.front());
    incoming.front() = DocumentAccess::WithData(leaf.block, std::move(prefix.data));
  } else if (prefix.length != 0) {
    replacement.push_back(DocumentAccess::WithData(leaf.block, std::move(prefix.data)));
  }
  const auto inserted_end = root + replacement.size() + incoming.size() - 1;
  replacement.insert(replacement.end(), std::make_move_iterator(incoming.begin()), std::make_move_iterator(incoming.end()));
  TextOffset suffix_length = 0;
  if (local.end < length) {
    if (fragment.trailing_partial && InlineText(replacement.back().Data().kind) &&
        (replacement.back().Data().kind != BlockKind::Heading ||
         DocumentAccess::Body(leaf.block).TextInRange({local.end, length}).find('\n') == std::string::npos)) {
      auto suffix = ForTextBlock(replacement.back());
      AppendFreshText(suffix, content.back());
      suffix.AppendSlice(leaf.block, {local.end, length});
      replacement.back() = Block(std::move(suffix.data));
      suffix_length = length - local.end;
    } else {
      auto suffix = ForTextBlock(leaf.block);
      suffix.AppendSlice(leaf.block, {local.end, length});
      replacement.emplace_back(std::move(suffix.data));
    }
  }
  auto next = Splice(document, ParentSlot(leaf.path), std::move(replacement), edit);
  Selection caret;
  SelectBlock(caret, next.Blocks()[inserted_end], true);
  return {next, Absolute(next, caret.active) - suffix_length};
}

Mark MarksAt(const RichDocument& document, TextOffset position) {
  const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, position));
  const auto local = position - leaf.start;
  return MarkAt(leaf.block, local == 0 ? 0 : local - 1);
}

CommandState MarkStatus(const RichDocument& document, TextRange range, Mark mark, std::optional<Mark> pending) {
  if (!CanEditTextRange(document, range)) {
    return {};
  }
  if (range.IsCollapsed()) {
    const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
    return {IsTextBlock(leaf.block.Data().kind) && leaf.block.Data().kind != BlockKind::CodeBlock,
            Has(pending.value_or(MarksAt(document, range.start)), mark) ? CheckState::On : CheckState::Off};
  }

  const auto& leaves = DocumentAccess::Get(document).index;
  const auto [first, last] = TouchedLeaves(document, range);
  TextOffset length = 0;
  TextOffset marked = 0;
  auto cursor = leaves.Leaves(first);
  for (std::size_t i = first; i <= last; ++i, cursor.Next()) {
    const auto& data = cursor->block.Data();
    if (data.kind == BlockKind::CodeBlock || IsAtomicBlock(data.kind)) {
      continue;
    }
    const auto part = LocalRange(*cursor, range);
    length += part.Length();
    for (const auto& run : data.marks) {
      if (Has(run.marks, mark)) {
        marked += Clip(run.range, part).Length();
      }
    }
  }
  return {length != 0, marked == 0 ? CheckState::Off : marked == length ? CheckState::On : CheckState::Mixed};
}

const LinkRange* LinkAt(const RichDocument& document, TextRange range) {
  const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
  return FindLink(leaf, range);
}

bool CanSetLink(const RichDocument& document, TextRange range) {
  return range.IsCollapsed() ? LinkAt(document, range) != nullptr
                            : MarkStatus(document, range, Mark::Bold, {}).enabled;
}

CommandState BlockKindStatus(const RichDocument& document, TextRange range, const SetBlockKind& command) {
  if (!IsTextBlock(command.kind) || TouchesRoot(document, range, RequiresDocumentRoot)) {
    return {};
  }
  const bool heading = command.kind == BlockKind::Heading;
  if (heading && (command.heading_level == 0 || command.heading_level > 6)) {
    return {};
  }
  const auto [first, last] = TouchedLeaves(document, range);
  const auto& leaves = DocumentAccess::Get(document).index;
  bool enabled = true;
  std::size_t matching = 0;
  auto cursor = leaves.Leaves(first);
  for (std::size_t i = first; i <= last; ++i, cursor.Next()) {
    const auto& data = cursor->block.Data();
    if (IsAtomicBlock(data.kind)) {
      return {};
    }
    if (heading && data.text.find('\n') != std::string::npos) {
      enabled = false;
    }
    if (command.kind == BlockKind::CodeBlock && !data.inline_objects.empty()) {
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
  const auto& leaves = DocumentAccess::Get(document).index;
  const auto* link = std::get_if<SetLink>(&command);
  if (link && range.IsCollapsed()) {
    const auto& leaf = leaves.At(LeafIndex(document, range.start));
    const auto* current = FindLink(leaf, range);
    if (!current) {
      return document;
    }
    range = {leaf.start + current->range.start, leaf.start + current->range.end};
  }
  const auto [first, last] = TouchedLeaves(document, range);
  auto cursor = leaves.Leaves(first);
  for (std::size_t i = first; i <= last; ++i, cursor.Next()) {
    const auto& leaf = *cursor;
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
  const auto selection = ResolveListSelection(document, range);
  if (const auto& items = selection.items) {
    const auto& data = BlockAt(document, items->parent).Data();
    if (data.ordered) {
      return data.list_start;
    }
  }
  return std::nullopt;
}

CommandState ContainerStatus(const RichDocument& document, TextRange range, const EditorCommand& command) {
  const bool protected_content = TouchesRoot(document, range, RequiresDocumentRoot);
  if (std::holds_alternative<ToggleQuote>(command)) {
    if (protected_content) {
      return {};
    }
    const auto selected = Siblings(document, range);
    return {true, !selected.parent.empty() && BlockAt(document, selected.parent).Data().kind == BlockKind::Quote
                      ? CheckState::On : CheckState::Off};
  }
  const auto selection = ResolveListSelection(document, range);
  if (const auto* list = std::get_if<ToggleList>(&command)) {
    const bool ordered = list->kind == ListKind::Ordered;
    const bool enabled = selection.items
        ? BlockAt(document, selection.items->parent).Data().ordered != ordered ||
              CanRemoveListMarkers(document, *selection.items)
        : selection.blocks.has_value();
    return {!protected_content && enabled, selection.Checked(ordered)};
  }
  return {!protected_content && selection.items &&
          (!std::holds_alternative<IndentList>(command) || selection.items->first > 0)};
}

RichDocument ChangeContainer(const RichDocument& document, TextRange range, const EditorCommand& command, EditChanges& edit) {
  if (std::holds_alternative<ToggleQuote>(command)) {
    return ChangeQuote(document, range, edit);
  }
  const auto selection = ResolveListSelection(document, range);
  if (const auto* list = std::get_if<ToggleList>(&command)) {
    return ChangeList(document, selection, list->kind == ListKind::Ordered, edit);
  }
  const auto& items = selection.items;
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

namespace {

EditResult SplitListItem(EditResult result, EditChanges& edit) {
  const auto& document = result.document;
  const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, result.caret));
  const auto slot = ParentSlot(leaf.path);
  if (slot.parent.empty() || slot.first == 0) {
    return result;
  }
  const auto& item = BlockAt(document, slot.parent);
  if (item.Data().kind != BlockKind::ListItem) {
    return result;
  }
  const auto caret = PositionAt(document, result.caret);
  auto data = item.Data();
  // Return at the end of the text adds a sibling below the existing sublist and continuation blocks.
  const auto end = leaf.block.Data().text.empty() ? slot.first + 1 : data.children.size();
  auto next = BlocksBetween(data.children, slot.first, end);
  data.children.erase(data.children.begin() + static_cast<std::ptrdiff_t>(slot.first),
                      data.children.begin() + static_cast<std::ptrdiff_t>(end));
  result.document = Splice(document, ParentSlot(slot.parent),
                           {DocumentAccess::WithData(item, std::move(data)),
                            Block(BlockData{.kind = BlockKind::ListItem, .children = std::move(next)})}, edit);
  result.caret = Absolute(result.document, caret);
  return result;
}

} // namespace

EditResult BreakTextLine(const RichDocument& document, TextRange range, std::optional<Mark> marks,
                         bool hard_break, EditChanges& edit) {
  const auto& index = DocumentAccess::Get(document).index;
  const auto leaf = index.At(LeafIndex(document, range.start));
  if (range.IsCollapsed() && IsAtomicBlock(leaf.block.Data().kind)) {
    return InsertParagraphAtAtomicBlock(document, range.start, edit);
  }
  const auto item = hard_break || leaf.block.Data().kind == BlockKind::CodeBlock
      ? std::nullopt : LeadingListItem(document, leaf.path);
  if (item && range.IsCollapsed() && range.start == leaf.start && !leaf.block.Data().text.empty()) {
    const auto& current = ChildrenAt(document, item->parent)[item->first];
    const Block empty(BlockData{.kind = BlockKind::ListItem, .children = {Block(BlockData{})}});
    return {Splice(document, *item, {empty, current}, edit), range.start};
  }
  auto split = ReplaceText(document, range, "\n", marks, hard_break, edit);
  return item ? SplitListItem(std::move(split), edit) : split;
}

std::optional<RichDocument> ExitContainer(const RichDocument& document, std::size_t ordinal, bool empty_only,
                                         EditChanges& edit) {
  const auto& index = DocumentAccess::Get(document).index;
  const auto leaf = index.At(ordinal);
  const bool empty = leaf.block.Data().text.empty();
  const auto slot = ParentSlot(leaf.path);
  if (!slot.parent.empty() && BlockAt(document, slot.parent).Data().kind == BlockKind::Quote &&
      (empty_only ? slot.last + 1 == ChildrenAt(document, slot.parent).size() : empty || slot.first == 0)) {
    return ChangeQuote(document, {leaf.start, leaf.start}, edit);
  }
  const auto items = LeadingListItem(document, leaf.path);
  if (!items) {
    return std::nullopt;
  }
  const bool empty_item = empty && ChildrenAt(document, slot.parent).size() == 1;
  if (!empty_item) {
    if (empty_only || items->first != 0) {
      return std::nullopt;
    }
    return CanRemoveListMarkers(document, *items) ? UnwrapListItems(document, *items, edit) : document;
  }
  const auto parent = ListAt(document, items->parent);
  if (parent && ordinal + 1 < index.LeafCount() &&
      StartsWith(index.At(ordinal + 1).path, parent->ItemPath(items->parent))) {
    if (empty_only && CanRemoveListMarkers(document, *items)) {
      return UnwrapListItems(document, *items, edit, 1);
    }
    // The next block starts at the old caret after removing the empty leaf and its separator.
    return Splice(document, *items, {}, edit);
  }
  return Outdent(document, *items, edit, true);
}

bool CanContinueInParagraph(const RichDocument& document, TextRange range) {
  const auto& index = DocumentAccess::Get(document).index;
  const auto first = index.AtOffset(range.start);
  return IsTextBlock(first.block.Data().kind) && first.ordinal == index.AtOffset(range.end).ordinal;
}

RichDocument ContinueParagraph(const RichDocument& document, TextRange range, Selection& selection, EditChanges& edit) {
  const auto root = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start)).path.front();
  const auto kind = document.Blocks()[root].Data().kind;
  if (kind != BlockKind::Paragraph && kind != BlockKind::Heading) {
    return ContinueAfterBlock(document, root, selection, edit);
  }
  auto split = ReplaceText(document, range, "\n", Mark::None, false, edit);
  const auto caret = PositionAt(split.document, split.caret);
  selection = {caret, caret};
  return std::move(split.document);
}

bool IsTableCommand(const EditorCommand& command) {
  return std::holds_alternative<InsertTable>(command) || std::holds_alternative<InsertTableRow>(command) ||
      std::holds_alternative<InsertTableColumn>(command) || std::holds_alternative<DeleteTableRow>(command) ||
      std::holds_alternative<DeleteTableColumn>(command) || std::holds_alternative<ToggleTableHeader>(command) ||
      std::holds_alternative<DeleteTable>(command) || std::holds_alternative<ExitTable>(command) ||
      std::holds_alternative<SetTableColumnAlignment>(command);
}

CommandState TableStatus(const RichDocument& document, TextRange range, const EditorCommand& command) {
  const auto& leaves = DocumentAccess::Get(document).index;
  const auto first = LeafIndex(document, range.start);
  const auto& leaf = leaves.At(first);
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
  if (const auto* alignment = std::get_if<SetTableColumnAlignment>(&command)) {
    return {ValidTableColumnAlignment(alignment->alignment),
            ColumnAlignment(table, cell->column) == alignment->alignment ? CheckState::On : CheckState::Off};
  }
  return {true};
}

RichDocument ChangeTable(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit, bool select_inserted_row) {
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.index.At(storage.index.IndexOf(selection.active.block).value());
  auto index = leaf.path.front();
  if (std::holds_alternative<ExitTable>(command)) {
    return ContinueAfterBlock(document, index, selection, edit);
  }
  auto blocks = BlocksBetween(document.Blocks(), 0, document.Blocks().size());
  if (const auto* insert = std::get_if<InsertTable>(&command)) {
    Block table(BlockData{.kind = BlockKind::Table,
                          .children = std::vector<Block>(insert->rows, EmptyRow(insert->columns)),
                          .table_has_header = insert->has_header});
    const bool replace = leaf.block.Data().text.empty();
    if (replace) {
      edit.Touch(blocks[index]);
      blocks[index] = std::move(table);
    } else {
      blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(++index), std::move(table));
    }
    auto next = edit.Build(document, std::move(blocks), {{0, {index, index + replace, index + 1}}});
    SelectBlock(selection, next.Blocks()[index]);
    return next;
  }

  const auto cell = *TableCellAt(document, leaf);
  if (std::holds_alternative<DeleteTable>(command)) {
    edit.Touch(blocks[index]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(index));
    auto next = edit.Build(document, std::move(blocks), {{0, {index, index + 1, index}}});
    const bool at_end = index >= next.Blocks().size();
    SelectBlock(selection, next.Blocks()[at_end ? index - 1 : index], at_end);
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
    if (select_inserted_row) {
      row = at;
      column = 0;
    }
  } else if (std::holds_alternative<DeleteTableRow>(command)) {
    edit.Touch(rows[row]);
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(row));
    if (row == 0) {
      data.table_has_header = false;
    }
    row = std::min(row, rows.size() - 1);
  } else if (std::holds_alternative<ToggleTableHeader>(command)) {
    data.table_has_header = !data.table_has_header;
  } else if (const auto* alignment = std::get_if<SetTableColumnAlignment>(&command)) {
    if (ColumnAlignment(data, column) == alignment->alignment) {
      return document;
    }
    data.table_column_alignments.resize(column_count, TableColumnAlignment::Default);
    data.table_column_alignments[column] = alignment->alignment;
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
    auto& alignments = data.table_column_alignments;
    if (!alignments.empty()) {
      if (insert) {
        alignments.insert(alignments.begin() + at, TableColumnAlignment::Default);
      } else {
        alignments.erase(alignments.begin() + at);
      }
    }
    if (!insert) {
      column = std::min(column, column_count - 2);
    }
  }
  blocks[index] = DocumentAccess::WithData(blocks[index], std::move(data));
  auto next = edit.Build(document, std::move(blocks), {{0, {index, index + 1, index + 1}}});
  if (select_inserted_row || !DocumentAccess::Get(next).index.FindLeaf(selection.active.block)) {
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
  const auto& leaf = DocumentAccess::Get(document).index.At(index);
  if (IsAtomicBlock(leaf.block.Data().kind) && range == TextRange{leaf.start, leaf.start + 1}) {
    return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> SelectedImageIndex(const RichDocument& document, TextRange range) {
  const auto index = SelectedAtomicIndex(document, range);
  return index && DocumentAccess::Get(document).index.At(*index).block.Data().image ? index : std::nullopt;
}

bool CanInsertAtomicBlock(const RichDocument& document, TextRange range) {
  const auto& leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
  if (!range.IsCollapsed()) {
    return leaf.path.size() == 1 && IsAtomicBlock(leaf.block.Data().kind) &&
        range == TextRange{leaf.start, leaf.start + 1};
  }
  const auto kind = leaf.block.Data().kind;
  return leaf.path.size() == 1 && (kind == BlockKind::Paragraph || IsAtomicBlock(kind));
}

bool CanInsertThematicBreak(const RichDocument& document, TextRange range) {
  const auto leaf = DocumentAccess::Get(document).index.At(LeafIndex(document, range.start));
  return range.IsCollapsed() && leaf.block.Data().kind == BlockKind::Paragraph && !TableCellAt(document, leaf);
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
  const auto& leaf = storage.index.At(storage.index.IndexOf(selection.active.block).value());
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
  const auto siblings = ChildrenAt(next, slot.parent);
  if (!at_atomic && offset < length) {
    SelectBlock(selection, siblings[block_index + 1]);
  } else {
    SelectBlock(selection, siblings[block_index], true);
  }
  return next;
}

RichDocument ChangeImage(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit) {
  if (const auto* insert = std::get_if<InsertImage>(&command)) {
    return InsertAtomicBlock(document, selection, {.kind = BlockKind::Image, .image = insert->image}, edit);
  }
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.index.At(storage.index.IndexOf(selection.active.block).value());
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
  const auto& leaf = storage.index.At(storage.index.IndexOf(selection.active.block).value());
  auto data = leaf.block.Data();
  data.custom = replacement;
  return Splice(document, ParentSlot(leaf.path), {DocumentAccess::WithData(leaf.block, std::move(data))}, edit);
}

} // namespace huxerui::richeditor::detail

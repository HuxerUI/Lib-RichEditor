#include "document_internal.h"

#include <array>
#include <stdexcept>

namespace huxerui::richeditor::detail {
namespace {

constexpr std::size_t child_page_size = 64;
constexpr std::size_t location_page_size = 128;

struct IndexEntry {
  Block block;
  std::shared_ptr<const IndexChildren> children;
  std::size_t first_leaf;
  TextOffset start;
};

struct ChildPage {
  std::vector<IndexEntry> entries;
  std::size_t leaf_count = 0;
  /// Includes one separator per leaf, including the last.
  TextOffset span = 0;
  std::size_t height = 0;
  bool has_inline_objects = false;
};

struct PagePosition {
  std::shared_ptr<const ChildPage> page;
  std::size_t first_child;
  std::size_t first_leaf;
  TextOffset start;
};

struct Location {
  BlockId parent = 0;
  /// A page is identified by the first child it contains, independent of its order in the container.
  BlockId page = 0;
  std::size_t slot = 0;
  bool operator==(const Location&) const = default;
};

struct LocationPage {
  std::array<Location, location_page_size> entries;
  std::size_t used = 0;
};

template <class Entries, class Value, class Projection>
std::size_t Preceding(const Entries& entries, Value value, Projection projection) {
  const auto after = std::ranges::upper_bound(entries, value, {}, projection);
  return static_cast<std::size_t>(after - entries.begin() - 1);
}

} // namespace

struct IndexChildren {
  std::vector<PagePosition> pages;
  std::unordered_map<BlockId, std::size_t> page_indexes;
  std::size_t child_count = 0;
  std::size_t leaf_count = 0;
  TextOffset span = 0;
  std::size_t height = 0;
  bool has_inline_objects = false;
};

struct IndexLocations {
  /// Chunking by ID lets snapshot updates copy only the pages with changed locations.
  std::unordered_map<BlockId, std::shared_ptr<LocationPage>> pages;

  Location Get(BlockId id) const {
    const auto page = pages.find(id / location_page_size);
    return page == pages.end() ? Location{} : page->second->entries[id % location_page_size];
  }
};

namespace {

struct LocatedEntry {
  const IndexEntry* entry = nullptr;
  std::size_t first_leaf = 0;
  TextOffset start = 0;
};

LocatedEntry Locate(const IndexChildren& root, const IndexLocations& locations, BlockId id) {
  const auto location = locations.Get(id);
  if (location.page == 0) {
    return {};
  }
  const auto parent = location.parent == 0 ? LocatedEntry{} : Locate(root, locations, location.parent);
  const auto* children = location.parent == 0 ? &root : parent.entry ? parent.entry->children.get() : nullptr;
  if (!children) {
    return {};
  }
  std::size_t page_index = 0;
  if (children->pages.size() == 1) {
    if (DocumentAccess::Id(children->pages.front().page->entries.front().block) != location.page) {
      return {};
    }
  } else {
    const auto found = children->page_indexes.find(location.page);
    if (found == children->page_indexes.end()) {
      return {};
    }
    page_index = found->second;
  }
  const auto& page = children->pages[page_index];
  if (location.slot >= page.page->entries.size()) {
    return {};
  }
  const auto& entry = page.page->entries[location.slot];
  if (DocumentAccess::Id(entry.block) != id) {
    return {};
  }
  return {&entry, parent.first_leaf + page.first_leaf + entry.first_leaf,
          parent.start + page.start + entry.start};
}

struct OrdinalPosition {
  std::size_t page;
  std::size_t slot;
};

OrdinalPosition LocateOrdinal(const IndexChildren& children, std::size_t ordinal) {
  const auto page = Preceding(children.pages, ordinal, &PagePosition::first_leaf);
  const auto& position = children.pages[page];
  return {page, Preceding(position.page->entries, ordinal - position.first_leaf, &IndexEntry::first_leaf)};
}

void AppendPage(IndexChildren& children, std::shared_ptr<const ChildPage> page) {
  if (children.pages.size() == 1) {
    children.page_indexes.emplace(DocumentAccess::Id(children.pages.front().page->entries.front().block), 0);
  }
  if (!children.pages.empty()) {
    children.page_indexes.emplace(DocumentAccess::Id(page->entries.front().block), children.pages.size());
  }
  children.pages.push_back({page, children.child_count, children.leaf_count, children.span});
  children.child_count += page->entries.size();
  children.leaf_count += page->leaf_count;
  children.span += page->span;
  children.height = std::max(children.height, page->height);
  children.has_inline_objects |= page->has_inline_objects;
}

ChildChange Difference(std::span<const Block> old, std::span<const Block> current) {
  const auto same = [](const Block& first, const Block& second) {
    return DocumentAccess::Id(first) == DocumentAccess::Id(second) && DocumentAccess::SameBlock(first, second);
  };
  const auto mismatch = std::ranges::mismatch(old, current, same);
  const auto first = static_cast<std::size_t>(mismatch.in1 - old.begin());
  auto old_end = old.size();
  auto new_end = current.size();
  while (old_end > first && new_end > first && same(old[old_end - 1], current[new_end - 1])) {
    --old_end;
    --new_end;
  }
  return {first, old_end, new_end};
}

} // namespace

class IndexBuilder {
public:
  IndexBuilder(BlockId& last_id, const DocumentIndex* previous, const ChildChanges& changes,
               std::vector<BlockId>* added_text_blocks)
      : last_id_(last_id), previous_(previous), changes_(changes), added_text_blocks_(added_text_blocks) {
    if (!previous_) {
      locations_ = std::make_shared<IndexLocations>();
    }
  }

  DocumentIndex Build(std::vector<Block>& blocks, std::span<const Block> old_blocks) {
    DocumentIndex result;
    result.root_ = BuildChildren(blocks, old_blocks, previous_ ? previous_->root_ : nullptr, 0, BlockKind::Paragraph, 0);
    // Reparented blocks have their new locations by this point. Retire only blocks absent from the new tree.
    for (const auto id : retired_) {
      RemoveUnreachable(*result.root_, id);
    }
    result.locations_ = locations_ ? locations_ : previous_->locations_;
    return result;
  }

private:
  const IndexLocations& Locations() const {
    return locations_ ? *locations_ : *previous_->locations_;
  }

  void SetLocation(BlockId id, Location location) {
    const auto old = Locations().Get(id);
    if (old == location) {
      return;
    }
    if (!locations_) {
      locations_ = std::make_shared<IndexLocations>(*previous_->locations_);
    }
    const auto page_id = id / location_page_size;
    auto& page = locations_->pages[page_id];
    if (!page) {
      page = std::make_shared<LocationPage>();
    } else if (page.use_count() != 1) {
      page = std::make_shared<LocationPage>(*page);
    }
    page->entries[id % location_page_size] = location;
    if (old.page == 0) {
      ++page->used;
    } else if (location.page == 0 && --page->used == 0) {
      locations_->pages.erase(page_id);
    }
  }

  void RemoveUnreachable(const IndexChildren& root, BlockId id) {
    if (Locations().Get(id).page == 0 || Locate(root, Locations(), id).entry) {
      return;
    }
    const auto* old = Locate(*previous_->root_, *previous_->locations_, id).entry;
    SetLocation(id, {});
    if (old && old->children) {
      for (const auto& position : old->children->pages) {
        for (const auto& entry : position.page->entries) {
          RemoveUnreachable(root, DocumentAccess::Id(entry.block));
        }
      }
    }
  }

  std::shared_ptr<const ChildPage> BuildPage(std::span<Block> blocks, BlockId parent, BlockKind parent_kind,
                                             std::size_t depth) {
    auto page = std::make_shared<ChildPage>();
    page->entries.reserve(blocks.size());
    for (auto& block : blocks) {
      const auto id = DocumentAccess::Id(block);
      const auto* old = previous_ && id ? Locate(*previous_->root_, *previous_->locations_, id).entry : nullptr;
      const auto same = old && DocumentAccess::SameBlock(block, old->block);
      if (!same || previous_->locations_->Get(id).parent != parent) {
        DocumentAccess::ValidatePlacement(block, parent_kind, parent == 0);
      }
      if (!previous_ || id == 0) {
        DocumentAccess::AssignId(block, last_id_, added_text_blocks_, !previous_);
      }
      std::shared_ptr<const IndexChildren> children;
      if (!IsLeaf(block.Data().kind)) {
        if (same) {
          children = old->children;
        } else {
          auto data = block.Data();
          const auto old_blocks = old && old->children ? std::span(old->block.Data().children) : std::span<const Block>{};
          const auto before = last_id_;
          children = BuildChildren(data.children, old_blocks, old ? old->children : nullptr,
                                   DocumentAccess::Id(block), data.kind, depth + 1);
          if (last_id_ != before) {
            block = DocumentAccess::WithData(block, std::move(data));
          }
        }
      }
      page->entries.push_back({block, children, page->leaf_count, page->span});
      page->leaf_count += children ? children->leaf_count : 1;
      page->span += children ? children->span : DocumentAccess::Body(block).Length() + 1;
      page->height = std::max(page->height, children ? children->height + 1 : 1);
      page->has_inline_objects |= children ? children->has_inline_objects : !block.Data().inline_objects.empty();
    }
    const auto key = DocumentAccess::Id(page->entries.front().block);
    for (std::size_t slot = 0; slot < page->entries.size(); ++slot) {
      SetLocation(DocumentAccess::Id(page->entries[slot].block), {parent, key, slot});
    }
    return page;
  }

  std::shared_ptr<const IndexChildren> BuildChildren(std::vector<Block>& blocks, std::span<const Block> old_blocks,
                                                     const std::shared_ptr<const IndexChildren>& old, BlockId parent,
                                                     BlockKind parent_kind, std::size_t depth) {
    if (depth >= max_nesting) {
      throw std::invalid_argument("Document nesting limit exceeded");
    }
    auto result = std::make_shared<IndexChildren>();
    std::size_t new_first = 0;
    std::size_t new_end = blocks.size();
    std::size_t page_end = 0;
    if (old) {
      const auto hint = changes_.find(parent);
      const auto change = hint != changes_.end() &&
              hint->second.first <= std::min(hint->second.old_end, hint->second.new_end) &&
              hint->second.old_end <= old_blocks.size() &&
              hint->second.new_end <= blocks.size() &&
              old_blocks.size() - hint->second.old_end == blocks.size() - hint->second.new_end
          ? hint->second : Difference(old_blocks, blocks);
      if (change.first == change.old_end && change.first == change.new_end) {
        if (depth + old->height > max_nesting) {
          throw std::invalid_argument("Document nesting limit exceeded");
        }
        return old;
      }

      for (auto i = change.first; i < change.old_end; ++i) {
        retired_.push_back(DocumentAccess::Id(old_blocks[i]));
      }
      auto page_first = Preceding(old->pages, change.first, &PagePosition::first_child);
      page_end = change.old_end > change.first
          ? Preceding(old->pages, change.old_end - 1, &PagePosition::first_child) + 1 : page_first + 1;
      const auto removed = change.old_end - change.first;
      const auto added = change.new_end - change.first;
      const auto boundary = [&] {
        return page_end == old->pages.size() ? old->child_count : old->pages[page_end].first_child;
      };
      new_first = old->pages[page_first].first_child;
      new_end = boundary() - removed + added;
      // Merge a small remainder into a neighboring page so repeated removals do not fragment the index.
      if (new_end > new_first && new_end - new_first < child_page_size / 2) {
        if (page_end < old->pages.size()) {
          ++page_end;
          new_end = boundary() - removed + added;
        } else if (page_first > 0) {
          --page_first;
          new_first = old->pages[page_first].first_child;
        }
      }
      result->pages.reserve(old->pages.size());
      for (std::size_t i = 0; i < page_first; ++i) {
        AppendPage(*result, old->pages[i].page);
      }
    }
    if (new_first < new_end) {
      const auto count = new_end - new_first;
      const auto page_count = (count + child_page_size - 1) / child_page_size;
      const auto per_page = (count + page_count - 1) / page_count;
      for (auto begin = new_first; begin < new_end; begin += per_page) {
        AppendPage(*result, BuildPage(std::span(blocks).subspan(begin, std::min(per_page, new_end - begin)),
                                      parent, parent_kind, depth));
      }
    }
    if (old) {
      for (auto i = page_end; i < old->pages.size(); ++i) {
        AppendPage(*result, old->pages[i].page);
      }
    }
    if (depth + result->height > max_nesting) {
      throw std::invalid_argument("Document nesting limit exceeded");
    }
    return result;
  }

  BlockId& last_id_;
  const DocumentIndex* previous_;
  const ChildChanges& changes_;
  std::vector<BlockId>* added_text_blocks_;
  std::shared_ptr<IndexLocations> locations_;
  std::vector<BlockId> retired_;
};

DocumentIndex DocumentIndex::Build(std::vector<Block>& blocks, BlockId& last_id, const DocumentIndex* previous,
                                    std::span<const Block> old_blocks, const ChildChanges& changes,
                                    std::vector<BlockId>* added_text_blocks) {
  return IndexBuilder(last_id, previous, changes, added_text_blocks).Build(blocks, old_blocks);
}

std::size_t DocumentIndex::LeafCount() const { return root_->leaf_count; }

TextOffset DocumentIndex::Length() const { return root_->span - 1; }

Leaf DocumentIndex::At(std::size_t ordinal) const {
  const auto* children = root_.get();
  BlockPath path;
  TextOffset start = 0;
  for (;;) {
    const auto position = LocateOrdinal(*children, ordinal);
    const auto& page = children->pages[position.page];
    const auto& entry = page.page->entries[position.slot];
    path.push_back(page.first_child + position.slot);
    start += page.start + entry.start;
    if (!entry.children) {
      return {entry.block, std::move(path), start};
    }
    ordinal -= page.first_leaf + entry.first_leaf;
    children = entry.children.get();
  }
}

IndexedPosition DocumentIndex::AtOffset(TextOffset absolute) const {
  const auto* children = root_.get();
  std::size_t ordinal = 0;
  TextOffset start = 0;
  for (;;) {
    const auto& page = children->pages[Preceding(children->pages, absolute, &PagePosition::start)];
    absolute -= page.start;
    const auto& entries = page.page->entries;
    const auto& entry = entries[Preceding(entries, absolute, &IndexEntry::start)];
    ordinal += page.first_leaf + entry.first_leaf;
    start += page.start + entry.start;
    if (!entry.children) {
      return {entry.block, ordinal, start};
    }
    absolute -= entry.start;
    children = entry.children.get();
  }
}

std::optional<std::size_t> DocumentIndex::IndexOf(BlockId id) const {
  const auto found = Locate(*root_, *locations_, id);
  return found.entry && !found.entry->children ? std::optional(found.first_leaf) : std::nullopt;
}

std::optional<std::size_t> DocumentIndex::FirstLeafOf(BlockId id) const {
  const auto found = Locate(*root_, *locations_, id);
  return found.entry ? std::optional(found.first_leaf) : std::nullopt;
}

const Block* DocumentIndex::FindLeaf(BlockId id) const {
  const auto found = Locate(*root_, *locations_, id);
  return found.entry && !found.entry->children ? &found.entry->block : nullptr;
}

TextOffset DocumentIndex::StartOf(BlockId id) const {
  const auto found = Locate(*root_, *locations_, id);
  if (!found.entry || found.entry->children) {
    throw std::out_of_range("Unknown leaf block ID");
  }
  return found.start;
}

DocumentIndex::Cursor DocumentIndex::Leaves(std::size_t first) const { return Cursor(*root_, first); }

void DocumentIndex::VisitInlineBlocks(const std::function<void(const Block&)>& visit) const {
  const auto walk = [&](const auto& self, const IndexChildren& children) -> void {
    if (!children.has_inline_objects) {
      return;
    }
    for (const auto& page : children.pages) {
      if (!page.page->has_inline_objects) {
        continue;
      }
      for (const auto& entry : page.page->entries) {
        if (entry.children) {
          self(self, *entry.children);
        } else if (!entry.block.Data().inline_objects.empty()) {
          visit(entry.block);
        }
      }
    }
  };
  walk(walk, *root_);
}

DocumentIndex::Cursor::Cursor(const IndexChildren& root, std::size_t ordinal) {
  if (ordinal >= root.leaf_count) {
    return;
  }
  const auto* children = &root;
  BlockPath path;
  TextOffset start = 0;
  for (;;) {
    const auto position = LocateOrdinal(*children, ordinal);
    const auto& page = children->pages[position.page];
    const auto& entry = page.page->entries[position.slot];
    frames_.push_back({children, position.page, position.slot});
    path.push_back(page.first_child + position.slot);
    start += page.start + entry.start;
    if (!entry.children) {
      leaf_.emplace(Leaf{entry.block, std::move(path), start});
      return;
    }
    ordinal -= page.first_leaf + entry.first_leaf;
    children = entry.children.get();
  }
}

void DocumentIndex::Cursor::Next() {
  leaf_->start += DocumentAccess::Body(leaf_->block).Length() + 1;
  while (!frames_.empty()) {
    auto& frame = frames_.back();
    const auto& pages = frame.children->pages;
    if (++frame.slot == pages[frame.page].page->entries.size()) {
      frame.slot = 0;
      ++frame.page;
    }
    if (frame.page == pages.size()) {
      frames_.pop_back();
      leaf_->path.pop_back();
      continue;
    }
    const auto& page = pages[frame.page];
    leaf_->path.back() = page.first_child + frame.slot;
    auto* entry = &page.page->entries[frame.slot];
    while (entry->children) {
      frames_.push_back({entry->children.get(), 0, 0});
      leaf_->path.push_back(0);
      entry = &entry->children->pages.front().page->entries.front();
    }
    leaf_->block = entry->block;
    return;
  }
  leaf_.reset();
}

} // namespace huxerui::richeditor::detail

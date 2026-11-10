#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor::detail {

using BlockId = std::uint64_t;
using BlockPath = std::vector<std::size_t>;

struct Leaf {
  Block block;
  /// Child indices from the document root.
  BlockPath path;
  /// Start in the flattened UTF-16 text, which places one LF between successive leaves.
  TextOffset start;
};

/// Position lookup result borrowing the leaf from its index snapshot.
struct IndexedPosition {
  const Block& block;
  std::size_t ordinal; ///< Zero-based leaf index in document order.
  TextOffset start;    ///< Start of the leaf in flattened UTF-16 text.
};

/// A container's differing child range before and after an edit.
struct ChildChange {
  std::size_t first;   ///< First affected child in both sequences.
  std::size_t old_end; ///< Exclusive end in the old sequence.
  std::size_t new_end; ///< Exclusive end in the new sequence.
};
/// Child changes keyed by container ID. Zero identifies the document root.
using ChildChanges = std::unordered_map<BlockId, ChildChange>;

struct IndexChildren;
struct IndexLocations;
class IndexBuilder;

/// @brief Immutable index of a document snapshot, with container-local pages shared across edits.
///
/// Leaf ordinals and absolute UTF-16 offsets are derived from page prefixes. The ID locator follows
/// parent and page references, so a shifted or reparented subtree can keep its local entries.
class DocumentIndex {
public:
  /// Visits leaves in order. The index must outlive the cursor. Access and advance only while it is valid.
  class Cursor {
  public:
    explicit operator bool() const { return leaf_.has_value(); }
    const Leaf& operator*() const { return *leaf_; }
    const Leaf* operator->() const { return &*leaf_; }
    void Next();

  private:
    struct Frame {
      const IndexChildren* children;
      std::size_t page;
      std::size_t slot;
    };

    Cursor(const IndexChildren& root, std::size_t ordinal);

    std::vector<Frame> frames_;
    std::optional<Leaf> leaf_;

    friend class DocumentIndex;
  };

  /// @brief Validates placement and indexes the candidate tree, assigning IDs to new blocks in place.
  /// @param blocks Candidate root blocks. Must contain at least one leaf.
  /// @param last_id ID high-water mark. Pass the current mark for incremental edits.
  /// @param previous Index of the old tree, or null to assign fresh identities and build from scratch.
  /// @param old_blocks Root blocks matching previous, when supplied.
  /// @param changes Known child splices. Modified containers without a hint derive the differing range.
  /// @param added_text_blocks Optional output for IDs of newly assigned text leaves.
  static DocumentIndex Build(std::vector<Block>& blocks, BlockId& last_id, const DocumentIndex* previous,
                             std::span<const Block> old_blocks, const ChildChanges& changes,
                             std::vector<BlockId>* added_text_blocks);

  std::size_t LeafCount() const;
  /// UTF-16 length of all leaves and their intervening LF separators.
  TextOffset Length() const;
  /// Looks up a leaf by an ordinal in [0, LeafCount()).
  Leaf At(std::size_t ordinal) const;
  /// Looks up the leaf at an offset in [0, Length()]. The boundary before a separator belongs to the preceding leaf.
  IndexedPosition AtOffset(TextOffset absolute) const;
  /// Returns the ordinal of a live leaf ID, or an empty value for absent IDs and containers.
  std::optional<std::size_t> IndexOf(BlockId id) const;
  /// Borrows a live leaf from this snapshot, or returns null for absent IDs and containers.
  const Block* FindLeaf(BlockId id) const;
  /// Returns the flattened start of a live leaf. Throws std::out_of_range for an absent ID or a container.
  TextOffset StartOf(BlockId id) const;
  /// Starts an ordered scan at first. An ordinal at or past LeafCount() yields an exhausted cursor.
  Cursor Leaves(std::size_t first = 0) const;
  /// Visits only blocks with inline objects, skipping pages without any.
  void VisitInlineBlocks(const std::function<void(const Block&)>& visit) const;

private:
  std::shared_ptr<const IndexChildren> root_;
  std::shared_ptr<const IndexLocations> locations_;

  friend class IndexBuilder;
};

} // namespace huxerui::richeditor::detail

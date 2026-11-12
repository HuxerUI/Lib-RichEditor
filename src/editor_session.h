#pragma once

#include <deque>

#include <huxerui/state.h>
#include <huxerui/text_input.h>

#include "edit_operations.h"

namespace huxerui::richeditor::detail {

struct EditState {
  RichDocument document;
  Selection selection;
  /// Explicit marks for subsequent typing. An absent value inherits from the caret.
  std::optional<Mark> pending;
};

/// Tracks the committed baseline and preedit range while input is provisional.
struct Composition {
  EditState original;
  /// The replacement range in the committed document's flattened UTF-16 text.
  TextRange original_range;
  /// The marked range in the visible draft's flattened UTF-16 text.
  TextRange range;
};

struct Draft {
  EditState edit;
  std::optional<Composition> composition;
  EditChanges changes;
};

class EditorSession {
public:
  explicit EditorSession(RichDocument initial);

  const RichDocument& VisibleDocument() const { return draft_.edit.document; }
  RichDocument Document() const;
  void ReplaceDocument(RichDocument document);

  TextSelection SelectionOffsets() const;
  std::optional<unsigned> OrderedListStart() const {
    return draft_.composition ? std::nullopt : ListStartAt(VisibleDocument(), SelectionOffsets().Range());
  }
  std::optional<std::size_t> SelectedImage() const {
    return draft_.composition ? std::nullopt : SelectedImageIndex(VisibleDocument(), SelectionOffsets().Range());
  }
  std::optional<std::size_t> SelectedAtomicBlock() const {
    return draft_.composition ? std::nullopt : SelectedAtomicIndex(VisibleDocument(), SelectionOffsets().Range());
  }
  const CustomBlockData* SelectedCustomBlock() const;
  std::optional<InlineObjectSelection> SelectedInlineObject() const;
  std::optional<DocumentFragment> SelectedFragment() const;
  /// Captures the object starting at a flattened UTF-16 offset. Available outside IME preedit.
  std::optional<InlineObjectSelection> InlineObjectAt(TextOffset absolute) const;
  std::optional<TextRange> CompositionRange() const;
  std::uint64_t Revision() const { return revision_; }
  std::uint64_t ContentRevision() const { return content_revision_; }
  std::uint64_t CommittedRevision() const { return committed_.Get(); }
  /// Returns false when the requested revision has aged out of the journal.
  bool TextBlockChangesSince(std::uint64_t revision, std::vector<BlockId>& blocks) const;
  /// Advances when a new document replaces the session's content so old text input clients can retire.
  std::uint64_t Generation() const { return generation_; }
  std::uint64_t Observe() const { return changed_.Get(); }

  bool Apply(std::span<const TextInputCommand> commands);
  bool Select(TextSelection selection);
  bool BreakLine(bool hard_break);
  /// Reports whether a structural Backspace action consumed the key at the start of the block.
  bool BackspaceAtStart();
  bool AppendTableRow();
  bool SetCodeLanguage(BlockId block, std::string_view language);
  /// Applies one undoable change to the indicated task outside IME preedit.
  bool ToggleTaskChecked(BlockId item);
  void FinishComposition();

  CommandState CommandStatus(const EditorCommand& command) const;
  bool Execute(const EditorCommand& command);

private:
  /// Current location of a captured object. Resolving it again allows the object to move between edits.
  struct InlineLocation {
    Leaf leaf;
    std::size_t object;
  };
  std::optional<InlineLocation> LocateInlineObject(const InlineObjectTarget& target) const;

  struct History {
    EditState before;
    EditState after;
    std::vector<BlockId> changed_blocks;
    bool task_state_only;
  };

  bool Publish(Draft next, bool remember, bool reset_block_changes = false);

  Draft draft_;
  std::deque<History> undo_;
  std::deque<History> redo_;
  /// One entry per committed snapshot, including structural edits with no changed text blocks.
  std::deque<std::vector<BlockId>> block_changes_;
  State<std::uint64_t> changed_{0};
  State<std::uint64_t> committed_{0};
  std::uint64_t revision_ = 0;
  std::uint64_t content_revision_ = 0;
  std::uint64_t generation_ = 0;
  /// Ownership token held weakly by targets. Renewed on each ReplaceDocument() call.
  std::shared_ptr<const void> inline_owner_ = std::make_shared<int>(0);
};

} // namespace huxerui::richeditor::detail

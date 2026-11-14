#pragma once

#include <huxerui/richeditor.h>

#include "document_internal.h"

namespace huxerui::richeditor::detail {

/// Collects text block IDs affected by tree edits until the session publishes a new snapshot.
struct EditChanges {
  BlockId last_id;
  std::vector<BlockId> touched;
  /// Set for a task state edit that preserves the ordered leaves and their UTF-16 text. Publishing consumes it.
  bool task_state_only = false;

  /// Records leaves before a subtree is replaced or erased, including atomic blocks that may become text.
  void Touch(const Block& block);
  /// Builds the next snapshot from the edited tree and child splices, recording newly assigned text leaf IDs.
  RichDocument Build(const RichDocument& document, std::vector<Block> blocks, const ChildChanges& changes);
};

struct EditResult {
  RichDocument document;
  TextOffset caret;
  /// Changes to the flattened separators around the replacement when editing next to or deleting an atomic block.
  TextOffset leading_separator = 0;
  TextOffset trailing_separator = 0;
};

bool CanEditTextRange(const RichDocument& document, TextRange range);
EditResult ReplaceText(const RichDocument& document, TextRange range, std::string_view text,
                       std::optional<Mark> marks, bool hard_break, EditChanges& edit, bool inherit_link = true);
/// Applies block-local replacements against one snapshot and remaps the flattened selection.
/// Throws std::invalid_argument for an invalid batch.
RichDocument ReplaceBlockTextRanges(const RichDocument& document, std::span<const BlockTextRange> ranges,
                                    std::string_view text, TextSelection& selection, EditChanges& edit);
/// Checks that both endpoints belong to one paragraph or heading. Offsets use the flattened UTF-16 text.
bool CanInsertInlineObject(const RichDocument& document, TextRange range);
/// Replaces a range allowed by CanInsertInlineObject with a new character and annotation.
EditResult InsertInlineContent(const RichDocument& document, TextRange range, const InlineObjectData& object,
                               std::optional<Mark> marks, EditChanges& edit);
/// Replaces the data at index in the leaf's inline_objects array, preserving object identity and offset.
RichDocument ReplaceInlineContent(const RichDocument& document, const Leaf& leaf, std::size_t index,
                                  const InlineObjectData& object, EditChanges& edit);
/// Copies a noncollapsed flattened UTF-16 range and records whether each edge slices a leaf.
DocumentFragment CopyFragment(const RichDocument& document, TextRange range);
/// Checks whether the fragment can be inserted at the range given the destination block and nesting.
bool CanInsertFragment(const RichDocument& document, TextRange range, const DocumentFragment& fragment);
/// Applies a fragment accepted by CanInsertFragment and returns the caret after the inserted content.
EditResult PasteFragment(const RichDocument& document, TextRange range, const DocumentFragment& fragment,
                         EditChanges& edit);
Mark MarksAt(const RichDocument& document, TextOffset position);
CommandState MarkStatus(const RichDocument& document, TextRange range, Mark mark, std::optional<Mark> pending);
const LinkRange* LinkAt(const RichDocument& document, TextRange range);
bool CanSetLink(const RichDocument& document, TextRange range);
CommandState BlockKindStatus(const RichDocument& document, TextRange range, const SetBlockKind& command);
RichDocument Format(const RichDocument& document, TextRange range, const EditorCommand& command, bool remove_mark,
                    EditChanges& edit);
std::optional<unsigned> ListStartAt(const RichDocument& document, TextRange range);
CommandState ContainerStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeContainer(const RichDocument& document, TextRange range, const EditorCommand& command, EditChanges& edit);
/// Toggles the task marker identified by a live list-item ID without changing text or selection.
RichDocument ToggleTaskAt(const RichDocument& document, BlockId item, EditChanges& edit);
/// Replaces the selection with a break, creating a sibling item when splitting a list item's leading text.
EditResult BreakTextLine(const RichDocument& document, TextRange range, std::optional<Mark> marks,
                         bool hard_break, EditChanges& edit);
/// Handles Return on an empty block or Backspace at a leading block boundary.
/// The caret must be at the start of the leaf identified by ordinal. For Return, the leaf is empty.
/// Returns the resulting snapshot, including the original snapshot when a guarded unwrap consumes the key.
/// An empty result lets the caller proceed with ordinary text editing.
std::optional<RichDocument> ExitContainer(const RichDocument& document, std::size_t ordinal, bool empty_only,
                                         EditChanges& edit);
/// Checks that the selection is confined to a single text leaf.
bool CanContinueInParagraph(const RichDocument& document, TextRange range);
/// Splits top-level text or places the caret in a paragraph after its enclosing top-level block.
RichDocument ContinueParagraph(const RichDocument& document, TextRange range, Selection& selection, EditChanges& edit);
bool IsTableCommand(const EditorCommand& command);
CommandState TableStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeTable(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit, bool select_inserted_row = false);
bool IsImageCommand(const EditorCommand& command);
std::optional<std::size_t> SelectedImageIndex(const RichDocument& document, TextRange range);
std::optional<std::size_t> SelectedAtomicIndex(const RichDocument& document, TextRange range);
bool CanInsertAtomicBlock(const RichDocument& document, TextRange range);
/// Checks for a paragraph caret at a location that accepts a thematic break.
bool CanInsertThematicBreak(const RichDocument& document, TextRange range);
RichDocument InsertAtomicBlock(const RichDocument& document, Selection& selection, BlockData data, EditChanges& edit);
RichDocument ReplaceSelectedCustomBlock(const RichDocument& document, const Selection& selection,
                                        const CustomBlockData& replacement, EditChanges& edit);
CommandState ImageStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeImage(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit);

} // namespace huxerui::richeditor::detail

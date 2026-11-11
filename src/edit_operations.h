#pragma once

#include <huxerui/richeditor.h>

#include "document_internal.h"

namespace huxerui::richeditor::detail {

/// Collects text block IDs affected by tree edits until the session publishes a new snapshot.
struct EditChanges {
  BlockId last_id;
  std::vector<BlockId> touched;

  /// Records leaves before a subtree is replaced or erased, including atomic blocks that may become text.
  void Touch(const Block& block);
  /// Builds the next snapshot from the edited tree and child splices, recording newly assigned text leaf IDs.
  RichDocument Build(const RichDocument& document, std::vector<Block> blocks, const ChildChanges& changes);
};

struct EditResult {
  RichDocument document;
  TextOffset caret;
  /// Changes to the flattened separators around the replacement when editing next to or deleting an image.
  TextOffset leading_separator = 0;
  TextOffset trailing_separator = 0;
};

bool CanEditTextRange(const RichDocument& document, TextRange range);
EditResult InsertParagraphAtAtomicBlock(const RichDocument& document, TextOffset caret, EditChanges& edit);
EditResult ReplaceText(const RichDocument& document, TextRange range, std::string_view text,
                       std::optional<Mark> marks, bool hard_break, EditChanges& edit);
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
/// Inserts an empty sibling before an item whose leading text is at the caret.
std::optional<RichDocument> InsertListItemBefore(const RichDocument& document, const Leaf& leaf, EditChanges& edit);
/// Places the paragraph created by Return in a sibling list item and relocates its flattened caret when needed.
EditResult SplitListItem(EditResult result, EditChanges& edit);
RichDocument ExitContainer(const RichDocument& document, TextOffset caret, bool empty_only, EditChanges& edit);
EditResult ExitCodeBlock(const RichDocument& document, TextOffset caret, EditChanges& edit);
bool IsTableCommand(const EditorCommand& command);
CommandState TableStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeTable(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit, bool select_inserted_row = false);
bool IsImageCommand(const EditorCommand& command);
std::optional<std::size_t> SelectedImageIndex(const RichDocument& document, TextRange range);
std::optional<std::size_t> SelectedAtomicIndex(const RichDocument& document, TextRange range);
bool CanInsertAtomicBlock(const RichDocument& document, TextRange range);
RichDocument InsertAtomicBlock(const RichDocument& document, Selection& selection, BlockData data, EditChanges& edit);
RichDocument ReplaceSelectedCustomBlock(const RichDocument& document, const Selection& selection,
                                        const CustomBlockData& replacement, EditChanges& edit);
CommandState ImageStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeImage(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit);

} // namespace huxerui::richeditor::detail

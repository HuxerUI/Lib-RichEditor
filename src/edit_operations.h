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
  /// Rebuilds the document and records any newly assigned text block IDs.
  RichDocument Build(std::vector<Block> blocks);
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
RichDocument SplitListItem(const RichDocument& document, TextOffset new_paragraph, EditChanges& edit);
RichDocument ExitContainer(const RichDocument& document, TextOffset caret, bool empty_only, EditChanges& edit);
EditResult ExitCodeBlock(const RichDocument& document, TextOffset caret, EditChanges& edit);
bool IsTableCommand(const EditorCommand& command);
CommandState TableStatus(const RichDocument& document, TextRange range, const EditorCommand& command);
RichDocument ChangeTable(const RichDocument& document, Selection& selection, const EditorCommand& command,
                         EditChanges& edit);
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

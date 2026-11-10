#include "editor_session.h"

#include "code_highlighting.h"

#include <bit>
#include <stdexcept>

namespace huxerui::richeditor::detail {
namespace {

constexpr std::size_t history_limit = 128;

EditState Initial(RichDocument document) {
  const auto start = PositionAt(document, 0);
  return {std::move(document), {start, start}, {}};
}

TextSelection Offsets(const EditState& edit) {
  return {Absolute(edit.document, edit.selection.anchor), Absolute(edit.document, edit.selection.active),
          edit.selection.active.affinity};
}

void SetSelection(EditState& edit, TextSelection selection, bool reset_marks) {
  const Selection positions{PositionAt(edit.document, selection.anchor),
                            PositionAt(edit.document, selection.active, selection.affinity)};
  if (reset_marks && positions != edit.selection) {
    edit.pending.reset();
  }
  edit.selection = positions;
}

const EditState& Committed(const Draft& draft) {
  return draft.composition ? draft.composition->original : draft.edit;
}

void KeepChangedTextBlocks(std::vector<BlockId>& blocks, const RichDocument& before, const RichDocument& after) {
  const auto& old = DocumentAccess::Get(before);
  const auto& current = DocumentAccess::Get(after);
  std::erase_if(blocks, [&](BlockId id) {
    const auto* first = FindTextLeaf(old, id);
    const auto* second = FindTextLeaf(current, id);
    return first && second ? first->block == second->block : !first && !second;
  });
}

TextOffset AfterDeletion(TextOffset offset, TextRange deletion) {
  return offset <= deletion.start ? offset : std::max(deletion.start, offset - deletion.Length());
}

TextRange AfterDeletion(TextRange range, TextRange deletion) {
  return {AfterDeletion(range.start, deletion), AfterDeletion(range.end, deletion)};
}

TextRange DeleteFrom(EditState& edit, TextRange range, EditChanges& changes) {
  const auto selection = Offsets(edit);
  auto result = ReplaceText(edit.document, range, {}, {}, false, changes);
  const TextRange erased{range.start + result.leading_separator, range.end - result.trailing_separator};
  edit.document = std::move(result.document);
  SetSelection(edit, {AfterDeletion(selection.anchor, erased), AfterDeletion(selection.active, erased),
                      selection.affinity}, false);
  return erased;
}

void DeleteRange(Draft& draft, TextRange range) {
  if (range.IsCollapsed()) {
    return;
  }
  if (draft.composition) {
    auto& composition = *draft.composition;
    const auto active = composition.range;
    if (range.end <= active.start || range.start >= active.end) {
      const auto shift = range.end <= active.start ? 0 : composition.original_range.end - active.end;
      const TextRange original{range.start + shift, range.end + shift};
      const auto erased = DeleteFrom(composition.original, original, draft.changes);
      composition.original_range = AfterDeletion(composition.original_range, erased);
    } else if (range.start < active.start || range.end > active.end) {
      throw std::invalid_argument("Deletion crosses an IME composition boundary");
    }
  }
  const auto erased = DeleteFrom(draft.edit, range, draft.changes);
  if (draft.composition) {
    draft.composition->range = AfterDeletion(draft.composition->range, erased);
  }
}

TextOffset MoveScalars(const RichDocument& document, TextOffset offset, TextOffset count, bool forward) {
  const auto end = forward ? DocumentAccess::Get(document).length : 0;
  const auto step = forward ? 1 : -1;
  for (TextOffset i = 0; i < count && offset != end; ++i) {
    offset += step;
    try {
      (void)PositionAt(document, offset);
    } catch (const std::invalid_argument&) {
      // The adjacent UTF-16 unit is inside a surrogate pair.
      offset += step;
    }
  }
  return offset;
}

void DeleteSurrounding(Draft& draft, const TextInputCommand& command) {
  const auto& document = draft.edit.document;
  const auto selected = Offsets(draft.edit).Range();
  if (!CanEditTextRange(document, selected)) {
    throw std::invalid_argument("Surrounding deletion crosses a protected block boundary");
  }
  const auto length = DocumentAccess::Get(document).length;
  if (command.delete_before < 0 || command.delete_after < 0) {
    throw std::invalid_argument("Negative surrounding deletion");
  }
  TextOffset first;
  TextOffset last;
  switch (command.delete_unit) {
  case TextInputUnit::Utf16CodeUnit:
    first = selected.start - std::min(selected.start, command.delete_before);
    last = selected.end + std::min(length - selected.end, command.delete_after);
    break;
  case TextInputUnit::UnicodeCodePoint:
    first = MoveScalars(document, selected.start, command.delete_before, false);
    last = MoveScalars(document, selected.end, command.delete_after, true);
    break;
  default:
    throw std::invalid_argument("Unknown deletion unit");
  }
  DeleteRange(draft, {selected.end, last});
  DeleteRange(draft, {first, selected.start});
}

void BeginComposition(Draft& draft, TextRange range) {
  if (draft.composition || !range.IsValid()) {
    throw std::invalid_argument("Invalid composition start");
  }
  (void)PositionAt(draft.edit.document, range.start);
  (void)PositionAt(draft.edit.document, range.end);
  if (!CanEditTextRange(draft.edit.document, range)) {
    throw std::invalid_argument("Composition crosses a protected block boundary");
  }
  draft.composition = Composition{draft.edit, range, range};
}

void ApplyCommand(Draft& draft, const TextInputCommand& command) {
  const auto kind = command.kind;
  const bool replacement = kind == TextInputCommandKind::UpdateComposition || kind == TextInputCommandKind::CommitText;
  if ((kind != TextInputCommandKind::DeleteSurrounding &&
       (command.delete_before != 0 || command.delete_after != 0 || command.delete_unit != TextInputUnit::Utf16CodeUnit)) ||
      (kind != TextInputCommandKind::UpdateComposition && command.coordinate_space != TextInputCoordinateSpace::Text) ||
      (!replacement && (!command.text.empty() || (command.target && kind != TextInputCommandKind::BeginComposition) ||
                       (command.selection_after && kind != TextInputCommandKind::SetSelection)))) {
    throw std::invalid_argument("Unexpected text input command fields");
  }

  if (replacement) {
    const auto selected = Offsets(draft.edit).Range();
    const auto active = draft.composition ? draft.composition->range : selected;
    TextRange target = command.target.value_or(active);
    if (draft.composition && command.target) {
      if (kind != TextInputCommandKind::UpdateComposition ||
          command.coordinate_space != TextInputCoordinateSpace::Composition ||
          !target.IsValid() || target.end > active.Length()) {
        throw std::invalid_argument("Invalid composition-relative replacement");
      }
      target = {target.start + active.start, target.end + active.start};
    } else if (command.coordinate_space != TextInputCoordinateSpace::Text) {
      throw std::invalid_argument("Invalid text input coordinate space");
    }
    if (kind == TextInputCommandKind::UpdateComposition && !draft.composition) {
      BeginComposition(draft, target);
    }
    auto result = ReplaceText(draft.edit.document, target, command.text, draft.edit.pending, false, draft.changes);
    draft.edit.document = std::move(result.document);
    auto caret = result.caret;
    if (kind == TextInputCommandKind::UpdateComposition) {
      draft.composition->range.start += result.leading_separator;
      draft.composition->range.end += result.caret - target.end;
      caret = draft.composition->range.end;
    } else {
      draft.composition.reset();
    }
    auto selection = TextSelection{caret, caret};
    if (command.selection_after) {
      // Platforms describe the selection after a flat replacement. Account for a paragraph separator
      // created beside an image or removed along with it.
      const auto adjust = [&](TextOffset offset) {
        if (offset >= target.start) {
          offset += result.leading_separator;
        }
        return offset > result.caret ? offset + result.trailing_separator : offset;
      };
      selection = {adjust(command.selection_after->anchor), adjust(command.selection_after->active),
                   command.selection_after->affinity};
    }
    SetSelection(draft.edit, selection, false);
    return;
  }

  switch (kind) {
  case TextInputCommandKind::SetSelection:
    if (!command.selection_after) {
      throw std::invalid_argument("Missing input selection");
    }
    SetSelection(draft.edit, *command.selection_after, !draft.composition);
    return;
  case TextInputCommandKind::BeginComposition:
    if (!command.target) {
      throw std::invalid_argument("Missing composition range");
    }
    BeginComposition(draft, *command.target);
    return;
  case TextInputCommandKind::FinishComposition:
    draft.composition.reset();
    return;
  case TextInputCommandKind::CancelComposition:
    if (draft.composition) {
      draft.edit = std::move(draft.composition->original);
      draft.composition.reset();
    }
    return;
  case TextInputCommandKind::DeleteSurrounding:
    DeleteSurrounding(draft, command);
    return;
  default:
    throw std::invalid_argument("Unknown text input command");
  }
}

bool ValidLink(const SetLink& command) {
  if (!command.target) {
    return command.title.empty();
  }
  const auto invalid = [](std::string_view text) {
    return text.find('\0') != text.npos || text.find('\r') != text.npos;
  };
  try {
    const AttributedText target(*command.target);
    const AttributedText title(command.title);
    return !invalid(*command.target) && !invalid(command.title);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

} // namespace

EditorSession::EditorSession(RichDocument initial)
    : draft_{Initial(initial), {}, {DocumentAccess::Get(initial).max_id}} {}

RichDocument EditorSession::Document() const {
  (void)committed_.Get();
  return Committed(draft_).document;
}

void EditorSession::ReplaceDocument(RichDocument document) {
  Draft next{Initial(document), {}, {std::max(draft_.changes.last_id, DocumentAccess::Get(document).max_id)}};
  undo_.clear();
  redo_.clear();
  block_changes_.clear();
  ++generation_;
  if (!Publish(std::move(next), false)) {
    changed_ = ++revision_;
  }
}

bool EditorSession::TextBlockChangesSince(std::uint64_t revision, std::vector<BlockId>& blocks) const {
  const auto current = CommittedRevision();
  if (revision > current || current - revision > block_changes_.size()) {
    return false;
  }
  const auto first = block_changes_.size() - (current - revision);
  for (auto index = first; index < block_changes_.size(); ++index) {
    const auto& change = block_changes_[index];
    blocks.insert(blocks.end(), change.begin(), change.end());
  }
  std::ranges::sort(blocks);
  blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
  return true;
}

TextSelection EditorSession::SelectionOffsets() const { return Offsets(draft_.edit); }

std::optional<TextRange> EditorSession::CompositionRange() const {
  return draft_.composition ? std::optional(draft_.composition->range) : std::nullopt;
}

bool EditorSession::Publish(Draft next, bool remember, bool reset_block_changes) {
  const auto& before = Committed(draft_);
  const auto& after = Committed(next);
  const bool snapshot_changed = !DocumentAccess::Same(before.document, after.document);
  const bool committed_changed = before.document != after.document;
  const bool visible_changed = !DocumentAccess::Same(draft_.edit.document, next.edit.document);
  const bool selection_changed = draft_.edit.selection != next.edit.selection || draft_.edit.pending != next.edit.pending;
  const auto composition = next.composition ? std::optional(next.composition->range) : std::nullopt;
  const bool changed = visible_changed || selection_changed || CompositionRange() != composition || committed_changed;
  const bool text_changed = visible_changed && !SameText(draft_.edit.document, next.edit.document);
  auto& touched = next.changes.touched;
  std::ranges::sort(touched);
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  std::vector<BlockId> changed_blocks;
  if (snapshot_changed) {
    changed_blocks = next.composition ? touched : std::move(touched);
    KeepChangedTextBlocks(changed_blocks, before.document, after.document);
  }
  if (next.composition) {
    // Keep only outstanding changes from the current committed baseline to the IME draft.
    KeepChangedTextBlocks(touched, after.document, next.edit.document);
  } else {
    touched.clear();
  }
  if (remember && committed_changed) {
    undo_.push_back({before, after, changed_blocks});
    if (undo_.size() > history_limit) {
      undo_.pop_front();
    }
    redo_.clear();
  }
  if (text_changed) {
    ++content_revision_;
  }
  draft_ = std::move(next);
  if (snapshot_changed) {
    if (reset_block_changes) {
      block_changes_.clear();
    } else {
      block_changes_.push_back(std::move(changed_blocks));
      if (block_changes_.size() > history_limit) {
        block_changes_.pop_front();
      }
    }
    committed_ = CommittedRevision() + 1;
  }
  if (changed) {
    changed_ = ++revision_;
  }
  return changed;
}

bool EditorSession::Apply(std::span<const TextInputCommand> commands) {
  if (commands.empty()) {
    return false;
  }
  auto next = draft_;
  try {
    for (const auto& command : commands) {
      ApplyCommand(next, command);
    }
    Publish(std::move(next), true);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool EditorSession::Select(TextSelection selection) {
  auto next = draft_;
  next.composition.reset();
  try {
    SetSelection(next.edit, selection, true);
    return Publish(std::move(next), true);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool EditorSession::BreakLine(bool hard_break) {
  auto next = draft_;
  next.composition.reset();
  const auto selected = Offsets(next.edit).Range();
  try {
    if (!hard_break && selected.IsCollapsed()) {
      auto exited = ExitContainer(next.edit.document, selected.start, true, next.changes);
      if (!DocumentAccess::Same(next.edit.document, exited)) {
        next.edit.document = std::move(exited);
        return Publish(std::move(next), true);
      }
    }
    const auto& leaf = DocumentAccess::Get(next.edit.document).leaves[LeafIndex(next.edit.document, selected.start)];
    const bool split_item = !hard_break && leaf.block.Data().kind != BlockKind::CodeBlock;
    auto result = selected.IsCollapsed() && IsAtomicBlock(leaf.block.Data().kind)
        ? InsertParagraphAtAtomicBlock(next.edit.document, selected.start, next.changes)
        : ReplaceText(next.edit.document, selected, "\n", next.edit.pending, hard_break, next.changes);
    next.edit.document = split_item ? SplitListItem(result.document, result.caret, next.changes) : std::move(result.document);
    SetSelection(next.edit, {result.caret, result.caret}, false);
    return Publish(std::move(next), true);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool EditorSession::ExitAtCaret() {
  if (draft_.composition) {
    return false;
  }
  const auto range = SelectionOffsets().Range();
  if (!range.IsCollapsed()) {
    return false;
  }
  auto next = draft_;
  try {
    next.edit.document = ExitContainer(next.edit.document, range.start, false, next.changes);
    return Publish(std::move(next), true);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool EditorSession::ExitCode() {
  const auto selection = SelectionOffsets();
  if (!selection.IsCollapsed() || draft_.composition) {
    return false;
  }
  auto next = draft_;
  auto result = ExitCodeBlock(next.edit.document, selection.active, next.changes);
  if (DocumentAccess::Same(result.document, next.edit.document)) {
    return false;
  }
  next.edit.document = std::move(result.document);
  SetSelection(next.edit, {result.caret, result.caret}, true);
  return Publish(std::move(next), true);
}

void EditorSession::FinishComposition() {
  if (draft_.composition) {
    auto next = draft_;
    next.composition.reset();
    Publish(std::move(next), true);
  }
}

bool EditorSession::SetCodeLanguage(BlockId block, std::string_view language) {
  const auto& storage = DocumentAccess::Get(VisibleDocument());
  const auto found = storage.indexes.find(block);
  if (found == storage.indexes.end()) {
    return false;
  }
  const auto& leaf = storage.leaves[found->second];
  const auto& data = leaf.block.Data();
  if (data.kind != BlockKind::CodeBlock || CodeFenceWord(data.fence_info) == language) {
    return false;
  }

  auto info = data.fence_info;
  const auto word = CodeFenceWord(info);
  if (word.empty()) {
    info = language;
  } else {
    info.replace(static_cast<std::size_t>(word.data() - info.data()), word.size(), language);
  }
  const auto offset = leaf.start;
  FinishComposition();
  auto next = draft_;
  next.edit.document = Format(next.edit.document, {offset, offset}, SetBlockKind{BlockKind::CodeBlock, 0, std::move(info)},
                              false, next.changes);
  return Publish(std::move(next), true);
}

const CustomBlockData* EditorSession::SelectedCustomBlock() const {
  if (const auto index = SelectedAtomicBlock()) {
    const auto& data = DocumentAccess::Get(VisibleDocument()).leaves[*index].block.Data().custom;
    return data ? &*data : nullptr;
  }
  return nullptr;
}

CommandState EditorSession::CommandStatus(const EditorCommand& command) const {
  const auto range = SelectionOffsets().Range();
  const auto& document = draft_.edit.document;
  if (const auto* toggle = std::get_if<ToggleMark>(&command)) {
    return std::has_single_bit(static_cast<unsigned>(toggle->mark)) && Has(all_marks, toggle->mark)
        ? MarkStatus(document, range, toggle->mark, draft_.edit.pending) : CommandState{};
  }
  if (std::holds_alternative<ClearFormatting>(command)) {
    return {MarkStatus(document, range, Mark::Bold, draft_.edit.pending).enabled};
  }
  if (const auto* link = std::get_if<SetLink>(&command)) {
    return {CanSetLink(document, range) && ValidLink(*link)};
  }
  if (const auto* block = std::get_if<SetBlockKind>(&command)) {
    return BlockKindStatus(document, range, *block);
  }
  if (std::holds_alternative<SelectAll>(command)) {
    const auto size = DocumentAccess::Get(document).length;
    return {size != 0 && range != TextRange{0, size}};
  }
  if (std::holds_alternative<Undo>(command)) {
    return {draft_.composition.has_value() || !undo_.empty()};
  }
  if (std::holds_alternative<Redo>(command)) {
    return {!draft_.composition && !redo_.empty()};
  }
  if (IsTableCommand(command)) {
    auto state = TableStatus(document, range, command);
    state.enabled = state.enabled && !draft_.composition;
    return state;
  }
  if (IsImageCommand(command)) {
    return draft_.composition ? CommandState{} : ImageStatus(document, range, command);
  }
  if (const auto* insert = std::get_if<InsertCustomBlock>(&command)) {
    return {!draft_.composition && CanInsertAtomicBlock(document, range) && ValidCustomData(insert->block)};
  }
  if (const auto* replace = std::get_if<ReplaceCustomBlock>(&command)) {
    const auto* selected = SelectedCustomBlock();
    return {selected && selected->type == replace->block.type && *selected != replace->block &&
            (!replace->expected || *selected == *replace->expected) && ValidCustomData(replace->block)};
  }
  if (std::holds_alternative<SetListStart>(command)) {
    return {OrderedListStart().has_value()};
  }
  return ContainerStatus(document, range, command);
}

bool EditorSession::Execute(const EditorCommand& command) {
  const auto status = CommandStatus(command);
  if (!status.enabled) {
    return false;
  }
  if (std::holds_alternative<Undo>(command) && draft_.composition) {
    const TextInputCommand cancel{.kind = TextInputCommandKind::CancelComposition};
    return Apply(std::span(&cancel, 1));
  }
  if (std::holds_alternative<Undo>(command) || std::holds_alternative<Redo>(command)) {
    const bool undoing = std::holds_alternative<Undo>(command);
    auto& source = undoing ? undo_ : redo_;
    auto& destination = undoing ? redo_ : undo_;
    auto next = draft_;
    const auto& entry = source.back();
    // An equivalent edit can replace block IDs without creating a history entry.
    const auto& expected = undoing ? entry.after.document : entry.before.document;
    const bool reset_block_changes = !DocumentAccess::Same(next.edit.document, expected);
    next.edit = undoing ? entry.before : entry.after;
    next.changes.touched = entry.changed_blocks;
    destination.push_back(std::move(source.back()));
    source.pop_back();
    return Publish(std::move(next), false, reset_block_changes);
  }

  FinishComposition();
  auto next = draft_;
  const auto selection = Offsets(next.edit);
  const auto range = selection.Range();
  try {
    if (std::holds_alternative<SelectAll>(command)) {
      SetSelection(next.edit, {0, DocumentAccess::Get(next.edit.document).length}, true);
    } else if (const auto* mark = std::get_if<ToggleMark>(&command); mark && range.IsCollapsed()) {
      const auto value = next.edit.pending.value_or(MarksAt(next.edit.document, range.start));
      next.edit.pending = status.checked == CheckState::On ? Without(value, mark->mark) : value | mark->mark;
    } else if (std::holds_alternative<ClearFormatting>(command) && range.IsCollapsed()) {
      next.edit.pending = Mark::None;
    } else if (std::holds_alternative<ToggleMark>(command) || std::holds_alternative<SetLink>(command) ||
               std::holds_alternative<ClearFormatting>(command) || std::holds_alternative<SetBlockKind>(command)) {
      next.edit.document = Format(next.edit.document, range, command, status.checked == CheckState::On, next.changes);
      next.edit.pending.reset();
    } else if (IsTableCommand(command)) {
      const auto before = next.edit.selection;
      next.edit.document = ChangeTable(next.edit.document, next.edit.selection, command, next.changes);
      if (next.edit.selection != before) {
        next.edit.pending.reset();
      }
    } else if (IsImageCommand(command)) {
      next.edit.document = ChangeImage(next.edit.document, next.edit.selection, command, next.changes);
      next.edit.pending.reset();
    } else if (const auto* insert = std::get_if<InsertCustomBlock>(&command)) {
      next.edit.document = InsertAtomicBlock(next.edit.document, next.edit.selection,
                                            {.kind = BlockKind::Custom, .custom = insert->block}, next.changes);
      next.edit.pending.reset();
    } else if (const auto* replace = std::get_if<ReplaceCustomBlock>(&command)) {
      next.edit.document = ReplaceSelectedCustomBlock(next.edit.document, next.edit.selection, replace->block,
                                                      next.changes);
    } else {
      next.edit.document = ChangeContainer(next.edit.document, range, command, next.changes);
    }
    return Publish(std::move(next), true);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

} // namespace huxerui::richeditor::detail

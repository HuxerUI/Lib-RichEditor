#include <huxerui/richeditor.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/clipboard.h>
#include <huxerui/gesture.h>
#include <huxerui/lifecycle.h>
#include <huxerui/paint.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/theme.h>

#include "code_highlighting.h"
#include "decoration_analysis.h"
#include "editor_layout.h"
#include "editor_session.h"
#include "editor_view_internal.h"
#include "inline_presentation.h"

namespace huxerui::richeditor {
namespace detail {
namespace {

constexpr std::uint64_t inline_semantic_id = std::uint64_t{1} << 63;

enum class WordUnit { Space, Word, Single };

WordUnit Classify(std::string_view grapheme) {
  if (grapheme.empty()) {
    return WordUnit::Single;
  }
  const auto first = static_cast<unsigned char>(grapheme.front());
  if (first < 128) {
    if (first == ' ' || first == '\t' || first == '\n') {
      return WordUnit::Space;
    }
    return ((first >= '0' && first <= '9') || (first >= 'a' && first <= 'z') ||
            (first >= 'A' && first <= 'Z') || first == '_') ? WordUnit::Word : WordUnit::Single;
  }
  // Use one caret cluster for Han, kana, emoji and other scripts without space-delimited words.
  return first >= 0xE0 ? WordUnit::Single : WordUnit::Word;
}

Point ResizeCorner(Rect bounds) { return {bounds.x + bounds.width, bounds.y + bounds.height}; }

class EditorClient final : public TextInputClient, public TextSelectionClient {
public:
  EditorClient(std::shared_ptr<EditorSession> editor, ScrollController scroll)
      : editor_(std::move(editor)), scroll_(std::move(scroll)), generation_(editor_->Generation()) {}

  EditorSession& Editor() const { return *editor_; }
  bool Current() const { return generation_ == editor_->Generation(); }
  bool HasGeometry() const { return Current() && layout_.IsCurrent(editor_->VisibleDocument()); }
  EditorLayout& Layout() { return layout_; }
  void GeometryChanged() { ++geometry_revision_; }
  void SetViewportOrigin(Point origin) {
    if (origin != viewport_origin_) {
      viewport_origin_ = origin;
      GeometryChanged();
    }
  }
  void Detach() { layout_ = {}; }
  void SetBlockControlActive(bool active) {
    if (active) {
      block_control_ = BlockControl{editor_->SelectionOffsets(), editor_->ContentRevision()};
    } else {
      block_control_.reset();
    }
  }

  bool CanRevealCaret(TextSelection selection) {
    if (block_control_ &&
        (selection != block_control_->selection || editor_->ContentRevision() != block_control_->content_revision)) {
      block_control_.reset();
    }
    return !block_control_;
  }

  TextInputConfiguration Configuration() const override {
    return {.multiline = true,
            .keyboard_policy = block_control_ || editor_->SelectedAtomicBlock()
                ? SoftwareKeyboardPolicy::Manual : SoftwareKeyboardPolicy::Automatic};
  }

  TextInputState State() const override {
    return {session_, editor_->Revision() + geometry_revision_, editor_->ContentRevision(), editor_->SelectionOffsets(),
            editor_->CompositionRange()};
  }

  TextInputState BeginTextInput(TextInputSessionId id) override {
    session_ = id;
    return State();
  }

  void EndTextInput(TextInputSessionId id, TextInputEndReason) override {
    if (id == session_) {
      if (Current()) {
        editor_->FinishComposition();
      }
      session_ = 0;
    }
  }

  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override {
    if (!Matches(batch.session_id)) {
      return {TextInputResultCode::SessionMismatch};
    }
    preferred_x_.reset();
    return {editor_->Apply(batch.commands) ? TextInputResultCode::Ok : TextInputResultCode::Rejected};
  }

  TextInputContext QueryTextInputContext(TextInputSessionId id, TextOffset start, TextOffset length) const override {
    if (!Matches(id)) {
      return {TextInputResultCode::SessionMismatch, id};
    }
    if (start < 0 || length < 0) {
      return {TextInputResultCode::Rejected, id};
    }
    const auto& document = editor_->VisibleDocument();
    const auto total = DocumentAccess::Get(document).index.Length();
#if defined(__EMSCRIPTEN__)
    // The Web input element synchronizes its complete value using absolute offsets.
    if (start == 0 && length == 0) {
      length = total;
    }
#endif
    start = std::min(start, total);
    auto end = start + std::min(length, total - start);
    try {
      (void)PositionAt(document, start);
    } catch (const std::invalid_argument&) {
      --start;
    }
    try {
      (void)PositionAt(document, end);
    } catch (const std::invalid_argument&) {
      ++end;
    }
    return {TextInputResultCode::Ok, id, start, total, TextSlice(document, {start, end}),
            editor_->SelectionOffsets(), editor_->CompositionRange()};
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId id, TextRange range) const override {
    if (!Matches(id)) {
      return {TextInputResultCode::SessionMismatch, id, {}, {}};
    }
    if (!HasGeometry() || !range.IsValid()) {
      return {TextInputResultCode::Rejected, id, {}, {}};
    }
    try {
      const auto& document = editor_->VisibleDocument();
      (void)PositionAt(document, range.start);
      (void)PositionAt(document, range.end);
      const auto selection = editor_->SelectionOffsets();
      const auto affinity = selection.active == range.end ? selection.affinity : TextAffinity::Downstream;
      auto fragments = layout_.Ranges(document, range);
      for (auto& rect : fragments) {
        rect = ToNode(rect);
      }
      return {TextInputResultCode::Ok, id, ToNode(layout_.Caret(document, {range.end, affinity})),
              std::move(fragments)};
    } catch (const std::invalid_argument&) {
      return {TextInputResultCode::Rejected, id, {}, {}};
    }
  }

  TextInputPositionResult QueryTextInputPosition(TextInputSessionId id, Point point) const override {
    if (!Matches(id)) {
      return {TextInputResultCode::SessionMismatch, id};
    }
    if (!HasGeometry() || !std::isfinite(point.x) || !std::isfinite(point.y)) {
      return {TextInputResultCode::Rejected, id};
    }
    return {TextInputResultCode::Ok, id, Hit(point)};
  }

  TextPosition Hit(Point point) const {
    return layout_.Hit(editor_->VisibleDocument(), ContentPoint(point));
  }

  Point ContentPoint(Point point) const { return point - ContentOrigin(); }
  Rect ToNode(Rect rect) const { return AtOrigin(rect, ContentOrigin()); }

  bool HitsAtomicBlock(Point point) const {
    const auto index = LeafIndex(editor_->VisibleDocument(), Hit(point).offset);
    const auto* bounds = layout_.Rows()[index].AtomicBounds();
    return bounds && bounds->Contains(ContentPoint(point));
  }

  void Select(Point point, bool extend) {
    preferred_x_.reset();
    const auto caret = Hit(point);
    const auto& document = editor_->VisibleDocument();
    const auto index = LeafIndex(document, caret.offset);
    const auto& row = layout_.Rows()[index];
    if (const auto* bounds = row.AtomicBounds(); !extend && bounds && bounds->Contains(ContentPoint(point))) {
      const auto start = DocumentAccess::Get(document).index.At(index).start;
      editor_->Select({start, start + 1});
      return;
    }
    SetCaret(caret, extend);
  }

  std::optional<TextOffset> InlineAt(Point point) const {
    const auto& document = editor_->VisibleDocument();
    const auto ordinal = LeafIndex(document, Hit(point).offset);
    const auto& row = layout_.Rows()[ordinal];
    const auto content = ContentPoint(point);
    if (row.cell && !layout_.VisibleCell(row).Contains(content)) {
      return std::nullopt;
    }
    const auto origin = layout_.Origin(row);
    const auto found = std::ranges::find_if(row.inline_objects, [&](const EditorLayout::InlineBox& box) {
      return AtOrigin(box.bounds, origin).Contains(content);
    });
    if (found == row.inline_objects.end()) {
      return std::nullopt;
    }
    return DocumentAccess::Get(document).index.At(ordinal).start + found->offset;
  }

  bool CanPerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) const override {
    if (!Current()) {
      return false;
    }
    const auto range = editor_->SelectionOffsets().Range();
    if (action == TextEditingAction::SelectAll) {
      return editor_->CommandStatus(SelectAll{}).enabled;
    }
    if (!clipboard) {
      return false;
    }
    if (action == TextEditingAction::Copy) {
      return !range.IsCollapsed();
    }
    if (!CanEditTextRange(editor_->VisibleDocument(), range)) {
      return false;
    }
    return action == TextEditingAction::Paste || !range.IsCollapsed();
  }

  bool PerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) override {
    if (!CanPerformTextEditingAction(action, clipboard)) {
      return false;
    }
    if (action == TextEditingAction::SelectAll) {
      return editor_->Execute(SelectAll{});
    }
    const auto selected = editor_->SelectionOffsets().Range();
    std::string text;
    if (action == TextEditingAction::Paste) {
      auto source = clipboard->ReadText();
      if (!source) {
        return false;
      }
      text = std::move(*source);
    } else {
      if (!clipboard->WriteText(PlainTextSlice(editor_->VisibleDocument(), selected))) {
        return false;
      }
      if (action == TextEditingAction::Copy) {
        return true;
      }
    }
    editor_->FinishComposition();
    const TextInputCommand command{.kind = TextInputCommandKind::CommitText, .target = selected, .text = std::move(text)};
    return editor_->Apply(std::span(&command, 1));
  }

  bool ClearSelection() override {
    // Dismiss the window's touch overlay while keeping the logical range available to an external toolbar.
    return false;
  }

  bool SelectWord(Point point) override {
    if (!HasGeometry()) {
      return false;
    }
    const auto hit = Hit(point);
    const auto object = InlineAt(point);
    const auto range = object ? TextRange{*object, *object + 1} : WordAt(hit.offset);
    editor_->Select({range.start, range.end, hit.affinity});
    return true;
  }

  bool ExtendSelection(Point point, bool start_handle) override {
    if (!HasGeometry()) {
      return false;
    }
    const auto range = editor_->SelectionOffsets().Range();
    if (range.IsCollapsed()) {
      return false;
    }
    const auto metrics = scroll_.Metrics();
    const auto y = point.y - viewport_origin_.y;
    if (y < 8.0F || y > metrics.viewport_extent - 8.0F) {
      scroll_.ScrollBy(y < 8.0F ? -14.0F : 14.0F);
    }
    const auto caret = Hit(point);
    editor_->Select(start_handle ? TextSelection{range.end, std::min(caret.offset, range.end), caret.affinity}
                                 : TextSelection{range.start, std::max(caret.offset, range.start), caret.affinity});
    return true;
  }

  std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
    if (!Current()) {
      return std::nullopt;
    }
    if (!HasGeometry() || editor_->SelectedAtomicBlock()) {
      return TextSelectionGeometry{};
    }
    const auto& document = editor_->VisibleDocument();
    const auto selection = editor_->SelectionOffsets();
    const auto range = selection.Range();
    const auto metrics = scroll_.Metrics();
    const auto visible = [&](Rect rect) -> std::optional<Rect> {
      if (rect.y + rect.height >= metrics.offset && rect.y <= metrics.offset + metrics.viewport_extent) {
        return ToNode(rect);
      }
      return std::nullopt;
    };
    const auto caret = [&](TextPosition position) -> std::optional<Rect> {
      const auto rect = layout_.Caret(document, position);
      const auto& row = layout_.Rows()[LeafIndex(document, position.offset)];
      if (row.cell && !rect.Intersects(layout_.VisibleCell(row))) {
        return std::nullopt;
      }
      return visible(rect);
    };
    const auto start = caret({range.start, selection.active == range.start
        ? selection.affinity : TextAffinity::Downstream});
    const auto end = caret({range.end, selection.active == range.end
        ? selection.affinity : TextAffinity::Upstream});
    auto anchor = start ? start : end;
    if (!anchor) {
      for (const auto& rect : layout_.Ranges(document, range)) {
        if ((anchor = visible(rect))) {
          break;
        }
      }
    }
    return TextSelectionGeometry{start, end, anchor};
  }

  Color SelectionHandleColor() const noexcept override { return layout_.AccentColor(); }

  TextInputKeyResult HandleTextKey(const KeyEvent& event) override;

private:
  // Language changes and popup focus restoration leave this editing position intact.
  struct BlockControl {
    TextSelection selection;
    std::uint64_t content_revision;
  };

  bool Matches(TextInputSessionId id) const { return Current() && id != 0 && id == session_; }
  Point ContentOrigin() const { return {viewport_origin_.x, viewport_origin_.y - scroll_.Offset()}; }

  void SetCaret(TextPosition position, bool extend) {
    const auto old = editor_->SelectionOffsets();
    if (editor_->Select({extend ? old.anchor : position.offset, position.offset, position.affinity}) &&
        HasGeometry() && extend && position.offset < old.anchor) {
      const auto caret = layout_.Caret(editor_->VisibleDocument(), position);
      const auto metrics = scroll_.Metrics();
      if (caret.y < metrics.offset + 6.0F) {
        scroll_.ScrollTo(caret.y - 6.0F);
      }
    }
  }

  TextOffset Adjacent(TextOffset offset, bool forward) const;
  TextRange WordAt(TextOffset offset) const;
  TextPosition Vertical(TextPosition position, bool down, bool page);
  TextInputKeyResult EditKey(const KeyEvent& event, TextSelection selection, bool word);

  std::shared_ptr<EditorSession> editor_;
  ScrollController scroll_;
  std::uint64_t generation_;
  std::uint64_t geometry_revision_ = 0;
  TextInputSessionId session_ = 0;
  std::optional<float> preferred_x_;
  Point viewport_origin_{};
  std::optional<BlockControl> block_control_;
  EditorLayout layout_;
};

TextOffset EditorClient::Adjacent(TextOffset offset, bool forward) const {
  const auto& document = editor_->VisibleDocument();
  const auto index = LeafIndex(document, offset);
  const auto& storage = DocumentAccess::Get(document);
  const auto& leaf = storage.index.At(index);
  const auto local = offset - leaf.start;
  const auto& row = layout_.Rows()[index];
  if (forward) {
    return local == DocumentAccess::Body(leaf.block).Length()
        ? std::min(storage.index.Length(), offset + 1) : leaf.start + row.AdjacentCaretOffset(local, true);
  }
  return local == 0 ? std::max<TextOffset>(0, offset - 1) : leaf.start + row.AdjacentCaretOffset(local, false);
}

TextRange EditorClient::WordAt(TextOffset offset) const {
  const auto& document = editor_->VisibleDocument();
  const auto index = LeafIndex(document, offset);
  const auto& leaf = DocumentAccess::Get(document).index.At(index);
  const auto& body = DocumentAccess::Body(leaf.block);
  const auto& row = layout_.Rows()[index];
  if (body.Length() == 0) {
    return {offset, offset};
  }
  const auto local = offset - leaf.start;
  auto start = local == body.Length() ? row.AdjacentCaretOffset(local, false) : local;
  auto end = row.AdjacentCaretOffset(start, true);
  const auto kind = Classify(body.TextInRange({start, end}));
  if (kind != WordUnit::Single) {
    while (start > 0) {
      const auto previous = row.AdjacentCaretOffset(start, false);
      if (Classify(body.TextInRange({previous, start})) != kind) {
        break;
      }
      start = previous;
    }
    while (end < body.Length()) {
      const auto next = row.AdjacentCaretOffset(end, true);
      if (Classify(body.TextInRange({end, next})) != kind) {
        break;
      }
      end = next;
    }
  }
  return {leaf.start + start, leaf.start + end};
}

TextPosition EditorClient::Vertical(TextPosition position, bool down, bool page) {
  const auto& document = editor_->VisibleDocument();
  const auto& leaves = DocumentAccess::Get(document).index;
  const auto& rows = layout_.Rows();
  auto index = LeafIndex(document, position.offset);
  auto line = rows[index].LineIndex({position.offset - leaves.At(index).start, position.affinity});
  preferred_x_ = preferred_x_.value_or(layout_.Caret(document, position).x);
  if (page) {
    const auto caret = layout_.Caret(document, position);
    return layout_.Hit(document,
                        {*preferred_x_, caret.y + caret.height / 2 + (down ? 1 : -1) * scroll_.ViewportExtent()});
  }
  if (down && line + 1 < rows[index].layout.Lines().size()) {
    ++line;
  } else if (!down && line != 0) {
    --line;
  } else {
    const auto next = layout_.AdjacentRow(index, down, *preferred_x_);
    if (next == index) {
      return rows[index].image ? TextPosition{leaves.At(index).start + down} : position;
    }
    index = next;
    if (rows[index].image) {
      return {leaves.At(index).start + !down};
    }
    line = down ? 0 : rows[index].layout.Lines().size() - 1;
  }
  const auto bounds = rows[index].layout.Lines()[line].bounds;
  return layout_.Hit(document, index, {*preferred_x_, rows[index].origin.y + bounds.y + bounds.height / 2});
}

TextInputKeyResult EditorClient::EditKey(const KeyEvent& event, TextSelection selection, bool word) {
  constexpr auto handled = TextInputKeyResult::Handled;
  constexpr auto unhandled = TextInputKeyResult::Unhandled;
  TextInputCommand command{.kind = TextInputCommandKind::CommitText};
  if (event.key == Key::Escape && editor_->CompositionRange()) {
    command.kind = TextInputCommandKind::CancelComposition;
  } else if (event.key == Key::Enter && !event.modifiers.control && !event.modifiers.meta && !event.modifiers.alt) {
    editor_->BreakLine(event.modifiers.shift);
    return handled;
  } else if (event.key == Key::Backspace || event.key == Key::Delete) {
    if (editor_->CompositionRange()) {
      return unhandled;
    }
    if (selection.IsCollapsed()) {
      if (event.key == Key::Backspace && !word && editor_->ExitAtCaret()) {
        return handled;
      }
      if (!HasGeometry()) {
        return unhandled;
      }
      const bool forward = event.key == Key::Delete;
      auto other = Adjacent(selection.active, forward);
      if (word) {
        const auto range = WordAt(forward ? selection.active : other);
        other = forward ? std::max(other, range.end) : std::min(other, range.start);
      }
      command.target = TextRange{std::min(selection.active, other), std::max(selection.active, other)};
    }
  } else if (!event.text.empty() && !event.modifiers.control && !event.modifiers.meta) {
    command.text = event.text;
  } else {
    return unhandled;
  }
  editor_->Apply(std::span(&command, 1));
  return handled;
}

TextInputKeyResult EditorClient::HandleTextKey(const KeyEvent& event) {
  constexpr auto handled = TextInputKeyResult::Handled;
  constexpr auto unhandled = TextInputKeyResult::Unhandled;
  if (!Current() || event.type != KeyEventType::Down) {
    return unhandled;
  }
  const auto mods = event.modifiers;
  if ((mods.control || mods.meta) && !mods.alt) {
    switch (event.key) {
    case Key::Z:
      mods.shift ? editor_->Execute(Redo{}) : editor_->Execute(Undo{});
      return handled;
    case Key::Y:
      if (mods.control && !mods.shift) {
        editor_->Execute(Redo{});
        return handled;
      }
      break;
    case Key::Enter:
      if (!mods.shift && editor_->SelectedInlineObject()) {
        return unhandled;
      }
      if (!mods.shift && (editor_->Execute(ExitTable{}) || editor_->ExitCode())) {
        return handled;
      }
      break;
    case Key::B:
    case Key::I:
    case Key::U:
      if (!mods.shift) {
        if (!event.repeat) {
          editor_->Execute(ToggleMark{event.key == Key::B ? Mark::Bold : event.key == Key::I ? Mark::Italic
                                                                                           : Mark::Underline});
        }
        return handled;
      }
      break;
    default:
      break;
    }
  }

  const auto selection = editor_->SelectionOffsets();
  const auto range = selection.Range();
  TextPosition caret{selection.active, selection.affinity};
  const bool word = mods.control || mods.alt;
  if (!HasGeometry()) {
    return EditKey(event, selection, word);
  }
  const auto& document = editor_->VisibleDocument();
  const auto index = LeafIndex(document, caret.offset);
  const auto& leaf = DocumentAccess::Get(document).index.At(index);
  const auto& row = layout_.Rows()[index];
  switch (event.key) {
  case Key::Tab: {
    if (mods.control || mods.meta || mods.alt || editor_->CompositionRange()) {
      return unhandled;
    }
    if (!row.cell) {
      const bool changed = mods.shift ? editor_->Execute(OutdentList{}) : editor_->Execute(IndentList{});
      if (changed) {
        preferred_x_.reset();
      }
      return changed ? handled : unhandled;
    }
    const auto& table = layout_.Tables()[row.cell->table];
    if (mods.shift ? index == table.first : index + 1 == table.first + table.rows * table.columns) {
      return unhandled;
    }
    const auto next = mods.shift ? index - 1 : index + 1;
    preferred_x_.reset();
    SetCaret({DocumentAccess::Get(document).index.At(next).start}, false);
    return handled;
  }
  case Key::ArrowLeft:
  case Key::ArrowRight: {
    preferred_x_.reset();
    const bool forward = event.key == Key::ArrowRight;
    if (!mods.shift && !range.IsCollapsed()) {
      caret.offset = forward ? range.end : range.start;
    } else if (mods.meta) {
      const auto line = row.LineRange(row.LineIndex({caret.offset - leaf.start, caret.affinity}));
      caret.offset = leaf.start + (forward ? line.end : line.start);
    } else {
      auto next = Adjacent(caret.offset, forward);
      if (word) {
        const auto boundary = WordAt(forward ? caret.offset : next);
        next = forward ? std::max(next, boundary.end) : std::min(next, boundary.start);
      }
      caret.offset = next;
    }
    caret.affinity = forward ? TextAffinity::Downstream : TextAffinity::Upstream;
    break;
  }
  case Key::Home:
  case Key::End: {
    preferred_x_.reset();
    const bool end = event.key == Key::End;
    const auto line = row.LineRange(row.LineIndex({caret.offset - leaf.start, caret.affinity}));
    caret = mods.meta || mods.control
        ? TextPosition{end ? DocumentAccess::Get(document).index.Length() : 0}
        : TextPosition{leaf.start + (end ? line.end : line.start),
                       end ? TextAffinity::Upstream : TextAffinity::Downstream};
    break;
  }
  case Key::ArrowUp:
  case Key::ArrowDown:
    if (mods.meta) {
      caret = {event.key == Key::ArrowDown ? DocumentAccess::Get(document).index.Length() : 0};
    } else {
      caret = Vertical(caret, event.key == Key::ArrowDown, false);
    }
    break;
  case Key::PageUp:
  case Key::PageDown:
    caret = Vertical(caret, event.key == Key::PageDown, true);
    break;
  default:
    preferred_x_.reset();
    return EditKey(event, selection, word);
  }
  SetCaret(caret, mods.shift);
  return handled;
}

struct Surface;
class EditorExtension final : public NodeExtension {
public:
  EditorExtension(ViewNode& node, const Surface& surface) : node_(node) { Update(node, surface); }
  ~EditorExtension() override { client_->Detach(); }

  void Update(ViewNode&, const Surface& surface);
  std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override { return client_; }
  TextSelectionClient* GetTextSelectionClient() noexcept override { return client_.get(); }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer& measurer) override {
    const auto bounds = node.ContentBounds();
    const auto width = bounds.width;
    if (!client_->Current()) {
      ResetClient();
    }
    const float content_width = width > 0.0F ? std::max(1.0F, width - viewport_->style.content_padding.Horizontal())
                                             : 0.0F;
    if (content_width != viewport_->content_width.Get() || !node.IsEnabled()) {
      resize_.reset();
    }
    viewport_->content_width = content_width;
    if (width <= 0) {
      return PaintInvalidation::None;
    }
    client_->SetViewportOrigin({bounds.x, bounds.y});
    const auto revision = editor_->Revision();
    const bool edited = revision != observed_revision_;
    if (edited) {
      if (resize_ && resize_->revision != revision) {
        resize_.reset();
      }
      observed_revision_ = revision;
      blink_start_.reset();
      caret_visible_ = true;
      InvalidateSemantics();
      if (node.IsFocused() && editor_->SelectedAtomicBlock()) {
        HideKeyboard();
      }
    }
    auto& layout = client_->Layout();
    const bool appearance_changed = layout.Theme() != viewport_->theme || layout.Style() != viewport_->style;
    const auto inline_revision = viewport_->inline_revision.Get();
    const auto image_revision = viewport_->image_revision.Get();
    const auto previous_scroll = viewport_->scroll.Metrics().offset;
    std::optional<std::pair<std::size_t, float>> anchor;
    if (!edited && !follow_caret_ && layout.IsCurrent(editor_->VisibleDocument()) &&
        (observed_inline_revision_ != inline_revision || observed_image_revision_ != image_revision)) {
      const auto& rows = layout.Rows();
      const auto row = std::ranges::lower_bound(rows, previous_scroll, {}, &EditorLayout::Row::Bottom);
      if (row != rows.end()) {
        anchor = {static_cast<std::size_t>(row - rows.begin()), row->Top()};
      }
    }
    observed_inline_revision_ = inline_revision;
    observed_image_revision_ = image_revision;
    const bool laid_out = layout.Prepare(
        measurer, editor_->VisibleDocument(), width, viewport_->theme, viewport_->style, viewport_->locale,
        viewport_->images, image_revision, resize_ && resize_->preview.width != resize_->original.width
            ? std::optional(resize_->preview) : std::nullopt, viewport_->extensions, viewport_->inline_graphics,
        inline_revision, viewport_->document_directory);
    if (laid_out) {
      viewport_->height = layout.Height();
      if (anchor && viewport_->scroll.ScrollTo(previous_scroll + layout.Rows()[anchor->first].Top() - anchor->second)) {
        observed_scroll_.offset = viewport_->scroll.Metrics().offset;
      }
      touch_table_.reset();
      InvalidateSemantics();
    }
    const auto selection = editor_->SelectionOffsets();
    const bool decorations_changed = UpdateDecorations();
    const TextPosition caret{selection.active, selection.affinity};
    const bool follow_focused_caret = client_->CanRevealCaret(selection) && node.IsFocused();
    const bool revealed = (edited || laid_out) && follow_focused_caret &&
        layout.RevealCaret(editor_->VisibleDocument(), caret);
    const bool scrolled = UpdateScroll(follow_focused_caret, caret, edited, laid_out);
    if (laid_out || revealed || scrolled) {
      client_->GeometryChanged();
    }
    if (scrolled) {
      InvalidateSemantics();
    }
    if (appearance_changed) {
      return PaintInvalidation::Both;
    }
    return edited || laid_out || revealed || scrolled || decorations_changed
        ? PaintInvalidation::Foreground : PaintInvalidation::None;
  }

  bool HitTest(ViewNode& node, Point point) const override {
    return node.IsEnabled() && node.ContentBounds().Contains(point);
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override;
  bool OnKey(ViewNode&, const KeyEvent& event) override {
    const auto modifiers = event.modifiers;
    return event.type == KeyEventType::Down && event.key == Key::Enter &&
        (modifiers.control || modifiers.meta) && !modifiers.shift && !modifiers.alt &&
        editor_->SelectedInlineObject() && ActivateInline(editor_->SelectionOffsets().Range().start);
  }
  FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override;

  void OnFocusChanged(ViewNode&, bool, bool) override {
    blink_start_.reset();
    caret_visible_ = true;
    InvalidatePaint();
  }

  void PaintBehindContent(const ViewNode& node, PaintContext& paint) const override {
    paint.DrawRect(node.ContentBounds(), viewport_->style.background.value_or(viewport_->theme.colors.surface));
  }
  void PaintAboveContent(const ViewNode& node, PaintContext& paint) const override;
  void BuildSemantics(SemanticBuilder& builder) const override;
  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override;

private:
  struct ImageResize {
    std::int64_t pointer_id;
    std::uint64_t revision;
    Point start;
    Size original;
    ImageWidthPreview preview;
  };

  void ResetClient();
  bool UpdateScroll(bool focused, TextPosition position, bool edited, bool laid_out);
  bool RevealText(const RichDocument& document, TextOffset offset);
  bool ScrollTable(std::size_t table, float delta);
  bool UpdateDecorations();
  void PaintDecorations(PaintContext& paint, const EditorLayout::Row& row, std::size_t index, bool behind) const;
  std::optional<std::size_t> CodeHeaderAt(Point point) const;
  bool OpenCodeLanguage(std::size_t index);
  bool ActivateInline(TextOffset offset);
  PointerResult UpdateImageResize(const PointerEvent& event);
  ViewNode& node_;
  std::shared_ptr<EditorClient> client_;
  std::shared_ptr<EditorSession> editor_;
  std::shared_ptr<EditorViewport> viewport_;
  std::shared_ptr<EditorViewport::ScrollInputHandler> scroll_input_;
  std::shared_ptr<EditorViewport::RevealTextHandler> reveal_text_;
  std::uint64_t observed_revision_ = 0;
  std::shared_ptr<const DecorationAnalysis> decoration_analysis_;
  std::uint64_t observed_decoration_revision_ = 0;
  std::uint64_t observed_inline_revision_ = 0;
  std::uint64_t observed_image_revision_ = 0;
  ScrollMetrics observed_scroll_;
  bool focused_ = false;
  bool follow_caret_ = true;
  std::optional<PointerEvent> touch_;
  std::optional<std::size_t> touch_table_;
  float table_drag_x_ = 0.0F;
  std::optional<std::int64_t> drag_id_;
  std::optional<ImageResize> resize_;
  Point drag_in_viewport_;
  std::optional<double> blink_start_;
  bool caret_visible_ = true;
};

struct Surface {
  using Extension = EditorExtension;
  std::shared_ptr<EditorSession> editor;
  std::shared_ptr<EditorViewport> viewport;
  bool operator==(const Surface&) const = default;
};

void EditorExtension::Update(ViewNode& node, const Surface& surface) {
  if (editor_ == surface.editor && viewport_ == surface.viewport) {
    return;
  }
  editor_ = surface.editor;
  viewport_ = surface.viewport;
  ResetClient();
  scroll_input_ = std::make_shared<EditorViewport::ScrollInputHandler>([this, &node](const ScrollInputEvent& event) {
    const float delta = event.delta_x == 0.0F && event.modifiers.shift ? event.delta_y : event.delta_x;
    if (delta == 0.0F || (!event.modifiers.shift && std::abs(delta) < std::abs(event.delta_y)) ||
        !client_->HasGeometry()) {
      return false;
    }
    const auto local = node.WindowToLocal(event.position);
    const auto table = local ? client_->Layout().ScrollableTableAt(client_->ContentPoint(*local)) : std::nullopt;
    return table && ScrollTable(*table, delta);
  });
  viewport_->scroll_input = scroll_input_;
  reveal_text_ = std::make_shared<EditorViewport::RevealTextHandler>(
      [this](const RichDocument& document, TextOffset offset) { return RevealText(document, offset); });
  viewport_->reveal_text = reveal_text_;
}

void EditorExtension::ResetClient() {
  if (client_) {
    client_->Detach();
  }
  client_ = std::make_shared<EditorClient>(editor_, viewport_->scroll);
  touch_.reset();
  touch_table_.reset();
  drag_id_.reset();
  resize_.reset();
  decoration_analysis_.reset();
  focused_ = false;
  follow_caret_ = true;
  blink_start_.reset();
  caret_visible_ = true;
  InvalidatePaint(PaintInvalidation::Both);
  InvalidateSemantics();
}

float OffsetToReveal(const ScrollMetrics& metrics, Rect caret) {
  const float margin = std::clamp((metrics.viewport_extent - caret.height) / 2.0F, 0.0F, 8.0F);
  const float top = caret.y - margin;
  const float bottom = caret.y + caret.height + margin - metrics.viewport_extent;
  return std::clamp(metrics.offset < bottom ? bottom : std::min(metrics.offset, top), 0.0F, metrics.maximum_offset);
}

bool EditorExtension::RevealText(const RichDocument& document, TextOffset offset) {
  const auto metrics = viewport_->scroll.Metrics();
  if (!client_->HasGeometry() || !DocumentAccess::Same(document, editor_->VisibleDocument()) ||
      metrics.viewport_extent <= 0.0F) {
    return false;
  }
  auto& layout = client_->Layout();
  const TextPosition position{offset, TextAffinity::Downstream};
  const bool table_moved = layout.RevealCaret(document, position);
  const auto next = OffsetToReveal(metrics, layout.Caret(document, position));
  follow_caret_ = false;
  if (next != metrics.offset) {
    viewport_->scroll.ScrollTo(next);
  }
  if (table_moved) {
    client_->GeometryChanged();
    InvalidatePaint(PaintInvalidation::Foreground);
    InvalidateSemantics();
  }
  return true;
}

bool EditorExtension::UpdateScroll(bool focused, TextPosition position, bool edited, bool laid_out) {
  auto metrics = viewport_->scroll.Metrics();
  const bool focus_gained = focused && !focused_;
  focused_ = focused;
  if (edited || focus_gained) {
    follow_caret_ = true;
  } else if (metrics.offset != std::min(observed_scroll_.offset, metrics.maximum_offset)) {
    // A deliberate scroll takes over from the caret. A layout clamp alone keeps the current mode.
    follow_caret_ = false;
  }
  const bool extent_changed = metrics.content_extent != observed_scroll_.content_extent ||
      metrics.viewport_extent != observed_scroll_.viewport_extent;
  if (focused && follow_caret_ && (edited || focus_gained || laid_out || extent_changed) &&
      metrics.viewport_extent > 0.0F && !editor_->SelectedAtomicBlock()) {
    const auto caret = client_->Layout().Caret(editor_->VisibleDocument(), position);
    const float offset = OffsetToReveal(metrics, caret);
    if (offset != metrics.offset && viewport_->scroll.ScrollTo(offset)) {
      metrics = viewport_->scroll.Metrics();
    }
  }
  const bool scrolled = metrics.offset != observed_scroll_.offset;
  observed_scroll_ = metrics;
  return scrolled;
}

bool EditorExtension::ScrollTable(std::size_t table, float delta) {
  if (!client_->Layout().ScrollTable(table, delta)) {
    return false;
  }
  client_->GeometryChanged();
  InvalidatePaint(PaintInvalidation::Foreground);
  return true;
}

NodeExtension::PointerResult EditorExtension::UpdateImageResize(const PointerEvent& event) {
  if (resize_->pointer_id != event.pointer_id) {
    return PointerResult::Ignored;
  }
  if (event.type == PointerEventType::Cancel || resize_->revision != editor_->Revision()) {
    resize_.reset();
    InvalidatePaint();
    return PointerResult::Handled;
  }
  if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
    const auto delta = event.position - resize_->start;
    const auto size = resize_->original;
    // Project the pointer motion onto the image diagonal so both axes contribute to the proportional resize.
    const double ratio = static_cast<double>(size.height) / size.width;
    const double proposed = size.width + (delta.x + delta.y * ratio) / (1.0 + ratio * ratio);
    const float limit = viewport_->content_width.Get();
    const double minimum = std::min({16.0F, size.width, limit});
    resize_->preview.width = static_cast<float>(std::clamp(proposed, minimum, static_cast<double>(limit)));
    if (event.type == PointerEventType::Up) {
      const float width = resize_->preview.width;
      resize_.reset();
      if (width != size.width) {
        editor_->Execute(ResizeImage{width});
      }
    }
    InvalidatePaint();
  }
  return PointerResult::Handled;
}

std::optional<std::size_t> EditorExtension::CodeHeaderAt(Point point) const {
  if (!client_->HasGeometry() || !node_.ContentBounds().Contains(point)) {
    return std::nullopt;
  }
  const auto& rows = client_->Layout().Rows();
  const auto content_point = client_->ContentPoint(point);
  const auto row = std::ranges::find_if(rows, [&](const EditorLayout::Row& row) {
    return row.code_header && row.code_header->bounds.Contains(content_point);
  });
  return row == rows.end() ? std::nullopt : std::optional(static_cast<std::size_t>(row - rows.begin()));
}

bool EditorExtension::OpenCodeLanguage(std::size_t index) {
  if (!node_.IsEnabled() || !viewport_->code_language_popup || !viewport_->code_language_anchor) {
    return false;
  }
  const auto& header = client_->Layout().Rows()[index].code_header;
  if (!header) {
    return false;
  }
  const auto bounds = client_->ToNode(header->bounds).Intersection(node_.ContentBounds());
  if (bounds.IsEmpty()) {
    return false;
  }
  const auto& block = DocumentAccess::Get(editor_->VisibleDocument()).index.At(index).block;
  const auto id = DocumentAccess::Id(block);
  const auto generation = editor_->Generation();
  const auto presentation = viewport_->extensions.code_blocks;
  const auto current = CodeLanguageId(block.Data().fence_info,
                                      presentation ? *presentation : DefaultCodeBlockHighlighting());
  const auto theme = viewport_->theme;
  const auto viewport = node_.ContentBounds();
  constexpr float popup_height = 319.0F;
  const float below = viewport.y + viewport.height - bounds.y - bounds.height;
  const bool above = below < popup_height && bounds.y - viewport.y > below;
  client_->SetBlockControlActive(true);
  viewport_->code_language_popup->ShowAtAnchor(*viewport_->code_language_anchor, header->bounds,
      [editor = std::weak_ptr(editor_), viewport = std::weak_ptr(viewport_), presentation, id, generation, current,
       theme, popup_height](PopupContext context) -> View {
        const auto& colors = theme.colors;
        const auto font = Font::FromFamily(theme.typography.family, 14.0F);
        std::vector<View> choices;
        const auto append = [&](const CodeLanguage& language) {
          const bool selected = language.id == current;
          const auto foreground = selected ? colors.on_secondary_container : colors.on_surface;
          choices.push_back(Row {
            Text(std::string(language.label))
                .Style({font.WithWeight(selected ? FontWeight::Medium : FontWeight::Regular), foreground})
                .With(Grow(), Semantics{.hidden = true}),
            Text(selected ? "✓" : "").Style({font, colors.primary}).With(Semantics{.hidden = true}),
          }.OnClick([=] {
              context.Dismiss();
              const auto active_viewport = viewport.lock();
              if (const auto session = editor.lock(); !selected && session && session->Generation() == generation &&
                  active_viewport && active_viewport->extensions.code_blocks == presentation) {
                session->SetCodeLanguage(id, language.id);
              }
            }).With(
                Frame{.height = 44.0F}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F)),
                CrossAlign(CrossAxisAlignment::Center),
                Background(selected ? colors.secondary_container : Color::Transparent()),
                CornerRadius(theme.shapes.small), Focusable{}, PointerCursor(PointerCursorKind::Hand),
                Semantics{.role = SemanticRole::Button, .label = std::string(language.label), .selected = selected}
            ));
        };
        append({"text", "Plain text"});
        const auto& catalog = presentation ? *presentation : DefaultCodeBlockHighlighting();
        for (const auto& language : catalog.languages) {
          if (!language.id.empty() && language.id != "text" && language.id.find_first_of(" \t\r\n") == std::string::npos) {
            append(language);
          }
        }
        auto outline = colors.outline;
        outline.alpha *= 0.25F;
        return Column {
          Text("Code language")
              .Style({font.WithWeight(FontWeight::Bold), colors.on_surface})
              .VerticalAlign(TextVerticalAlign::Center)
              .With(Frame{.height = 42.0F}, Padding(EdgeInsets::Symmetric(16.0F, 0.0F))),
          Divider(),
          ScrollView(Column {std::move(choices)}.With(CrossAlign(CrossAxisAlignment::Stretch)))
              .With(Padding(6.0F), ScrollBar(), Grow()),
        }.With(Frame{.width = 208.0F, .height = popup_height}, CrossAlign(CrossAxisAlignment::Stretch),
               Background(colors.surface_container_low), Border{outline, 1.0F},
               CornerRadius(theme.shapes.medium), Shadow{colors.scrim, {}, 8.0F, 0.0F}, ClipChildren{});
      }, {.placement = {.side = above ? AnchorSide::Above : AnchorSide::Below}, .trap_focus = true});
  return true;
}

bool EditorExtension::ActivateInline(TextOffset offset) {
  const auto object = editor_->InlineObjectAt(offset);
  if (!object) {
    return false;
  }
  editor_->Select({offset, offset + 1});
  EmitEvent<RichEditorEvents::InlineObjectActivated>(*object);
  return true;
}

NodeExtension::PointerResult EditorExtension::OnPointer(ViewNode& node, const PointerEvent& event) {
  if (resize_) {
    return UpdateImageResize(event);
  }
  const bool primary_down = event.type == PointerEventType::Down &&
      (event.changed_button == PointerButton::Primary || event.changed_button == PointerButton::None);
  if (primary_down && client_->HasGeometry()) {
    // Keep the keyboard closed before touch selection is resolved on release.
    client_->SetBlockControlActive(CodeHeaderAt(event.position).has_value() ||
        (!event.modifiers.shift && client_->HitsAtomicBlock(event.position)));
  }
  if (primary_down && client_->HasGeometry() && !event.modifiers.shift) {
    if (const auto selected = editor_->SelectedImage()) {
      const auto& box = *client_->Layout().Rows()[*selected].image;
      const auto corner = ResizeCorner(box.bounds);
      const Rect hit{corner.x - 18.0F, corner.y - 16.0F, 28.0F, 28.0F};
      if (box.asset.HasValue() && hit.Contains(client_->ContentPoint(event.position))) {
        const auto& leaf = DocumentAccess::Get(editor_->VisibleDocument()).index.At(*selected);
        resize_ = ImageResize{event.pointer_id, editor_->Revision(), event.position,
                              {box.bounds.width, box.bounds.height},
                              {DocumentAccess::Id(leaf.block), box.bounds.width}};
        return PointerResult::Capture;
      }
    }
  }
  if (event.device_kind == PointerDeviceKind::Touch) {
    if (event.type == PointerEventType::Down && !touch_ && client_->HasGeometry()) {
      touch_ = event;
      return PointerResult::Observe;
    }
    if (!touch_ || touch_->pointer_id != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (touch_table_) {
      if (event.type == PointerEventType::Move) {
        ScrollTable(*touch_table_, table_drag_x_ - event.position.x);
        table_drag_x_ = event.position.x;
      } else if (event.type == PointerEventType::Cancel || event.type == PointerEventType::Up) {
        touch_.reset();
        touch_table_.reset();
      }
      return PointerResult::Handled;
    }
    const auto delta = event.position - touch_->position;
    if (event.type == PointerEventType::Move && client_->HasGeometry() &&
        std::abs(delta.x) > std::abs(delta.y) && std::abs(delta.x) >= GestureSettings{}.pointer_slop) {
      const auto table = client_->Layout().ScrollableTableAt(client_->ContentPoint(touch_->position));
      if (table && ScrollTable(*table, -delta.x)) {
        touch_table_ = table;
        table_drag_x_ = event.position.x;
        return PointerResult::CancelTarget;
      }
    }
    if (event.type == PointerEventType::Cancel ||
        std::max(std::abs(delta.x), std::abs(delta.y)) >= GestureSettings{}.pointer_slop) {
      touch_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      const auto start = touch_->position;
      touch_.reset();
      if (client_->HasGeometry() && HitTest(node, event.position)) {
        if (const auto header = CodeHeaderAt(start); header && header == CodeHeaderAt(event.position) &&
            OpenCodeLanguage(*header)) {
          return PointerResult::Handled;
        }
        client_->Select(event.position, event.modifiers.shift);
        return PointerResult::Handled;
      }
      return PointerResult::Ignored;
    }
    return PointerResult::Observe;
  }

  if (primary_down && client_->HasGeometry()) {
    if (event.modifiers.meta || event.modifiers.control) {
      if (const auto object = client_->InlineAt(event.position); object && ActivateInline(*object)) {
        return PointerResult::Handled;
      }
    }
    if (const auto header = CodeHeaderAt(event.position); header && OpenCodeLanguage(*header)) {
      return PointerResult::Handled;
    }
    drag_id_ = event.pointer_id;
    drag_in_viewport_ = event.position;
    client_->Select(event.position, event.modifiers.shift);
    return PointerResult::Capture;
  }
  if (drag_id_ == event.pointer_id) {
    if (event.type == PointerEventType::Move && client_->HasGeometry()) {
      drag_in_viewport_ = event.position;
      client_->Select(event.position, true);
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      drag_id_.reset();
    }
  }
  return PointerResult::Ignored;
}

NodeExtension::FrameResult EditorExtension::OnFrame(ViewNode& node, const FrameInfo& frame) {
  bool scrolling = false;
  if (drag_id_ && client_->HasGeometry() && node.IsEnabled()) {
    const auto metrics = viewport_->scroll.Metrics();
    const float y = drag_in_viewport_.y - node.ContentBounds().y;
    const float over = y < 0 ? y : y > metrics.viewport_extent ? y - metrics.viewport_extent : 0;
    if ((over < 0 && metrics.offset > 0) || (over > 0 && metrics.offset < metrics.maximum_offset)) {
      const auto delta = std::clamp(over * 8.0F, -650.0F, 650.0F) * static_cast<float>(frame.delta_time);
      scrolling = viewport_->scroll.ScrollBy(delta);
      client_->Select(drag_in_viewport_, true);
    }
  }
  if (!node.IsFocused() || frame.reduced_motion || editor_->CompositionRange() ||
      !editor_->SelectionOffsets().IsCollapsed()) {
    return {.needs_frame = scrolling};
  }
  blink_start_ = blink_start_.value_or(frame.timestamp);
  constexpr double interval = 0.5;
  const double elapsed = frame.timestamp - *blink_start_;
  const bool next = std::fmod(elapsed, interval * 2) < interval;
  if (caret_visible_ != next) {
    caret_visible_ = next;
    InvalidatePaint();
  }
  return {.needs_frame = scrolling, .wake_after = interval - std::fmod(elapsed, interval)};
}

void PaintTables(PaintContext& paint, const EditorLayout& layout, Rect visible) {
  const auto& colors = layout.Theme().colors;
  auto rule = colors.outline;
  rule.alpha *= 0.55F;
  for (const auto& table : layout.Tables()) {
    const auto& bounds = table.viewport;
    if (!bounds.Intersects(visible)) {
      continue;
    }
    const auto& rows = layout.Rows();
    const float right = bounds.x + bounds.width;
    const float bottom = bounds.y + bounds.height;
    paint.PushClip(bounds);
    if (table.has_header) {
      paint.DrawRect({bounds.x, bounds.y, bounds.width, rows[table.first].Bottom() - bounds.y},
                     colors.surface_container_highest);
    }
    for (std::size_t row = 1; row < table.rows; ++row) {
      const float y = rows[table.first + row * table.columns].Top();
      if (y >= visible.y && y < visible.y + visible.height) {
        paint.DrawRect({bounds.x, y - 0.5F, bounds.width, 1.0F}, rule);
      }
    }
    for (std::size_t column = 1; column < table.columns; ++column) {
      const float x = bounds.x + static_cast<float>(column) * table.column_width - table.offset;
      if (x > bounds.x && x < right) {
        paint.DrawRect({x - 0.5F, bounds.y, 1.0F, bounds.height}, rule);
      }
    }
    paint.DrawRect({bounds.x, bounds.y, bounds.width, 1.0F}, rule);
    paint.DrawRect({bounds.x, bottom - 1.0F, bounds.width, 1.0F}, rule);
    paint.DrawRect({bounds.x, bounds.y, 1.0F, bounds.height}, rule);
    paint.DrawRect({right - 1.0F, bounds.y, 1.0F, bounds.height}, rule);
    if (table.MaxOffset() > 0.0F) {
      const float thumb = std::min(bounds.width, std::max(16.0F, bounds.width * bounds.width / table.Width()));
      const float x = bounds.x + (bounds.width - thumb) * table.offset / table.MaxOffset();
      paint.DrawRect({x, bottom - 3.0F, thumb, 2.0F}, rule, CornerRadii{1.0F});
    }
    paint.PopClip();
  }
}

bool EditorExtension::UpdateDecorations() {
  const auto revision = viewport_->decoration_revision.Get();
  const auto& analysis = viewport_->decoration_analysis;
  const bool changed = decoration_analysis_ != analysis || observed_decoration_revision_ != revision;
  decoration_analysis_ = analysis;
  observed_decoration_revision_ = revision;
  return changed;
}

void EditorExtension::PaintDecorations(PaintContext& paint, const EditorLayout::Row& row, std::size_t index,
                                       bool behind) const {
  const auto& analysis = decoration_analysis_;
  if (!analysis || analysis->generation != editor_->Generation()) {
    return;
  }

  const auto& document = editor_->VisibleDocument();
  const bool matches_document = DocumentAccess::Same(analysis->document, document);
  const auto& block = DocumentAccess::Get(document).index.At(index).block;
  const auto id = DocumentAccess::Id(block);
  for (const auto& layer : analysis->layers) {
    const auto entry = layer->blocks.find(id);
    if (entry == layer->blocks.end() || entry->second.block != block ||
        (!matches_document && layer->provider->scope == DecorationScope::Document)) {
      continue;
    }
    for (const auto& decoration : entry->second.ranges) {
      if ((decoration.kind == DecorationKind::Background) != behind) {
        continue;
      }
      for (auto fragment : client_->Layout().TextFragments(row, decoration.range)) {
        switch (decoration.kind) {
        case DecorationKind::Background:
          paint.DrawRect(fragment, decoration.color, CornerRadii{3.0F});
          break;
        case DecorationKind::Underline:
          paint.DrawRect({fragment.x, fragment.y + fragment.height - 1.0F, fragment.width, 1.0F}, decoration.color);
          break;
        case DecorationKind::Outline:
          paint.DrawBorder(fragment, decoration.color, StrokeStyle{.width = 1.0F});
          break;
        }
      }
    }
  }
}

void EditorExtension::PaintAboveContent(const ViewNode& node, PaintContext& paint) const {
  if (!client_->HasGeometry()) {
    return;
  }
  const auto& document = editor_->VisibleDocument();
  const auto& layout = client_->Layout();
  const auto& style = layout.Style();
  const auto& colors = layout.Theme().colors;
  const auto accent = layout.AccentColor();
  const auto bounds = node.ContentBounds();
  const auto scroll = viewport_->scroll.Metrics();
  paint.PushClip(bounds);
  paint.PushTransform({.translate_x = bounds.x, .translate_y = bounds.y - scroll.offset});
  const Rect visible{0.0F, scroll.offset, bounds.width, bounds.height};
  const auto quote_color = style.quote_rule_color.value_or(colors.outline);
  for (const auto& rule : layout.QuoteRules()) {
    if (rule.Intersects(visible)) {
      paint.DrawRect(rule, quote_color, CornerRadii{rule.width / 2.0F});
    }
  }
  PaintTables(paint, layout, visible);
  for (std::size_t index = 0; index < layout.Rows().size(); ++index) {
    const auto& row = layout.Rows()[index];
    if (row.Bottom() + style.block_spacing < scroll.offset ||
        row.Top() - style.block_spacing > scroll.offset + bounds.height) {
      continue;
    }
    if (row.custom) {
      const auto& box = *row.custom;
      paint.DrawRect(box.bounds, colors.surface_container_low, CornerRadii{8.0F});
      paint.PushClip(box.bounds);
      if (box.graphic) {
        std::visit([&](const auto& asset) { paint.DrawImage(asset, box.content); }, *box.graphic);
      } else if (box.fallback) {
        paint.PushClip(box.content);
        paint.DrawText({box.content.x, box.content.y}, *box.fallback);
        paint.PopClip();
      }
      paint.PopClip();
      continue;
    }
    if (row.image) {
      const auto& image = *row.image;
      if (image.asset.HasValue()) {
        paint.DrawImage(image.asset, image.bounds);
      } else {
        paint.DrawRect(image.bounds, colors.surface_container_highest, CornerRadii{8.0F});
        paint.DrawText(image.bounds, image.label, {layout.MarkerStyle().font, colors.on_surface_variant},
                       {.align = TextAlign::Center, .vertical_align = TextVerticalAlign::Center});
      }
      continue;
    }
    if (row.cell) {
      const auto cell = layout.VisibleCell(row);
      if (cell.IsEmpty()) {
        continue;
      }
      paint.PushClip(cell);
    }
    const auto origin = layout.Origin(row);
    if (row.code_bounds && !row.code_bounds->IsEmpty()) {
      paint.DrawRect(*row.code_bounds, style.code_block_background.value_or(colors.surface_container_highest),
                     CornerRadii{style.code_block_corner_radius});
    }
    if (row.code_header && row.code_header->bounds.width >= 34.0F) {
      const auto& header = *row.code_header;
      const auto& box = header.bounds;
      paint.DrawRect({box.x, box.y + 6.0F, box.width, box.height - 12.0F},
                     colors.surface_container_highest, CornerRadii{layout.Theme().shapes.small});
      paint.DrawText({box.x + 8.0F, box.y, box.width - 30.0F, box.height}, header.label, header.style,
                     {.vertical_align = TextVerticalAlign::Center, .wrap = TextWrap::NoWrap});
      const Point tip{box.x + box.width - 12.0F, box.y + box.height / 2.0F + 2.0F};
      paint.DrawLine({tip.x - 3.0F, tip.y - 3.0F}, tip, header.style.foreground, StrokeStyle{.width = 1.25F});
      paint.DrawLine(tip, {tip.x + 3.0F, tip.y - 3.0F}, header.style.foreground, StrokeStyle{.width = 1.25F});
    }
    for (const auto& marker : row.markers) {
      paint.DrawText({marker.left, row.origin.y, marker.width, row.layout.Metrics().size.height},
                     marker.text, layout.MarkerStyle(),
                     {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap});
    }
    for (const auto& fill : row.inline_backgrounds) {
      paint.DrawRect(AtOrigin(fill.bounds, origin), fill.color, CornerRadii{fill.corner_radius});
    }
    PaintDecorations(paint, row, index, true);
    paint.DrawText(origin, row.layout);
    for (const auto& object : row.inline_objects) {
      const auto rect = AtOrigin(object.bounds, origin);
      if (rect.IsEmpty()) {
        continue;
      }
      if (object.graphic) {
        std::visit([&](const auto& asset) { paint.DrawImage(asset, rect); }, *object.graphic);
      } else if (object.fallback) {
        paint.PushClip(rect);
        paint.DrawText({rect.x, rect.y}, *object.fallback);
        paint.PopClip();
      }
    }
    PaintDecorations(paint, row, index, false);
    if (row.cell) {
      paint.PopClip();
    }
  }
  const auto selection = editor_->SelectionOffsets();
  if (const auto selected = editor_->SelectedImage()) {
    const auto& image = *layout.Rows()[*selected].image;
    const auto corner = ResizeCorner(image.bounds);
    const Point top_left{image.bounds.x, image.bounds.y};
    const Point top_right{corner.x, top_left.y};
    const Point bottom_left{top_left.x, corner.y};
    const StrokeStyle stroke{.width = 1.5F};
    paint.DrawLine(top_left, top_right, accent, stroke);
    paint.DrawLine(top_right, corner, accent, stroke);
    paint.DrawLine(corner, bottom_left, accent, stroke);
    paint.DrawLine(bottom_left, top_left, accent, stroke);
    if (image.asset.HasValue()) {
      paint.DrawRect({corner.x - 6.0F, corner.y - 6.0F, 12.0F, 12.0F}, colors.surface, CornerRadii{3.0F});
      paint.DrawRect({corner.x - 4.5F, corner.y - 4.5F, 9.0F, 9.0F}, accent, CornerRadii{2.0F});
    }
  } else {
    auto selection_color = accent;
    selection_color.alpha *= node.IsFocused() ? 0.27F : 0.16F;
    for (const auto& rect : layout.Ranges(document, selection.Range())) {
      paint.DrawRect(rect, selection_color);
    }
  }
  if (const auto composition = editor_->CompositionRange()) {
    for (const auto& rect : layout.Ranges(document, *composition)) {
      paint.DrawRect({rect.x, rect.y + rect.height - 1.5F, rect.width, 1.5F}, accent);
    }
  }
  if (node.IsFocused() && selection.IsCollapsed() && (caret_visible_ || editor_->CompositionRange())) {
    auto caret = layout.Caret(document, {selection.active, selection.affinity});
    caret.width = 1.5F;
    const auto& row = layout.Rows()[LeafIndex(document, selection.active)];
    if (row.cell) {
      caret = caret.Intersection(layout.VisibleCell(row));
    }
    if (!caret.IsEmpty()) {
      paint.DrawRect(caret, accent);
    }
  }
  paint.PopTransform();
  paint.PopClip();
}

void EditorExtension::BuildSemantics(SemanticBuilder& builder) const {
  const auto& document = editor_->VisibleDocument();
  builder.SetOwner({.role = SemanticRole::TextField, .label = "Document",
                    .value = TextSlice(document, {0, DocumentAccess::Get(document).index.Length()}),
                    .text_selection = editor_->SelectionOffsets().Range()});
  builder.AddAction(0, SemanticActionKind::SetText);
  builder.AddAction(0, SemanticActionKind::SetSelection);
  if (client_->HasGeometry()) {
    const auto& leaves = DocumentAccess::Get(document).index;
    const auto& rows = client_->Layout().Rows();
    for (std::size_t i = 0; i < leaves.LeafCount(); ++i) {
      const auto& row = rows[i];
      for (const auto& object : row.inline_objects) {
        auto bounds = AtOrigin(object.bounds, client_->Layout().Origin(row));
        if (row.cell) {
          bounds = bounds.Intersection(client_->Layout().VisibleCell(row));
        }
        if (!bounds.IsEmpty()) {
          const auto id = inline_semantic_id | object.id;
          builder.AddChild(id, client_->ToNode(bounds),
                           {.role = SemanticRole::Button, .label = object.label}, true);
          builder.AddAction(id, SemanticActionKind::Activate);
        }
      }
      if (rows[i].image) {
        const auto& image = *rows[i].image;
        builder.AddChild(DocumentAccess::Id(leaves.At(i).block), client_->ToNode(image.bounds),
                         {.role = SemanticRole::Image, .label = image.label}, true);
      } else if (const auto& custom = rows[i].custom) {
        builder.AddChild(DocumentAccess::Id(leaves.At(i).block), client_->ToNode(custom->bounds),
                         {.role = SemanticRole::Text, .label = custom->label}, true);
      } else if (const auto& header = rows[i].code_header) {
        const auto id = DocumentAccess::Id(leaves.At(i).block);
        builder.AddChild(id, client_->ToNode(header->bounds),
                         {.role = SemanticRole::Button, .label = "Code language, " + header->label}, true);
        builder.AddAction(id, SemanticActionKind::Activate);
      }
    }
  }
}

bool EditorExtension::OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) {
  if (local_id != 0) {
    if (action.kind == SemanticActionKind::Activate && client_->HasGeometry()) {
      const auto& index = DocumentAccess::Get(editor_->VisibleDocument()).index;
      if (local_id & inline_semantic_id) {
        const auto& rows = client_->Layout().Rows();
        for (std::size_t i = 0; i < rows.size(); ++i) {
          const auto& objects = rows[i].inline_objects;
          const auto found = std::ranges::find(objects, local_id & ~inline_semantic_id, &EditorLayout::InlineBox::id);
          if (found != objects.end()) {
            return ActivateInline(index.At(i).start + found->offset);
          }
        }
        return false;
      }
      if (const auto found = index.IndexOf(local_id)) {
        return OpenCodeLanguage(*found);
      }
    }
    return false;
  }
  if (action.kind == SemanticActionKind::SetSelection) {
    if (const auto* range = std::get_if<TextRange>(&action.value)) {
      return editor_->Select({range->start, range->end});
    }
  }
  if (action.kind == SemanticActionKind::SetText) {
    if (const auto* text = std::get_if<std::string>(&action.value)) {
      editor_->FinishComposition();
      const auto length = DocumentAccess::Get(editor_->VisibleDocument()).index.Length();
      const TextInputCommand command{.kind = TextInputCommandKind::CommitText, .target = TextRange{0, length},
                                     .text = *text};
      return editor_->Apply(std::span(&command, 1));
    }
  }
  return false;
}

[[huxerui::composable]]
View EditorContent(std::shared_ptr<EditorSession> editor, std::shared_ptr<EditorViewport> viewport,
                   RichEditorOptions options) {
  (void)editor->Observe();
  viewport->theme = UseTheme();
  viewport->style = std::move(options.style);
  viewport->document_directory = options.document_directory;
  viewport->extensions = {.image_resolver = options.extensions.image_resolver,
                          .code_blocks = options.extensions.code_blocks,
                          .custom_blocks = options.extensions.custom_blocks,
                          .inline_objects = options.extensions.inline_objects};
  viewport->locale = UseEnvironment<Locale>().LanguageTag();
  viewport->code_language_popup = UsePopup();
  const auto code_language_anchor = UseViewHandle();
  viewport->code_language_anchor = code_language_anchor;
  const auto tasks = UseTaskScope();
  ScheduleDecorations(editor, viewport, tasks, options.extensions.decorations);
  ScheduleInlineObjects(editor, viewport, tasks, options.document_directory);
  const auto generation = editor->Generation();
  auto resolver = viewport->extensions.image_resolver;
  const auto directory = resolver ? std::nullopt : options.document_directory;
  if (directory) {
    resolver = viewport->image_directory == directory ? viewport->image_resolver
        : std::make_shared<const ImageResolver>([directory = *directory](std::string source) {
            return ResolveImageFile(directory, std::move(source));
          });
  }
  viewport->image_directory = directory;
  if (viewport->image_generation != generation || viewport->image_resolver != resolver) {
    viewport->images.clear();
    viewport->image_generation = generation;
    viewport->image_resolver = resolver;
    viewport->image_revision = viewport->image_revision.Get() + 1;
  }
  (void)viewport->image_revision.Get();
  std::vector<std::string> sources;
  if (resolver && *resolver) {
    for (const auto& block : editor->VisibleDocument().Blocks()) {
      if (const auto& image = block.Data().image) {
        sources.push_back(image->source);
      }
    }
    DocumentAccess::Get(editor->VisibleDocument()).index.VisitInlineBlocks([&](const Block& block) {
      for (const auto& object : block.Data().inline_objects) {
        if (object.data.type == inline_image_type && object.data.attachments.size() == 1) {
          sources.push_back(object.data.attachments.front().source);
        }
      }
    });
    std::ranges::sort(sources);
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  }
  Lifecycle([=] {
    std::erase_if(viewport->images, [&](const auto& entry) {
      return !std::ranges::binary_search(sources, entry.first);
    });
    std::vector<TaskHandle> requests;
    for (const auto& source : sources) {
      if (viewport->images.contains(source)) {
        continue;
      }
      requests.push_back(tasks.Launch([=]() -> Task<void> {
        ImageAsset asset;
        try {
          auto result = co_await (*resolver)(source);
          if (result.Succeeded()) {
            asset = std::move(result.Value());
          }
        } catch (const std::exception&) {
          // Keep the placeholder if the application fails to resolve this reference.
        }
        if (editor->Generation() == generation && viewport->image_resolver == resolver) {
          viewport->images.insert_or_assign(source, std::move(asset));
          viewport->image_revision = viewport->image_revision.Get() + 1;
        }
      }));
    }
    return [requests = std::move(requests)] {
      for (const auto& request : requests) {
        request.Cancel();
      }
    };
  }, generation, resolver, sources);
  return Canvas([](PaintContext&, Size) {}).With(Frame{.width = 640.0F, .height = viewport->height.Get()},
                                                code_language_anchor);
}

View EditorView(std::shared_ptr<EditorSession> editor, std::shared_ptr<EditorViewport> viewport,
                RichEditorOptions options) {
  // Keep the text input owner outside the scroll node so the editor can reveal the caret only when needed.
  const auto content = ScrollView(EditorContent(editor, viewport, std::move(options)))
      .Controller(viewport->scroll)
      .On<ViewEvents::ScrollInput>([viewport](const ScrollInputEvent& event) {
        const auto handler = viewport->scroll_input.lock();
        return handler && (*handler)(event);
      })
      .With(ScrollBar(), Grow());
  return Column {
    content,
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Focusable{}, Surface{std::move(editor), std::move(viewport)});
}

} // namespace
} // namespace detail

View RichEditor(RichEditorController editor, RichEditorOptions options) {
  return detail::EditorView(std::move(editor.session_), std::move(editor.viewport_), std::move(options));
}

} // namespace huxerui::richeditor

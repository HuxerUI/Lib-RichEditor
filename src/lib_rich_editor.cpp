#include <huxerui/richeditor.h>

#include <algorithm>
#include <cmath>

#include "editor_session.h"
#include "editor_view_internal.h"

namespace huxerui::richeditor {

RichEditorController::RichEditorController(std::shared_ptr<detail::EditorSession> session,
                                           std::shared_ptr<detail::EditorViewport> viewport)
    : session_(std::move(session)), viewport_(std::move(viewport)) {}

RichDocument RichEditorController::Document() const { return session_->Document(); }

void RichEditorController::ReplaceDocument(RichDocument document) const { session_->ReplaceDocument(std::move(document)); }

void RichEditorController::SetReadOnly(bool read_only) const { session_->SetReadOnly(read_only); }

bool RichEditorController::IsReadOnly() const { return session_->IsReadOnly(); }

bool RichEditorController::IsComposing() const {
  (void)session_->Observe();
  return session_->CompositionRange().has_value();
}

bool RichEditorController::ReplaceTextRanges(std::span<const BlockTextRange> ranges, std::string_view text) const {
  return session_->ReplaceTextRanges(ranges, text);
}

CommandState RichEditorController::GetCommandState(const EditorCommand& command) const {
  (void)session_->Observe();
  return session_->CommandStatus(command);
}

std::optional<LinkRange> RichEditorController::CurrentLink() const {
  (void)session_->Observe();
  if (const auto* link = detail::LinkAt(session_->VisibleDocument(), session_->SelectionOffsets().Range())) {
    return *link;
  }
  return std::nullopt;
}

std::optional<unsigned> RichEditorController::OrderedListStart() const {
  (void)session_->Observe();
  return session_->OrderedListStart();
}

std::optional<Size> RichEditorController::SelectedImageSize() const {
  (void)session_->Observe();
  (void)viewport_->image_revision.Get();
  const auto selected = session_->SelectedImage();
  if (!selected || viewport_->image_generation != session_->Generation()) {
    return std::nullopt;
  }
  const auto& image = *detail::DocumentAccess::Get(session_->VisibleDocument()).index.At(*selected).block.Data().image;
  const auto asset = viewport_->images.find(image.source);
  if (asset == viewport_->images.end() || !asset->second.HasValue()) {
    return std::nullopt;
  }
  const float width = image.width.value_or(std::min(viewport_->content_width.Get(), asset->second.IntrinsicSize().width));
  const float height = detail::ImageHeight(asset->second, width);
  if (width <= 0.0F || height <= 0.0F || !std::isfinite(height)) {
    return std::nullopt;
  }
  return Size{width, height};
}

std::optional<CustomBlockData> RichEditorController::SelectedCustomBlock() const {
  (void)session_->Observe();
  if (const auto* selected = session_->SelectedCustomBlock()) {
    return *selected;
  }
  return std::nullopt;
}

std::optional<InlineObjectSelection> RichEditorController::SelectedInlineObject() const {
  (void)session_->Observe();
  return session_->SelectedInlineObject();
}

std::optional<DocumentFragment> RichEditorController::SelectedFragment() const {
  (void)session_->Observe();
  return session_->SelectedFragment();
}

std::optional<EditorViewportLocation> RichEditorController::ViewportLocation(float y) const {
  if (!std::isfinite(y)) {
    return std::nullopt;
  }
  if (const auto locate = viewport_->viewport_location.lock()) {
    return (*locate)(session_->Document(), y);
  }
  // Subscribe before the mounted surface installs its geometry query.
  (void)viewport_->geometry.Get();
  return std::nullopt;
}

bool RichEditorController::RevealTextRange(const Block& block, TextRange range,
                                           std::optional<ScrollAlignment> alignment) const {
  const auto reveal = viewport_->reveal_text.lock();
  if (!reveal || session_->CompositionRange()) {
    return false;
  }
  const auto document = session_->Document();
  const auto& index = detail::DocumentAccess::Get(document).index;
  const auto id = detail::DocumentAccess::Id(block);
  const auto* current = detail::FindTextBlock(index, id);
  if (!current || !detail::DocumentAccess::SameBlock(*current, block) || !detail::ValidTextRange(*current, range)) {
    return false;
  }
  const auto start = index.StartOf(id);
  return (*reveal)(document, {start + range.start, start + range.end}, alignment);
}

bool RichEditorController::Execute(const EditorCommand& command) const { return session_->Execute(command); }

RichEditorController UseRichEditor(RichDocument initial, const std::source_location& location) {
  const auto retained = UseState(std::optional<RichEditorController>{}, location);
  if (!retained.Get()) {
    retained = RichEditorController(std::make_shared<detail::EditorSession>(std::move(initial)),
                                    std::make_shared<detail::EditorViewport>());
  }
  return *retained.Get();
}

} // namespace huxerui::richeditor

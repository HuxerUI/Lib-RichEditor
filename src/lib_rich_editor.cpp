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
  const auto& image = *detail::DocumentAccess::Get(session_->VisibleDocument()).leaves[*selected].block.Data().image;
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

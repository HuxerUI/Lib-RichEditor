#pragma once

#include "editor_view_internal.h"

namespace huxerui::richeditor::detail {

/// @brief Binds inline rendering requests to the mounted viewport's lifecycle.
///
/// Reconciles committed objects with pending and resolved requests. Appearance, resource context and
/// renderer identity participate in reuse. Preedit uses the view's fallback when no matching result exists.
void ScheduleInlineObjects(const std::shared_ptr<EditorSession>& editor,
                           const std::shared_ptr<EditorViewport>& viewport, const TaskScope& tasks,
                           const std::optional<File>& directory);

} // namespace huxerui::richeditor::detail

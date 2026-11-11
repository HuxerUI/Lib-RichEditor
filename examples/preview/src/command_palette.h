#pragma once

#include <functional>

#include <huxerui/presentation.h>
#include <huxerui/task.h>

#include "shortcuts.h"

namespace folio {

/// Supplies the active workbench's command state and the focus policy for this invocation.
struct CommandPaletteOptions {
  huxerui::ViewHandle initial_focus;
  huxerui::TaskScope window_tasks;
  /// Reads availability and checked state from the original window and editor.
  std::function<huxerui::richeditor::CommandState(const ShortcutCommand&)> state;
  /// Applies an editor command to its retained selection or prepares a workbench action.
  /// An empty continuation means the command became unavailable. The continuation runs after dismissal.
  std::function<std::function<void()>(const ShortcutCommand&)> invoke;
  /// Runs in the owning window after the layer is unmounted, before the chosen command's continuation.
  std::function<void(bool invoked)> on_closed;
};

huxerui::View CommandPalette(huxerui::DialogContext context, CommandPaletteOptions options);

} // namespace folio

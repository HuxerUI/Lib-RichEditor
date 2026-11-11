#pragma once

#include <array>
#include <optional>
#include <string>

#include <huxerui/file.h>
#include <huxerui/state.h>

#include "appearance.h"
#include "shortcuts.h"

namespace huxerui {
class DialogContext;
}

namespace folio {

inline constexpr std::array editor_text_sizes{14, 16, 19};
inline constexpr std::array editor_reading_widths{560, 684, 920};

/// Document presentation in logical pixels, shared by all editor tabs.
struct EditorSettings {
  int text_size = editor_text_sizes[1];
  int reading_width = editor_reading_widths[1];

  bool operator==(const EditorSettings&) const = default;
};

/// Application preferences shared across windows, independent of documents and editor history.
struct FolioSettings {
  std::string theme{"parchment"};
  EditorSettings editor;
  ShortcutOverrides shortcuts;

  bool operator==(const FolioSettings&) const = default;
};

/// Owns the live settings and persists each update in the application's data directory.
class SettingsStore {
public:
  explicit SettingsStore(std::optional<huxerui::File> directory);

  const FolioSettings& Get() const { return value_.Get(); }
  const std::shared_ptr<const ResolvedShortcuts>& Bindings() const { return bindings_.Get(); }
  bool SaveFailed() const { return save_failed_.Get(); }

  /// Applies settings across windows and saves them. Web storage completes asynchronously in the application scope.
  void Set(FolioSettings value);

private:
  std::optional<huxerui::File> directory_;
  huxerui::State<FolioSettings> value_;
  huxerui::State<std::shared_ptr<const ResolvedShortcuts>> bindings_;
  huxerui::State<bool> save_failed_{false};
#if defined(__EMSCRIPTEN__)
  huxerui::State<bool> writing_{false};
#endif
};

huxerui::View SettingsDialog(huxerui::DialogContext context);

} // namespace folio

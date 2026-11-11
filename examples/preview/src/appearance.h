#pragma once

#include <span>
#include <string_view>

#include <huxerui/theme.h>

namespace folio {

inline constexpr float control_icon_size = 18.0F;
inline constexpr std::string_view system_theme = "system";

/// Selects touch-first affordances on Android and iOS, independent of window width.
bool IsMobilePlatform();

/// A named palette shared by the workbench, the document editor, and its settings preview.
struct ThemePreset {
  std::string_view id;
  std::string_view name;
  huxerui::ThemeBrightness brightness;
  huxerui::ColorScheme colors;
};

std::span<const ThemePreset> ThemePresets();
const ThemePreset& ResolveTheme(std::string_view id, huxerui::ThemeBrightness system_brightness);
huxerui::ThemeDefinition FolioTheme(const ThemePreset& preset);
huxerui::Color PendingEditsColor(huxerui::ThemeBrightness brightness);
huxerui::View PrimaryAction(huxerui::View button, const huxerui::ThemeSpec& theme);
huxerui::View DialogPanel(huxerui::View content, const huxerui::ThemeSpec& theme, float width);

/// Fits the header and actions before sizing the scrollable body to the remaining height.
huxerui::View ScrollableDialogPanel(huxerui::View header, huxerui::View body, huxerui::View actions,
                                  const huxerui::ThemeSpec& theme, float width);

/// Keeps the dismiss and primary actions together when a secondary action needs its own row.
huxerui::View DialogActions(const huxerui::ThemeSpec& theme, huxerui::View cancel, huxerui::View primary,
                          huxerui::View secondary = {}, bool destructive = false);
} // namespace folio

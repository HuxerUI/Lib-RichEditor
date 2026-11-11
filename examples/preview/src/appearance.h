#pragma once

#include <huxerui/theme.h>

namespace folio {

inline constexpr float control_icon_size = 18.0F;

huxerui::ThemeDefinition FolioTheme(huxerui::ThemeBrightness brightness);
huxerui::Color PendingEditsColor(huxerui::ThemeBrightness brightness);
huxerui::View PrimaryAction(huxerui::View button, const huxerui::ThemeSpec& theme);
huxerui::View DialogPanel(huxerui::View content, const huxerui::ThemeSpec& theme, float width);

} // namespace folio

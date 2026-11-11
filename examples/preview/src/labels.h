#pragma once

#include <optional>
#include <string>

#include <huxerui/semantics.h>
#include <huxerui/theme.h>

namespace folio {

/// Draws one line within its allocated width and exposes the complete label to accessibility.
huxerui::View EllipsizedLabel(std::string label, huxerui::Color color, huxerui::Semantics semantics = {},
                             std::optional<huxerui::Font> font = {});

/// Reserves the same footer height for the workbench whether or not it has a status to announce.
huxerui::View StatusBar(std::string message);

} // namespace folio

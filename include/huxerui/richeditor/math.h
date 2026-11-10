#pragma once

/// @file
/// @brief Document representation for a standalone LaTeX formula.

#include <string>
#include <string_view>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor {

/// Persistent custom block type for standalone LaTeX source.
inline constexpr std::string_view latex_block_type = "huxerui.latex";

/// @brief Wraps LaTeX math source without delimiters as a version-one custom block.
/// Source is also used as the plain text fallback. The block stays editable when a renderer cannot typeset it.
CustomBlockData LatexBlock(std::string source);

} // namespace huxerui::richeditor

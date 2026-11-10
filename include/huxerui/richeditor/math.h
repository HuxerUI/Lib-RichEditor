#pragma once

/// @file
/// @brief Persistent representations for display and inline LaTeX formulas.

#include <string>
#include <string_view>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor {

/// Persistent type identifier shared by LatexBlock() and LatexInline().
inline constexpr std::string_view latex_block_type = "huxerui.latex";

/// @brief Wraps LaTeX math source without delimiters as a version-one custom block.
/// Source is also used as the plain text fallback. The block stays editable when a renderer cannot typeset it.
CustomBlockData LatexBlock(std::string source);

/// @brief Wraps LaTeX math source as a version-one inline object.
/// @param source UTF-8 LaTeX math source without delimiters.
/// @return Persistent data using LF line endings in the payload and spaces in place of line breaks in the fallback.
/// @see InsertInlineObject
InlineObjectData LatexInline(std::string source);

} // namespace huxerui::richeditor

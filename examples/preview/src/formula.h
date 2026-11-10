#pragma once

#include <huxerui/richeditor/extension.h>
#include <huxerui/richeditor/format.h>

namespace folio {

/// Persistent type shared by Folio's display and inline formulas, including previously saved documents.
inline constexpr std::string_view latex_type = "huxerui.latex";

/// Creates a display formula from delimiter-free LaTeX source with a plain-text fallback.
huxerui::richeditor::CustomBlockData LatexBlock(std::string source);

/// Creates an inline formula from delimiter-free LaTeX source with a single-line fallback.
huxerui::richeditor::InlineObjectData LatexInline(std::string source);

/// Maps Markdown math spans and math fences to Folio's persistent formula data.
huxerui::richeditor::MarkdownExtension MarkdownMath();

/// Shared configuration for opening and exporting Folio Markdown documents.
const huxerui::richeditor::MarkdownCodec& DocumentMarkdown();

std::shared_ptr<const huxerui::richeditor::CustomBlockRenderer> FormulaRenderer();

/// Registers Folio's asynchronous LaTeX renderer for formulas embedded in text.
huxerui::richeditor::InlineObjectExtension InlineFormulaExtension();

} // namespace folio

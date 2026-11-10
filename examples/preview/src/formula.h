#pragma once

#include <huxerui/richeditor/extension.h>

namespace folio {

std::shared_ptr<const huxerui::richeditor::CustomBlockRenderer> FormulaRenderer();

/// Registers Folio's asynchronous LaTeX renderer for formulas embedded in text.
huxerui::richeditor::InlineObjectExtension InlineFormulaExtension();

} // namespace folio

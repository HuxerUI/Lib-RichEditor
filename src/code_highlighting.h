#pragma once

#include <string>
#include <string_view>

#include <huxerui/richeditor/extension.h>

namespace huxerui::richeditor::detail {

std::string_view CodeFenceWord(std::string_view info);
std::string CodeLanguageId(std::string_view info, const CodeBlockHighlighting& presentation);
std::string CodeLanguageLabel(std::string_view info, const CodeBlockHighlighting& presentation);

} // namespace huxerui::richeditor::detail

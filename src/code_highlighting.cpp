#include <huxerui/richeditor/extension.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <sweetline/highlight.h>

#include "code_highlighting.h"

namespace huxerui::richeditor {
namespace {

struct SyntaxProfile {
  struct {
    std::string_view id;
    std::string_view label;
  } language;
  std::string_view syntax;
};

constexpr SyntaxProfile profiles[]{
#include "richeditor_syntax_profiles.inc"
};

} // namespace

namespace detail {

std::string_view CodeFenceWord(std::string_view info) {
  const auto start = info.find_first_not_of(" \t");
  if (start == info.npos) {
    return {};
  }
  info.remove_prefix(start);
  return info.substr(0, info.find_first_of(" \t"));
}

std::string CodeLanguageId(std::string_view info, const CodeBlockHighlighting& presentation) {
  std::string language(CodeFenceWord(info));
  std::ranges::transform(language, language.begin(), [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
  });
  if (language.empty() || language == "text" || language == "txt" || language == "plain" || language == "plaintext") {
    return "text";
  }
  for (const auto& entry : presentation.languages) {
    if (entry.id == language || std::ranges::find(entry.aliases, language) != entry.aliases.end()) {
      return entry.id;
    }
  }
  return language;
}

std::string CodeLanguageLabel(std::string_view info, const CodeBlockHighlighting& presentation) {
  const auto id = CodeLanguageId(info, presentation);
  if (id == "text") {
    return "Plain text";
  }
  for (const auto& language : presentation.languages) {
    if (language.id == id) {
      return std::string(language.label);
    }
  }
  return std::string(CodeFenceWord(info));
}

} // namespace detail

namespace {

bool FitsAnalysisBudget(std::string_view code) {
  if (code.size() > 64 * 1024) {
    return false;
  }
  while (!code.empty()) {
    const auto end = code.find('\n');
    if (std::min(end, code.size()) > 2048) {
      return false;
    }
    if (end == code.npos) {
      break;
    }
    code.remove_prefix(end + 1);
  }
  return true;
}

std::optional<Color> TokenColor(std::string_view name, bool dark) {
  if (name == "keyword" || name == "preprocessor" || name == "macro") {
    return dark ? Color::Rgb(218, 178, 255) : Color::Rgb(105, 48, 149);
  }
  if (name == "string" || name == "character" || name == "escape") {
    return dark ? Color::Rgb(164, 214, 162) : Color::Rgb(36, 104, 54);
  }
  if (name == "number" || name == "constant") {
    return dark ? Color::Rgb(255, 193, 137) : Color::Rgb(151, 64, 15);
  }
  if (name == "comment") {
    return dark ? Color::Rgb(166, 174, 186) : Color::Rgb(88, 98, 113);
  }
  if (name == "class" || name == "type" || name == "annotation" || name == "function" ||
      name == "builtin" || name == "selector" || name == "url") {
    return dark ? Color::Rgb(143, 204, 251) : Color::Rgb(25, 91, 148);
  }
  return std::nullopt;
}

std::vector<TextOffset> Utf16Boundaries(std::string_view code) {
  std::vector<TextOffset> offsets;
  offsets.reserve(code.size() + 1);
  TextOffset offset = 0;
  for (const unsigned char byte : code) {
    if ((byte & 0xC0) != 0x80) {
      offsets.push_back(offset);
      offset += byte >= 0xF0 ? 2 : 1;
    }
  }
  offsets.push_back(offset);
  return offsets;
}

class SweetLineHighlighter {
public:
  std::vector<CodeHighlight> Highlight(std::string_view code, std::string_view language, Color background) {
    if (code.empty() || !FitsAnalysisBudget(code)) {
      return {};
    }
    const auto profile = std::ranges::find_if(profiles, [&](const auto& entry) { return entry.language.id == language; });
    if (profile == std::end(profiles)) {
      return {};
    }

    const std::scoped_lock lock(mutex_);
    auto [entry, inserted] = analyzers_.try_emplace(std::string(language));
    auto& analyzer = entry->second;
    if (inserted) {
      try {
        engine_.compileSyntaxFromJson(std::string(profile->syntax));
        analyzer = engine_.createAnalyzerBySyntaxName(entry->first);
      } catch (const sweetline::SyntaxCompileError&) {
        return {};
      }
    }
    if (!analyzer) {
      return {};
    }

    const auto highlighted = analyzer->analyzeText(std::string(code));
    const auto offsets = Utf16Boundaries(code);
    const bool dark = 0.2126F * background.red + 0.7152F * background.green + 0.0722F * background.blue < 0.5F;
    std::vector<CodeHighlight> result;
    TextOffset end = 0;
    for (const auto& line : highlighted->lines) {
      for (const auto& span : line.spans) {
        const auto first = span.range.start.index;
        const auto last = span.range.end.index;
        if (first >= last || last >= offsets.size() || offsets[first] < end) {
          continue;
        }
        if (const auto color = TokenColor(engine_.getStyleName(span.style_id), dark)) {
          end = offsets[last];
          result.push_back({{offsets[first], end}, *color});
        }
      }
    }
    return result;
  }

private:
  std::mutex mutex_;
  sweetline::HighlightEngine engine_{sweetline::HighlightConfig{.show_index = true}};
  std::unordered_map<std::string, std::shared_ptr<sweetline::TextAnalyzer>> analyzers_;
};

} // namespace

const CodeBlockHighlighting& DefaultCodeBlockHighlighting() {
  static const auto presentation = [] {
    constexpr std::pair<std::string_view, std::string_view> aliases[]{
        {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"c#", "csharp"}, {"cs", "csharp"},
        {"js", "javascript"}, {"ts", "typescript"}, {"py", "python"}, {"kt", "kotlin"}, {"rs", "rust"},
        {"bash", "shell"}, {"sh", "shell"}, {"zsh", "shell"}, {"yml", "yaml"},
    };
    CodeBlockHighlighting result;
    result.languages.reserve(std::size(profiles));
    for (const auto& profile : profiles) {
      CodeLanguage language{std::string(profile.language.id), std::string(profile.language.label)};
      for (const auto& [alias, id] : aliases) {
        if (id == language.id) {
          language.aliases.emplace_back(alias);
        }
      }
      result.languages.push_back(std::move(language));
    }
    result.highlight = [](std::string_view code, std::string_view language, Color background) {
      static SweetLineHighlighter highlighter;
      return highlighter.Highlight(code, language, background);
    };
    return result;
  }();
  return presentation;
}

} // namespace huxerui::richeditor

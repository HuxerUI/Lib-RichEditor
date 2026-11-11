#include "labels.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include <huxerui/huxerui.h>

using namespace huxerui;

namespace folio {
namespace {

constexpr std::string_view ellipsis = "…";

AttributedText SingleLine(std::string text) {
  std::ranges::replace_if(text, [](unsigned char ch) { return ch < 32; }, ' ');
  for (const auto separator : {"\u0085", "\u2028", "\u2029"}) {
    for (auto index = text.find(separator); index != text.npos; index = text.find(separator, index + 1)) {
      text.replace(index, std::char_traits<char>::length(separator), " ");
    }
  }
  return AttributedText(std::move(text));
}

struct LabelPaint {
  class Extension;

  AttributedText source;
  TextStyle style;
  TextLayoutOptions options;

  bool operator==(const LabelPaint&) const = default;
};

class LabelPaint::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const LabelPaint& spec) : spec_(spec) {}

  void Update(ViewNode&, const LabelPaint& spec) {
    spec_ = spec;
    bounds_ = std::nullopt;
  }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer& measurer) override {
    const auto bounds = node.ContentBounds();
    if (bounds_ == bounds) {
      return PaintInvalidation::None;
    }
    bounds_ = bounds;
    visible_text_ = Fit(measurer, bounds.width);
    return PaintInvalidation::Foreground;
  }

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
    if (!visible_text_.empty()) {
      context.DrawText(node.ContentBounds(), visible_text_, spec_.style, spec_.options);
    }
  }

private:
  std::string Fit(TextMeasurer& measurer, float width) const {
    constexpr float unconstrained = std::numeric_limits<float>::infinity();
    const auto measure = [&](std::string_view text) {
      return measurer.MeasureText(text, spec_.style, unconstrained, spec_.options).size.width;
    };
    const float full_width = measure(spec_.source.PlainText());
    if (full_width <= width) {
      return spec_.source.PlainText();
    }
    if (measure(ellipsis) > width) {
      return {};
    }
    const auto layout = measurer.CreateTextLayout(spec_.source, spec_.style, full_width + 1.0F, spec_.options);
    // Use the paragraph's caret boundaries so truncation preserves combining marks and emoji sequences.
    std::vector<TextOffset> boundaries{0};
    for (TextOffset offset = layout.NextCaretOffset(0); offset < spec_.source.Length();
         offset = layout.NextCaretOffset(offset)) {
      boundaries.push_back(offset);
    }
    const auto truncated = [&](TextOffset end) {
      return spec_.source.TextInRange({0, end}) + std::string(ellipsis);
    };
    const auto end = std::ranges::partition_point(boundaries, [&](TextOffset offset) {
      return measure(truncated(offset)) <= width;
    });
    return truncated(*std::prev(end));
  }

  LabelPaint spec_;
  std::optional<Rect> bounds_;
  std::string visible_text_;
};

} // namespace

[[huxerui::composable]]
View EllipsizedLabel(std::string label, Color color, Semantics semantics, std::optional<Font> font) {
  const auto& theme = UseTheme();
  const TextStyle style{font.value_or(Font::FromFamily(theme.typography.family, theme.typography.label_large)), color};
  const TextLayoutOptions options{
      .shaping = {.locale = std::string(UseEnvironment<Locale>().LanguageTag())},
      .vertical_align = TextVerticalAlign::Center, .wrap = TextWrap::NoWrap,
  };
  auto accessibility = semantics;
  accessibility.role = SemanticRole::Text;
  accessibility.label = label;
  return Row {}.With(Frame{.height = 20.0F}, LabelPaint{SingleLine(label), style, options}, accessibility);
}

[[huxerui::composable]]
View StatusBar(std::string message) {
  const auto& colors = UseTheme().colors;
  const auto label = message.empty() ? View{} :
      EllipsizedLabel(message, colors.on_surface_variant, {
          .identifier = "preview-status", .live_region = SemanticLiveRegion::Polite,
      }).With(Grow(), Tooltip(message));
  return Row {
    label,
  }.With(Frame{.height = 25.0F}, Padding(EdgeInsets::Symmetric(12.0F, 0.0F)),
         CrossAlign(CrossAxisAlignment::Center), Background(colors.surface_container)).Key("status-bar");
}

} // namespace folio

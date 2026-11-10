#include "editor_layout.h"

#include "code_highlighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace huxerui::richeditor::detail {
namespace {

constexpr float image_gap = 12.0F;
constexpr float image_gutter = 7.0F;

void ValidateStyle(const RichEditorStyle& style) {
  const auto nonnegative = [](float value) { return std::isfinite(value) && value >= 0.0F; };
  const auto valid_insets = [&](EdgeInsets insets) {
    return std::ranges::all_of(
        std::array{insets.top, insets.right, insets.bottom, insets.left, insets.Horizontal(), insets.Vertical()},
        nonnegative);
  };
  if (!valid_insets(style.content_padding) || !valid_insets(style.code_block_padding) ||
      !std::ranges::all_of(std::array{style.block_spacing, style.quote_rule_width, style.inline_code_corner_radius,
                                    style.inline_code_horizontal_padding,
                                    style.highlight_corner_radius, style.code_block_corner_radius}, nonnegative)) {
    throw std::invalid_argument("RichEditor padding, spacing, quote rule width, and radii must be finite and nonnegative");
  }
  if (style.heading_font_sizes &&
      !std::ranges::all_of(*style.heading_font_sizes, [](float size) { return std::isfinite(size) && size > 0.0F; })) {
    throw std::invalid_argument("RichEditor heading sizes must be finite and positive");
  }
}

struct InlineFill {
  Color color;
  float corner_radius;
  float horizontal_padding = 0.0F;
  bool operator==(const InlineFill&) const = default;

  bool UsesLayoutGeometry() const { return corner_radius > 0.0F || horizontal_padding > 0.0F; }
};

std::optional<InlineFill> FillFor(Mark marks, const ColorScheme& colors, const RichEditorStyle& editor_style) {
  if (Has(marks, Mark::Highlight)) {
    return InlineFill{editor_style.highlight_background.value_or(colors.tertiary_container),
                      editor_style.highlight_corner_radius};
  }
  if (Has(marks, Mark::Code)) {
    return InlineFill{editor_style.inline_code_background.value_or(colors.surface_container_highest),
                      editor_style.inline_code_corner_radius, editor_style.inline_code_horizontal_padding};
  }
  return std::nullopt;
}

TextSpanStyle VisualStyle(Mark marks, bool link, const TextStyle& base, const Font& code_font,
                          const ColorScheme& colors, const RichEditorStyle& editor_style) {
  TextSpanStyle style;
  TextDecoration decoration = TextDecoration::None;
  if (Has(marks, Mark::Code)) {
    style.font = code_font.WithSize(base.font.Size());
  }
  if (Has(marks, Mark::Bold)) {
    style.font_weight = FontWeight::Bold;
  }
  if (Has(marks, Mark::Italic)) {
    style.font_slant = FontSlant::Italic;
  }
  if (const auto fill = FillFor(marks, colors, editor_style); fill && !fill->UsesLayoutGeometry()) {
    style.background = fill->color;
  }
  if (Has(marks, Mark::Underline) || link) {
    decoration = decoration | TextDecoration::Underline;
  }
  if (Has(marks, Mark::Strike)) {
    decoration = decoration | TextDecoration::StrikeThrough;
  }
  if (decoration != TextDecoration::None) {
    style.decoration = decoration;
  }
  if (link) {
    style.foreground = editor_style.accent_color.value_or(colors.primary);
  }
  return style;
}

AttributedText Attributed(const Block& block, const TextStyle& base, const Font& code_font,
                          const ColorScheme& colors, const RichEditorStyle& style,
                          const CodeBlockHighlighting& code_blocks) {
  const auto& data = block.Data();
  const auto& body = DocumentAccess::Body(block);
  if (data.kind == BlockKind::CodeBlock) {
    if (!code_blocks.highlight || data.text.empty()) {
      return body;
    }
    const auto language = CodeLanguageId(data.fence_info, code_blocks);
    if (language == "text" || std::ranges::find(code_blocks.languages, language, &CodeLanguage::id) ==
        code_blocks.languages.end()) {
      return body;
    }
    const auto background = style.code_block_background.value_or(colors.surface_container_highest);
    const auto highlights = code_blocks.highlight(data.text, language, background);
    std::vector<TextStyleRange> ranges;
    ranges.reserve(highlights.size());
    for (const auto& highlight : highlights) {
      ranges.push_back({highlight.range, {.foreground = highlight.foreground}});
    }
    try {
      return body.WithStyles(std::move(ranges));
    } catch (const std::invalid_argument&) {
      return body;
    }
  }
  std::vector<TextOffset> boundaries;
  for (const auto& run : data.marks) {
    boundaries.insert(boundaries.end(), {run.range.start, run.range.end});
  }
  for (const auto& link : data.links) {
    boundaries.insert(boundaries.end(), {link.range.start, link.range.end});
  }
  std::ranges::sort(boundaries);
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
  std::vector<TextStyleRange> styles;
  auto mark = data.marks.begin();
  auto link = data.links.begin();
  for (std::size_t i = 1; i < boundaries.size(); ++i) {
    const auto start = boundaries[i - 1];
    while (mark != data.marks.end() && mark->range.end <= start) {
      ++mark;
    }
    while (link != data.links.end() && link->range.end <= start) {
      ++link;
    }
    const auto marks = mark != data.marks.end() && mark->range.start <= start ? mark->marks : Mark::None;
    const bool linked = link != data.links.end() && link->range.start <= start;
    auto span_style = VisualStyle(marks, linked, base, code_font, colors, style);
    if (span_style != TextSpanStyle{}) {
      styles.push_back({{start, boundaries[i]}, std::move(span_style)});
    }
  }
  return body.WithStyles(std::move(styles));
}

TextStyle BlockStyle(const BlockData& data, TextStyle body, const TextStyle& code, const RichEditorStyle& style) {
  if (data.kind == BlockKind::CodeBlock) {
    return code;
  }
  if (data.kind == BlockKind::Heading) {
    const auto size = style.heading_font_sizes ? (*style.heading_font_sizes)[data.heading_level - 1]
                                               : body.font.Size() + 2.0F * static_cast<float>(7 - data.heading_level);
    body.font = body.font.WithSize(size).WithWeight(FontWeight::Bold);
  }
  return body;
}

struct InlineFillRange {
  TextRange range;
  InlineFill fill;
};

std::vector<InlineFillRange> CustomInlineFills(const BlockData& data, const ColorScheme& colors,
                                             const RichEditorStyle& style) {
  std::vector<InlineFillRange> fills;
  // Keep one background and pair of insets across changes to other marks within a code span.
  for (const auto& run : data.marks) {
    const auto fill = FillFor(run.marks, colors, style);
    if (!fill || !fill->UsesLayoutGeometry()) {
      continue;
    }
    if (!fills.empty() && fills.back().fill == *fill && fills.back().range.end == run.range.start) {
      fills.back().range.end = run.range.end;
    } else {
      fills.push_back({run.range, *fill});
    }
  }
  return fills;
}

struct InlineSpacer {
  std::string text;
  Font font;
  TextOffset length;
};

std::optional<InlineSpacer> MeasureInlineSpacer(TextMeasurer& measurer, const Font& font, float padding,
                                                float available, const TextShapingOptions& shaping) {
  constexpr std::string_view nonbreaking_space = "\xc2\xa0";
  const float advance = measurer.MeasureRun(nonbreaking_space, TextStyle{font}, shaping).advance;
  if (!(advance > 0.0F) || !std::isfinite(advance)) {
    return std::nullopt;
  }
  const float width = std::min(padding, available / 4.0F);
  // Multiple spaces accommodate a larger inset without increasing the font's line height.
  const float count = std::clamp(std::ceil(width / advance), 1.0F, 32.0F);
  const float size = font.Size() * std::min(1.0F, (width / count) / advance);
  if (!(size > 0.0F)) {
    return std::nullopt;
  }
  const auto length = static_cast<TextOffset>(count);
  std::string text;
  for (TextOffset i = 0; i < length; ++i) {
    text += nonbreaking_space;
  }
  return InlineSpacer{std::move(text), font.WithSize(size), length};
}

AttributedText PadInlineCode(const AttributedText& source, const TextLayout& unpadded,
                             const std::vector<InlineFillRange>& fills, const InlineSpacer& spacer,
                             InlineOffsets& offsets) {
  std::string text;
  std::vector<TextStyleRange> styles;
  const auto source_styles = source.StyleRanges();
  auto style = source_styles.begin();
  TextOffset source_start = 0;
  TextOffset layout_start = 0;
  const auto append_source = [&](TextOffset end) {
    if (source_start == end) {
      return;
    }
    text += source.TextInRange({source_start, end});
    while (style != source_styles.end() && style->range.start < end) {
      const auto start = std::max(source_start, style->range.start);
      const auto stop = std::min(end, style->range.end);
      styles.push_back({{layout_start + start - source_start, layout_start + stop - source_start}, style->style});
      if (style->range.end > end) {
        break;
      }
      ++style;
    }
    layout_start += end - source_start;
    source_start = end;
  };
  const auto append_space = [&](TextOffset offset) {
    append_source(offset);
    const TextRange range{layout_start, layout_start + spacer.length};
    text += spacer.text;
    styles.push_back({range, {.font = spacer.font}});
    offsets.insertions.push_back({offset, range});
    layout_start = range.end;
  };
  const auto is_caret_boundary = [&](TextOffset offset) {
    return offset == 0 || offset == source.Length() ||
        unpadded.NextCaretOffset(unpadded.PreviousCaretOffset(offset)) == offset;
  };

  for (const auto& [range, fill] : fills) {
    if (fill.horizontal_padding == 0.0F) {
      continue;
    }
    const auto content = source.TextInRange(range);
    const auto first = content.find_first_not_of('\n');
    if (first == content.npos) {
      continue;
    }
    const auto last = content.find_last_not_of('\n');
    const auto start = range.start + static_cast<TextOffset>(first);
    const auto end = range.end - static_cast<TextOffset>(content.size() - 1 - last);
    // A mark may start inside a grapheme. Inserting space there would split the cluster.
    if (is_caret_boundary(start) && is_caret_boundary(end)) {
      append_space(start);
      append_space(end);
    }
  }
  if (offsets.insertions.empty()) {
    return source;
  }
  append_source(source.Length());
  return AttributedText::FromRanges(std::move(text), std::move(styles));
}

std::vector<EditorLayout::InlineBackground> InlineBackgrounds(const TextLayout& layout, const InlineOffsets& offsets,
                                                               const std::vector<InlineFillRange>& fills) {
  std::vector<EditorLayout::InlineBackground> backgrounds;
  for (const auto& [range, fill] : fills) {
    if (fill.color.alpha == 0.0F) {
      continue;
    }
    const bool padded = fill.horizontal_padding > 0.0F;
    const auto start = offsets.ToLayout({range.start, padded ? TextAffinity::Upstream : TextAffinity::Downstream});
    const auto end = offsets.ToLayout({range.end, padded ? TextAffinity::Downstream : TextAffinity::Upstream});
    for (const auto& rect : layout.RangeRects({start.offset, end.offset})) {
      if (!rect.IsEmpty()) {
        backgrounds.push_back({rect, fill.color, fill.corner_radius});
      }
    }
  }
  return backgrounds;
}

} // namespace

TextPosition InlineOffsets::ToLayout(TextPosition position) const {
  const auto next = std::ranges::lower_bound(insertions, position.offset, {}, &Insertion::source);
  if (next != insertions.end() && next->source == position.offset) {
    position.offset = position.affinity == TextAffinity::Upstream ? next->layout.start : next->layout.end;
  } else if (next != insertions.begin()) {
    const auto& previous = *std::prev(next);
    position.offset += previous.layout.end - previous.source;
  }
  return position;
}

TextPosition InlineOffsets::ToSource(TextPosition position) const {
  const auto next = std::ranges::upper_bound(insertions, position.offset, {},
      [](const Insertion& insertion) { return insertion.layout.start; });
  if (next != insertions.begin()) {
    const auto& previous = *std::prev(next);
    if (position.offset <= previous.layout.end) {
      const bool before = position.offset - previous.layout.start < previous.layout.end - position.offset;
      return {previous.source, before ? TextAffinity::Upstream : TextAffinity::Downstream};
    }
    position.offset -= previous.layout.end - previous.source;
  }
  return position;
}

TextOffset EditorLayout::Row::AdjacentCaretOffset(TextOffset offset, bool forward) const {
  const auto start = offsets.ToLayout({offset, forward ? TextAffinity::Downstream : TextAffinity::Upstream}).offset;
  const auto next = forward ? layout.NextCaretOffset(start) : layout.PreviousCaretOffset(start);
  return offsets.ToSource({next}).offset;
}

std::size_t EditorLayout::Row::LineIndex(TextPosition position) const {
  return layout.LineIndex(offsets.ToLayout(position));
}

TextRange EditorLayout::Row::LineRange(std::size_t line) const {
  const auto range = layout.Lines()[line].range;
  return {offsets.ToSource({range.start}).offset, offsets.ToSource({range.end}).offset};
}

bool EditorLayout::Prepare(TextMeasurer& measurer, const RichDocument& document, float width, const ThemeSpec& theme,
                           const RichEditorStyle& style, const std::string& locale, const ImageAssets& images,
                           std::uint64_t image_revision, std::optional<ImageWidthPreview> image_preview,
                           const RichEditorExtensions& extensions) {
  const bool style_changed = style_ != style;
  const bool reflow = width_ != width || theme_ != theme || style_changed || locale_ != locale ||
      code_blocks_ != extensions.code_blocks || custom_blocks_ != extensions.custom_blocks;
  if (document_ && DocumentAccess::Same(*document_, document) && !reflow && image_revision_ == image_revision &&
      image_preview_ == image_preview) {
    return false;
  }
  if (style_changed) {
    ValidateStyle(style);
  }
  if (reflow) {
    cache_.clear();
  }
  width_ = width;
  theme_ = theme;
  style_ = style;
  code_blocks_ = extensions.code_blocks;
  custom_blocks_ = extensions.custom_blocks;
  const auto& code_presentation = code_blocks_ ? *code_blocks_ : DefaultCodeBlockHighlighting();
  locale_ = locale;
  image_revision_ = image_revision;
  image_preview_ = image_preview;
  document_ = document;
  rows_.clear();
  quote_rules_.clear();
  std::unordered_map<BlockId, float> table_offsets;
  for (const auto& table : tables_) {
    if (table.offset > 0.0F) {
      table_offsets.emplace(table.id, table.offset);
    }
  }
  tables_.clear();
  std::unordered_map<BlockId, CachedLayout> next_cache;
  std::unordered_map<BlockId, std::size_t> quote_indices;
  std::vector<std::size_t> leaf_quote_rules;
  const TextStyle body{
      style.body_font.value_or(Font::FromFamily(theme.typography.family, theme.typography.body_large)),
      style.text_color.value_or(theme.colors.on_surface),
  };
  const TextStyle code{
      style.code_font.value_or(Font::Monospace(body.font.Size())),
      style.code_block_text_color.value_or(body.foreground),
  };
  marker_style_ = {body.font, style.list_marker_color.value_or(theme.colors.on_surface_variant)};
  const auto& padding = style.content_padding;
  const float indent_scale = std::max(1.0F, body.font.Size() / 16.0F);
  const auto append = [&](const Leaf& leaf, Point origin, float block_width, EdgeInsets insets,
                          std::vector<ListMarker> markers, bool header, std::optional<Cell> cell = {}) {
    const auto& data = leaf.block.Data();
    const float available = std::max(1.0F, block_width - insets.Horizontal());
    const auto id = DocumentAccess::Id(leaf.block);
    const auto found = cache_.find(id);
    const auto create = [&] {
      auto base = BlockStyle(data, body, code, style);
      if (header) {
        base.font = base.font.WithWeight(FontWeight::Bold);
      }
      const TextLayoutOptions options{.shaping = {.locale = locale}};
      const auto attributed = Attributed(leaf.block, base, code.font, theme.colors, style, code_presentation);
      auto layout = measurer.CreateTextLayout(attributed, base, available, options);
      const auto fills = CustomInlineFills(data, theme.colors, style);
      InlineOffsets offsets;
      if (std::ranges::any_of(fills, [](const InlineFillRange& fill) { return fill.fill.horizontal_padding > 0.0F; })) {
        const auto spacer = MeasureInlineSpacer(measurer, code.font.WithSize(base.font.Size()),
                                                style.inline_code_horizontal_padding, available, options.shaping);
        if (spacer) {
          const auto padded = PadInlineCode(attributed, layout, fills, *spacer, offsets);
          if (!offsets.insertions.empty()) {
            layout = measurer.CreateTextLayout(padded, base, available, options);
          }
        }
      }
      auto backgrounds = InlineBackgrounds(layout, offsets, fills);
      std::optional<CustomBlockGraphic> graphic;
      if (data.custom) {
        const auto presenter = std::ranges::find(custom_blocks_, data.custom->type, &CustomBlockExtension::type);
        if (presenter != custom_blocks_.end() && presenter->render && *presenter->render) {
          graphic = (*presenter->render)(*data.custom, base.foreground, base.font.Size());
        }
      }
      return CachedLayout{leaf.block, available, std::move(layout), std::move(offsets), std::move(backgrounds),
                          std::move(graphic), header};
    };
    auto cached = found != cache_.end() && found->second.width == available && found->second.block == leaf.block &&
            found->second.header == header ? found->second : create();
    std::optional<Rect> code_bounds;
    std::optional<CodeHeader> code_header;
    float header_height = 0.0F;
    if (data.kind == BlockKind::CodeBlock) {
      header_height = 38.0F;
      const auto inset = std::min(insets.left, block_width / 4.0F);
      auto label = CodeLanguageLabel(data.fence_info, code_presentation);
      const TextStyle label_style{
          body.font.WithSize(12.0F).WithWeight(FontWeight::Medium),
          style.code_block_text_color.value_or(theme.colors.on_surface_variant),
      };
      const float label_width = measurer.MeasureRun(label, label_style, {.locale = locale}).advance;
      code_header = CodeHeader{
          {origin.x + inset, origin.y, std::min(label_width + 34.0F, block_width - 2.0F * inset),
           header_height},
          std::move(label), label_style,
      };
      code_bounds = Rect{origin.x, origin.y, block_width,
                         cached.layout.Metrics().size.height + insets.Vertical() + header_height};
    }
    rows_.push_back({
        .origin = {origin.x + insets.left, origin.y + insets.top + header_height},
        .layout = cached.layout,
        .offsets = cached.offsets,
        .markers = std::move(markers),
        .code_bounds = code_bounds,
        .code_header = std::move(code_header),
        .inline_backgrounds = cached.inline_backgrounds,
        .cell = cell,
    });
    if (data.custom) {
      const float inset = std::min(12.0F, block_width / 4.0F);
      const auto label = data.custom->fallback_text.empty() ? data.custom->type : data.custom->fallback_text;
      const auto available_width = std::max(1.0F, block_width - 2.0F * inset);
      auto graphic = cached.custom_graphic;
      std::optional<TextLayout> fallback;
      Size size{};
      if (graphic) {
        size = std::visit([](const auto& asset) { return asset.IntrinsicSize(); }, *graphic);
        if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0.0F || size.height <= 0.0F) {
          graphic.reset();
        }
      }
      if (graphic) {
        const auto scale = std::min({1.0F, available_width / size.width, 480.0F / size.height});
        size = {size.width * scale, size.height * scale};
      } else {
        fallback = measurer.CreateTextLayout(AttributedText(label), code, available_width, {.align = TextAlign::Center});
        size = {available_width, std::min(fallback->Metrics().size.height, 480.0F)};
      }
      rows_.back().custom = CustomBox{{origin.x, origin.y, block_width, size.height + 2.0F * inset},
                                     {origin.x + (block_width - size.width) / 2.0F, origin.y + inset,
                                      size.width, size.height},
                                     std::move(graphic), std::move(fallback), label};
    }
    next_cache.emplace(id, std::move(cached));
  };

  const auto& leaves = DocumentAccess::Get(document).leaves;
  const float content_width = std::max(1.0F, width - padding.Horizontal());
  float y = padding.top;
  for (std::size_t i = 0; i < leaves.size();) {
    const auto& leaf = leaves[i];
    if (const auto cell = TableCellAt(document, leaf)) {
      const auto& block = document.Blocks()[cell->table];
      const auto& table = block.Data();
      const auto columns = table.children.front().Data().children.size();
      const auto rows = table.children.size();
      const float viewport_width = content_width;
      const float column_width = std::max({88.0F, body.font.Size() * 5.0F,
                                           viewport_width / static_cast<float>(columns)});
      const auto table_index = tables_.size();
      auto& grid = tables_.emplace_back(TableGrid{DocumentAccess::Id(block),
          {padding.left, y, viewport_width, 0.0F}, i, rows, columns, column_width, table.table_has_header});
      const auto old = table_offsets.find(grid.id);
      if (old != table_offsets.end()) {
        grid.offset = std::min(old->second, grid.MaxOffset());
      }
      const auto insets = EdgeInsets::Symmetric(10.0F, 8.0F);
      for (std::size_t row = 0; row < rows; ++row) {
        const auto first = i;
        float height = body.font.Size();
        for (std::size_t column = 0; column < columns; ++column) {
          const float x = padding.left + static_cast<float>(column) * column_width;
          append(leaves[i++], {x, y}, column_width, insets, {}, grid.has_header && row == 0,
                 Cell{table_index, {x, y, column_width, 0.0F}});
          height = std::max(height, rows_.back().layout.Metrics().size.height);
        }
        height += insets.Vertical();
        for (auto c = first; c < i; ++c) {
          rows_[c].cell->bounds.height = height;
        }
        y += height;
      }
      grid.viewport.height = y - grid.viewport.y;
      y += style.block_spacing;
      continue;
    }
    if (leaf.block.Data().image) {
      const auto first = i;
      float used = 0.0F;
      float height = 0.0F;
      for (; i < leaves.size(); ++i) {
        const auto& item = leaves[i];
        const auto& image = item.block.Data().image;
        if (!image) {
          break;
        }
        const auto found = images.find(image->source);
        const auto asset = found != images.end() ? found->second : ImageAsset{};
        const float automatic_width = asset.HasValue() ? asset.IntrinsicSize().width : content_width;
        const float preferred = image_preview && image_preview->block == DocumentAccess::Id(item.block)
            ? image_preview->width : image->width.value_or(automatic_width);
        const float image_width = std::min(content_width, preferred);
        if (used + image_width > content_width) {
          break;
        }
        const float image_height = asset.HasValue() ? ImageHeight(asset, image_width) : 100.0F;
        const float x = padding.left + used;
        // The object's text projection is independent of its preview width.
        append(item, {x, y}, content_width, {}, {}, false);
        rows_.back().image = ImageBox{
            .bounds = {x, y + image_gutter, image_width, image_height},
            .asset = asset,
            .label = image->alt_text.empty() ? "Image" : image->alt_text,
            .line_first = first,
        };
        height = std::max(height, image_height);
        used += image_width + image_gap;
      }
      const float bottom = y + height + 2.0F * image_gutter;
      for (auto member = first; member < i; ++member) {
        rows_[member].image->line_end = i;
        rows_[member].image->line_bottom = bottom;
      }
      y = bottom + style.block_spacing;
      continue;
    }
    ++i;
    float x = padding.left;
    leaf_quote_rules.clear();
    std::vector<ListMarker> markers;
    for (std::size_t depth = 0; depth + 1 < leaf.path.size(); ++depth) {
      const auto& container = BlockAt(document, std::span(leaf.path).first(depth + 1));
      const auto& data = container.Data();
      if (data.kind == BlockKind::Quote) {
        if (style.quote_rule_width > 0.0F) {
          const auto [entry, inserted] = quote_indices.try_emplace(DocumentAccess::Id(container), quote_rules_.size());
          if (inserted) {
            quote_rules_.push_back({x, y, style.quote_rule_width, 0.0F});
          }
          leaf_quote_rules.push_back(entry->second);
        }
        x += style.quote_rule_width + 12.0F * indent_scale;
      } else if (data.kind == BlockKind::List) {
        const auto start = static_cast<std::uint64_t>(data.list_start);
        const std::string marker = data.ordered
            ? std::to_string(start + leaf.path[depth + 1]) + "."
            : "•";
        // Keep item text aligned when the list crosses a digit boundary.
        const float indent = (data.ordered
            ? std::max(24.0F, 9.0F * (std::to_string(start + data.children.size() - 1).size() + 1) + 5.0F)
            : 22.0F) * indent_scale;
        const auto inside_item = std::span(leaf.path).subspan(depth + 2);
        if (std::ranges::all_of(inside_item, [](std::size_t index) { return index == 0; })) {
          markers.push_back({marker, x, indent - 5.0F});
        }
        x += indent;
      }
    }
    x = std::min(x, std::max(padding.left, width - padding.right - 32.0F));
    const auto& data = leaf.block.Data();
    const bool is_code = data.kind == BlockKind::CodeBlock;
    const auto code_padding = is_code ? style.code_block_padding : EdgeInsets{};
    const float block_width = std::max(0.0F, width - x - padding.right);
    append(leaf, {x, y}, block_width, code_padding, std::move(markers), false);
    const float bottom = rows_.back().Bottom();
    for (const auto index : leaf_quote_rules) {
      auto& rule = quote_rules_[index];
      rule.height = bottom - rule.y;
    }
    y = bottom + style.block_spacing;
  }
  cache_ = std::move(next_cache);
  height_ = rows_.back().Bottom() + padding.bottom;
  return true;
}

bool EditorLayout::IsCurrent(const RichDocument& document) const {
  return document_ && DocumentAccess::Same(*document_, document);
}

Point EditorLayout::Origin(const Row& row) const {
  auto origin = row.origin;
  if (row.cell) {
    origin.x -= tables_[row.cell->table].offset;
  }
  return origin;
}

Rect EditorLayout::VisibleCell(const Row& row) const {
  const auto& table = tables_[row.cell->table];
  auto bounds = row.cell->bounds;
  bounds.x -= table.offset;
  return bounds.Intersection(table.viewport);
}

std::optional<std::size_t> EditorLayout::ScrollableTableAt(Point point) const {
  const auto table = std::ranges::find_if(tables_, [point](const TableGrid& grid) {
    return grid.MaxOffset() > 0.0F && grid.viewport.Contains(point);
  });
  return table == tables_.end() ? std::nullopt : std::optional(static_cast<std::size_t>(table - tables_.begin()));
}

bool EditorLayout::ScrollTable(std::size_t index, float delta) {
  auto& table = tables_[index];
  const float offset = std::clamp(table.offset + delta, 0.0F, table.MaxOffset());
  return std::exchange(table.offset, offset) != offset;
}

bool EditorLayout::RevealCaret(const RichDocument& document, TextPosition position) {
  const auto& row = rows_[LeafIndex(document, position.offset)];
  if (!row.cell) {
    return false;
  }
  const auto& table = tables_[row.cell->table];
  const auto caret = Caret(document, position);
  const float margin = std::min(5.0F, table.viewport.width / 3.0F);
  const float left = table.viewport.x + margin;
  const float right = table.viewport.x + table.viewport.width - margin;
  if (caret.x < left || caret.x + caret.width > right) {
    return ScrollTable(row.cell->table, caret.x < left ? caret.x - left : caret.x + caret.width - right);
  }
  return false;
}

std::size_t EditorLayout::LeafInRow(std::size_t index, float x) const {
  if (const auto& image = rows_[index].image) {
    index = image->line_first;
    while (index + 1 < image->line_end && x >= rows_[index + 1].origin.x - image_gap / 2.0F) {
      ++index;
    }
    return index;
  }
  if (!rows_[index].cell) {
    return index;
  }
  const auto& table = tables_[rows_[index].cell->table];
  const auto row = (index - table.first) / table.columns;
  const float right = std::nextafter(table.viewport.width, 0.0F);
  const float local_x = std::clamp(x - table.viewport.x, 0.0F, right);
  const auto column = std::min(static_cast<std::size_t>((local_x + table.offset) / table.column_width),
                               table.columns - 1);
  return table.first + row * table.columns + column;
}

std::size_t EditorLayout::AdjacentRow(std::size_t index, bool down, float x) const {
  auto boundary = index;
  if (const auto& cell = rows_[index].cell) {
    const auto& table = tables_[cell->table];
    const auto row = (index - table.first) / table.columns;
    if (down && row + 1 < table.rows) {
      return index + table.columns;
    }
    if (!down && row > 0) {
      return index - table.columns;
    }
    boundary = down ? table.first + table.rows * table.columns - 1 : table.first;
  } else if (const auto& image = rows_[index].image) {
    boundary = down ? image->line_end - 1 : image->line_first;
  }
  if (down ? boundary + 1 < rows_.size() : boundary > 0) {
    return LeafInRow(down ? boundary + 1 : boundary - 1, x);
  }
  return index;
}

TextPosition EditorLayout::Hit(const RichDocument& document, Point point) const {
  auto row = std::ranges::lower_bound(rows_, point.y, {}, [&](const Row& value) {
    return value.Bottom() + style_.block_spacing / 2.0F;
  });
  if (row == rows_.end()) {
    row = std::prev(row);
  }
  const auto index = static_cast<std::size_t>(row - rows_.begin());
  return Hit(document, LeafInRow(index, point.x), point);
}

TextPosition EditorLayout::Hit(const RichDocument& document, std::size_t index, Point point) const {
  const auto& row = rows_[index];
  if (const auto* atomic = row.AtomicBounds()) {
    const auto& bounds = *atomic;
    return {DocumentAccess::Get(document).leaves[index].start + (point.y >= bounds.y + bounds.height / 2.0F)};
  }
  auto result = row.offsets.ToSource(row.layout.HitTest(point - Origin(row)));
  result.offset += DocumentAccess::Get(document).leaves[index].start;
  return result;
}

Rect EditorLayout::Caret(const RichDocument& document, TextPosition position) const {
  const auto index = LeafIndex(document, position.offset);
  const auto& row = rows_[index];
  position.offset -= DocumentAccess::Get(document).leaves[index].start;
  if (const auto* atomic = row.AtomicBounds()) {
    const auto& bounds = *atomic;
    return {bounds.x, position.offset == 0 ? row.origin.y : bounds.y + bounds.height,
            1.5F, std::max(8.0F, bounds.y - row.origin.y)};
  }
  return AtOrigin(row.layout.CaretRect(row.offsets.ToLayout(position)), Origin(row));
}

std::vector<Rect> EditorLayout::Ranges(const RichDocument& document, TextRange range) const {
  const auto& leaves = DocumentAccess::Get(document).leaves;
  std::vector<Rect> result;
  const auto last = LeafIndex(document, range.end);
  for (auto i = LeafIndex(document, range.start); i <= last; ++i) {
    const auto& leaf = leaves[i];
    const auto& row = rows_[i];
    const auto end = leaf.start + DocumentAccess::Body(leaf.block).Length();
    const auto start = std::max(range.start, leaf.start);
    const auto stop = std::min(range.end, end);
    const auto append = [&](Rect rect) {
      if (row.cell) {
        rect = rect.Intersection(VisibleCell(row));
      }
      if (!rect.IsEmpty()) {
        result.push_back(rect);
      }
    };
    if (start < stop) {
      if (const auto* atomic = row.AtomicBounds()) {
        append(*atomic);
      } else {
        auto fragments = TextFragments(row, {start - leaf.start, stop - leaf.start});
        result.insert(result.end(), fragments.begin(), fragments.end());
      }
    }
    if (i + 1 < leaves.size() && range.start <= end && range.end > end) {
      auto separator = Caret(document, {end, TextAffinity::Upstream});
      separator.width = 6.0F;
      append(separator);
    }
  }
  return result;
}

std::vector<Rect> EditorLayout::TextFragments(const Row& row, TextRange range) const {
  const auto from = row.offsets.ToLayout({range.start, TextAffinity::Downstream});
  const auto to = row.offsets.ToLayout({range.end, TextAffinity::Upstream});
  auto fragments = row.layout.RangeRects({from.offset, to.offset});
  for (auto& rect : fragments) {
    rect = AtOrigin(rect, Origin(row));
    if (row.cell) {
      rect = rect.Intersection(VisibleCell(row));
    }
  }
  std::erase_if(fragments, [](const Rect& rect) { return rect.IsEmpty(); });
  return fragments;
}

} // namespace huxerui::richeditor::detail

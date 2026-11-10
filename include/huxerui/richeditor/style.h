#pragma once

/// @file
/// @brief Presentation settings for the mounted rich-text editor.
///
/// Set RichEditorOptions::style when declaring the view. Use a new value in composition to change
/// its appearance while retaining the controller's document, selection, and history.

#include <array>
#include <optional>

#include <huxerui/paint.h>

namespace huxerui::richeditor {

/// @brief Color, typography, and spacing overrides for RichEditor().
///
/// Unset colors and fonts use the enclosing HuxerUI theme. Distances use logical units and must be finite and
/// nonnegative. Explicit heading sizes must be finite and positive.
/// Invalid geometry or sizes throw std::invalid_argument when the editor prepares its layout.
/// @code{.cpp}
/// RichEditorStyle style;
/// style.background = Color::Rgb(250, 249, 246);
/// style.body_font = Font::System(18.0F);
/// style.content_padding = EdgeInsets::All(24.0F);
/// style.inline_code_corner_radius = 4.0F;
/// style.inline_code_horizontal_padding = 3.0F;
/// style.code_block_corner_radius = 12.0F;
/// return RichEditor(editor, {.style = style});
/// @endcode
struct RichEditorStyle {
  /// Viewport background, including the area below the last block. Defaults to the theme surface.
  std::optional<Brush> background{};
  /// Base font for paragraphs, headings, and list markers. Defaults to the theme family and body_large size.
  std::optional<Font> body_font{};
  /// Font for code blocks. Inline code uses this font at the surrounding text size.
  /// Defaults to a monospace font at the body size.
  std::optional<Font> code_font{};
  /// Heading font sizes in level order from H1 to H6. Defaults to the body size plus 12 through 2 logical units.
  std::optional<std::array<float, 6>> heading_font_sizes{};
  /// Foreground for ordinary text and headings. Defaults to on_surface.
  std::optional<Color> text_color{};
  /// Link, caret, selection, and IME accent. Defaults to the theme primary color.
  std::optional<Color> accent_color{};
  /// Foreground for list markers. Defaults to on_surface_variant.
  std::optional<Color> list_marker_color{};
  /// Color of the vertical rule for quotes. Defaults to outline.
  std::optional<Color> quote_rule_color{};
  /// Background for inline code. Defaults to surface_container_highest.
  std::optional<Color> inline_code_background{};
  /// Background for highlighted text. Defaults to tertiary_container and takes precedence over inline code.
  std::optional<Color> highlight_background{};
  /// Background for code blocks. Defaults to surface_container_highest.
  std::optional<Color> code_block_background{};
  /// Foreground for code blocks. Defaults to the editor's text color.
  std::optional<Color> code_block_text_color{};
  /// Space between the viewport edge and the document content.
  EdgeInsets content_padding = EdgeInsets::All(16.0F);
  /// Insets between a code block's background and its text.
  EdgeInsets code_block_padding = EdgeInsets::Symmetric(8.0F, 6.0F);
  /// Vertical spacing between text blocks.
  float block_spacing = 8.0F;
  /// Width of the rounded rule beside a quote. A value of zero hides the rule.
  float quote_rule_width = 2.0F;
  /// Radius for each visual fragment of inline code. Zero draws square corners.
  float inline_code_corner_radius = 0.0F;
  /// Radius for each visual fragment of highlighted text. Zero draws square corners.
  float highlight_corner_radius = 0.0F;
  /// Radius of the background surrounding a complete code block.
  float code_block_corner_radius = 5.0F;
  /// Horizontal inset on each side of a contiguous inline code span, in logical units. Defaults to zero.
  /// The inset participates in wrapping and applies at the span's start and end. Narrow text columns can reduce
  /// the inset. A highlight takes precedence over the inline code background and its padding.
  float inline_code_horizontal_padding = 0.0F;
  bool operator==(const RichEditorStyle&) const = default;
};

} // namespace huxerui::richeditor

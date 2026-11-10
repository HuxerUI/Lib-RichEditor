#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/richeditor/style.h>
#include <huxerui/richeditor/extension.h>
#include <huxerui/resource.h>
#include <huxerui/theme.h>

#include "document_internal.h"

namespace huxerui::richeditor::detail {

using ImageAssets = std::unordered_map<std::string, ImageAsset>;

/// Retains a completed render and the request inputs layout must match before reusing it.
struct ResolvedInlineGraphic {
  InlineObjectData data;
  InlineObjectRenderContext context;
  std::shared_ptr<const InlineObjectRenderer> renderer;
  InlineObjectGraphic graphic;
};
/// Completed inline graphics keyed by transient object ID.
using InlineGraphics = std::unordered_map<std::uint64_t, std::shared_ptr<const ResolvedInlineGraphic>>;

/// Resolves the same appearance at a block-local UTF-16 offset for request scheduling and layout.
InlineObjectRenderContext ResolveInlineContext(const BlockData& data, TextOffset offset, const ThemeSpec& theme,
                                               const RichEditorStyle& style, const std::string& locale,
                                               const std::optional<File>& directory);

inline float ImageHeight(const ImageAsset& asset, float width) {
  return width * (static_cast<float>(asset.PixelHeight()) / static_cast<float>(asset.PixelWidth()));
}

/// Temporary width used during a gesture. The document snapshot is committed only when the gesture finishes.
struct ImageWidthPreview {
  BlockId block;
  float width;
  bool operator==(const ImageWidthPreview&) const = default;
};

inline Rect AtOrigin(Rect rect, Point origin) {
  rect.x += origin.x;
  rect.y += origin.y;
  return rect;
}

/// Maps a leaf's UTF-16 positions to and from its shaped text, which may reserve space at inline code edges.
struct InlineOffsets {
  struct Insertion {
    TextOffset source;
    TextRange layout;
  };

  TextPosition ToLayout(TextPosition position) const;
  TextPosition ToSource(TextPosition position) const;

  /// Insertions in source and layout order. The whole inserted range belongs to a single source offset.
  std::vector<Insertion> insertions;
};

/// @brief Owns one text layout per leaf in the visible document snapshot.
///
/// Use on the UI thread that creates the TextLayout values. Query offsets in the document's flattened UTF-16
/// text. Points and rectangles use content coordinates before the view applies its origin and scroll offset.
class EditorLayout {
public:
  struct ListMarker {
    std::string text;
    float left;
    float width;
  };

  struct InlineBackground {
    Rect bounds;
    Color color;
    float corner_radius;
  };

  struct Cell {
    std::size_t table;
    /// Cell bounds before applying the table's horizontal offset.
    Rect bounds;
  };

  struct TableGrid {
    BlockId id;
    Rect viewport;
    std::size_t first;
    std::size_t rows;
    std::size_t columns;
    float column_width;
    bool has_header;
    float offset = 0.0F;

    float Width() const { return column_width * static_cast<float>(columns); }
    float MaxOffset() const {
      const float overflow = Width() - viewport.width;
      return overflow > 0.5F ? overflow : 0.0F;
    }
  };

  struct ImageBox {
    Rect bounds;
    ImageAsset asset;
    std::string label;
    /// The shared vertical band of the consecutive images that fit on this line.
    std::size_t line_first;
    std::size_t line_end;
    float line_bottom;
  };

  struct CodeHeader {
    Rect bounds;
    std::string label;
    TextStyle style;
  };

  struct CustomBox {
    Rect bounds;
    Rect content;
    std::optional<CustomBlockGraphic> graphic;
    std::optional<TextLayout> fallback;
    std::string label;
  };

  /// Layout and accessibility data for one atomic object on a text row.
  struct InlineBox {
    TextOffset offset; ///< Block-local UTF-16 offset in the document projection.
    std::uint64_t id;   ///< Transient object ID shared by rendering and interaction.
    Rect bounds;
    std::optional<CustomBlockGraphic> graphic;
    std::optional<TextLayout> fallback;
    std::string label;
  };

  /// One text layout and its decorations for a document leaf.
  struct Row {
    Point origin;
    TextLayout layout;
    InlineOffsets offsets;
    std::vector<ListMarker> markers;
    std::optional<Rect> code_bounds;
    std::optional<CodeHeader> code_header;
    std::vector<InlineBackground> inline_backgrounds;
    std::vector<InlineBox> inline_objects;
    std::optional<Cell> cell;
    std::optional<ImageBox> image;
    std::optional<CustomBox> custom;

    const Rect* AtomicBounds() const {
      return image ? &image->bounds : custom ? &custom->bounds : nullptr;
    }

    TextOffset AdjacentCaretOffset(TextOffset offset, bool forward) const;
    std::size_t LineIndex(TextPosition position) const;
    TextRange LineRange(std::size_t line) const;

    float Top() const { return cell ? cell->bounds.y : code_bounds ? code_bounds->y : origin.y; }
    float Bottom() const {
      if (image) {
        return image->line_bottom;
      }
      if (custom) {
        return custom->bounds.y + custom->bounds.height;
      }
      if (cell) {
        return cell->bounds.y + cell->bounds.height;
      }
      return code_bounds ? code_bounds->y + code_bounds->height : origin.y + layout.Metrics().size.height;
    }
  };

  /// @brief Prepares rows and their decorations for the current viewport width, appearance, and shaping locale.
  /// Unchanged leaves reuse their cached text layout and inline backgrounds when the layout inputs match.
  /// @param width Positive viewport width in logical units.
  /// @return True if the rows were rebuilt.
  bool Prepare(TextMeasurer& measurer, const RichDocument& document, float width, const ThemeSpec& theme,
               const RichEditorStyle& style, const std::string& locale, const ImageAssets& images,
               std::uint64_t image_revision, std::optional<ImageWidthPreview> image_preview,
               const RichEditorExtensions& extensions, const InlineGraphics& inline_graphics,
               std::uint64_t inline_revision, const std::optional<File>& document_directory);

  /// Tests whether the rows refer to the exact snapshot prepared for layout.
  bool IsCurrent(const RichDocument& document) const;

  float Height() const { return height_; }
  const ThemeSpec& Theme() const { return theme_; }
  const RichEditorStyle& Style() const { return style_; }
  const TextStyle& MarkerStyle() const { return marker_style_; }
  Color AccentColor() const noexcept { return style_.accent_color.value_or(theme_.colors.primary); }
  /// Rows in document order. Use their layouts and decorations until the next Prepare() or reset.
  const std::vector<Row>& Rows() const { return rows_; }
  /// One content-local rule spanning each quote's descendant rows.
  const std::vector<Rect>& QuoteRules() const { return quote_rules_; }
  const std::vector<TableGrid>& Tables() const { return tables_; }

  /// Resolved text origin and clipped cell bounds after horizontal table scrolling.
  Point Origin(const Row& row) const;
  Rect VisibleCell(const Row& row) const;
  std::optional<std::size_t> ScrollableTableAt(Point point) const;
  bool ScrollTable(std::size_t index, float delta);
  bool RevealCaret(const RichDocument& document, TextPosition position);
  std::size_t AdjacentRow(std::size_t index, bool down, float x) const;

  /// Finds the caret nearest the point. Query the same snapshot used by Prepare().
  TextPosition Hit(const RichDocument& document, Point point) const;
  /// Finds the caret within a known row, for navigation between visual lines.
  TextPosition Hit(const RichDocument& document, std::size_t index, Point point) const;
  /// Resolves a document position and affinity to a content-local caret rectangle.
  Rect Caret(const RichDocument& document, TextPosition position) const;
  /// Returns selection or composition fragments for a document range, including selected block separators.
  std::vector<Rect> Ranges(const RichDocument& document, TextRange range) const;
  std::vector<Rect> TextFragments(const Row& row, TextRange range) const;

private:
  /// Inputs compared when deciding whether a cached text layout can reuse its inline boxes.
  struct InlineSource {
    std::uint64_t id = 0;
    std::shared_ptr<const ResolvedInlineGraphic> custom;
    ImageAsset image;
    bool operator==(const InlineSource&) const = default;
  };

  struct CachedLayout {
    Block block;
    float width;
    TextLayout layout;
    InlineOffsets offsets;
    std::vector<InlineBackground> inline_backgrounds;
    std::vector<InlineBox> inline_objects;
    std::vector<InlineSource> inline_sources;
    std::optional<CustomBlockGraphic> custom_graphic;
    bool header;
  };

  std::size_t LeafInRow(std::size_t index, float x) const;

  std::optional<RichDocument> document_;
  std::unordered_map<BlockId, CachedLayout> cache_;
  std::vector<Row> rows_;
  std::vector<Rect> quote_rules_;
  std::vector<TableGrid> tables_;
  ThemeSpec theme_;
  RichEditorStyle style_;
  std::shared_ptr<const CodeBlockHighlighting> code_blocks_;
  std::vector<CustomBlockExtension> custom_blocks_;
  TextStyle marker_style_;
  std::string locale_;
  std::uint64_t image_revision_ = 0;
  std::uint64_t inline_revision_ = 0;
  std::optional<ImageWidthPreview> image_preview_;
  float width_ = 0.0F;
  float height_ = 80.0F;
};

} // namespace huxerui::richeditor::detail

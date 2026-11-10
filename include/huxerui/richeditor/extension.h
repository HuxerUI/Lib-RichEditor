#pragma once

/// @file
/// @brief Optional presentation providers for a mounted rich-text editor.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/richeditor/document.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>
#include <huxerui/vector.h>

namespace huxerui::richeditor {

/// @brief Asynchronously resolves one document image reference into a PNG or JPEG asset.
/// The mounted editor invokes it on the application thread in a cancellable task. Keep the resolver instance
/// stable for its document storage context. A failed request displays the image's alternative text.
using ImageResolver = std::function<Task<IoResult<ImageAsset>>(std::string source)>;

/// @brief Foreground color for a half-open UTF-16 range in a code block.
struct CodeHighlight {
  TextRange range;
  Color foreground;
};

/// @brief Computes foreground colors for UTF-8 code, its canonical language ID, and the resolved background.
/// Called on the UI thread during layout. Return ordered, nonoverlapping ranges with endpoints on Unicode
/// scalar boundaries. Gaps use the editor's code foreground. An empty result displays plain text.
using CodeHighlighter = std::function<std::vector<CodeHighlight>(std::string_view code, std::string_view language,
                                                                 Color background)>;

/// @brief A language available in the code block menu and understood by its highlighter.
struct CodeLanguage {
  std::string id;                  ///< Unique lowercase fence word, such as "python". "text" is reserved.
  std::string label;               ///< Display name shown in the menu and on the block.
  std::vector<std::string> aliases; ///< Lowercase alternative fence words recognized on import.
};

/// @brief Replaces the language catalog and highlighter together.
///
/// Plain text is always available. The highlighter receives the canonical language ID as its second argument,
/// and runs only for registered languages. Keep the instance stable while its catalog and rules are unchanged.
struct CodeBlockHighlighting {
  /// Menu order. Each ID and alias identifies one language within the catalog.
  std::vector<CodeLanguage> languages;
  /// Optional syntax coloring. An empty function keeps the catalog and displays uncolored code.
  CodeHighlighter highlight;
};

/// @brief Returns the built-in SweetLine catalog and highlighter with process lifetime.
/// Copy this value to extend the catalog or wrap its highlighter. The built-in highlighter returns plain text
/// for unknown languages, blocks over 64 KiB, and lines over 2048 UTF-8 bytes.
const CodeBlockHighlighting& DefaultCodeBlockHighlighting();

/// One transient effect painted over a range in a text block.
enum class DecorationKind {
  Background, ///< Draws behind the glyphs.
  Underline,  ///< Draws a rule below the text.
  Outline,    ///< Draws a thin border around each visual fragment.
};

/// @brief A transient visual annotation for one block in the current decoration request.
struct TextDecorationRange {
  std::size_t block_index; ///< Zero-based index in DecorationRequest::blocks.
  TextRange range;         ///< Nonempty half-open UTF-16 range with scalar-aligned endpoints.
  DecorationKind kind = DecorationKind::Background;
  Color color;
};

/// Controls how much context a decoration analyzer receives and when results can be reused.
enum class DecorationScope {
  Block,    ///< Analyze new and changed text blocks. Each result must depend only on its block and the provider.
  Document, ///< Analyze a complete document snapshot, including relationships between blocks.
};

/// A text block to analyze in the current request.
struct DecorationBlock {
  Block block;
  /// Zero-based child indices from the root. Available for document scope.
  std::vector<std::size_t> path;
};

/// @brief Immutable analysis inputs from a committed editor snapshot.
struct DecorationRequest {
  /// Present for document scope, including documents without text blocks.
  std::optional<RichDocument> document;
  /// Text blocks in document order. For block scope, contains only blocks that need analysis.
  std::vector<DecorationBlock> blocks;
};

/// @brief Analyzes committed content and returns its decorations in paint order.
///
/// The editor starts tasks on the application thread after mount, a document commit or replacement, or a change
/// to the registered providers. Use RunWorker() for expensive analysis. The editor discards stale or invalid
/// results. Selection and IME preedit do not start analysis. Reuse the same provider instance to keep its cache
/// and pending work when other providers change. Replace it when its own inputs such as a search query change.
struct DecorationProvider {
  /// Block scope reuses results for unchanged blocks, including blocks that move.
  DecorationScope scope = DecorationScope::Block;
  std::function<Task<std::vector<TextDecorationRange>>(DecorationRequest)> analyze;
};

/// One immutable graphic fitted inside a custom block without changing its aspect ratio.
using CustomBlockGraphic = std::variant<ImageAsset, VectorAsset>;

/// @brief Produces an optional custom block graphic using the resolved text color and font size in logical units.
/// Called synchronously on the UI thread during layout. Results are cached per unchanged block and appearance.
/// Return an empty value for unsupported content or a rendering error to display the block's fallback text.
using CustomBlockRenderer =
    std::function<std::optional<CustomBlockGraphic>(const CustomBlockData&, Color foreground, float font_size)>;

/// @brief Associates a renderer with a persistent custom block type.
struct CustomBlockExtension {
  /// Exact identifier stored in CustomBlockData::type.
  std::string type;
  /// An empty renderer displays the block's fallback text.
  std::shared_ptr<const CustomBlockRenderer> render;
  bool operator==(const CustomBlockExtension&) const = default;
};

/// @brief Presentation extensions owned by the mounted view.
struct RichEditorExtensions {
  /// Optional application policy for image sources. The view can resolve local files when this is absent.
  std::shared_ptr<const ImageResolver> image_resolver;
  /// One catalog and highlighter shared by code block headers and syntax colors.
  std::shared_ptr<const CodeBlockHighlighting> code_blocks;
  /// Independent analyzers. Results are applied in provider order within each paint layer.
  std::vector<std::shared_ptr<const DecorationProvider>> decorations;
  /// Registers one presenter per custom type. When registrations repeat a type, the first takes precedence.
  std::vector<CustomBlockExtension> custom_blocks;
};

} // namespace huxerui::richeditor

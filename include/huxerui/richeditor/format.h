#pragma once

/// @file
/// @brief Synchronous conversion between UTF-8 formats and rich-text document snapshots.
///
/// Include <huxerui/richeditor/format.h> and link HuxerUI::RichEditor for JsonCodec() and MarkdownCodec().
/// Applications can implement DocumentCodec for their own formats.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor {

namespace detail {
struct MarkdownObjectSyntaxAccess;
}

/// @brief Controls what a codec may return when the target representation cannot express the input.
enum class LossPolicy {
  /// Return diagnostics without a converted value when editable content or formatting would be lost.
  Strict,
  /// Permit a supported approximation and report the losses. Invalid input may still leave the value absent.
  AllowLoss,
};

/// @brief A conversion failure or a detail about content lost during conversion.
struct ConversionIssue {
  /// Machine-readable code for application handling, such as "markdown.underline".
  std::string code;
  /// Zero-based child indices from the root to the affected block. Markdown import uses the source syntax tree.
  /// Export uses the document tree. An empty path denotes the whole input.
  std::vector<std::size_t> block_path;
  /// Half-open UTF-16 range relative to the affected block's text, when the issue concerns a text span.
  std::optional<TextRange> range;
  /// Human-readable description of the issue.
  std::string message;
};

/// @brief Outcome of importing UTF-8 source, with diagnostics available on failure or a lossy success.
struct ImportResult {
  /// Owned snapshot to pass to RichEditorController::ReplaceDocument() when present.
  std::optional<RichDocument> document;
  /// Reasons import failed or details of losses in the returned snapshot.
  std::vector<ConversionIssue> issues;
};

/// @brief Outcome of serializing a snapshot, with diagnostics available on failure or a lossy success.
struct ExportResult {
  /// Owned UTF-8 output to write when present. A present empty string is a successful result.
  std::optional<std::string> text;
  /// Reasons export failed or details of losses in the returned text.
  std::vector<ConversionIssue> issues;
};

/// @brief Format adapter operating on serialized UTF-8 text and validated RichDocument snapshots.
///
/// Conversions run synchronously and return owned results. Applications choose a codec, handle file I/O, and
/// present issues. A custom importer maps supported content to the existing block and mark types and validates
/// the tree with RichDocument::Create(). A custom exporter traverses RichDocument::Blocks().
/// @see LossPolicy, JsonCodec(), MarkdownCodec()
class DocumentCodec {
public:
  virtual ~DocumentCodec() = default;

  /// @brief Parses serialized source into an editable snapshot.
  /// @param source UTF-8 contents of the selected format, borrowed for the duration of the call.
  /// @param policy Whether loss is permitted. The default requires a faithful import.
  /// @return A validated snapshot when conversion succeeds under the requested policy, plus any diagnostics.
  /// Invalid input or an unsupported version returns issues and an absent document.
  virtual ImportResult Import(std::string_view source, LossPolicy policy = LossPolicy::Strict) const = 0;

  /// @brief Serializes the contents of an existing snapshot.
  /// @param document Committed or application-created snapshot, borrowed for the duration of the call.
  /// @param policy Whether loss is permitted. The default requires a faithful export.
  /// @return UTF-8 text when conversion succeeds under the requested policy, plus any diagnostics.
  /// Check for a present value before writing the destination.
  virtual ExportResult Export(const RichDocument& document, LossPolicy policy = LossPolicy::Strict) const = 0;
};

/// @brief Returns the shared Lib-RichEditor JSON codec.
///
/// Version 1 preserves all supported blocks, task completion states, inline styles, links, object payloads,
/// and attachment references under either loss policy. Applications store attachment bytes separately.
/// Import reports "json.version" for an unsupported version and "json.invalid" for malformed input or invalid
/// document content.
/// @return A reference to the built-in codec with process lifetime.
const DocumentCodec& JsonCodec();

/// @brief Identifies a Markdown spelling that an object mapping can import and export.
class MarkdownObjectSyntax {
public:
  /// @brief Matches a dollar-delimited span in a paragraph, heading, or table cell.
  static MarkdownObjectSyntax DollarSpan();

  /// @brief Matches a double-dollar span occupying an entire top-level paragraph.
  static MarkdownObjectSyntax DoubleDollarBlock();

  /// @brief Matches a fenced code block by its complete info string.
  /// @param fence_info Exact UTF-8 info string to copy. Registration requires a nonempty single line
  /// without leading or trailing spaces or tabs.
  static MarkdownObjectSyntax FencedBlock(std::string_view fence_info);

  /// Compares the Markdown spelling, including the complete info string for a fenced block.
  bool operator==(const MarkdownObjectSyntax&) const = default;

private:
  enum class Kind;
  explicit MarkdownObjectSyntax(Kind kind, std::string_view fence_info = {});

  Kind kind_;
  std::string fence_info_;

  friend struct detail::MarkdownObjectSyntaxAccess;
};

/// @brief The syntax and literal body of one Markdown object.
struct MarkdownObjectText {
  MarkdownObjectSyntax syntax; ///< Syntax matched by the parser or selected by the encoder.
  /// UTF-8 body with LF line endings, without delimiters or the fence's final line terminator.
  /// On import, math spans follow the parser's whitespace rules.
  std::string text;
};

/// @brief Bidirectional mapping between a Markdown object and persistent document data.
///
/// Callbacks execute synchronously during conversion and may be called again during round-trip validation.
/// Return an empty value for unsupported input. Keep results deterministic for the codec's lifetime.
/// Callbacks shared across threads must be safe to invoke concurrently.
/// @tparam Data CustomBlockData for block mappings or InlineObjectData for inline mappings.
template<class Data>
struct MarkdownObjectMapping {
  /// Persistent type identifier. The decoder must return this type and valid document data.
  std::string type;
  /// Accepted input syntax. The encoder must select one of these entries.
  std::vector<MarkdownObjectSyntax> syntax;
  /// Converts one recognized object to persistent data, with its own version and fallback text.
  std::function<std::optional<Data>(const MarkdownObjectText&)> decode;
  /// Encodes persistent data. The codec writes delimiters and validates the result in context.
  std::function<std::optional<MarkdownObjectText>(const Data&)> encode;
};

/// @brief Registers Markdown conversions for a custom block, an inline object, or both.
struct MarkdownExtension {
  /// Optional mapping for one top-level custom block type.
  std::optional<MarkdownObjectMapping<CustomBlockData>> block;
  /// Optional mapping for one atomic inline object type.
  std::optional<MarkdownObjectMapping<InlineObjectData>> inline_object;
};

/// @brief Opt-in syntax and object mappings for a Markdown codec instance.
struct MarkdownOptions {
  /// Import and export inline images. A standalone image in a top-level paragraph remains an image block.
  /// Markdown image syntax preserves the reference, alternative text and title.
  bool inline_images = false;
  /// Registrations owned by the codec. Types are unique per placement and input syntax has one owner.
  std::vector<MarkdownExtension> extensions;
};

/// @brief CommonMark codec with GFM strikethrough, task items, top-level tables, and optional object mappings.
///
/// Import maps paragraphs, headings, lists, quotes, code, thematic breaks, emphasis, and links to editable content.
/// Soft breaks become spaces in paragraphs and hard breaks become LF. Raw HTML is stored as literal text.
/// Task markers set the owning list item's completion state. Tables retain their column alignments and follow
/// the parsed GFM grid, which pads missing cells and ignores cells beyond the header width. Nested tables report
/// diagnostics. Recognized leading YAML front matter reports "markdown.front_matter". AllowLoss discards the metadata.
///
/// An image occupying its own top-level paragraph becomes an Image block with its reference, alternative text,
/// and title. Other image placements require inline_images. Codecs preserve image references while the application
/// resolves and stores their bytes.
///
/// Export writes canonical Markdown and checks that re-import preserves the document. Strict withholds output
/// when content or styling such as a missing table header, a cell line break, underline, highlight, or a
/// preferred image width cannot be represented. AllowLoss returns Markdown with issues describing the losses.
/// Registered object mappings participate in both directions and in the round-trip check. An unclaimed fence
/// remains a code block. Export reports a loss if a regular code block uses info claimed by a mapping.
class MarkdownCodec final : public DocumentCodec {
public:
  /// @brief Constructs a codec that owns its configuration.
  /// @param options Opt-in syntax for import and export.
  /// @throws std::invalid_argument If a registration is incomplete, incompatible with its placement,
  /// or conflicts with another registration.
  explicit MarkdownCodec(MarkdownOptions options = {});

  /// @copydoc DocumentCodec::Import
  ImportResult Import(std::string_view source, LossPolicy policy = LossPolicy::Strict) const override;
  /// @copydoc DocumentCodec::Export
  ExportResult Export(const RichDocument& document, LossPolicy policy = LossPolicy::Strict) const override;

private:
  MarkdownOptions options_;
};

} // namespace huxerui::richeditor

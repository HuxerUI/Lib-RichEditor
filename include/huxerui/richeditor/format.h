#pragma once

/// @file
/// @brief Synchronous conversion between UTF-8 formats and rich-text document snapshots.
///
/// Include <huxerui/richeditor/format.h> and link HuxerUI::RichEditor. Use JsonCodec() for full-fidelity storage
/// and MarkdownCodec() for CommonMark interchange. Applications can implement DocumentCodec for their own formats.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/richeditor/document.h>

namespace huxerui::richeditor {

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
/// Version 1 preserves all supported block types, inline styles, links, and metadata under either loss policy.
/// Import reports "json.version" for an unsupported version and "json.invalid" for malformed input or invalid
/// document content.
/// @return A reference to the built-in codec with process lifetime.
const DocumentCodec& JsonCodec();

/// @brief Returns the shared CommonMark codec with GFM strikethrough and top-level table support.
///
/// Import maps paragraphs, headings, lists, quotes, code, emphasis, and links to editable content. Soft breaks
/// become spaces in paragraphs and hard breaks become LF. Raw HTML is stored as literal text. Tables follow the
/// parsed GFM grid, which pads missing cells and ignores cells beyond the header width. Explicit column alignment,
/// nested tables and tasks report diagnostics. An image occupying its own top-level paragraph becomes an Image
/// block with its reference, alternative text, and title. Other image placements report diagnostics. AllowLoss
/// can return an approximation. Codecs preserve image references; the application resolves and stores their bytes.
///
/// Export writes canonical Markdown and checks that re-import preserves the document. Strict withholds output
/// when content or styling such as a missing table header, a cell line break, underline, highlight, or a
/// preferred image width cannot be represented. AllowLoss returns Markdown with issues describing the losses.
/// @return A reference to the built-in codec with process lifetime.
const DocumentCodec& MarkdownCodec();

/// @brief Controls opt-in Markdown syntax without changing the default CommonMark codec.
struct MarkdownOptions {
  /// Recognize standalone top-level display math and fenced code with the exact info string "math" as LaTeX.
  /// Display spans use the parser's whitespace rules. Fences preserve multiline source, including line breaks.
  /// Inline and nested math report a conversion loss. Export verifies that the source re-imports faithfully.
  bool display_math = false;
};

/// @brief Returns the shared Markdown codec for the requested syntax options.
const DocumentCodec& MarkdownCodec(MarkdownOptions options);

} // namespace huxerui::richeditor

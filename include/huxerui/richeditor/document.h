#pragma once

/// @file
/// @brief Immutable document snapshots and the block and inline types shared by editors and codecs.
///
/// Include <huxerui/richeditor/document.h> and link HuxerUI::RichEditor. Use RichDocument::Create() to validate
/// a tree built by application code. Use <huxerui/richeditor/format.h> to convert serialized content.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/text.h>

namespace huxerui::richeditor {

namespace detail {
struct DocumentAccess;
struct DocumentStorage;
class EditorSession;
}

/// @brief Structural role of a block in the document tree.
enum class BlockKind {
  Paragraph,     ///< Text leaf supporting inline marks, links, and line breaks.
  Heading,       ///< Single-line text leaf supporting inline marks, links, and a heading level.
  CodeBlock,     ///< Plain-text leaf supporting line breaks and code fence metadata.

  Quote,         ///< Container holding one or more blocks.
  List,          ///< Ordered or unordered container holding one or more list items.
  ListItem,      ///< Container holding the item's content and any nested blocks. Its parent is a List.
  Table,         ///< Top-level rectangular grid of TableRow blocks, optionally with a header in its first row.
  TableRow,      ///< Nonempty sequence of TableCell blocks. All rows of a table have the same number of cells.
  TableCell,     ///< Container holding one paragraph, which may be empty and may contain inline styles and breaks.

  ThematicBreak, ///< Selectable divider at the document root or inside a quote or list item.
  Image,         ///< Top-level image referenced by ImageBlockData.
  Custom,        ///< Top-level atomic block with application-defined content.
};

/// @brief Horizontal alignment shared by the header and body cells of one table column.
enum class TableColumnAlignment {
  Default, ///< Use the leading edge of each cell's paragraph direction.
  Left,    ///< Align each visual line to the physical left edge of its cell.
  Center,  ///< Center each visual line horizontally within its cell.
  Right,   ///< Align each visual line to the physical right edge of its cell.
};

/// @brief Combinable inline styles supported by paragraphs and headings.
///
/// Use operator| to combine styles in a MarkRange and operator& to test membership.
enum class Mark : std::uint8_t {
  None = 0,           ///< Empty set of marks.
  Bold = 1 << 0,      ///< Bold font weight.
  Italic = 1 << 1,    ///< Italic font slant.
  Strike = 1 << 2,    ///< Strikethrough decoration.
  Code = 1 << 3,      ///< Inline code style.
  Underline = 1 << 4, ///< Underline decoration.
  Highlight = 1 << 5, ///< Highlight using the editor theme's background treatment.
};

constexpr Mark operator|(Mark left, Mark right) noexcept {
  return static_cast<Mark>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

constexpr Mark operator&(Mark left, Mark right) noexcept {
  return static_cast<Mark>(static_cast<unsigned>(left) & static_cast<unsigned>(right));
}

/// @brief The complete set of marks over one nonempty slice of paragraph or heading text.
struct MarkRange {
  /// Half-open UTF-16 range [start, end) relative to the start of its block.
  /// Both endpoints must lie on Unicode scalar boundaries. For example, the emoji in "A🙂" occupies [1, 3).
  TextRange range;
  /// The nonempty combination of supported marks active over the whole range.
  Mark marks = Mark::None;
  bool operator==(const MarkRange&) const = default;
};

/// @brief Link destination and optional title over a nonempty slice of paragraph or heading text.
struct LinkRange {
  /// Half-open UTF-16 range [start, end) relative to the start of its block, with endpoints on scalar boundaries.
  TextRange range;
  /// UTF-8 destination stored with the linked text.
  std::string target;
  /// UTF-8 link title. An empty string represents an absent title.
  std::string title;
  bool operator==(const LinkRange&) const = default;
};

/// @brief Persistent metadata for one image block.
///
/// The application resolves the reference in the document's storage context. A saved document can be
/// moved together with its attachments while keeping the relative references intact.
struct ImageBlockData {
  /// Nonempty, single-line UTF-8 reference, such as "attachments/photo.jpg" or an HTTPS URL.
  std::string source;
  /// Plain UTF-8 alternative text used in accessibility and when the image is unavailable.
  std::string alt_text;
  /// UTF-8 Markdown image title. An empty string represents an absent title.
  std::string title;
  /// Preferred width in logical units. Omit it to use the resolved image's intrinsic width.
  /// A set width must be finite and positive. The view fits it to the content and derives the height from the image.
  std::optional<float> width{};
  bool operator==(const ImageBlockData&) const = default;
};

/// @brief Persistent content for a custom block, independent of its presentation.
///
/// Use a namespaced type such as "folio.formula". Versions belong to the type and are independent of the
/// document format version. The editor treats the payload as opaque UTF-8 and preserves it on save and reopen.
struct CustomBlockData {
  std::string type;          ///< Namespaced ASCII identifier with letters, digits, '.', '_' and '-'.
  unsigned version = 1;      ///< Positive schema version interpreted by the application.
  std::string payload;       ///< Application-owned data, such as LaTeX source or serialized JSON.
  std::string fallback_text; ///< Plain text for placeholder display, application copy actions and lossy export.
  bool operator==(const CustomBlockData&) const = default;
};

/// @brief A named resource reference stored alongside an inline object's payload.
///
/// Keeping references here lets the application copy or rebase attachments without interpreting the payload.
/// The application owns the resource bytes and decides how to resolve the reference.
struct InlineAttachment {
  /// Nonempty name unique within this object. Uses ASCII letters, digits, '.', '_' and '-'.
  std::string role;
  /// Nonempty, single-line UTF-8 reference. A relative path such as "attachments/photo.png" is interpreted
  /// relative to the document directory by resolvers that support local files.
  std::string source;
  bool operator==(const InlineAttachment&) const = default;
};

/// @brief Persistent data for an atomic inline object, independent of its renderer.
///
/// JSON preserves this value even when the application has no renderer for its type or version. The object
/// occupies one UTF-16 code unit in the document regardless of its rendered size or fallback text.
/// @see InlineObject, InlineObjectExtension
struct InlineObjectData {
  /// Namespaced ASCII identifier such as "folio.mention". Separate nonempty components with '.'.
  /// Components use letters, digits, '_' and '-'.
  std::string type;
  /// Positive schema version of the payload. Independent of the document's JSON format version.
  unsigned version = 1;
  /// Application-owned UTF-8 data. LF is allowed. CR and NUL are invalid.
  std::string payload;
  /// Single-line UTF-8 text without NUL, used for plain-text conversion and when presentation is unavailable.
  /// The view uses the type as its display and accessibility label if the fallback is empty.
  std::string fallback_text;
  /// Named references the application can inspect when migrating the document.
  std::vector<InlineAttachment> attachments;
  bool operator==(const InlineObjectData&) const = default;
};

/// @brief Associates persistent object data with a U+FFFC in a paragraph or heading.
///
/// The annotation gives the replacement character object semantics. An unannotated U+FFFC is ordinary text.
/// Use InsertInlineObject to insert the character and its annotation together in an editor.
struct InlineObject {
  /// @brief Constructs an annotation to place in BlockData::inline_objects.
  /// @param offset Block-local UTF-16 offset of an existing U+FFFC in BlockData::text.
  /// @param data Persistent object data. Document construction validates the offset and data.
  InlineObject(TextOffset offset, InlineObjectData data) : offset(offset), data(std::move(data)) {}

  TextOffset offset;     ///< Block-local UTF-16 offset of the replacement character.
  InlineObjectData data; ///< Persistent value stored at that offset.
  /// Compares offset and data. The editor's transient identity is independent of document equality.
  bool operator==(const InlineObject& other) const { return offset == other.offset && data == other.data; }

private:
  std::uint64_t id_ = 0;
  friend struct detail::DocumentAccess;
};

/// @brief Opaque editing handle for an inline object in one controller's current document.
///
/// A handle follows the object through text edits and moves. It can resolve again after undo restores a
/// removed object. Replacing the document invalidates handles from the previous document. Store a handle
/// while an application editor or dialog is open and use ReplaceInlineObject to apply the result.
/// @see InlineObjectSelection, ReplaceInlineObject
class InlineObjectTarget {
public:
  /// Constructs an empty handle. Obtain a usable handle from a selection or activation event.
  InlineObjectTarget() = default;

private:
  std::weak_ptr<const void> owner_;
  std::uint64_t id_ = 0;
  friend class detail::EditorSession;
};

/// @brief Captures an inline object's editing handle and a copy of its data at selection or activation.
struct InlineObjectSelection {
  InlineObjectTarget target; ///< Handle to pass to ReplaceInlineObject.
  InlineObjectData data;    ///< Captured value. Use it as the expected value when applying an edit.
};

/// Persistent type used by InlineImage() and recognized by the built-in image presenter.
inline constexpr std::string_view inline_image_type = "huxerui.image";

/// @brief Encodes image metadata as a version-one inline object with one named image reference.
/// @param image Reference, alternative text, title and optional preferred width.
/// @return Object data with a single-line fallback. The original alternative text is retained for conversion.
/// @see ReadInlineImage
InlineObjectData InlineImage(const ImageBlockData& image);

/// @brief Decodes image metadata without resolving or loading the referenced asset.
/// @param object Candidate inline object.
/// @return Image metadata for valid version-one inline images, or an empty value for other types,
/// unsupported versions, or malformed image data.
std::optional<ImageBlockData> ReadInlineImage(const InlineObjectData& object);

struct BlockData;

/// @brief Immutable, shared-storage value used to build or inspect a document tree.
///
/// To derive a different block, copy Data() into a BlockData value, update that value, and construct a new Block.
class Block {
public:
  /// @brief Stores construction data for one block.
  /// @param data Text and inline ranges for a leaf or child blocks for a container, with kind-specific attributes.
  /// @see RichDocument::Create() for validation of the complete tree.
  explicit Block(BlockData data);

  /// @return Borrowed data. Retain this block or a copy sharing its storage while using the reference.
  const BlockData& Data() const noexcept;

  /// Compares text, formatting, attributes, and children by value.
  bool operator==(const Block& other) const;

private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  std::uint64_t id_ = 0;
  friend struct detail::DocumentAccess;
};

/// @brief Construction data for a single document block.
///
/// Set fields appropriate to kind and leave the rest at their defaults. RichDocument::Create() checks the
/// combination when it validates the tree.
struct BlockData {
  /// Structural role that determines which fields can be set.
  BlockKind kind = BlockKind::Paragraph;
  /// UTF-8 text with LF line breaks and no embedded NUL. Headings occupy one logical line.
  /// Leave empty for containers and atomic blocks.
  std::string text;
  /// Nonoverlapping ranges sorted by starting offset. Combine marks in one range when they apply together.
  /// Available for paragraphs and headings.
  std::vector<MarkRange> marks;
  /// Nonoverlapping link ranges sorted by starting offset. A link may cross mark boundaries.
  /// Available for paragraphs and headings.
  std::vector<LinkRange> links;
  /// Nonempty child sequence for containers. Lists contain list items, tables contain rows, and rows contain cells.
  /// Each table cell contains exactly one paragraph.
  std::vector<Block> children;
  /// Level from 1 through 6 for a heading. Leave at zero for other blocks.
  unsigned heading_level = 0;
  /// True for a numbered list and false for a bulleted list.
  bool ordered = false;
  /// First ordinal of an ordered list. Zero is valid. Leave at one for unordered lists and other blocks.
  unsigned list_start = 1;
  /// Whether a list is tight in formats that distinguish tight and loose lists.
  bool tight = true;
  /// Single-line UTF-8 metadata for a code block, such as a language name. An empty string omits the info string.
  std::string fence_info;
  /// Whether the first row is a header. Available on Table blocks.
  bool table_has_header = false;
  /// Required for Image blocks. Its bytes are provided separately by the application.
  std::optional<ImageBlockData> image;
  /// Required for Custom blocks. A custom block cannot have child blocks or inline ranges.
  std::optional<CustomBlockData> custom;
  /// Inline objects in increasing block-local UTF-16 offset order. Each offset must identify a distinct
  /// U+FFFC in text. Available in paragraphs, including table cell paragraphs, and in headings.
  std::vector<InlineObject> inline_objects;
  /// Column alignments in document order. Available on Table blocks only.
  /// Leave empty for an all-default table, or supply one value per column. All-default arrays
  /// are stored as an empty vector in the validated document.
  std::vector<TableColumnAlignment> table_column_alignments;
  bool operator==(const BlockData&) const = default;
};

/// @brief Validated, immutable snapshot of document content.
///
/// Copies retain shared storage. A saved copy remains valid after the editor replaces its document or unmounts.
/// Selection, pending typing marks, IME composition, and undo history belong to the editing session.
/// @code{.cpp}
/// const auto document = RichDocument::Create({
///     Block(BlockData{.text = "A🙂", .marks = {{{1, 3}, Mark::Bold}}}),
/// });
/// const auto& paragraph = document.Blocks().front().Data();
/// @endcode
class RichDocument {
public:
  /// Creates a document containing one empty paragraph.
  RichDocument();

  /// @brief Validates a block tree and builds a snapshot with fresh internal block identities.
  /// Adjacent mark ranges with identical styles are merged. An empty root becomes one empty paragraph.
  /// @param blocks Top-level blocks in document order. A root-to-leaf path may contain up to 128 blocks.
  /// Tables must be top-level, with at least one row and column and at most 4096 cells per table.
  /// @return A snapshot suitable for editing or conversion.
  /// @throws std::invalid_argument If nesting, attributes, UTF-8, or range boundaries are invalid.
  static RichDocument Create(std::vector<Block> blocks);

  /// @return A borrowed span of the top-level blocks in document order. Retain a snapshot sharing this storage
  /// while using the span, or copy individual Block values to retain them independently.
  std::span<const Block> Blocks() const noexcept;

  /// Compares document content, including structure, inline formatting, and metadata.
  bool operator==(const RichDocument& other) const;

private:
  explicit RichDocument(std::shared_ptr<const detail::DocumentStorage> storage);
  std::shared_ptr<const detail::DocumentStorage> storage_;
  friend struct detail::DocumentAccess;
};

} // namespace huxerui::richeditor

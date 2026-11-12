#pragma once

/// @file
/// @brief Rich-text editing sessions, commands, and the mounted editor component.
///
/// Include <huxerui/richeditor.h> and link HuxerUI::RichEditor. Create the session in composition with
/// UseRichEditor(), then render RichEditor() and bind application controls to RichEditorController.
/// Use <huxerui/richeditor/format.h> to open or save serialized document content.
/// Pass RichEditorOptions to RichEditor() for appearance, resources, presentation providers, and keyboard shortcuts.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/file.h>
#include <huxerui/event.h>
#include <huxerui/richeditor/document.h>
#include <huxerui/richeditor/extension.h>
#include <huxerui/richeditor/style.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

namespace huxerui::richeditor {

/// @brief Toggles one inline mark on selected text in paragraphs and headings.
///
/// When the mark applies to the whole formattable selection, the command removes it. Otherwise it applies the mark
/// across the selection. At a caret the command updates the style for subsequent typing.
/// A selection in a table can be formatted within one cell.
struct ToggleMark {
  /// One supported style bit from Mark.
  Mark mark;
};

/// @brief Clears inline marks and links on selected text in ordinary blocks or within one table cell.
/// At a caret it clears the marks for subsequent typing.
struct ClearFormatting {};

/// @brief Sets or removes links on selected paragraph or heading text, including text in one table cell.
/// At a caret inside a link, the command updates the entire link and leaves the caret in place.
struct SetLink {
  /// UTF-8 link destination. Use an absent target and an empty title to remove links.
  std::optional<std::string> target;
  /// Optional UTF-8 title, represented by an empty string when absent.
  std::string title;
};

/// @brief Changes the text block at the caret or the text blocks covered by the selection.
///
/// A heading requires single-line text. Changing to a code block clears its inline marks and links.
/// Table cells retain their paragraph and are excluded from block conversion.
/// GetCommandState() reports whether the affected blocks match the requested kind and heading level.
struct SetBlockKind {
  /// Target text block kind. Use Paragraph, Heading, or CodeBlock.
  BlockKind kind;
  /// Required for headings. Valid levels are 1 through 6.
  unsigned heading_level = 0;
  /// Optional single-line UTF-8 code fence info, such as a language name.
  std::string fence_info;
};

/// @brief List style requested by ToggleList.
enum class ListKind {
  Unordered, ///< Bulleted list.
  Ordered,   ///< Numbered list.
};

/// @brief Wraps the affected blocks in a list or changes the selected items of an existing list.
///
/// If the requested kind is already active, selected items can be unwrapped when each holds one text
/// block and the unwrapped text does not become the lead of an enclosing item. Changing kind keeps
/// each item's blocks together.
/// A selection limited to unmarked continuation blocks within one item can form a nested list.
/// GetCommandState() reports the applied style independently of whether the selected blocks or sibling
/// items can be edited as one group. A selection crossing list levels can have a check state while disabled.
struct ToggleList {
  /// Style to apply, or to toggle off when the selected items already use it.
  ListKind kind;
};

/// @brief Adds or removes task markers using the same selection rules as ToggleList.
///
/// Wraps eligible non-list blocks in a bulleted list with unchecked task items. For sibling list items,
/// adds unchecked markers to ordinary items and preserves existing completion states. If every selected item
/// already has a marker, removes the markers while retaining the list and its child blocks.
/// GetCommandState() reports marker presence independently of command availability.
struct ToggleTaskMarker {};

/// @brief Sets the completion state of the task items targeted by the selection in one undoable edit.
///
/// The selection must target task items in one list. GetCommandState() reports how much of the selection
/// matches checked. The command preserves the selection and any pending typing marks.
struct SetTaskChecked {
  /// True marks the tasks complete. False marks them incomplete.
  bool checked;
};

/// @brief Changes the first number of the ordered list targeted by the current list selection.
/// Targets the same list as ToggleList. The items and selection are preserved in one undoable edit.
struct SetListStart {
  /// Ordinal of the first item. Following items count up from this value. Zero is valid.
  unsigned start = 1;
};

/// @brief Wraps the affected sibling blocks in a quote or unwraps them from their immediate quote container.
struct ToggleQuote {};

/// @brief Nests the selected list items under their preceding sibling.
/// The selection must target consecutive items in one list level and have a preceding item.
struct IndentList {};

/// @brief Moves selected items from a nested list up one level or unwraps them at the outermost list level.
struct OutdentList {};

/// @brief Inserts a rectangular table at a caret in a top-level paragraph.
///
/// An empty paragraph is replaced. Otherwise the table follows the paragraph. The caret moves to the first cell.
/// The dimensions must be positive and describe at most 4096 cells.
struct InsertTable {
  /// Total number of rows, including the header when enabled.
  std::size_t rows = 2;
  /// Number of cells in each row.
  std::size_t columns = 2;
  /// Whether to use the first row as the header.
  bool has_header = true;
};

/// @brief Position of an inserted row or column relative to the current cell, in document order.
enum class TableInsertSide { Before, After };

/// @brief Inserts an empty row next to the current cell's row.
/// A header remains the first row. Inserting before a header is unavailable.
struct InsertTableRow {
  TableInsertSide side = TableInsertSide::After;
};

/// @brief Inserts an empty cell in every row next to the current cell's column.
struct InsertTableColumn {
  TableInsertSide side = TableInsertSide::After;
};

/// @brief Deletes the current row and moves the caret to the nearest surviving row in the same column.
/// Deleting the header leaves a headerless table. Use DeleteTable when the table has one row.
struct DeleteTableRow {};

/// @brief Deletes the current column and moves the caret to the nearest surviving column in the same row.
/// Use DeleteTable when the table has one column.
struct DeleteTableColumn {};

/// @brief Changes whether the first row is a header, preserving all cell content.
/// GetCommandState() reports the header state in CommandState::checked when the selection is within a single cell,
/// including during IME preedit while the command is disabled.
struct ToggleTableHeader {};

/// @brief Changes the alignment of every cell in the column containing the selection.
///
/// Requires the caret or both selection endpoints to lie within a single cell. A change preserves
/// the selection and is undone in one step. GetCommandState() reports CheckState::On when the
/// column already has the requested alignment, including during IME preedit.
struct SetTableColumnAlignment {
  /// Alignment to apply. Default restores the paragraph's leading alignment.
  TableColumnAlignment alignment = TableColumnAlignment::Default;
};

/// @brief Deletes the current table and moves the caret to the following block, or the preceding block at the end.
/// A document with no remaining blocks becomes an empty paragraph.
struct DeleteTable {};

/// @brief Moves the caret to a paragraph after the current table.
/// If the next block is not a paragraph, inserts an empty paragraph as one undoable edit.
struct ExitTable {};

/// @brief Continues typing with plain text at the current location or below the enclosing block.
///
/// In a top-level paragraph or heading, replaces the selection with a paragraph break and keeps the
/// existing formatting of the text on either side. In a list, quote, table, or code block, moves to the
/// paragraph after the enclosing top-level block, inserting one if needed. New typing starts without marks.
/// Requires a caret or selection within one text block, outside IME preedit. Ctrl+Enter and Command+Enter
/// invoke this command when the editor owns focus, except when activating a selected inline object.
struct ContinueInParagraph {};

/// @brief Inserts a selectable thematic break at a paragraph caret.
///
/// Splits the paragraph at the caret when it has text on both sides, preserving its inline content.
/// The break stays in the paragraph's container, and the caret moves to the text after it or to
/// its trailing boundary. Available at the document root and in quotes and list items, outside
/// table cells and IME preedit.
struct InsertThematicBreak {};

/// @brief Inserts a top-level image at a paragraph caret or next to another image.
/// A paragraph split preserves the text and formatting on both sides. The caret moves after the image.
/// With an entire image selected, inserts after that image.
struct InsertImage {
  /// Reference and alternative text for the new block. Store any local attachment before executing the command.
  ImageBlockData image;
};

/// @brief Changes the selected image's reference, preserving its alternative text, title, and preferred width.
struct ReplaceImage {
  /// Nonempty, single-line UTF-8 reference for the replacement image.
  std::string source;
};

/// @brief Sets the preferred width of the selected image as one undoable edit.
/// The editor derives the height from the resolved image's aspect ratio and fits the image to the content width.
struct ResizeImage {
  /// Finite positive width in logical units. Omit it to restore automatic sizing.
  std::optional<float> width{};
};

/// @brief Deletes the selected image block. The application owns its attachment and its storage lifetime.
struct DeleteImage {};

/// @brief Inserts a custom block at a top-level paragraph caret or beside an atomic block.
/// Splits a nonempty paragraph as needed and places the caret after the inserted block.
struct InsertCustomBlock {
  CustomBlockData block; ///< Validated persistent content to insert.
};

/// @brief Replaces the selected custom block's content, preserving selection and block identity.
/// The type must match the selected block. If supplied, expected must match its current content.
struct ReplaceCustomBlock {
  CustomBlockData block;
  std::optional<CustomBlockData> expected{};
};

/// @brief Inserts one inline object at the caret or replaces a selection within one paragraph or heading.
///
/// Inserts the replacement character and annotation as one undoable edit, then places the caret after it.
/// Table cell paragraphs are eligible. The command is unavailable during IME preedit.
struct InsertInlineObject {
  InlineObjectData object; ///< Persistent object data to validate and insert.
};

/// @brief Updates a captured inline object, preserving its identity and the current selection.
///
/// The target must still resolve in this controller's document. The replacement must have the same type.
/// This is one undoable edit and is unavailable during IME preedit.
struct ReplaceInlineObject {
  InlineObjectTarget target;   ///< Handle obtained from selection or activation.
  InlineObjectData replacement; ///< New persistent value, which may use a different schema version.
  /// If present, the update succeeds only when the object's current data matches this value.
  std::optional<InlineObjectData> expected;
};

/// @brief Rich clipboard value containing a validated snapshot and its selection boundary information.
///
/// A fragment retains resource references. When copying between documents with different storage directories,
/// the application must arrange for those references to resolve in the destination.
struct DocumentFragment {
  RichDocument content; ///< Selected structure, marks, links and inline objects.
  /// True when the selection starts inside its first leaf. An eligible edge can merge with the target prefix.
  bool leading_partial = false;
  /// True when the selection ends inside its last leaf. An eligible edge can merge with the target suffix.
  bool trailing_partial = false;
};

/// @brief Inserts a rich fragment at the caret or replaces a selection within one paragraph or heading.
///
/// A single paragraph or heading contributes its inline contents to the destination block. Other fragments
/// require a top-level paragraph or heading as the destination. Paste assigns new identities to inserted
/// objects and records one undo entry. The command is unavailable during IME preedit.
struct InsertFragment {
  DocumentFragment fragment; ///< Rich clipboard value to insert.
};

/// @brief Selects all document text across blocks.
struct SelectAll {};

/// @brief Cancels active IME preedit or restores the document and selection before the latest committed edit.
struct Undo {};

/// @brief Restores the document and selection after the most recently undone edit.
struct Redo {};

/// @brief Typed command accepted by RichEditorController::Execute() and GetCommandState().
using EditorCommand = std::variant<
    ToggleMark, ClearFormatting, SetLink, SetBlockKind,
    ToggleList, ToggleTaskMarker, SetTaskChecked, SetListStart, ToggleQuote, IndentList, OutdentList,
    InsertTable, InsertTableRow, InsertTableColumn, DeleteTableRow, DeleteTableColumn,
    ToggleTableHeader, SetTableColumnAlignment, DeleteTable, ExitTable,
    ContinueInParagraph, InsertThematicBreak,
    InsertImage, ReplaceImage, ResizeImage, DeleteImage,
    InsertCustomBlock, ReplaceCustomBlock, InsertInlineObject, ReplaceInlineObject, InsertFragment,
    SelectAll, Undo, Redo>;

/// @brief A portable key and its complete modifier state.
///
/// Matching uses the key identity from KeyEvent, independently of the text produced by the keyboard layout.
struct KeyCombination {
  Key key = Key::Unknown;
  KeyModifiers modifiers{};

  bool operator==(const KeyCombination&) const = default;
};

/// @brief Associates one key combination with a command while the mounted editor owns text focus.
struct EditorKeyBinding {
  KeyCombination keys;
  EditorCommand command;
};

/// @brief A complete set of configurable editor shortcuts. The first matching entry owns the key.
using EditorKeymap = std::vector<EditorKeyBinding>;

/// @brief A command offered by the editor's built-in shortcut catalog.
struct EditorShortcutDefinition {
  std::string_view id;       ///< Stable identifier for settings and localization.
  std::string_view label;    ///< English display label.
  EditorCommand command;     ///< Typed command passed to the editing session.
  std::span<const KeyCombination> defaults; ///< Default combinations, including aliases.
};

/// @brief Returns the built-in shortcut catalog for the lifetime of the library.
/// A command with an empty default list can be assigned a shortcut by the application.
std::span<const EditorShortcutDefinition> DefaultEditorShortcuts();

/// @brief Appearance, resources, and keyboard configuration for a mounted editor.
struct RichEditorOptions {
  /// Unset colors and fonts inherit the enclosing theme.
  RichEditorStyle style;
  /// When provided, the built-in image resolver loads relative PNG and JPEG references from this directory.
  /// Supply the document's parent directory, or a working directory containing its attachments. Absolute
  /// paths and URLs require an application resolver. The directory is never serialized with the document.
  std::optional<File> document_directory;
  /// An explicit image resolver takes precedence over document_directory.
  RichEditorExtensions extensions;
  /// A null pointer selects the built-in bindings. A supplied immutable map replaces them, including aliases.
  /// An empty map clears the configurable bindings. Use Control or Meta with a letter, digit, punctuation,
  /// or function key. Shift can participate. Text entry, navigation, clipboard, and contextual Enter keys
  /// retain their usual behavior. Changing the map keeps the document, selection, IME, and undo history.
  std::shared_ptr<const EditorKeymap> keymap{};
};

/// @brief Typed events emitted by the mounted RichEditor view.
struct RichEditorEvents {
  /// @brief Requests navigation after a primary Control-click or Command-click on linked text.
  ///
  /// Subscribe with View::On() and return true to handle the click while preserving the editor selection.
  /// The destination policy belongs to the application. The borrowed link includes its block-local, half-open
  /// UTF-16 range and is valid for the duration of the callback.
  struct LinkActivated : Event<bool(const LinkRange&)> {};

  /// @brief Requests the application-defined action for an explicitly activated inline object.
  ///
  /// The view selects the object before emitting this event. Control-click or Command-click, the
  /// corresponding Enter shortcut on a selected object, and accessibility activation can emit it.
  /// Subscribe with View::On(). The event argument is borrowed for the call. Copy it to retain the
  /// data and target for an application dialog.
  struct InlineObjectActivated : Event<void(const InlineObjectSelection&)> {};
};

/// @brief State of a format for the current selection or caret.
enum class CheckState {
  Off,   ///< The requested format is inactive.
  On,    ///< The requested format applies to the current editing context.
  Mixed, ///< The format applies to only part of the affected content.
};

/// @brief Availability and optional check state for an application command control.
struct CommandState {
  /// Whether the command can be attempted at the current selection and editing state.
  bool enabled = false;
  /// Current state for commands that describe an active format. Actions such as Undo leave it absent.
  std::optional<CheckState> checked{};
};

/// @brief Position in the committed document as seen by the mounted editor.
/// Indices are zero-based in Document().Blocks() and belong to the snapshot current at the query.
struct EditorViewportLocation {
  /// Top-level block at the queried height. Space between blocks belongs to the preceding block.
  std::size_t root_block_index;
  /// Top-level block containing the start of the active explicitly aligned reveal, if any.
  std::optional<std::size_t> revealed_root_block_index;
  /// Whether the scrollable document has reached its maximum vertical offset.
  bool at_end;
};

namespace detail {
class EditorSession;
struct EditorViewport;
}

/// @brief Application-thread handle to a retained editing session.
///
/// Copies address the same session, including the current document, selection, IME state, and undo history.
/// Bind the session to one mounted RichEditor() at a time. Application toolbars can use copies of the same handle.
/// Retain a RichDocument snapshot from Document() for saving or conversion independent of the mounted view.
class RichEditorController {
public:
  /// @brief Reads the committed document and observes committed changes when called in composition.
  /// @return A snapshot with its own shared storage. Active IME preedit is kept in the editing session.
  RichDocument Document() const;

  /// @brief Opens a snapshot at its beginning and clears pending typing marks, IME preedit, and history.
  /// A focused editor starts a fresh text input session on its next update.
  /// @param document Validated snapshot to open.
  void ReplaceDocument(RichDocument document) const;

  /// @brief Reads command state for the current selection, typing marks, and history.
  /// Calls in composition observe changes so application controls can refresh.
  /// Table actions other than insertion require a caret or selection within a single cell. Image replacement,
  /// deletion, and resizing require the entire image to be selected. Table, image, and task actions are
  /// unavailable during preedit.
  /// @param command The same command value that the control passes to Execute().
  /// @return Availability and a check state when the command describes an active format.
  CommandState GetCommandState(const EditorCommand& command) const;

  /// @brief Reads the link under the caret or the single link covering the selected text.
  /// LinkRange::range uses UTF-16 offsets relative to its text block. Its end is exclusive.
  /// Calls in composition observe selection and editing changes.
  /// @return A copy of the link, or an empty value when the range is outside a link or spans multiple links.
  std::optional<LinkRange> CurrentLink() const;

  /// @brief Reads the first ordinal of the list targeted by SetListStart.
  /// Calls in composition observe selection and editing changes.
  /// @return An empty value during IME preedit or when the selection does not target items in one ordered list.
  std::optional<unsigned> OrderedListStart() const;

  /// @brief Reads the dimensions of the selected image for an application resize control.
  /// When a preferred width is set, returns that width and the corresponding height at the original aspect ratio.
  /// For automatic sizing, returns the intrinsic dimensions fitted to the current content width.
  /// Calls in composition observe selection and image loading. Dimensions use logical units.
  /// @return The size, or an empty value until an entire image is selected and its asset has resolved.
  std::optional<Size> SelectedImageSize() const;

  /// @brief Reads the selected custom block for application commands, such as editing its source.
  /// Returns an empty value unless exactly one custom block is selected. Calls in composition observe selection.
  std::optional<CustomBlockData> SelectedCustomBlock() const;

  /// @brief Reads the object whose replacement character is exactly covered by the current selection.
  /// Calls in composition observe selection and editing changes.
  /// @return A target and a copy of its data, or an empty value for any other selection or during IME preedit.
  std::optional<InlineObjectSelection> SelectedInlineObject() const;

  /// @brief Copies selected structure, inline objects and formatting for an application clipboard bridge.
  ///
  /// Selecting a complete table keeps its grid. A partial table selection contributes selected cell text as
  /// paragraphs. Calls in composition observe selection and editing changes.
  /// @return A rich fragment, or an empty value for a collapsed selection or during IME preedit.
  std::optional<DocumentFragment> SelectedFragment() const;

  /// @brief Resolves a vertical position against the mounted editor's committed layout.
  /// Calls in composition observe viewport movement, layout changes and explicitly aligned reveals.
  /// @param y Distance from the viewport top in logical pixels. Finite values are clamped to the viewport.
  /// @return The location, or an empty value if y is nonfinite or the committed layout is unavailable,
  /// such as before mounting or while IME preedit changes the visible document.
  std::optional<EditorViewportLocation> ViewportLocation(float y) const;

  /// @brief Reveals a nonempty block-local UTF-16 range in the mounted editor.
  ///
  /// Use a text block from Document() or a decoration request. The editing selection and focus are preserved.
  /// @param block The text block from the current document snapshot.
  /// @param range Half-open UTF-16 offsets relative to the block.
  /// @param alignment Start aligns the first visual line to the viewport top, Center centers the range, and End
  /// aligns the last visual line to the bottom. The offset is clamped to the scrollable extent. Omit the alignment
  /// to scroll only as far as necessary to reveal the first line. An explicit alignment follows viewport changes
  /// until the user scrolls, caret navigation scrolls the viewport, the document changes, or another range is revealed.
  /// @return True when the mounted editor accepted the range. Returns false for stale blocks, invalid ranges,
  /// unavailable layout, or active IME composition.
  bool RevealTextRange(const Block& block, TextRange range,
                       std::optional<ScrollAlignment> alignment = std::nullopt) const;

  /// @brief Applies a command to the retained selection or caret.
  /// A formatting command records changes across all affected blocks as one undo entry. An active IME composition
  /// may be committed separately before the command runs.
  /// @param command The operation to apply.
  /// @return True when the command changes editing state. Returns false when it is unavailable, invalid, or a no-op.
  bool Execute(const EditorCommand& command) const;

  /// Compares session and viewport identity.
  bool operator==(const RichEditorController&) const = default;

private:
  RichEditorController(std::shared_ptr<detail::EditorSession> session,
                       std::shared_ptr<detail::EditorViewport> viewport);
  std::shared_ptr<detail::EditorSession> session_;
  std::shared_ptr<detail::EditorViewport> viewport_;
  friend RichEditorController UseRichEditor(RichDocument, const std::source_location&);
  friend View RichEditor(RichEditorController, RichEditorOptions);
};

/// @brief Retains one editing session per call site in the current composition.
/// @param initial Snapshot for the first evaluation. Open later snapshots with RichEditorController::ReplaceDocument().
/// @param location Call site used to identify the retained value. Normally supplied by the default argument.
/// @return A controller that can be used by the view and application controls.
/// @pre Call on the application thread during composition.
RichEditorController UseRichEditor(RichDocument initial = {},
                                   const std::source_location& location = std::source_location::current());

/// @brief Declares a focusable, vertically scrolling editor backed by the supplied session.
/// Place the view in a layout with a bounded viewport. Attach a ViewHandle to request focus from an application
/// toolbar after executing a command.
/// A table that exceeds the content width scrolls horizontally within the document. Each cell owns its text range.
/// Consecutive images share a line when their fitted widths fit side by side. A selected, resolved image has
/// a corner handle for proportional resizing.
/// @param editor Controller returned by UseRichEditor().
/// @param options Appearance, document directory, and optional presentation providers. Keep provider instances
/// stable across composition. Replacing the document, directory, or resolver invalidates loaded images.
/// @return A View that handles text input, selection, and editing gestures when mounted.
View RichEditor(RichEditorController editor, RichEditorOptions options = {});

} // namespace huxerui::richeditor

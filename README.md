# Lib-RichEditor

A C++20 rich-text editor for HuxerUI 0.4.0. It supports paragraphs, headings, nested lists and quotes, code blocks, editable tables, images, links, and inline formatting. One editing session owns selection, IME composition, and undo/redo across the entire document.

## Use in a HuxerUI app

```cmake
huxerui_use_library(my_app
    TARGET HuxerUI::RichEditor
    PATH "/path/to/Lib-RichEditor")
```

The build uses the pinned Lib-Markdown parser and nlohmann/json. If the app also uses Lib-Markdown's rendering component, load that library before Lib-RichEditor so both can reuse the existing `HuxerUI::Markdown` target.

```cpp
#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

using namespace huxerui;
using namespace huxerui::richeditor;

[[huxerui::composable]]
View Article(RichDocument initial) {
  const auto editor = UseRichEditor(initial);
  const auto focus = UseViewHandle();
  const EditorCommand bold = ToggleMark{Mark::Bold};

  return Column {
    Button("Bold").OnClick([=] {
      if (editor.Execute(bold)) {
        focus.RequestFocus();
      }
    }).With(Enabled(editor.GetCommandState(bold).enabled)),
    RichEditor(editor).With(focus, Grow(), Semantics{.label = "Article"}),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}
```

Use one controller per mounted editor. `Document()` returns the committed snapshot while an IME composition is active. To open a document, call `JsonCodec().Import(source)` or `MarkdownCodec().Import(source)` and pass the returned document to `ReplaceDocument()` when present. Replacing a document clears undo and redo. To save, call `JsonCodec().Export(editor.Document())` and write the returned text when present.

Applications can implement `DocumentCodec` for another UTF-8 format. A custom codec imports through `RichDocument::Create()` and exports by reading `Blocks()` on the snapshot. Codecs map to the editor's existing block and mark types and return diagnostics to the application, which owns storage and presentation.

`GetCommandState(SetBlockKind{...})` reports the active paragraph kind or heading level, including mixed selections. `CurrentLink()` reads the link under the caret or covering the selection. Execute `SetLink{target, title}` on selected text to set a link, or at a caret inside an existing link to update the entire link. `SetLink{}` removes the link over the same range.

## Lists

`OrderedListStart()` reads the starting number of the list at the caret or shared by the selected items. Execute `SetListStart{number}` to renumber that list in one undo step. Nested lists keep their own starting numbers. Tab indents list items when they have a preceding sibling, and Shift+Tab outdents them.

An unmarked continuation inside a list item keeps its nesting, and Enter splits the continuation inside that item. Applying a list to those blocks creates a sublist. Applying or changing a list style can join adjacent groups of the same kind and spacing at the edit site, provided their existing numbers stay the same. Across list levels, the toolbar reports the selected styles and disables structural list commands. Removing a marker is available for a single text block when the parent retains its own marker and the edit keeps existing child groups together.

## Task lists

Task state belongs to the list item. Ordinary items omit `BlockData::task_checked`, and tasks store `false` for incomplete or `true` for complete. Both numbered and bulleted lists can mix ordinary and task items. The checkbox occupies the list marker area, so text offsets refer to the item content.

Execute `ToggleTaskMarker{}` to create tasks from eligible paragraphs or to add or remove checkboxes on selected list items. Execute `SetTaskChecked{true}` or `SetTaskChecked{false}` to set completion on task items in the same list. Tapping a checkbox changes the associated item in one undoable edit. Return creates an unchecked task when it splits a task item, and Shift+Return adds a line break within the item.

JSON v1 stores the optional `task_checked` boolean on `list_item` blocks. Markdown round trips representable tasks with `[ ]` and `[x]` markers, including mixed and nested lists.

## Tables

A top-level `Table` contains a rectangular grid of `TableRow` and `TableCell` blocks, up to 4096 cells per table. Each cell contains one paragraph with its own text, line breaks, inline marks, and links. The table's `table_has_header` flag gives the first row the header role.

Use `InsertTable{.rows = 3, .columns = 2}` at a top-level paragraph caret. Once the selection is inside one cell, `InsertTableRow`, `InsertTableColumn`, `DeleteTableRow`, and `DeleteTableColumn` act on that cell's row or column. Use `ToggleTableHeader` to change the first row's role and `DeleteTable` to remove the whole table. Query `GetCommandState()` before offering an action and pass the same command to `Execute()`.

Use `SetTableColumnAlignment{TableColumnAlignment::Center}` to align the header and body cells of the current column. Default alignment follows each cell's reading direction. Explicit left and right refer to physical edges. The command preserves the selection and participates in undo and redo.

Tab and Shift+Tab navigate within a table. Tab from the last cell adds a row and moves the caret into its first cell. Enter adds a line break within a cell. Ctrl+Enter, Command+Enter, or `ExitTable` continues in a paragraph after the table. Cross-cell selections can be copied. Text and formatting edits stay inside a single cell. Each grid change is one undoable edit. A table wider than the editor's content area scrolls horizontally with a touch drag, horizontal wheel, or Shift+wheel.

## Read-only sessions

Call `RichEditorController::SetReadOnly(true)` to pause editing while keeping the same document, selection, and undo history. Entering read-only mode cancels unfinished IME composition. The editor remains scrollable and selectable, and users can copy text, follow links, and navigate with the keyboard. Editing commands and text-range replacements are unavailable until editing is enabled again. Use `IsReadOnly()` to reflect the mode in application controls. `ReplaceDocument()` remains available to load another snapshot into the session.

## Thematic breaks

Use `InsertThematicBreak{}` at a paragraph caret to insert a divider in the current document, quote, or list item. Inserting within text preserves the inline content on both sides. Select the divider to delete it, or move past it to continue typing. Markdown exports it as `***`.

## Images

An `Image` block stores `source`, `alt_text`, and optional `title` and `width` fields in JSON v1. Width is in logical units. Set `RichEditorOptions::document_directory` to the document's parent or working directory to load relative PNG and JPEG references, including paths under `attachments/`. An application can override loading with `extensions.image_resolver` for URLs or a different storage provider. The editor displays an alternative-text placeholder until the reference resolves.

Store an image file before calling `InsertImage` or `ReplaceImage`. A pointer can select the entire block for replacement or deletion. Place the caret after an image to continue typing. The first text commit there creates the paragraph as part of the same undoable edit.

Consecutive images share a line when they fit and wrap at narrower widths. Drag the bottom-right handle of a selected image to resize it in proportion. Application controls can read `SelectedImageSize()` and execute `ResizeImage{width}` to set the width, or `ResizeImage{}` to restore automatic sizing. A resize is one undoable edit. Strict Markdown export reports `markdown.image_size` for explicitly sized images. AllowLoss exports their references with automatic sizing.

## Appearance

Use `RichEditorOptions::style` to set the background, text and accent colors, fonts, padding, block spacing, and the corner radii of inline code, highlights, and code blocks. `inline_code_horizontal_padding` reserves space on either side of a code span and participates in wrapping.

```cpp
RichEditorStyle style;
style.background = Color::Rgb(253, 252, 247);
style.body_font = Font::System(18.0F);
style.content_padding = EdgeInsets::Symmetric(24.0F, 20.0F);
style.quote_rule_width = 3.5F;
style.inline_code_corner_radius = 5.0F;
style.inline_code_horizontal_padding = 3.0F;
style.code_block_corner_radius = 12.0F;
style.link_hover_color = Color::Rgb(105, 66, 32);
return RichEditor(editor, {.style = style, .document_directory = document_file.Parent()}).With(Grow());
```

Unset colors and fonts inherit from the enclosing HuxerUI theme. To change the style while editing, pass the updated value in composition with the same controller. The document and its history belong to the session.

## Links

Subscribe to `RichEditorEvents::LinkActivated` for Control-click or Command-click on linked text. The handler receives the `LinkRange` and returns `true` to handle navigation while keeping the current selection. Holding Control or Command over a link uses `RichEditorStyle::link_hover_color`, which defaults to the theme's `on_primary_container`.

## Revealing text ranges

Call `editor.RevealTextRange(block, range, ScrollAlignment::Center)` to center a search match or another range from the current document. `ScrollAlignment::Start` and `ScrollAlignment::End` align its top or bottom to the viewport. Omitting the alignment scrolls just far enough to reveal the first line. The editor keeps the selection and focus during navigation.

## Keyboard shortcuts

`DefaultEditorShortcuts()` lists the configurable editor commands and their default aliases. Pass a shared immutable `EditorKeymap` as `RichEditorOptions::keymap` to supply the complete editor binding table. A null pointer uses the defaults and an empty map clears them. Replace the shared map in composition to apply changes without restarting the text-input session. Use `RichEditorController::Execute()` for the same editing commands from toolbar controls.

Use Ctrl or Command with 0–6 for the paragraph and heading styles. Add Shift with T, R, or Backslash to insert a table, a row below, or a column after the current cell.

Ctrl+Enter or Command+Enter runs `ContinueInParagraph{}`. It splits a top-level paragraph or heading at the caret and starts typing without marks. Inside a list, quote, table, or code block, it goes to the paragraph below the enclosing top-level block and inserts one if needed. The existing content retains its formatting.

## Code highlighting

Code blocks use [SweetLine](https://github.com/FinalScave/SweetLine) by default. The first word of the fence info selects the language, so a fence such as `cpp example` uses the C++ profile. Bundled profiles cover C, C++, C#, CSS, Go, HTML, Java, JavaScript, JSON, Kotlin, Python, Rust, shell, SQL, Swift, TypeScript, and YAML. Common aliases such as `c++`, `js`, and `bash` also work. Unlabeled or unsupported code uses the regular code block color. The default provider also uses that color for blocks over 64 KiB or lines over 2048 UTF-8 bytes to bound work on the UI thread.

Click or tap the language above a code block to choose a bundled language or **Plain text**. The change updates that block's first fence word and retains any extra fence info. Undo restores the previous language.

Supply a shared `CodeBlockHighlighting` in `extensions.code_blocks` to configure the menu, aliases and syntax coloring together. The highlighter receives UTF-8 code, the canonical language ID, and the background color. Return foreground spans using block-local UTF-16 offsets. Copy `DefaultCodeBlockHighlighting()` to extend its catalog or delegate to its highlighter for bundled languages.

```cpp
auto code = std::make_shared<CodeBlockHighlighting>();
code->languages = {{"notes", "Notes script", {"nt"}}};
code->highlight = HighlightMyLanguage;
return RichEditor(editor, {.style = style, .extensions = {.code_blocks = code}});
```

Syntax colors are computed during layout. Markdown and JSON store the editable code and fence info.
The bundled engine and its dependencies are listed in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).

## Decorations and custom blocks

Register `DecorationProvider` instances in `extensions.decorations`. Every analyzer receives a `DecorationRequest` and returns ranges indexed by block in that request. The default `DecorationScope::Block` uses the edit's changed block IDs, including on undo and redo. It sends only new or changed text blocks after the initial request. Results must depend only on their block and the provider's configuration. Cached decorations follow unchanged blocks when they move and remain visible while other blocks are being analyzed.

Choose `DecorationScope::Document` for analysis that needs context across blocks. Requests include the committed snapshot, its text blocks, and their paths. The editor cancels obsolete document requests and discards stale results. Reuse a provider instance to keep its results and pending work when other providers are replaced or reordered. Use `RunWorker()` for expensive analysis.

A top-level `Custom` block stores a namespaced type, payload version, UTF-8 payload, and fallback text. JSON v1 preserves these fields even when no renderer is registered. Register a `CustomBlockExtension` per type to display an image or vector asset. Use `InsertCustomBlock`, `SelectedCustomBlock()` and `ReplaceCustomBlock` to build application controls for its content. Edits and removal are undoable.

## Inline objects

Paragraphs and headings, including text in lists, quotes and table cells, can contain atomic inline objects. Each object occupies one UTF-16 U+FFFC in the editing projection. The document stores its type, version, payload, fallback text and optional named attachment references. Undo and redo restore objects with their edited context. An inline object target remains valid when the object moves within the open document.

Use `InsertInlineObject` with application-defined object data or the result of `InlineImage(image)`. `SelectedInlineObject()` returns the captured target and data for `ReplaceInlineObject`. Bind `RichEditorEvents::InlineObjectActivated` with `.On<...>()` to open an application editor. Control-click or Command-click, the corresponding Enter shortcut when an object is selected, and semantic activation emit the same event.

Register an `InlineObjectExtension` per type for custom async rendering. The renderer supplies an image or vector with baseline dimensions. A missing or unresolved renderer shows fallback text. The built-in inline image presentation uses the same resolver and document directory as image blocks.

## Clipboard

The focused editor handles copy, cut, and paste from keyboard shortcuts and the SDK's selection menu. A copy can carry plain text, HTML, and a RichEditor fragment in one system clipboard item. RichEditor tries the fragment first, then HTML, then plain text. HTML exchanges bold, italic, underline, strikethrough, links, and line breaks. Use Ctrl+Shift+V, Command+Shift+V, or `RichEditorController::PastePlainText()` to paste the plain representation.

`SelectedFragment()` and `InsertFragment` are also available to applications. A complete table keeps its grid and a partial table selection contributes cell text as paragraphs. RichEditor records a paste as one undo entry. Supply a `ClipboardAttachmentProvider` in `RichEditorOptions::clipboard_attachments` to read the requested resources and stage their bytes in the destination. RichEditor applies the returned references, then commits the staged resources once insertion succeeds. A cut removes the selection only after a format that preserves its content has been published. `RichEditorEvents::ClipboardWriteFailed` reports the action and the reason for a failed publication.

## Formats

JSON version 1 stores all editable content and styles. A `table` block stores its `has_header` flag and its ordered `table_row` children. Rows contain `table_cell` blocks that each contain a `paragraph`. Optional `column_alignments` contains one `default`, `left`, `center`, or `right` value per column. Omit it when all columns use the default. A divider is `{"type":"thematic_break"}`.

Text is UTF-8 with LF line endings. Mark and link ranges use half-open UTF-16 offsets relative to their text block. The root contains `version` and `blocks`. The editor normalizes an empty root to one empty paragraph and merges adjacent equal mark ranges. Paragraphs and headings can include an optional `inline_objects` array. Each record stores a block-local UTF-16 `offset`, `type`, `version`, `payload`, `fallback_text` and optional `attachments` with `role` and `source`. Cursor positions, IME state, layout, and history live in the editing session.

Markdown import converts the Lib-Markdown syntax tree to editable content. The supported subset uses CommonMark paragraphs, headings, lists, quotes, code, thematic breaks, emphasis and links plus GFM strikethrough, task items, and top-level tables with column alignment. A standalone image in a top-level paragraph preserves its reference, alternative text, and title. GFM pads short data rows with empty cells and ignores extra cells beyond the header width. Ordinary source line breaks become spaces and explicit hard breaks remain line breaks. Raw HTML is literal text. Recognized unsupported content includes nested tables and common leading YAML front matter with simple or quoted keys between `---` fences.

Markdown export checks semantic round trips with the same parser. Both import and export default to `LossPolicy::Strict`. Use `LossPolicy::AllowLoss` to receive a conversion with reported losses such as omitted underline or highlight, a synthesized header row, or cell line breaks replaced by spaces. Each issue carries a code and a zero-based tree path, with a block-local range when available.

Enable `MarkdownOptions::inline_images` to import and export images embedded in text. Standalone Markdown images remain image blocks.

### Markdown object mappings

A `MarkdownCodec` instance owns its extensions independently of the editor view. A `MarkdownExtension` can map a top-level custom block, an inline object, or both. Use `MarkdownObjectSyntax::DollarSpan()` for inline dollar spans, `DoubleDollarBlock()` for standalone double-dollar spans, and `FencedBlock(info)` to match a complete fence info string. The codec writes delimiters and verifies the document by importing the generated Markdown with the same extensions. Duplicate syntax or duplicate types in the same placement cause construction to fail.

```cpp
const auto diagram_fence = MarkdownObjectSyntax::FencedBlock("diagram");
const MarkdownCodec markdown({.extensions = {MarkdownExtension{
    .block = MarkdownObjectMapping<CustomBlockData>{
        .type = "example.diagram",
        .syntax = {diagram_fence},
        .decode = [](const MarkdownObjectText& source) {
          return CustomBlockData{"example.diagram", 1, source.text, "Diagram"};
        },
        .encode = [diagram_fence](const CustomBlockData& block) -> std::optional<MarkdownObjectText> {
          if (block.version != 1) {
            return std::nullopt;
          }
          return MarkdownObjectText{diagram_fence, block.payload};
        },
    },
}}});
```

An unregistered fence is ordinary code. When a registered mapping claims a fence info string, exporting a regular code block with that info reports a loss. `AllowLoss` keeps the code body and omits the conflicting info. Missing or unrepresentable object mappings report losses with block paths and, for inline objects, UTF-16 ranges. `AllowLoss` can write the object's fallback text.

## Tests

For MSVC builds, add `/utf-8` and `/bigobj` to the `CL` environment variable in the Visual Studio developer shell.

```sh
cmake -S . -B build -DHUXERUI_RICHEDITOR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The mounted tests use `HuxerUI::testing` when the SDK includes it.

## Example application

See the [example application README](examples/preview/README.md) for build and usage instructions.

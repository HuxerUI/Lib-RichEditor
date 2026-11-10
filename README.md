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

## Tables

A top-level `Table` contains a rectangular grid of `TableRow` and `TableCell` blocks, up to 4096 cells per table. Each cell contains one paragraph with its own text, line breaks, inline marks, and links. The table's `table_has_header` flag gives the first row the header role.

Use `InsertTable{.rows = 3, .columns = 2}` at a top-level paragraph caret. Once the selection is inside one cell, `InsertTableRow`, `InsertTableColumn`, `DeleteTableRow`, and `DeleteTableColumn` act on that cell's row or column. Use `ToggleTableHeader` to change the first row's role and `DeleteTable` to remove the whole table. Query `GetCommandState()` before offering an action and pass the same command to `Execute()`.

Tab and Shift+Tab navigate within a table. Enter adds a line break within a cell. Ctrl+Enter, Command+Enter, or `ExitTable` continues in a paragraph after the table. Cross-cell selections can be copied. Text and formatting edits stay inside a single cell. Each grid change is one undoable edit. A table wider than the editor's content area scrolls horizontally with a touch drag, horizontal wheel, or Shift+wheel.

## Images

An `Image` block stores `source`, `alt_text`, and optional `title` and `width` fields in JSON v1. Width is in logical units. Set `RichEditorOptions::document_directory` to the document's parent or working directory to load relative PNG and JPEG references, including paths under `attachments/`. An application can override loading with `extensions.image_resolver` for URLs or a different storage provider. The editor displays an alternative-text placeholder until the reference resolves.

Store an image file before calling `InsertImage` or `ReplaceImage`. A pointer can select the entire block for replacement or deletion. Place the caret after an image to continue typing. The first text commit there creates the paragraph as part of the same undoable edit.

Consecutive images share a line when they fit and wrap at narrower widths. Drag the bottom-right handle of a selected image to resize it in proportion. Application controls can read `SelectedImageSize()` and execute `ResizeImage{width}` to set the width, or `ResizeImage{}` to restore automatic sizing. A resize is one undoable edit. Strict Markdown export reports `markdown.image_size` for explicitly sized images. AllowLoss exports their references with automatic sizing.

Folio stores local images under `attachments/` and uses relative references. Replacement gives the new file its own name, keeping the original available to Undo. For documents with local images, **Save** and **Export Markdown…** ask for a destination folder and create a Folio subfolder containing the document and its referenced attachments. Move the whole subfolder to keep the document and its images together. **Open folder…** reads an `article.json` or `article.md` and its attachments into a working copy.

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
return RichEditor(editor, {.style = style, .document_directory = document_file.Parent()}).With(Grow());
```

Unset colors and fonts inherit from the enclosing HuxerUI theme. To change the style while editing, pass the updated value in composition with the same controller. The document and its history belong to the session.

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

`LatexBlock(source)` creates a standalone formula. Folio offers source editing and copying LaTeX from the formatting bar. Its optional RaTeX renderer runs locally on macOS and Android when the corresponding Rust target is installed. Unsupported expressions and builds without the renderer display editable source. The renderer is a Folio dependency. Applications using Lib-RichEditor can supply their own renderer.

## Inline objects

Paragraphs and headings, including text in lists, quotes and table cells, can contain atomic inline objects. Each object occupies one UTF-16 U+FFFC in the editing projection. The document stores its type, version, payload, fallback text and optional named attachment references. Undo and redo restore objects with their edited context. An inline object target remains valid when the object moves within the open document.

Use `InsertInlineObject{LatexInline(source)}` or `InsertInlineObject{InlineImage(image)}` to insert an object. `SelectedInlineObject()` returns the captured target and data for `ReplaceInlineObject`. Bind `RichEditorEvents::InlineObjectActivated` with `.On<...>()` to open an application editor. Control-click or Command-click, the corresponding Enter shortcut when an object is selected, and semantic activation emit the same event.

Register an `InlineObjectExtension` per type for custom async rendering. The renderer supplies an image or vector with baseline dimensions. A missing or unresolved renderer shows fallback text. The built-in inline image presentation uses the same resolver and document directory as image blocks. Folio resolves local PNG and JPEG files from `attachments/` and migrates structured references alongside the document.

`SelectedFragment()` and `InsertFragment` provide rich clipboard primitives for application bridges. Copying part of a table returns the selected cell text as paragraphs. Pasting inline text preserves its marks, links and objects in a text block. Rich block fragments can be inserted in a top-level text block. The SDK's system clipboard service carries plain text.

## Formats

JSON version 1 stores all editable content and styles. A `table` block stores its `has_header` flag and its ordered `table_row` children. Rows contain `table_cell` blocks that each contain a `paragraph`. Text is UTF-8 with LF line endings. Mark and link ranges use half-open UTF-16 offsets relative to their text block. The root contains `version` and `blocks`. The editor normalizes an empty root to one empty paragraph and merges adjacent equal mark ranges. Paragraphs and headings can include an optional `inline_objects` array. Each record stores a block-local UTF-16 `offset`, `type`, `version`, `payload`, `fallback_text` and optional `attachments` with `role` and `source`. Cursor positions, IME state, layout, and history live in the editing session.

Markdown import converts the Lib-Markdown syntax tree to editable content. The supported subset uses CommonMark paragraphs, headings, lists, quotes, code, emphasis and links plus GFM strikethrough and top-level tables. A standalone image in a top-level paragraph preserves its reference, alternative text, and title. GFM pads short data rows with empty cells and ignores extra cells beyond the header width. Ordinary source line breaks become spaces and explicit hard breaks remain line breaks. Raw HTML is literal text. Recognized unsupported content, including explicit table column alignment, nested tables, and tasks, returns issues.

Markdown export checks semantic round trips with the same parser. Both import and export default to `LossPolicy::Strict`. Use `LossPolicy::AllowLoss` to receive a conversion with reported losses such as omitted underline or highlight, a synthesized header row, or cell line breaks replaced by spaces. Each issue carries a code and a zero-based tree path, with a block-local range when available. Plain clipboard actions use text fallback. Application clipboard bridges can preserve structure with document fragments.

`MarkdownCodec({.display_math = true})` opts into standalone LaTeX. Folio enables it alongside `inline_math` and `inline_images`. Display spans use the parser's whitespace rules. The codec uses `math` fences for multiline formulas to preserve exact source. The default codec reports formula loss under Strict. AllowLoss writes fallback text and returns diagnostics for formulas or other custom blocks the selected codec cannot represent.

Enable `MarkdownOptions::inline_math` to round trip supported dollar-delimited LaTeX and `inline_images` to import and export images embedded in a paragraph. Standalone Markdown images remain image blocks. Strict export reports object ranges that cannot be represented, and AllowLoss writes fallback text.

## Preview and tests

Folio's math renderer uses Cargo and the matching Rust target. For Android arm64, install it with
`rustup target add aarch64-linux-android`. CMake can disable the preview renderer with `FOLIO_BUILD_MATH_RENDERER=OFF`.

```sh
huxerui run macos --profile debug
huxerui run android --profile debug --abi arm64-v8a --java-home "$(/usr/libexec/java_home -v 17)"

cmake -S . -B build -DHUXERUI_RICHEDITOR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The mounted tests use `HuxerUI::testing` when the SDK includes it. The Folio preview uses the Material theme and follows the system light or dark appearance. The editor fills the window inside its content margins. On compact screens, the toolbar pins **Undo** and the overflow menu and puts the actions for the current selection first in the horizontally scrollable area. Hover or long-press an icon to see its command.

**Open file…** accepts RichEditor JSON and Markdown. If the file references local images, Folio asks for the containing folder so it can import them. The **Save JSON** icon and **Export Markdown…** use the system picker. A document without local attachments can be saved as a single file.

Choose **File actions** → **Load sample**, then enter Chinese text with an IME and select across paragraphs. Apply underline and bold in separate actions, undo bold, and check that the underline and committed text remain. Choose **Save** and select a destination, then reopen the saved JSON with **Open file…**. **Export Markdown…** reports the underline before opening a save picker. Confirm **Export with loss** to continue.

Open the text style menu to switch between paragraph, heading levels 1 through 6, and code block. Select text to insert a link, or place the caret inside an existing link to edit, copy, or remove its address.

Place the caret in the sample's ordered list and open **List numbering** to set a starting number, restart at one, or change the indent. Undo restores the previous numbering, and the new start can be saved in JSON and Markdown.

To exercise tables, select a cell in the sample. The compact toolbar shows **+ Row** and **+ Column**. Use **Table actions** for the other grid changes and undo them. Enter Chinese text in one cell, save, and reopen JSON. **Export Markdown…** preserves the table while it has a header and representable styles. Turn off **Header row** to verify the diagnostic, then export with loss to save a table with a synthesized header.

To exercise images, place the caret in a top-level paragraph and choose **Insert image** to select a PNG or JPEG. On compact screens, find it under **More formatting** → **Insert**. Selecting an image dismisses the software keyboard and shows **Size**, **Replace**, and **+ Image** on the compact toolbar. Drag a corner or open the size dialog to set the width or height and watch the other dimension update. Size the pair to fit on the same line and undo a resize. Choose **Save**, move the resulting Folio subfolder, and reopen it with **Open folder…**. **Export Markdown…** saves representable Markdown and its local attachments together.

Enter splits ordinary blocks or inserts a literal newline in code. Shift+Enter inserts a paragraph-internal break. Ctrl+Enter or Command+Enter leaves a code block by inserting a paragraph after it.

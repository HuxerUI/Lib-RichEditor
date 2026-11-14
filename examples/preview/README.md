# Draftspan

Draftspan is a writing app built with [Lib-RichEditor](../../README.md) and HuxerUI.

## Build and run

The `Draftspan` CMake target in this directory builds the app. Its math renderer is enabled by default and requires Cargo and Rust. For native builds, run `rustup target add` with every target selected for the build.

| Preview target | Rust target |
| --- | --- |
| Windows x64 | `x86_64-pc-windows-msvc` |
| Android arm64-v8a / x86_64 | `aarch64-linux-android` / `x86_64-linux-android` |
| macOS arm64 / x86_64 | `aarch64-apple-darwin` / `x86_64-apple-darwin` |
| iOS arm64 device | `aarch64-apple-ios` |
| iOS arm64 / x86_64 simulator | `aarch64-apple-ios-sim` / `x86_64-apple-ios` |
| Linux arm64 / x86_64 | `aarch64-unknown-linux-gnu` / `x86_64-unknown-linux-gnu` |
| Web | `wasm32-unknown-emscripten` |

For Web, run `rustup toolchain install nightly --profile minimal --component rust-src`. The build compiles Rust's standard library with HuxerUI's Emscripten toolchain.

For source-only formulas, configure CMake with `-DFOLIO_BUILD_MATH_RENDERER=OFF`. On Android, pass `-PfolioBuildMathRenderer=OFF` to Gradle. On iOS, set `FOLIO_BUILD_MATH_RENDERER = OFF` in `platform/ios/Config/Local.xcconfig`.

Run these commands from this directory. The Android example selects Java 17 on macOS.

```sh
huxerui run macos --profile debug
huxerui run android --profile debug --abi arm64-v8a --java-home "$(/usr/libexec/java_home -v 17)"
```

## Sidebar and document library

The sidebar switches between **Files** and **Outline**. **Files** shows the app-owned **Workspace**, mounted directories under **External locations**, and **Recently opened** files and folders. **Outline** shows headings from the current document, highlights the section in view, and jumps to a selected heading while preserving the caret and selection. Resize or hide the sidebar on desktop. On compact hosts, **Show sidebar** opens the drawer and the file name in the header opens the tab switcher.

On hosts with app storage, the first **Save** of a new document creates a RichEditor JSON package in **Workspace**. Each package holds the document and its attachments under a stable ID. Use **Import to Workspace…** or **Import into this folder…** to copy a file and its attachments into the library. Use **Save to Workspace** to copy an open external document into the library and switch the tab to that copy. The library item menu supports renaming, moving, and permanent deletion. The confirmation for a nonempty folder shows the number of affected documents and subfolders. Deleting an open document warns if it has unsaved changes.

**Open file…** opens RichEditor JSON, `.md`, and `.markdown` directly from their source. **Open folder…** mounts an external directory for the current window session. Expand it to browse its files, or choose **Remove from external locations** to unmount it. Use **Recently opened** to reopen an external file or remount a folder. If relative images need access to their source directory, use **Locate attachments…**.

## Workspace restoration

At startup, Draftspan restores the recorded active window with its saved tabs, selected tab, and sidebar settings. On hosts that support multiple windows, it also reopens the other recorded windows. Tabs reopen with their last saved contents. If an external source is unavailable or needs authorization, use **Locate again** in its tab to select the file or folder. Closing prompts you to handle any unsaved changes. Closed tabs and windows are removed from the restore record.

Each restored tab returns to its last reading position without moving the caret or opening the software keyboard. Draftspan tries to find the same paragraph after nearby edits and falls back to the preceding heading when it cannot. Following a document link goes to its heading even if the destination tab has a saved reading position.

Use the title bar to switch between **Reading** and **Editing**. Reading mode keeps text selection, copying, find, outline navigation, and links available. Each tab remembers its mode across workspace restoration. On Android and iOS, a newly opened external document starts in reading mode. New documents and documents opened from the app workspace start in editing mode.

## Opening and saving files

Draftspan accepts `.md` and `.markdown` files opened from Windows File Explorer, macOS Finder, and Files on iOS. The Windows installer adds Draftspan to **Open with**. On Android, file managers can open a Markdown document in Draftspan by sending it as `text/markdown`. Opening a file starts Draftspan or opens it in the running app. If Draftspan can identify an already open file, it switches to its tab.

If a document fails to open, the dialog lists the import issues by file. When Markdown can be converted with reported losses, **Open lossy copy** opens an unsaved tab. Use **Save** to add the copy to **Workspace**, or **Save as…** to choose another destination. The copy can resolve relative attachments from the source folder if it has already been authorized.

Desktop windows have independent tabs and sidebars. The library and **Recently opened** are shared across windows. When saving back to an external file, **Save** checks the original for changes and uses its existing format. **Save as…** creates and binds a file in a writable directory. **Export copy…** creates a separate JSON or Markdown copy.

## Attachments

Draftspan stages newly inserted images under `attachments/` and uses relative references. Replacement gives the new image its own name, keeping the original available to Undo. **Save as…** and **Export copy…** create a subfolder for a document with local attachments. Move the entire subfolder to keep the references valid. **Open document package…** opens an existing `article.json` or `article.md` and resolves its images from the selected folder.

Draftspan resolves local PNG and JPEG files from `attachments/` and migrates structured references alongside the document.

## Settings and shortcuts

open **Settings** from the sidebar or **File actions**. **Appearance** offers **Follow system** and eight themes. **Editor** controls text size and reading width. In **Keyboard**, choose a command and press a combination on a physical keyboard to replace its current shortcuts. Confirming a conflict removes just the overlapping combination from the other command. Clear or restore a command from its shortcut dialog, or use **Restore all defaults** in the keyboard settings. Preferences apply immediately across windows and are restored on launch. On compact screens, the toolbar pins **Undo** and the overflow menu and puts the actions for the current selection first in the horizontally scrollable area. Hover or long-press an icon to see its command.

On desktop, open **Command palette…** from **File actions**, or press **Command+Shift+P** on macOS or **Ctrl+Shift+P** on other platforms. Search by command name or a common Chinese alias. Results show the current shortcut and whether the action can run at the current caret or selection. Use the arrow keys and Enter to run a result, or select it directly. Android and iOS omit the menu entry and the **Keyboard** settings page. The shortcuts, including the palette binding, remain available with a physical keyboard.

Draftspan follows the system language. Its interface supports English, Simplified Chinese, Japanese, Korean, German, French, and Spanish. English is the fallback.

Draftspan's desktop **Keyboard** settings list the configurable grid commands.

## Find and replace

Open **Find** and expand **Find and replace**. The desktop shortcut is **Command+R** on macOS or **Ctrl+R** on Windows and Linux. On Android, **Ctrl+R** works with a physical keyboard. Use the find bar on the Web to leave the browser reload shortcut available.

Search matches literal, case-sensitive text within each paragraph, heading, table cell, or code block. **Replace** advances to the next match. **Replace all** uses the matches found before the edit and can be undone in one step. Leave **Replace with** empty to delete matching text.

## Copy and paste

Copy from an editor selection and paste into another tab to keep the document structure, including formulas and full tables. On native apps, local attachments that fit in the clipboard item are copied with the text and saved alongside the destination document. If a cut cannot preserve the selected content, Draftspan keeps the selection and reports the reason. Browsers without support for the RichEditor format can still cut plain paragraphs and copy HTML or plain text.

Rich text exchanged with other apps supports bold, italic, underline, strikethrough, links, and line breaks. Choose **Paste as plain text** in the formatting overflow menu, or press Ctrl+Shift+V or Command+Shift+V.

## Formulas

Draftspan uses a custom block for standalone LaTeX and offers source editing and copying LaTeX from the formatting bar. RaTeX renders formulas locally on each supported preview platform. Unsupported expressions and builds with rendering disabled display editable source.

Draftspan registers its own formula mappings and retains the existing `huxerui.latex` type in JSON v1. It imports inline dollar spans, standalone double-dollar spans, and `math` fences. Display formulas export as `math` fences to preserve the source, including whitespace and line breaks. The JSON codec preserves formula data regardless of the active Markdown mappings.

## Try the editor

Choose **File actions** → **Load sample**, then enter Chinese text with an IME and select across paragraphs. Apply underline and bold in separate actions, undo bold, and check that the underline and committed text remain. Choose **Save as…** with JSON, then reopen the saved file with **Open file…**. Export a Markdown copy to see the underline diagnostic. Confirm **Export with loss…** to continue.

Open the text style menu to switch between paragraph, heading levels 1 through 6, and code block. Select text to insert a link, or place the caret inside an existing link to edit, copy, or remove its address.

Use **Open link** in the link menu or the mobile editing toolbar. On desktop, Command-click on macOS or Ctrl-click on Windows and Linux follows the link. HTTP and HTTPS links open in the system browser.

In the outline, use the heading's context menu or long-press it to **Copy heading link**. A link such as `#安装-1` jumps to the second matching heading in the current document. Links use the visible title and document order, so renaming or reordering headings can change their addresses.

Relative links such as `./Guide.md#setup` and `../Guide/Deploy.md` open external Markdown or RichEditor JSON documents from the source file's folder. Open a parent folder to follow links between subfolders inside it. If you opened a single file and the platform requires directory access, Draftspan asks for its containing folder. A missing file keeps the current tab open. A missing heading opens the target document and shows a status message.

Draftspan centers search matches as the viewport resizes, including when the software keyboard is dismissed.

Place the caret in the sample's ordered list and open **List numbering** to set a starting number, restart at one, or change the indent. Undo restores the previous numbering, and the new start can be saved in JSON and Markdown.

Toggle a sample task by clicking or tapping its checkbox. Use **Task list** in the desktop toolbar or under **More formatting** → **Lists and quote** on a compact screen to add a checkbox at the caret. Enter creates an unchecked item. Undo and redo restore task changes, and reopening JSON or Markdown restores their completion states.

To exercise tables, select a cell in the sample. The compact toolbar shows **+ Row** and **+ Column**. Open **Table actions** → **Column alignment** to change the selected column. Enter Chinese text in one cell, save, and reopen JSON. A Markdown copy preserves the table and its alignments while it has a header and representable styles. Turn off **Header row** to verify the diagnostic, then export with loss to save a table with a synthesized header. Use **More formatting** → **Insert** → **Insert divider** to add a thematic break.

To exercise images, place the caret in a top-level paragraph and choose **Insert image** to select a PNG or JPEG. On compact screens, find it under **More formatting** → **Insert**. Selecting an image dismisses the software keyboard and shows **Size**, **Replace**, and **+ Image** on the compact toolbar. Drag a corner or open the size dialog to set the width or height and watch the other dimension update. Size the pair to fit on the same line and undo a resize. Choose **Save as…**, move the resulting subfolder, and mount it with **Open folder…** to reopen the document from **External locations**. A Markdown copy includes the referenced local attachments.

Enter starts a new paragraph without carrying over pending inline formats. At the start of a list item's text, Enter creates an empty sibling above it and puts the caret in the new item. At the end of a parent item's leading text, Enter adds the next sibling after its sublist. With content still following an empty subitem in the parent, Enter leaves an unmarked continuation when it can preserve the parent's marker. Otherwise it removes the empty item and moves the caret to the next block, as Backspace does. Enter or Backspace on a trailing empty list item or quote paragraph exits that structure at the caret. In code and table cells, the keyboard's Enter key inserts a newline inside the block. On mobile, **Continue below** in the toolbar moves the caret to the following paragraph. Shift+Enter keeps the format on a break within a paragraph.

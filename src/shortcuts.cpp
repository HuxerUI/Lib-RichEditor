#include <huxerui/richeditor.h>

#include <array>

namespace huxerui::richeditor {
namespace {

constexpr auto DefaultCombinations(Key key, bool shift = false) {
  return std::array{
      KeyCombination{key, {.shift = shift, .meta = true}},
      KeyCombination{key, {.shift = shift, .control = true}},
      KeyCombination{key, {.shift = shift, .control = true, .meta = true}},
  };
}

constexpr auto undo = DefaultCombinations(Key::Z);
constexpr auto redo_z = DefaultCombinations(Key::Z, true);
constexpr std::array redo{
    redo_z[0], redo_z[1], redo_z[2],
    KeyCombination{Key::Y, {.control = true}},
    KeyCombination{Key::Y, {.control = true, .meta = true}},
};
constexpr auto bold = DefaultCombinations(Key::B);
constexpr auto italic = DefaultCombinations(Key::I);
constexpr auto underline = DefaultCombinations(Key::U);
constexpr auto clear_formatting = DefaultCombinations(Key::Backslash);
constexpr auto paragraph = DefaultCombinations(Key::Digit0);
constexpr auto heading_1 = DefaultCombinations(Key::Digit1);
constexpr auto heading_2 = DefaultCombinations(Key::Digit2);
constexpr auto heading_3 = DefaultCombinations(Key::Digit3);
constexpr auto heading_4 = DefaultCombinations(Key::Digit4);
constexpr auto heading_5 = DefaultCombinations(Key::Digit5);
constexpr auto heading_6 = DefaultCombinations(Key::Digit6);
constexpr auto numbered_list = DefaultCombinations(Key::Digit7, true);
constexpr auto bulleted_list = DefaultCombinations(Key::Digit8, true);
constexpr auto indent_list = DefaultCombinations(Key::BracketLeft);
constexpr auto outdent_list = DefaultCombinations(Key::BracketRight);
constexpr auto insert_table = DefaultCombinations(Key::T, true);
constexpr auto insert_row = DefaultCombinations(Key::R, true);
constexpr auto insert_column = DefaultCombinations(Key::Backslash, true);

const std::array shortcuts{
    EditorShortcutDefinition{"editor.undo", "Undo", Undo{}, undo},
    EditorShortcutDefinition{"editor.redo", "Redo", Redo{}, redo},
    EditorShortcutDefinition{"editor.bold", "Bold", ToggleMark{Mark::Bold}, bold},
    EditorShortcutDefinition{"editor.italic", "Italic", ToggleMark{Mark::Italic}, italic},
    EditorShortcutDefinition{"editor.underline", "Underline", ToggleMark{Mark::Underline}, underline},
    EditorShortcutDefinition{"editor.strikethrough", "Strikethrough", ToggleMark{Mark::Strike}, {}},
    EditorShortcutDefinition{"editor.inline-code", "Inline code", ToggleMark{Mark::Code}, {}},
    EditorShortcutDefinition{"editor.highlight", "Highlight", ToggleMark{Mark::Highlight}, {}},
    EditorShortcutDefinition{"editor.clear-formatting", "Clear formatting", ClearFormatting{}, clear_formatting},
    EditorShortcutDefinition{"editor.paragraph", "Paragraph", SetBlockKind{BlockKind::Paragraph}, paragraph},
    EditorShortcutDefinition{"editor.heading-1", "Heading 1", SetBlockKind{BlockKind::Heading, 1}, heading_1},
    EditorShortcutDefinition{"editor.heading-2", "Heading 2", SetBlockKind{BlockKind::Heading, 2}, heading_2},
    EditorShortcutDefinition{"editor.heading-3", "Heading 3", SetBlockKind{BlockKind::Heading, 3}, heading_3},
    EditorShortcutDefinition{"editor.heading-4", "Heading 4", SetBlockKind{BlockKind::Heading, 4}, heading_4},
    EditorShortcutDefinition{"editor.heading-5", "Heading 5", SetBlockKind{BlockKind::Heading, 5}, heading_5},
    EditorShortcutDefinition{"editor.heading-6", "Heading 6", SetBlockKind{BlockKind::Heading, 6}, heading_6},
    EditorShortcutDefinition{"editor.numbered-list", "Numbered list", ToggleList{ListKind::Ordered}, numbered_list},
    EditorShortcutDefinition{"editor.bulleted-list", "Bulleted list", ToggleList{ListKind::Unordered}, bulleted_list},
    EditorShortcutDefinition{"editor.task-list", "Task list", ToggleTaskMarker{}, {}},
    EditorShortcutDefinition{"editor.task.complete", "Mark task complete", SetTaskChecked{true}, {}},
    EditorShortcutDefinition{"editor.task.incomplete", "Mark task incomplete", SetTaskChecked{false}, {}},
    EditorShortcutDefinition{"editor.indent-list", "Increase list indent", IndentList{}, indent_list},
    EditorShortcutDefinition{"editor.outdent-list", "Decrease list indent", OutdentList{}, outdent_list},
    EditorShortcutDefinition{"editor.quote", "Block quote", ToggleQuote{}, {}},
    EditorShortcutDefinition{"editor.code-block", "Code block", SetBlockKind{BlockKind::CodeBlock}, {}},
    EditorShortcutDefinition{"editor.table.insert", "Insert table", InsertTable{}, insert_table},
    EditorShortcutDefinition{"editor.table.row-before", "Insert row above", InsertTableRow{TableInsertSide::Before}, {}},
    EditorShortcutDefinition{"editor.table.row-after", "Insert row below", InsertTableRow{}, insert_row},
    EditorShortcutDefinition{"editor.table.delete-row", "Delete row", DeleteTableRow{}, {}},
    EditorShortcutDefinition{"editor.table.column-before", "Insert column before",
                             InsertTableColumn{TableInsertSide::Before}, {}},
    EditorShortcutDefinition{"editor.table.column-after", "Insert column after", InsertTableColumn{}, insert_column},
    EditorShortcutDefinition{"editor.table.delete-column", "Delete column", DeleteTableColumn{}, {}},
    EditorShortcutDefinition{"editor.table.header", "Header row", ToggleTableHeader{}, {}},
    EditorShortcutDefinition{"editor.table.delete", "Delete table", DeleteTable{}, {}},
};

} // namespace

std::span<const EditorShortcutDefinition> DefaultEditorShortcuts() { return shortcuts; }

} // namespace huxerui::richeditor

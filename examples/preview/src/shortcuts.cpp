#include "shortcuts.h"

#include <algorithm>
#include <array>

#include "app_resources.h"

using namespace huxerui;
using namespace huxerui::richeditor;

namespace l10n = app::strings;

namespace folio {
namespace {

constexpr bool command_platform =
#if defined(__APPLE__)
    true;
#else
    false;
#endif

constexpr auto Primary(Key key, bool shift = false) {
  return std::array{KeyCombination{key, {.shift = shift, .control = !command_platform, .meta = command_platform}}};
}

constexpr auto new_document = Primary(Key::N);
constexpr auto open_file = Primary(Key::O);
constexpr auto save = Primary(Key::S);
constexpr auto save_as = Primary(Key::S, true);
constexpr auto toggle_sidebar = Primary(Key::L, true);
constexpr auto settings = Primary(Key::Comma);
constexpr auto command_palette = Primary(Key::P, true);
constexpr std::array find{
    KeyCombination{Key::F, {.meta = true}},
    KeyCombination{Key::F, {.control = true}},
    KeyCombination{Key::F, {.control = true, .meta = true}},
};
#if defined(__EMSCRIPTEN__)
constexpr std::array<KeyCombination, 0> find_replace{};
#else
constexpr auto find_replace = Primary(Key::R);
#endif

constexpr std::array punctuation{"Backquote", "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
                                 "Semicolon", "Quote", "Comma", "Period", "Slash"};
constexpr std::string_view punctuation_labels = "`-=[]\\;',./";
constexpr std::array functions{"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view digits = "0123456789";

std::string_view EditorSearchAliases(std::string_view id) {
  using Alias = std::pair<std::string_view, std::string_view>;
  static constexpr std::array aliases{
      Alias{"editor.bold", "加粗|粗体"},
      Alias{"editor.italic", "斜体"},
      Alias{"editor.undo", "撤销"},
      Alias{"editor.redo", "重做"},
      Alias{"editor.task-list", "checklist|todo|待办|任务列表"},
      Alias{"editor.task.complete", "勾选|完成任务"},
      Alias{"editor.task.incomplete", "取消勾选|未完成"},
      Alias{"editor.table.insert", "add table|插入表格"},
      Alias{"editor.table.row-before", "add row above|上方加行"},
      Alias{"editor.table.row-after", "add row below|加行"},
      Alias{"editor.table.delete-row", "remove row|删行"},
      Alias{"editor.table.column-after", "add column after|加列"},
      Alias{"editor.table.delete-column", "remove column|删列"},
      Alias{"editor.table.header", "表头"},
  };
  const auto alias = std::ranges::find(aliases, id, &Alias::first);
  return alias == aliases.end() ? std::string_view{} : alias->second;
}

std::string_view KeyName(Key key) {
  if (key >= Key::A && key <= Key::Z) {
    return letters.substr(static_cast<int>(key) - static_cast<int>(Key::A), 1);
  }
  if (key >= Key::Digit0 && key <= Key::Digit9) {
    return digits.substr(static_cast<int>(key) - static_cast<int>(Key::Digit0), 1);
  }
  if (key >= Key::Backquote && key <= Key::Slash) {
    return punctuation[static_cast<int>(key) - static_cast<int>(Key::Backquote)];
  }
  if (key >= Key::F1 && key <= Key::F12) {
    return functions[static_cast<int>(key) - static_cast<int>(Key::F1)];
  }
  return {};
}

Key KeyFromName(std::string_view name) {
  if (name.size() == 1) {
    const auto letter = letters.find(name.front());
    if (letter != letters.npos) {
      return static_cast<Key>(static_cast<int>(Key::A) + letter);
    }
    const auto digit = digits.find(name.front());
    if (digit != digits.npos) {
      return static_cast<Key>(static_cast<int>(Key::Digit0) + digit);
    }
  }
  if (const auto key = std::ranges::find(punctuation, name); key != punctuation.end()) {
    return static_cast<Key>(static_cast<int>(Key::Backquote) + (key - punctuation.begin()));
  }
  if (const auto key = std::ranges::find(functions, name); key != functions.end()) {
    return static_cast<Key>(static_cast<int>(Key::F1) + (key - functions.begin()));
  }
  return Key::Unknown;
}

std::string_view UnavailableKey(KeyCombination keys) {
  if (!(keys.modifiers.control || keys.modifiers.meta) || keys.modifiers.alt) {
    return "Use Ctrl or Command, with optional Shift.";
  }
  if (keys.key == Key::A || keys.key == Key::C || keys.key == Key::V || keys.key == Key::X) {
    return "This key is reserved for clipboard or selection commands.";
  }
  if (KeyName(keys.key).empty()) {
    return "Use a letter, number, punctuation key, or F1–F12.";
  }
  if (command_platform && keys.modifiers.meta && !keys.modifiers.control && !keys.modifiers.shift &&
      (keys.key == Key::H || keys.key == Key::M || keys.key == Key::Q || keys.key == Key::W)) {
    return "This shortcut is used by the system.";
  }
  return {};
}

std::optional<KeyCombination> ParseKey(std::string_view value) {
  KeyModifiers modifiers;
  while (true) {
    const auto separator = value.find('+');
    if (separator == value.npos) {
      break;
    }
    const auto token = value.substr(0, separator);
    if (token == "Control" && !modifiers.control) {
      modifiers.control = true;
    } else if (token == "Meta" && !modifiers.meta) {
      modifiers.meta = true;
    } else if (token == "Shift" && !modifiers.shift) {
      modifiers.shift = true;
    } else {
      return std::nullopt;
    }
    value.remove_prefix(separator + 1);
  }
  const KeyCombination keys{KeyFromName(value), modifiers};
  return UnavailableKey(keys).empty() ? std::optional{keys} : std::nullopt;
}

} // namespace

std::span<const ShortcutCommand> ShortcutCommands() {
  static const auto commands = [] {
    std::vector<ShortcutCommand> entries{
        {"folio.new", "New document", FolioCommand::NewDocument, new_document, "新建"},
        {"folio.open", "Open file", FolioCommand::OpenFile, open_file, "打开文件"},
        {"folio.save", "Save", FolioCommand::Save, save, "保存"},
        {"folio.save-as", "Save as", FolioCommand::SaveAs, save_as, "另存为"},
        {"folio.find", "Find in document", FolioCommand::Find, find, "search|查找|搜索"},
        {"folio.find-replace", "Find and replace", FolioCommand::FindReplace, find_replace, "replace|替换|查找替换"},
        {"folio.toggle-sidebar", "Toggle file sidebar", FolioCommand::ToggleSidebar, toggle_sidebar},
        {"folio.settings", "Open settings", FolioCommand::Settings, settings, "preferences|设置"},
        {"folio.command-palette", "Command palette", FolioCommand::OpenCommandPalette, command_palette},
    };
    for (const auto& editor : DefaultEditorShortcuts()) {
      entries.push_back({editor.id, editor.label, editor.command, editor.defaults, EditorSearchAliases(editor.id)});
    }
    return entries;
  }();
  return commands;
}

StringVariant CommandLabel(const ShortcutCommand& command) {
  struct Label {
    std::string_view id;
    StringResource resource;
  };
  static const std::array labels{
      Label{"folio.new", l10n::new_document},
      Label{"folio.open", l10n::open_file},
      Label{"folio.save", l10n::save},
      Label{"folio.save-as", l10n::save_as},
      Label{"folio.find", l10n::find_in_document},
      Label{"folio.find-replace", l10n::find_and_replace},
      Label{"folio.toggle-sidebar", l10n::toggle_sidebar},
      Label{"folio.settings", l10n::open_settings},
      Label{"folio.command-palette", l10n::command_palette},
      Label{"editor.undo", l10n::undo},
      Label{"editor.redo", l10n::redo},
      Label{"editor.bold", l10n::bold},
      Label{"editor.italic", l10n::italic},
      Label{"editor.underline", l10n::underline},
      Label{"editor.strikethrough", l10n::strikethrough},
      Label{"editor.inline-code", l10n::inline_code},
      Label{"editor.highlight", l10n::highlight},
      Label{"editor.clear-formatting", l10n::clear_formatting},
      Label{"editor.paragraph", l10n::paragraph},
      Label{"editor.heading-1", l10n::heading_1},
      Label{"editor.heading-2", l10n::heading_2},
      Label{"editor.heading-3", l10n::heading_3},
      Label{"editor.heading-4", l10n::heading_4},
      Label{"editor.heading-5", l10n::heading_5},
      Label{"editor.heading-6", l10n::heading_6},
      Label{"editor.numbered-list", l10n::numbered_list},
      Label{"editor.bulleted-list", l10n::bulleted_list},
      Label{"editor.task-list", l10n::task_list},
      Label{"editor.task.complete", l10n::mark_task_complete},
      Label{"editor.task.incomplete", l10n::mark_task_incomplete},
      Label{"editor.indent-list", l10n::increase_list_indent},
      Label{"editor.outdent-list", l10n::decrease_list_indent},
      Label{"editor.quote", l10n::block_quote},
      Label{"editor.code-block", l10n::code_block},
      Label{"editor.table.insert", l10n::insert_table},
      Label{"editor.table.row-before", l10n::insert_row_above},
      Label{"editor.table.row-after", l10n::insert_row_below},
      Label{"editor.table.delete-row", l10n::delete_row},
      Label{"editor.table.column-before", l10n::insert_column_before},
      Label{"editor.table.column-after", l10n::insert_column_after},
      Label{"editor.table.delete-column", l10n::delete_column},
      Label{"editor.table.header", l10n::header_row},
      Label{"editor.table.delete", l10n::delete_table},
  };
  const auto match = std::ranges::find(labels, command.id, &Label::id);
  return match == labels.end() ? StringVariant{command.label} : StringVariant{match->resource};
}

StringResource CommandGroup(const ShortcutCommand& command) {
  if (std::holds_alternative<FolioCommand>(command.command)) {
    return l10n::workbench;
  }
  return command.id.starts_with("editor.table.") ? l10n::table : l10n::editor;
}

std::span<const KeyCombination> EffectiveShortcuts(const ShortcutOverrides& overrides, const ShortcutCommand& command) {
  const auto entry = overrides.find(command.id);
  return entry == overrides.end() ? command.defaults : std::span<const KeyCombination>{entry->second};
}

ShortcutChange PlanShortcutChange(const ShortcutOverrides& current, const ShortcutCommand& target,
                                  const std::optional<ShortcutKeys>& replacement) {
  ShortcutChange change{current, {}};
  const std::string id(target.id);
  if (replacement) {
    change.overrides[id] = *replacement;
  } else {
    change.overrides.erase(id);
  }
  const auto desired = EffectiveShortcuts(change.overrides, target);
  for (const auto& other : ShortcutCommands()) {
    if (other.id == target.id) {
      continue;
    }
    const auto before = EffectiveShortcuts(current, other);
    ShortcutKeys remaining;
    for (const auto& keys : before) {
      if (std::ranges::find(desired, keys) != desired.end()) {
        change.conflicts.push_back({&other, keys});
      } else {
        remaining.push_back(keys);
      }
    }
    if (remaining.size() != before.size()) {
      change.overrides[std::string(other.id)] = std::move(remaining);
    }
  }
  return change;
}

void ReconcileShortcutOverrides(ShortcutOverrides& overrides) {
  std::vector<const ShortcutCommand*> customized;
  for (const auto& command : ShortcutCommands()) {
    if (overrides.contains(command.id)) {
      customized.push_back(&command);
    }
  }
  std::vector<std::string_view> conflicted;
  for (std::size_t i = 0; i < customized.size(); ++i) {
    const auto first = EffectiveShortcuts(overrides, *customized[i]);
    for (std::size_t j = i + 1; j < customized.size(); ++j) {
      const auto second = EffectiveShortcuts(overrides, *customized[j]);
      if (std::ranges::find_first_of(first, second) != first.end()) {
        conflicted.push_back(customized[i]->id);
        conflicted.push_back(customized[j]->id);
      }
    }
  }
  std::erase_if(overrides, [&](const auto& entry) {
    return std::ranges::find(conflicted, entry.first) != conflicted.end();
  });
  std::erase_if(customized, [&](const ShortcutCommand* command) { return !overrides.contains(command->id); });
  // A shortcut saved by an earlier version takes precedence over a newly added default.
  for (const auto* command : customized) {
    overrides = PlanShortcutChange(overrides, *command, overrides.find(command->id)->second).overrides;
  }
}

ResolvedShortcuts ResolveShortcuts(const ShortcutOverrides& overrides) {
  auto editor = std::make_shared<EditorKeymap>();
  ResolvedShortcuts resolved{.editor = editor};
  for (const auto& entry : ShortcutCommands()) {
    for (const auto& keys : EffectiveShortcuts(overrides, entry)) {
      if (const auto* command = std::get_if<FolioCommand>(&entry.command)) {
        resolved.window.push_back({keys, *command});
      } else {
        editor->push_back({keys, std::get<EditorCommand>(entry.command)});
      }
    }
  }
  return resolved;
}

std::optional<ShortcutKeys> ParseShortcutKeys(std::string_view value) {
  ShortcutKeys keys;
  if (value == "none") {
    return keys;
  }
  while (true) {
    const auto separator = value.find(',');
    const auto key = ParseKey(value.substr(0, separator));
    if (!key || std::ranges::find(keys, *key) != keys.end()) {
      return std::nullopt;
    }
    keys.push_back(*key);
    if (separator == value.npos) {
      return keys;
    }
    value.remove_prefix(separator + 1);
  }
}

std::string SerializeShortcutKeys(std::span<const KeyCombination> keys) {
  std::string value;
  for (const auto& chord : keys) {
    if (!value.empty()) {
      value += ',';
    }
    if (chord.modifiers.control) {
      value += "Control+";
    }
    if (chord.modifiers.meta) {
      value += "Meta+";
    }
    if (chord.modifiers.shift) {
      value += "Shift+";
    }
    value += KeyName(chord.key);
  }
  return value.empty() ? "none" : value;
}

std::string_view ShortcutRecordingError(KeyCombination keys) {
  if (keys.modifiers.control && keys.modifiers.meta) {
    return "Use either Ctrl or Command.";
  }
  return UnavailableKey(keys);
}

std::string ShortcutLabel(KeyCombination keys) {
  std::string label;
  if (keys.modifiers.control) {
    label += "Ctrl + ";
  }
  if (keys.modifiers.meta) {
    label += command_platform ? "⌘ + " : "Meta + ";
  }
  if (keys.modifiers.shift) {
    label += "Shift + ";
  }
  if (keys.key >= Key::Backquote && keys.key <= Key::Slash) {
    label += punctuation_labels[static_cast<int>(keys.key) - static_cast<int>(Key::Backquote)];
  } else {
    label += KeyName(keys.key);
  }
  return label;
}

std::string PreferredShortcutLabel(std::span<const KeyCombination> keys) {
  if (keys.empty()) {
    return "Unassigned";
  }
  const auto preferred = std::ranges::find_if(keys, [](KeyCombination keys) {
    return keys.modifiers.meta == command_platform && keys.modifiers.control != command_platform;
  });
  return ShortcutLabel(preferred == keys.end() ? keys.front() : *preferred);
}

} // namespace folio

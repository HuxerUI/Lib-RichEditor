#pragma once

#include <map>
#include <optional>
#include <string>

#include <huxerui/richeditor.h>

namespace folio {

enum class FolioCommand { NewDocument, OpenFile, Save, SaveAs, Find, ToggleSidebar, Settings, OpenCommandPalette };

/// Stable identity and default bindings for a Folio action or a RichEditor command.
struct ShortcutCommand {
  std::string_view id;
  std::string_view label;
  std::variant<FolioCommand, huxerui::richeditor::EditorCommand> command;
  std::span<const huxerui::richeditor::KeyCombination> defaults;
  std::string_view search_aliases{};
};

using ShortcutKeys = std::vector<huxerui::richeditor::KeyCombination>;

/// An absent entry inherits defaults. An empty list explicitly disables the command's shortcuts.
using ShortcutOverrides = std::map<std::string, ShortcutKeys, std::less<>>;

struct ShortcutConflict {
  const ShortcutCommand* owner;
  huxerui::richeditor::KeyCombination keys;

  bool operator==(const ShortcutConflict&) const = default;
};

struct ShortcutChange {
  ShortcutOverrides overrides;
  std::vector<ShortcutConflict> conflicts;
};

struct WindowKeyBinding {
  huxerui::richeditor::KeyCombination keys;
  FolioCommand command;
};

struct ResolvedShortcuts {
  std::vector<WindowKeyBinding> window;
  std::shared_ptr<const huxerui::richeditor::EditorKeymap> editor;
};

std::span<const ShortcutCommand> ShortcutCommands();
std::string_view CommandGroup(const ShortcutCommand& command);
std::span<const huxerui::richeditor::KeyCombination> EffectiveShortcuts(
    const ShortcutOverrides& overrides, const ShortcutCommand& command);

/// Plans a change without mutating settings. Null restores defaults; an empty list clears the shortcut.
/// Conflicting commands retain each effective binding outside the replacement list.
ShortcutChange PlanShortcutChange(const ShortcutOverrides& current, const ShortcutCommand& target,
                                  const std::optional<ShortcutKeys>& replacement);

/// Discards ambiguous saved overrides and preserves valid custom shortcuts when defaults change.
void ReconcileShortcutOverrides(ShortcutOverrides& overrides);
ResolvedShortcuts ResolveShortcuts(const ShortcutOverrides& overrides);

/// Parses the platform-independent setting value, including an explicit unbinding written as "none".
std::optional<ShortcutKeys> ParseShortcutKeys(std::string_view value);
std::string SerializeShortcutKeys(std::span<const huxerui::richeditor::KeyCombination> keys);

/// Returns an error for a combination the recorder cannot assign. An empty string means it can be assigned.
std::string_view ShortcutRecordingError(huxerui::richeditor::KeyCombination keys);
std::string ShortcutLabel(huxerui::richeditor::KeyCombination keys);
std::string PreferredShortcutLabel(std::span<const huxerui::richeditor::KeyCombination> keys);

} // namespace folio

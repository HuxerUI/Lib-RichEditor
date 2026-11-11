#include "shortcut_settings.h"

#include <huxerui/huxerui.h>

#include "settings.h"
#include "app_resources.h"

using namespace huxerui;
using namespace huxerui::richeditor;

namespace l10n = app::strings;

namespace folio {
namespace {

#if defined(__APPLE__)
constexpr auto plain_paragraph_shortcut = "⌘ + Enter";
#else
constexpr auto plain_paragraph_shortcut = "Ctrl + Enter";
#endif

struct ShortcutDraft {
  std::optional<KeyCombination> keys;
  bool restoring = false;
  std::string_view error;

  bool operator==(const ShortcutDraft&) const = default;
};

View KeyLabel(StringVariant label, const ThemeSpec& theme) {
  return Text(std::move(label), TextRole::Label).Style(TextStyle{Font::Monospace(12.0F), theme.colors.on_surface})
      .With(Padding(EdgeInsets::Symmetric(7.0F, 4.0F)), Background(theme.colors.surface_container_high),
            CornerRadius(theme.shapes.extra_small));
}

void ApplyShortcutChange(const std::shared_ptr<SettingsStore>& store, DialogContext context,
                         const ShortcutCommand& command,
                         const std::optional<ShortcutKeys>& replacement,
                         const std::vector<ShortcutConflict>& confirmed = {}) {
  auto settings = store->Get();
  auto change = PlanShortcutChange(settings.shortcuts, command, replacement);
  if (change.conflicts == confirmed) {
    settings.shortcuts = std::move(change.overrides);
    store->Set(std::move(settings));
    context.Dismiss();
  }
}

[[huxerui::composable]]
View EditShortcutDialog(DialogContext context, const ShortcutCommand* command) {
  const auto& theme = UseTheme();
  const auto store = UseService<SettingsStore>();
  const auto focus = UseViewHandle();
  const auto draft = UseState(ShortcutDraft{});
  const auto focused = UseState(false);
  Lifecycle([focus] { focus.RequestFocus(true); });
  const auto& value = draft.Get();
  const auto captured_label = value.keys ? StringVariant{ShortcutLabel(*value.keys)} : StringVariant{l10n::press_shortcut};
  const auto& overrides = store->Get().shortcuts;
  const auto keys = EffectiveShortcuts(overrides, *command);
  Views key_labels;
  if (keys.empty()) {
    key_labels.Add(KeyLabel(l10n::unassigned, theme));
  } else {
    for (const auto& combination : keys) {
      key_labels.Add(KeyLabel(ShortcutLabel(combination), theme));
    }
  }
  const auto capture = Row {
    Text(captured_label, TextRole::Label).With(Grow(), Foreground(theme.colors.on_surface)),
  }.OnClick([focus] { focus.RequestFocus(true); })
    .On<ViewEvents::FocusChanged>([focused](bool value) { focused = value; })
    .On<ViewEvents::KeyIntercept>([draft](const KeyEvent& event) {
      if (event.type != KeyEventType::Down || event.key == Key::Tab || event.key == Key::Escape ||
          (event.key >= Key::ShiftLeft && event.key <= Key::ScrollLock)) {
        return false;
      }
      if (!event.repeat) {
        const KeyCombination keys{event.key, event.modifiers};
        const auto error = ShortcutRecordingError(keys);
        draft = ShortcutDraft{.keys = error.empty() ? std::optional{keys} : std::nullopt, .error = error};
      }
      return true;
    }).With(focus, Focusable{}, Frame{.min_height = 48.0F}, CrossAlign(CrossAxisAlignment::Center),
            Padding(EdgeInsets::Symmetric(12.0F, 5.0F)), Background(theme.colors.surface_container),
            Border{focused.Get() ? theme.colors.primary : theme.colors.outline, 1.0F},
            CornerRadius(theme.shapes.small), PointerCursor(PointerCursorKind::Hand),
            Semantics{.role = SemanticRole::Button, .label = l10n::record_shortcut, .value = captured_label,
                      .identifier = "shortcut-capture", .descendants = SemanticDescendantPolicy::Exclude})
      .Key("shortcut-capture");
  const bool can_apply = value.keys.has_value() || value.restoring;
  std::optional<ShortcutKeys> replacement;
  if (value.keys) {
    replacement = ShortcutKeys{*value.keys};
  }
  auto conflicts = can_apply ? PlanShortcutChange(overrides, *command, replacement).conflicts
                             : std::vector<ShortcutConflict>{};
  Views notices;
  if (!value.error.empty()) {
    notices.Add(Text(std::string(value.error), TextRole::Label).With(
        Foreground(theme.colors.error), Semantics{.live_region = SemanticLiveRegion::Polite}));
  }
  if (!conflicts.empty()) {
    notices.Add(Text(l10n::replace_shortcut_for, TextRole::Label).With(
        Foreground(theme.colors.on_surface_variant), Semantics{.live_region = SemanticLiveRegion::Polite}));
    for (const auto& conflict : conflicts) {
      notices.Add(Text(UseString(CommandLabel(*conflict.owner)) + " · " + UseString(CommandGroup(*conflict.owner)) + " · " +
                       ShortcutLabel(conflict.keys), TextRole::Label)
                      .With(Semantics{.identifier = "shortcut-conflict-" + std::string(conflict.owner->id)}));
    }
  }
  const auto body = ScrollView(Column {
    Text(l10n::current_shortcuts, TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    Flow {std::move(key_labels)}.With(Spacing(5.0F)),
    capture,
    Text(l10n::physical_keyboard, TextRole::Label)
        .With(Foreground(theme.colors.on_surface_variant)),
    Column {std::move(notices)}.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Flow {
      Button(l10n::clear_shortcut).OnClick([=] { ApplyShortcutChange(store, context, *command, ShortcutKeys{}); })
          .With(Enabled(!keys.empty()), Semantics{.identifier = "shortcut-clear"}),
      Button(l10n::restore_default).OnClick([=] {
        if (PlanShortcutChange(store->Get().shortcuts, *command, std::nullopt).conflicts.empty()) {
          ApplyShortcutChange(store, context, *command, std::nullopt);
        } else {
          draft = ShortcutDraft{.restoring = true};
        }
      }).With(Enabled(overrides.contains(command->id)), Semantics{.identifier = "shortcut-restore"}),
    }.With(Spacing(6.0F)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch))).With(ScrollBar());
  const auto apply = value.restoring ? l10n::replace_and_restore : conflicts.empty() ? l10n::set_shortcut : l10n::replace_shortcut;
  return ScrollableDialogPanel(
      Text(CommandLabel(*command), TextRole::Title), body,
      DialogActions(theme, Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
          Button(apply).OnClick([store, context, command, replacement, conflicts = std::move(conflicts)] {
            ApplyShortcutChange(store, context, *command, replacement, conflicts);
          }).With(Enabled(can_apply), Semantics{.identifier = "shortcut-apply"})),
      theme, 416.0F).With(Semantics{.label = l10n::change_shortcut, .identifier = "shortcut-dialog"});
}

[[huxerui::composable]]
View RestoreShortcutsDialog(DialogContext context) {
  const auto& theme = UseTheme();
  const auto store = UseService<SettingsStore>();
  return ScrollableDialogPanel(
      Text(l10n::restore_shortcuts_title, TextRole::Title),
      Text(l10n::restore_shortcuts_hint, TextRole::Body),
      DialogActions(theme, Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
          Button(l10n::restore_defaults).OnClick([store, context] {
            auto settings = store->Get();
            settings.shortcuts.clear();
            store->Set(std::move(settings));
            context.Dismiss();
          }).With(Semantics{.identifier = "shortcuts-restore-confirm"})),
      theme, 376.0F);
}

View ShortcutHeading(StringVariant name, const ThemeSpec& theme) {
  return Text(name, TextRole::Label).Style(TextStyle{
      Font::FromFamily(theme.typography.family, theme.typography.label_large).WithWeight(FontWeight::SemiBold),
      theme.colors.on_surface}).With(Semantics{.role = SemanticRole::Heading, .heading_level = 2});
}

[[huxerui::composable]]
View ShortcutRows(const ThemeSpec& theme, const DialogHandle& dialog, const ShortcutOverrides& overrides,
                  const StringResource& group) {
  Views rows;
  for (const auto& command : ShortcutCommands()) {
    if (CommandGroup(command) != group) {
      continue;
    }
    const auto bindings = EffectiveShortcuts(overrides, command);
    const auto keys = bindings.empty() ? UseString(l10n::unassigned) : PreferredShortcutLabel(bindings);
    const auto label = UseString(CommandLabel(command));
    const auto id = "shortcut-" + std::string(command.id);
    rows.Add(Row {
      Text(label, TextRole::Label).With(Grow()),
      KeyLabel(keys, theme),
    }.OnClick([dialog, command = &command] { dialog.Show(EditShortcutDialog, command); })
      .With(Frame{.min_height = 44.0F}, Padding(EdgeInsets::Symmetric(8.0F, 3.0F)), Spacing(8.0F),
            CrossAlign(CrossAxisAlignment::Center), Focusable{}, CornerRadius(theme.shapes.small),
            PointerCursor(PointerCursorKind::Hand), theme.interactions.indication,
            Semantics{.role = SemanticRole::Button, .label = label + ", " + keys,
                      .identifier = id, .descendants = SemanticDescendantPolicy::Exclude}).Key(id));
  }
  return Column {std::move(rows)}.With(CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]]
View KeyboardSettings() {
  const auto& theme = UseTheme();
  const auto dialog = UseDialog();
  const auto& overrides = UseService<SettingsStore>()->Get().shortcuts;
  const auto fixed_key = [&](StringVariant label, const char* keys) {
    return Row {Text(label, TextRole::Label).With(Grow()), KeyLabel(keys, theme)}
        .With(Frame{.min_height = 35.0F}, Padding(EdgeInsets::Symmetric(8.0F, 3.0F)),
              Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
  };
  return Column {
    Row {
      ShortcutHeading(l10n::workbench, theme).With(Grow()),
      Button(l10n::restore_all_defaults).OnClick([dialog] { dialog.Show(RestoreShortcutsDialog); })
          .With(Enabled(!overrides.empty()), Semantics{.identifier = "shortcuts-restore-all"}).Key("shortcuts-restore-all"),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    ShortcutRows(theme, dialog, overrides, l10n::workbench),
    Divider(),
    ShortcutHeading(l10n::editor, theme),
    ShortcutRows(theme, dialog, overrides, l10n::editor),
    fixed_key(l10n::continue_in_plain_paragraph, plain_paragraph_shortcut),
    Divider(),
    ShortcutHeading(l10n::table, theme),
    ShortcutRows(theme, dialog, overrides, l10n::table),
    fixed_key(l10n::next_cell_or_row, "Tab"),
    fixed_key(l10n::previous_cell, "Shift + Tab"),
    Divider(),
    ShortcutHeading(l10n::find_controls, theme),
    fixed_key(l10n::next_match, "Enter"),
    fixed_key(l10n::previous_match, "Shift + Enter"),
    fixed_key(l10n::close_find, "Esc"),
  }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace folio

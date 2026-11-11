#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

#include "../examples/preview/src/appearance.h"
#include "../examples/preview/src/labels.h"
#include "../examples/preview/src/settings.h"

using namespace huxerui;
using namespace huxerui::richeditor;
using namespace huxerui::testing;
using namespace folio;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, std::string message) {
  if (!condition) {
    throw std::runtime_error(std::move(message));
  }
}

Point Center(Rect rect) { return {rect.x + rect.width / 2.0F, rect.y + rect.height / 2.0F}; }

double Luminance(Color color) {
  const auto linear = [](double channel) {
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.red) + 0.7152 * linear(color.green) + 0.0722 * linear(color.blue);
}

void PresetContrast() {
  std::set<std::string_view> ids;
  std::size_t dark = 0;
  for (const auto& preset : ThemePresets()) {
    Check(ids.insert(preset.id).second && preset.id != system_theme, "Each theme has a unique explicit ID");
    Check(&ResolveTheme(preset.id, ThemeBrightness::Dark) == &preset &&
              &ResolveTheme(preset.id, ThemeBrightness::Light) == &preset,
          "An explicit theme is independent of the system preference");
    dark += preset.brightness == ThemeBrightness::Dark;
    const auto& colors = preset.colors;
    const auto readable = [&](Color foreground, Color background, const char* role) {
      const auto [low, high] = std::minmax({Luminance(foreground), Luminance(background)});
      Check((high + 0.05) / (low + 0.05) >= 4.5, std::string(preset.name) + " " + role + " contrast");
    };
    readable(colors.on_surface, colors.surface, "editor text");
    readable(colors.on_surface, colors.surface_container_highest, "code text");
    readable(colors.on_surface_variant, colors.surface_container, "toolbar label");
    readable(colors.on_surface_variant, colors.surface_container_low, "sidebar label");
    readable(colors.on_surface_variant, colors.surface_container_high, "menu label");
    readable(colors.primary, colors.surface, "editor link");
    readable(colors.primary, colors.primary_container, "selected theme indicator");
    readable(colors.on_primary, colors.primary, "primary action");
    readable(colors.on_primary_container, colors.primary_container, "active control");
    readable(colors.on_tertiary_container, colors.tertiary_container, "highlighted text");
  }
  Check(ids.size() == 8 && dark == 4, "Four light and four dark presets are available");
  Check(ResolveTheme(system_theme, ThemeBrightness::Light).id == "grove" &&
            ResolveTheme(system_theme, ThemeBrightness::Dark).id == "evergreen",
        "Follow system resolves to the matching default preset");
}

[[huxerui::composable]]
View SettingsLauncher() {
  const auto dialog = UseDialog();
  const bool dark = UseTheme().brightness == ThemeBrightness::Dark;
  return Column {
    Button("Open settings").OnClick([dialog] { dialog.Show(SettingsDialog); }),
    Text(dark ? "Dark workbench" : "Light workbench"),
  };
}

View SettingsFixture() {
  const auto& settings = UseService<SettingsStore>()->Get();
  const auto brightness = settings.theme == system_theme ? UseSystemThemeBrightness() : ThemeBrightness::Light;
  return Theme(FolioTheme(ResolveTheme(settings.theme, brightness)), SettingsLauncher());
}

void SettingsFlow(Size viewport, std::optional<File> directory, bool compact) {
  const Application app(SettingsFixture, {
      .show_debug_overlay = false,
      .application_hooks = {[directory](ApplicationContext& context) {
        context.Provide(std::make_shared<SettingsStore>(directory));
      }},
  });
  UiTest ui(app, {.viewport = viewport, .safe_area = {.top = 22.0F, .bottom = 18.0F}});
  ui.Find(UiSelector::Text("Open settings")).Tap();
  ui.Pump(300ms);

  const auto panel = ui.Find(UiSelector::Key("settings-dialog")).One().bounds;
  const auto scroll = ui.Find(UiSelector::Key("appearance-options"));
  const auto options = scroll.One().bounds;
  Check(options.y + options.height <= panel.y + panel.height,
        "The options scroll viewport stays inside the dialog in a short window");
  const auto theme_bounds = [&](std::string_view key) { return ui.Find(UiSelector::Key(key)).One().bounds; };
  Check((theme_bounds("theme-grove").y == theme_bounds("theme-parchment").y) == !compact,
        "Theme grid adapts to available width");
  const auto checked = [&](const char* id) {
    return ui.FindSemantics(UiSemanticSelector::Identifier(id)).One().checked == SemanticCheckedState::Checked;
  };
  Check(checked("theme-parchment") && !checked("theme-system"), "A fresh settings panel selects Parchment");
  for (const auto& preset : ThemePresets()) {
    const auto option = ui.FindSemantics(UiSemanticSelector::Identifier("theme-" + std::string(preset.id))).One();
    Check(option.role == SemanticRole::RadioButton && option.label == preset.name, "A theme has an accessible label");
  }
  ui.Find(UiSelector::Key("theme-porcelain")).Tap();
  Check(checked("theme-porcelain") && !checked("theme-system"), "Theme choice updates while the dialog is open");
  ui.Find(UiSelector::Key("theme-system")).Tap();
  Check(checked("theme-system"), "Follow system can be restored");
  scroll.ScrollUntil(UiSelector::Key("theme-ember"), {.step = {0.0F, 80.0F}}).Tap();
  Check(checked("theme-ember"), "Dark preset is selectable");
  const auto select_page = [&](const char* label) {
    const auto tab = ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Tab), UiSemanticSelector::Label(label))).One();
    ui.TapAt(Center(tab.bounds));
  };
  select_page("Editor");
  const auto editor_options = ui.Find(UiSelector::Key("editor-options"));
  editor_options.ScrollUntil(UiSelector::Key("text-size-2")).Tap();
  Check(checked("text-size-2"), "Document text size is selectable");
  editor_options.ScrollUntil(UiSelector::Key("reading-width-2")).Tap();
  Check(checked("reading-width-2"), "Reading width is selectable");
  select_page("Keyboard");
  Check(ui.Find(UiSelector::Key("keyboard-shortcuts")).Exists() &&
            ui.Find(UiSelector::Text("Find in document")).Exists(),
        "Settings offers a keyboard page with the search shortcut");
  ui.Find(UiSelector::Text("Done")).Tap();
  ui.Pump(300ms);
  Check(!ui.FindSemantics(UiSemanticSelector::Identifier("settings-dialog")).Exists() &&
            ui.Find(UiSelector::Text("Dark workbench")).Exists(),
        "Closing settings reveals the workbench with the chosen theme");

  if (directory) {
    ui.PumpUntil([&] {
      std::this_thread::sleep_for(2ms);
      const auto stored = directory->Child("settings.cfg").ReadString();
      return stored.Succeeded() && stored.Value() == "theme=ember\ntext_size=19\nreading_width=920\n";
    });
  }
}

void CheckLoaded(const File& directory, FolioSettings expected) {
  const Application app([]() -> View { return Text(""); }, {
      .show_debug_overlay = false,
      .application_hooks = {[&](ApplicationContext&) {
        Check(SettingsStore(directory).Get() == expected, "Settings load in a new application");
      }},
  });
  UiTest ui(app);
}

#if !defined(__EMSCRIPTEN__)
FolioSettings SettingsBeforeQuit() {
  return {.theme = "lilac", .editor = {.text_size = 14, .reading_width = 560},
          .shortcuts = {{"folio.find", {{Key::G, {.control = true}}}}}};
}

void SettingsSurviveQuit(const File& directory) {
  {
    const Application app([]() -> View {
      const auto store = UseService<SettingsStore>();
      const auto application = UseApplication();
      return Text("Settings").On<ViewEvents::KeyIntercept>([store, application](const KeyEvent& event) {
        if (event.type != KeyEventType::Down || event.key != Key::F8) {
          return false;
        }
        store->Set({.theme = "grove"});
        store->Set(SettingsBeforeQuit());
        application.Quit();
        return true;
      });
    }, {
        .show_debug_overlay = false,
        .application_hooks = {[directory](ApplicationContext& context) {
          context.Provide(std::make_shared<SettingsStore>(directory));
        }},
    });
    UiTest ui(app);
    Check(ui.SendKey({.type = KeyEventType::Down, .key = Key::F8}), "Apply settings and quit before the next frame");
  }
  CheckLoaded(directory, SettingsBeforeQuit());
}
#endif

const ShortcutCommand& Shortcut(std::string_view id) {
  const auto commands = ShortcutCommands();
  const auto entry = std::ranges::find(commands, id, &ShortcutCommand::id);
  Check(entry != commands.end(), "Expected shortcut command " + std::string(id));
  return *entry;
}

void ShortcutChanges() {
  const auto& find = Shortcut("folio.find");
  const auto& redo = Shortcut("editor.redo");
  const auto& bold = Shortcut("editor.bold");
  const KeyCombination control_y{Key::Y, {.control = true}};
  const KeyCombination control_f{Key::F, {.control = true}};
  const KeyCombination meta_f{Key::F, {.meta = true}};
  const auto relocated = PlanShortcutChange({}, find, ShortcutKeys{control_y});
  Check(relocated.conflicts == std::vector<ShortcutConflict>{{&redo, control_y}}, "Only Ctrl+Y conflicts");
  auto expected_redo = ShortcutKeys(redo.defaults.begin(), redo.defaults.end());
  std::erase(expected_redo, control_y);
  Check(relocated.overrides.at("editor.redo") == expected_redo, "Redo retains every other default alias");
  const auto bindings = ResolveShortcuts(relocated.overrides);
  const auto new_find = std::ranges::find(bindings.window, control_y, &WindowKeyBinding::keys);
  Check(std::ranges::find(bindings.window, control_f, &WindowKeyBinding::keys) == bindings.window.end() &&
            std::ranges::find(bindings.window, meta_f, &WindowKeyBinding::keys) == bindings.window.end() &&
            new_find != bindings.window.end() && new_find->command == FolioCommand::Find,
        "Find releases its previous keys and resolves to the replacement");
  const auto restored = PlanShortcutChange(relocated.overrides, redo, std::nullopt);
  Check(restored.conflicts == std::vector<ShortcutConflict>{{&find, control_y}} &&
            restored.overrides.at("folio.find").empty() && !restored.overrides.contains("editor.redo"),
        "Single reset restores defaults and removes just the conflicting key from Find");
  const auto cleared = PlanShortcutChange(restored.overrides, bold, ShortcutKeys{});
  Check(cleared.conflicts.empty() && EffectiveShortcuts(cleared.overrides, bold).empty(), "Clear explicitly unbinds");

  const ShortcutKeys sample{
      {Key::Comma, {.shift = true, .control = true}},
      {Key::F4, {.meta = true}},
      {Key::Y, {.control = true, .meta = true}},
  };
  Check(ParseShortcutKeys(SerializeShortcutKeys(sample)) == sample && ShortcutLabel(sample[0]) == "Ctrl + Shift + ,",
        "Punctuation, function keys, and default aliases round-trip");
  Check(ParseShortcutKeys("none") == ShortcutKeys{} && !ParseShortcutKeys("Control+Control+Y") &&
            !ParseShortcutKeys("Control+Y,Control+Y") && !ParseShortcutKeys("Control+V") &&
            !ShortcutRecordingError({Key::Space, {.control = true}}).empty(),
        "Invalid keys are rejected and explicit unbindings survive parsing");
  const auto& row = Shortcut("editor.table.row-after");
  const KeyCombination control_shift_r{Key::R, {.shift = true, .control = true}};
  ShortcutOverrides existing{{"editor.bold", {control_shift_r}}};
  ReconcileShortcutOverrides(existing);
  auto remaining = ShortcutKeys(row.defaults.begin(), row.defaults.end());
  std::erase(remaining, control_shift_r);
  Check(existing.at("editor.bold") == ShortcutKeys{control_shift_r} &&
            existing.at("editor.table.row-after") == remaining,
        "A newly added default does not replace a user's existing custom shortcut");

  ShortcutOverrides ambiguous{{"folio.find", {control_y}}, {"editor.bold", {control_y}}, {"folio.settings", {}}};
  ReconcileShortcutOverrides(ambiguous);
  Check(ambiguous == ShortcutOverrides{{"folio.settings", {}}},
        "A corrupt file drops duplicate custom shortcuts and keeps unrelated overrides");
}

void ShortcutSettingsFlow(const File& directory, bool restore) {
  const Application app(SettingsFixture, {
      .show_debug_overlay = false,
      .application_hooks = {[directory](ApplicationContext& context) {
        context.Provide(std::make_shared<SettingsStore>(directory));
      }},
  });
  UiTest ui(app, {.viewport = {360.0F, 630.0F}});
  const auto by_id = [&](const char* id) { return ui.FindSemantics(UiSemanticSelector::Identifier(id)); };
  const auto tap = [&](const char* id) { ui.TapAt(Center(by_id(id).One().bounds)); };
  ui.Find(UiSelector::Text("Open settings")).Tap();
  ui.Pump(300ms);
  const auto tab = ui.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::Tab), UiSemanticSelector::Label("Keyboard"))).One();
  ui.TapAt(Center(tab.bounds));
  const auto list = ui.Find(UiSelector::Key("keyboard-shortcuts"));
  const auto edit = [&](const char* id) {
    list.ScrollUntil(UiSelector::Key(id), {.step = {0.0F, 60.0F}}).Tap();
    ui.Pump(300ms);
    Check(by_id("shortcut-dialog").Exists(), std::string("The shortcut editor opens for ") + id);
    Check(!ui.ActiveTextInput(), "The recorder does not open a text input session");
  };
  const auto wait_for_recorder_exit = [&] {
    ui.PumpUntil([&] { return !ui.Find(UiSelector::Key("shortcut-capture")).Exists(); });
  };
  const auto apply = [&] {
    tap("shortcut-apply");
    wait_for_recorder_exit();
    Check(!by_id("shortcut-dialog").Exists(), "Applying a shortcut dismisses the recorder");
  };
  if (!restore) {
    edit("shortcut-folio.find");
    ui.PressKey(Key::A, {.control = true});
    Check(ui.Find(UiSelector::Text("This key is reserved for clipboard or selection commands.")).Exists(),
          "The recorder starts focused and reports a reserved combination");
    ui.PressKey(Key::Y, {.control = true});
    Check(by_id("shortcut-conflict-editor.redo").Exists(), "Recording Ctrl+Y reports the specific conflicting alias");
    ui.PressKey(Key::Escape);
    wait_for_recorder_exit();
    Check(!by_id("shortcut-dialog").Exists() && by_id("shortcut-folio.find").One().label != "Find in document, Ctrl + Y",
          "Cancel leaves the shortcut unchanged");
    edit("shortcut-folio.find");
    ui.PressKey(Key::Y, {.control = true});
    apply();
    Check(by_id("shortcut-folio.find").One().label == "Find in document, Ctrl + Y",
          "The settings list shows the new shortcut immediately");
  } else {
    Check(by_id("shortcut-folio.find").One().label == "Find in document, Ctrl + Y",
          "Reopening the application restores the custom shortcut in the settings UI");
    edit("shortcut-editor.redo");
    tap("shortcut-restore");
    Check(by_id("shortcut-conflict-folio.find").Exists(), "Restoring one command asks before reassigning its alias");
    apply();
    Check(by_id("shortcut-folio.find").One().label == "Find in document, Unassigned",
          "The conflicting owner loses its last shortcut");
    edit("shortcut-folio.find");
    tap("shortcut-restore");
    wait_for_recorder_exit();
    Check(!by_id("shortcut-dialog").Exists() && by_id("shortcut-folio.find").One().label != "Find in document, Unassigned",
          "An uncontested reset restores the single command immediately");
    edit("shortcut-folio.find");
    tap("shortcut-clear");
    wait_for_recorder_exit();
    Check(by_id("shortcut-folio.find").One().label == "Find in document, Unassigned", "Clear removes every alias");
    list.ScrollUntil(UiSelector::Key("shortcuts-restore-all"), {.step = {0.0F, -60.0F}}).Tap();
    ui.Pump(300ms);
    tap("shortcuts-restore-confirm");
    ui.Pump(300ms);
    Check(by_id("shortcut-folio.find").One().label != "Find in document, Unassigned",
          "Restore all brings back the default shortcuts");
  }
  const std::string preferences = "theme=ember\ntext_size=19\nreading_width=920\n";
  ui.PumpUntil([&] {
    std::this_thread::sleep_for(2ms);
    const auto saved = directory.Child("settings.cfg").ReadString();
    return saved.Succeeded() && (restore ? saved.Value() == preferences
        : saved.Value().starts_with(preferences) && saved.Value().find("shortcut.folio.find=Control+Y\n") !=
              std::string::npos);
  });
}

constexpr auto long_filename = "很长的文档 👩‍💻 e\u0301 — A detailed research outline for November.md";

[[huxerui::composable]]
View TitleStatusFixture() {
  const auto showing = UseState(false);
  return Column {
    Column {
      Row {
        EllipsizedLabel(long_filename, UseTheme().colors.on_surface, {.identifier = "filename"})
            .With(Grow(), Tooltip(long_filename)).Key("filename"),
        Button("Toggle status").OnClick([showing] { showing = !showing.Get(); }),
      }.With(Frame{.width = 255.0F}, CrossAlign(CrossAxisAlignment::Center)),
    }.With(CrossAlign(CrossAxisAlignment::Start)),
    Row {Text("Document")}.With(Grow()).Key("document"),
    StatusBar(showing.Get() ? std::string("Saved ") + long_filename : ""),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

void TitleAndStatus(Size viewport) {
  const Application app(TitleStatusFixture, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = viewport});
  Check(ui.CaptureSnapshot().ToString().find("…") != std::string::npos, "A narrow file title draws an ellipsis");
  Check(ui.FindSemantics(UiSemanticSelector::Identifier("filename")).One().label == long_filename,
        "Accessibility retains the complete Unicode filename");
  if (viewport.width > 600.0F) {
    const auto name = ui.Find(UiSelector::Key("filename")).One().bounds;
    ui.SendPointer({.type = PointerEventType::Move, .position = Center(name)});
    ui.Pump();
    ui.Pump(750ms);
    ui.Pump();
    Check(ui.Find(UiSelector::Text(long_filename)).Exists(), "Desktop hover reveals the full name");
    ui.SendPointer({.type = PointerEventType::Move, .position = {viewport.width - 10.0F, viewport.height - 10.0F}});
    ui.Pump(250ms);
  }
  const auto document = ui.Find(UiSelector::Key("document"));
  const auto bounds = document.One().bounds;
  const auto status = ui.FindSemantics(UiSemanticSelector::Identifier("preview-status"));
  const auto toggle = ui.Find(UiSelector::Text("Toggle status"));
  toggle.Tap();
  Check(document.One().bounds == bounds && status.One().label == std::string("Saved ") + long_filename,
        "A status message leaves the document bounds fixed and announces the full message");
  toggle.Tap();
  Check(document.One().bounds == bounds && !status.Exists(), "Clearing the status leaves the document bounds fixed");
}

[[huxerui::composable]]
View DialogActionsFixture() {
  const auto& theme = UseTheme();
  const auto confirmation = ScrollableDialogPanel(
      Text("Save this document?", TextRole::Title),
      ScrollView(Text("The document has unsaved changes.")).With(Frame{.max_height = 168.0F}),
      DialogActions(theme, Button("Cancel").Key("cancel"), Button("Save as JSON").Key("primary"),
                    Button("Export with formatting loss").Key("secondary"), true),
      theme, 600.0F).Key("confirmation");
  return Column {confirmation}.With(CrossAlign(CrossAxisAlignment::Center));
}

void DialogActionLayout() {
  const Application app(DialogActionsFixture, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {620.0F, 300.0F}});
  const auto secondary = ui.Find(UiSelector::Key("secondary"));
  const auto cancel = ui.Find(UiSelector::Key("cancel"));
  const auto primary = ui.Find(UiSelector::Key("primary"));
  Check(ui.Find(UiSelector::Key("confirmation")).One().bounds.height < 220.0F,
        "A short confirmation wraps its message and actions instead of filling the window");
  Check(secondary.One().bounds.y == primary.One().bounds.y && secondary.One().bounds.x < cancel.One().bounds.x,
        "The secondary action stays to the left when the footer fits on one line");
  ui.SetWindowMetrics({.viewport = {260.0F, 320.0F}});
  ui.Pump();
  Check(secondary.One().bounds.y < cancel.One().bounds.y && cancel.One().bounds.y == primary.One().bounds.y,
        "A narrow footer wraps only between the secondary and the cancel/primary pair");
}

} // namespace

int main() {
  const auto directory = File(std::filesystem::temp_directory_path().u8string()).Child(
      "folio-appearance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    PresetContrast();
    TitleAndStatus({1050.0F, 640.0F});
    TitleAndStatus({360.0F, 620.0F});
    DialogActionLayout();
    ShortcutChanges();
    const auto legacy = directory.Child("legacy");
    Check(legacy.CreateDirectories() &&
              legacy.Child("appearance.cfg").WriteString("theme=lilac\ntext_size=14\nreading_width=560\n"),
          "Write legacy preferences");
    CheckLoaded(legacy, {.theme = "lilac", .editor = {.text_size = 14, .reading_width = 560}});
    SettingsFlow({1060.0F, 500.0F}, std::nullopt, false);
    SettingsFlow({360.0F, 600.0F}, directory, true);
    CheckLoaded(directory, {.theme = "ember", .editor = {.text_size = 19, .reading_width = 920}});
    ShortcutSettingsFlow(directory, false);
    const auto remapped = PlanShortcutChange({}, Shortcut("folio.find"), ShortcutKeys{{Key::Y, {.control = true}}});
    CheckLoaded(directory, {.theme = "ember", .editor = {.text_size = 19, .reading_width = 920},
                            .shortcuts = remapped.overrides});
    ShortcutSettingsFlow(directory, true);
    CheckLoaded(directory, {.theme = "ember", .editor = {.text_size = 19, .reading_width = 920}});
#if !defined(__EMSCRIPTEN__)
    SettingsSurviveQuit(directory);
#endif
    Check(directory.Child("settings.cfg").WriteString("theme=unknown\ntext_size=19\nreading_width=0\n"),
          "Write invalid preferences");
    CheckLoaded(directory, {.editor = {.text_size = 19}});
    Check(directory.DeleteRecursively(), "Remove the settings test directory");
    return 0;
  } catch (const std::exception& error) {
    (void)directory.DeleteRecursively();
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "settings.h"

#include <algorithm>
#include <charconv>

#include <huxerui/huxerui.h>

#include "shortcut_settings.h"

using namespace huxerui;

namespace folio {
namespace {

constexpr auto settings_file = "settings.cfg";
constexpr auto staged_settings_file = "settings.cfg.tmp";
constexpr std::string_view shortcut_prefix = "shortcut.";

void ReadChoice(std::string_view entry, std::span<const int> choices, int& value) {
  if (entry.empty()) {
    return;
  }
  int parsed = 0;
  const auto end = entry.data() + entry.size();
  const auto result = std::from_chars(entry.data(), end, parsed);
  if (result.ec == std::errc{} && result.ptr == end && std::ranges::find(choices, parsed) != choices.end()) {
    value = parsed;
  }
}

FolioSettings LoadSettings(const std::optional<File>& directory) {
  FolioSettings settings;
  if (!directory) {
    return settings;
  }
  auto content = directory->Child(settings_file).ReadString();
  if (!content.Succeeded() && content.Error().code == IoErrorCode::NotFound) {
    content = directory->Child("appearance.cfg").ReadString();
  }
  if (!content.Succeeded()) {
    return settings;
  }
  const auto themes = ThemePresets();
  auto lines = std::string_view(content.Value());
  while (!lines.empty()) {
    const auto newline = lines.find('\n');
    auto line = lines.substr(0, newline);
    lines = newline == lines.npos ? std::string_view{} : lines.substr(newline + 1);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    const auto separator = line.find('=');
    if (separator == line.npos) {
      continue;
    }
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "theme" && (value == system_theme || std::ranges::find(themes, value, &ThemePreset::id) != themes.end())) {
      settings.theme = value;
    } else if (key == "text_size") {
      ReadChoice(value, editor_text_sizes, settings.editor.text_size);
    } else if (key == "reading_width") {
      ReadChoice(value, editor_reading_widths, settings.editor.reading_width);
    } else if (key.starts_with(shortcut_prefix) && key.size() > shortcut_prefix.size()) {
      if (auto shortcuts = ParseShortcutKeys(value)) {
        settings.shortcuts[std::string(key.substr(shortcut_prefix.size()))] = std::move(*shortcuts);
      }
    }
  }
  ReconcileShortcutOverrides(settings.shortcuts);
  return settings;
}

std::string SerializeSettings(const FolioSettings& settings) {
  auto content = "theme=" + settings.theme + "\ntext_size=" + std::to_string(settings.editor.text_size) +
      "\nreading_width=" + std::to_string(settings.editor.reading_width) + "\n";
  for (const auto& [id, keys] : settings.shortcuts) {
    content += shortcut_prefix;
    content += id + "=" + SerializeShortcutKeys(keys) + "\n";
  }
  return content;
}

#if defined(__EMSCRIPTEN__)
Task<void> SaveSettings(File directory, State<FolioSettings> value, State<bool> writing, State<bool> failed) {
  const auto file = directory.Child(settings_file);
  const auto staged = directory.Child(staged_settings_file);
  // Web filesystem persistence requires asynchronous operations. Coalesce edits behind the current write.
  while (true) {
    const auto snapshot = value.Get();
    const bool saved = (co_await directory.CreateDirectoriesAsync()) &&
        (co_await staged.WriteStringAsync(SerializeSettings(snapshot))) && (co_await staged.MoveToAsync(file, true));
    if (!saved) {
      (void)co_await staged.DeleteAsync();
    }
    failed = !saved;
    if (!saved || snapshot == value.Get()) {
      writing = false;
      co_return;
    }
  }
}
#else
bool SaveSettings(const File& directory, const FolioSettings& settings) {
  const auto staged = directory.Child(staged_settings_file);
  // Commit this small application-owned file before the event returns so an immediate quit can reload it.
  if (directory.CreateDirectories() && staged.WriteString(SerializeSettings(settings)) &&
      staged.MoveTo(directory.Child(settings_file), true)) {
    return true;
  }
  (void)staged.Delete();
  return false;
}
#endif

View Choice(View content, const ThemeSpec& theme, std::string label, std::string id,
            bool selected, std::function<void()> select) {
  const auto& colors = theme.colors;
  return std::move(content).OnClick(std::move(select)).Key(id).With(
      Background(selected ? colors.primary_container : colors.surface_container),
      Border{selected ? colors.primary : colors.outline, 1.0F}, CornerRadius(theme.shapes.medium),
      Focusable{}, PointerCursor(PointerCursorKind::Hand), theme.interactions.indication,
      Semantics{.role = SemanticRole::RadioButton, .label = std::move(label), .identifier = std::move(id),
                .checked = selected ? SemanticCheckedState::Checked : SemanticCheckedState::Unchecked,
                .descendants = SemanticDescendantPolicy::Exclude});
}

View PreviewMark(Color color, float width, float height) {
  return Row{}.With(Frame{.width = width, .height = height}, Background(color), CornerRadius(height / 2.0F));
}

View ChoiceMark(const ThemeSpec& theme, bool selected) {
  const auto color = selected ? theme.colors.primary : theme.colors.on_surface_variant;
  return Row {selected ? PreviewMark(color, 6.0F, 6.0F) : View{}}.With(
      Frame{.width = 14.0F, .height = 14.0F}, MainAlign(MainAxisAlignment::Center),
      CrossAlign(CrossAxisAlignment::Center), Border{color, 1.0F}, CornerRadius(7.0F));
}

View ThemePreview(const ThemePreset& preset) {
  const auto& colors = preset.colors;
  auto toolbar = Row {
    PreviewMark(colors.primary, 4.0F, 4.0F),
    PreviewMark(colors.outline, 12.0F, 3.0F),
    Spacer().With(Grow()),
    PreviewMark(colors.primary, 8.0F, 3.0F),
  }.With(Frame{.height = 13.0F}, Padding(EdgeInsets::Symmetric(5.0F, 0.0F)), Spacing(3.0F),
         CrossAlign(CrossAxisAlignment::Center), Background(colors.surface_container));
  auto sidebar = Column {
    PreviewMark(colors.primary, 10.0F, 2.0F),
    PreviewMark(colors.outline, 17.0F, 3.0F),
    PreviewMark(colors.outline, 13.0F, 3.0F),
  }.With(Frame{.width = 27.0F}, Padding(EdgeInsets{.top = 7.0F, .left = 5.0F}), Spacing(4.0F),
         Background(colors.surface_container_low));
  auto document = Column {
    Text("Aa").Style(TextStyle{Font::System(16.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
    PreviewMark(colors.on_surface_variant, 35.0F, 2.0F),
    Row {PreviewMark(colors.primary, 9.0F, 2.0F)}.With(
        Frame{.height = 8.0F}, Padding(EdgeInsets{.left = 3.0F}), CrossAlign(CrossAxisAlignment::Center),
        Background(colors.surface_container_highest), CornerRadius(2.0F)),
  }.With(Padding(EdgeInsets::Symmetric(6.0F, 4.0F)), Spacing(3.0F),
         Background(colors.surface), CornerRadius(3.0F));
  auto canvas = Column {document}.With(
      Grow(), Padding(EdgeInsets{.top = 5.0F, .right = 6.0F, .left = 6.0F}),
      Background(colors.background), CrossAlign(CrossAxisAlignment::Stretch));
  return Column {
    toolbar,
    Row {sidebar, canvas}.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Frame{.height = 67.0F}, CrossAlign(CrossAxisAlignment::Stretch), ClipChildren(),
         Border{colors.outline, 1.0F}, CornerRadius(5.0F));
}

Text SettingsLabel(std::string label, const ThemeSpec& theme, FontWeight weight = FontWeight::Medium) {
  return Text(std::move(label), TextRole::Label).Style(TextStyle{
      Font::FromFamily(theme.typography.family, theme.typography.label_large).WithWeight(weight),
      theme.colors.on_surface});
}

View SectionLabel(const char* title, const ThemeSpec& theme) {
  return SettingsLabel(title, theme, FontWeight::SemiBold).With(
      Semantics{.role = SemanticRole::Heading, .heading_level = 2});
}

View ThemeGroup(ThemeBrightness brightness, bool compact, const ThemeSpec& theme,
                std::string_view active, const std::function<void(std::string_view)>& select) {
  Views rows;
  Views cards;
  const std::size_t columns = compact ? 2 : 4;
  std::size_t count = 0;
  for (const auto& preset : ThemePresets()) {
    if (preset.brightness != brightness) {
      continue;
    }
    const bool selected = active == preset.id;
    auto content = Column {
      ThemePreview(preset),
      Row {
        SettingsLabel(std::string(preset.name), theme).With(Grow()),
        ChoiceMark(theme, selected),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(Padding(7.0F), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
    cards.Add(Choice(std::move(content), theme, std::string(preset.name), "theme-" + std::string(preset.id),
                     selected, [select, id = preset.id] { select(id); }).With(Grow()));
    if (++count % columns == 0) {
      rows.Add(Row {std::exchange(cards, Views{})}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));
    }
  }
  if (count % columns != 0) {
    rows.Add(Row {std::move(cards)}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  }
  return Column {
    Text(brightness == ThemeBrightness::Dark ? "Dark" : "Light", TextRole::Label)
        .With(Foreground(theme.colors.on_surface_variant)),
    Column {std::move(rows)}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View OptionRow(const ThemeSpec& theme, const std::array<int, 3>& values, const std::array<std::string, 3>& labels,
               const char* id, const char* group, int active, const std::function<void(int)>& select) {
  Views options;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const int value = values[i];
    const bool selected = value == active;
    auto content = Row {
      SettingsLabel(labels[i], theme),
    }.With(Frame{.min_height = 44.0F}, MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center));
    auto label = std::string(group) + ", " + labels[i];
    options.Add(Choice(std::move(content), theme, std::move(label), id + std::to_string(i),
                       selected, [select, value] { select(value); }).With(Grow()));
  }
  return Row {std::move(options)}.With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View AppearancePage(const ThemeSpec& theme, bool compact, const std::string& active, const ThemePreset& system_preset,
                    const std::shared_ptr<SettingsStore>& store) {
  const bool follows_system = active == system_theme;
  const auto select_theme = [store](std::string_view id) {
    auto next = store->Get();
    next.theme = id;
    store->Set(std::move(next));
  };
  return Column {
    Choice(Row {
      SettingsLabel("Follow system", theme).With(Grow()),
      Text(std::string(system_preset.name), TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
      ChoiceMark(theme, follows_system),
    }.With(Frame{.min_height = 46.0F}, Padding(EdgeInsets::Symmetric(11.0F, 5.0F)), Spacing(8.0F),
           CrossAlign(CrossAxisAlignment::Center)),
        theme, "Follow system", "theme-system", follows_system, [select_theme] { select_theme(system_theme); }),
    ThemeGroup(ThemeBrightness::Light, compact, theme, active, select_theme),
    ThemeGroup(ThemeBrightness::Dark, compact, theme, active, select_theme),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View EditorPage(const ThemeSpec& theme, bool compact, const EditorSettings& editor,
                const std::shared_ptr<SettingsStore>& store) {
  std::array<std::string, editor_text_sizes.size()> text_sizes;
  for (std::size_t i = 0; i < text_sizes.size(); ++i) {
    text_sizes[i] = std::to_string(editor_text_sizes[i]) + " px";
  }
  const auto options = Column {
    SectionLabel("Text size", theme),
    OptionRow(theme, editor_text_sizes, text_sizes, "text-size-", "Text size", editor.text_size,
              [store](int size) {
                auto next = store->Get();
                next.editor.text_size = size;
                store->Set(std::move(next));
              }),
    SectionLabel("Reading width", theme),
    OptionRow(theme, editor_reading_widths, {"Narrow", "Standard", "Wide"}, "reading-width-", "Reading width",
              editor.reading_width, [store](int width) {
                auto next = store->Get();
                next.editor.reading_width = width;
                store->Set(std::move(next));
              }),
    compact ? Text("Used in wider windows", TextRole::Label)
                  .With(FontSize(12.0F), Foreground(theme.colors.on_surface_variant)) : View{},
  }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Stretch));
  return Column {
    options,
    Column {
      SectionLabel("Text preview", theme),
      Text("Collect ideas worth keeping. 中文输入也可以。").Style(TextStyle{
          Font::FromFamily(theme.typography.family, static_cast<float>(editor.text_size)), theme.colors.on_surface}),
    }.With(Padding(14.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch),
           Background(theme.colors.surface_container), Border{theme.colors.outline, 1.0F},
           CornerRadius(theme.shapes.medium)),
  }.With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

enum class SettingsPage : std::size_t { Appearance, Editor, Keyboard };
constexpr std::array page_ids{"appearance-options", "editor-options", "keyboard-shortcuts"};

} // namespace

SettingsStore::SettingsStore(std::optional<File> directory)
    : directory_(std::move(directory)), value_(LoadSettings(directory_)),
      bindings_(std::make_shared<const ResolvedShortcuts>(ResolveShortcuts(value_->shortcuts))) {}

void SettingsStore::Set(FolioSettings value) {
  if (value == value_.Get() && !save_failed_.Get()) {
    return;
  }
  if (value.shortcuts != value_->shortcuts) {
    bindings_ = std::make_shared<const ResolvedShortcuts>(ResolveShortcuts(value.shortcuts));
  }
  value_ = std::move(value);
#if defined(__EMSCRIPTEN__)
  if (directory_ && !writing_.Get()) {
    UseApplicationTaskScope().Launch(SaveSettings(*directory_, value_, writing_, save_failed_));
    writing_ = true;
  }
#else
  if (directory_) {
    save_failed_ = !SaveSettings(*directory_, value_.Get());
  }
#endif
}

[[huxerui::composable]]
View SettingsDialog(DialogContext context) {
  const auto& theme = UseTheme();
  const auto store = UseService<SettingsStore>();
  const auto page = UseState(std::size_t{0});
  const auto& settings = store->Get();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  auto tabs_style = UseEnvironment<TabsStyle>();
  tabs_style.label_style = TextStyle{
      Font::FromFamily(theme.typography.family, theme.typography.label_large).WithWeight(FontWeight::Medium),
      theme.colors.on_surface_variant};
  tabs_style.selected_label = theme.colors.primary;
  tabs_style.indicator = theme.colors.primary;
  tabs_style.indicator_sizing = TabIndicatorSizing::Content;
  tabs_style.divider_color = theme.colors.outline;
  tabs_style.divider_height = 1.0F;
  tabs_style.minimum_height = 44.0F;
  tabs_style.expand_items = compact;
  std::vector<StringVariant> pages{"Appearance", "Editor"};
  if (!IsMobilePlatform()) {
    pages.emplace_back("Keyboard");
  }
  const auto navigation = ProvideEnvironment(std::move(tabs_style), Tabs(std::move(pages), page)
      .OnChanged([page](std::size_t next) { page = next; }));
  View content;
  switch (static_cast<SettingsPage>(page.Get())) {
  case SettingsPage::Appearance:
    content = AppearancePage(theme, compact, settings.theme, ResolveTheme(system_theme, UseSystemThemeBrightness()),
                             store);
    break;
  case SettingsPage::Editor:
    content = EditorPage(theme, compact, settings.editor, store);
    break;
  case SettingsPage::Keyboard:
    content = KeyboardSettings();
    break;
  }
  View error;
  if (store->SaveFailed()) {
    error = Row {
      Text("Couldn’t save settings", TextRole::Label).With(Grow(), Foreground(theme.colors.error),
          Semantics{.live_region = SemanticLiveRegion::Polite}),
      Button("Retry").OnClick([store] { store->Set(store->Get()); }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
  }
  const auto header = Column {
    Row {
      Text("Settings", TextRole::Title).Style(TextStyle{
          Font::FromFamily(theme.typography.family, 22.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
          .With(Grow(), Semantics{.role = SemanticRole::Heading, .heading_level = 1}),
      Button("Done").OnClick([context] { context.Dismiss(); })
          .With(Frame{.min_height = 44.0F}, Semantics{.identifier = "settings-done"}),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    navigation,
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
  const auto body = ScrollView(Column {
    error,
    content,
  }.With(Padding(EdgeInsets{.right = 5.0F, .bottom = 4.0F}), Spacing(10.0F),
         CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Frame{.max_height = compact ? 550.0F : 430.0F}, ScrollBar(),
            Semantics{.identifier = page_ids[page.Get()]}).Key(page_ids[page.Get()]);
  return ScrollableDialogPanel(header, body, {}, theme, compact ? 366.0F : 662.0F)
      .With(Semantics{.label = "Settings", .identifier = "settings-dialog"})
      .Key("settings-dialog");
}

} // namespace folio

#include "command_palette.h"

#include <algorithm>
#include <utility>

#include <huxerui/huxerui.h>

#include "appearance.h"
#include "settings.h"

using namespace huxerui;
using namespace huxerui::richeditor;

namespace folio {
namespace {

using MatchRank = std::pair<int, std::size_t>;

std::string NormalizeSearch(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool space = false;
  for (const auto character : text) {
    if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
      space = !normalized.empty();
      continue;
    }
    if (space) {
      normalized += ' ';
      space = false;
    }
    normalized += character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
  }
  return normalized;
}

std::size_t Utf8CharacterLength(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if (lead < 0xe0) {
    return 2;
  }
  return lead < 0xf0 ? 3 : 4;
}

std::optional<MatchRank> RankMatch(std::string_view query, std::string_view text) {
  const auto candidate = NormalizeSearch(text);
  if (candidate == query) {
    return MatchRank{0, 0};
  }
  if (candidate.starts_with(query)) {
    return MatchRank{1, candidate.size() - query.size()};
  }
  if (const auto match = candidate.find(query); match != candidate.npos) {
    return MatchRank{2, match};
  }
  std::size_t start = candidate.npos;
  std::size_t end = 0;
  std::size_t matched = 0;
  while (!query.empty()) {
    const auto character = query.substr(0, Utf8CharacterLength(static_cast<unsigned char>(query.front())));
    query.remove_prefix(character.size());
    if (character == " ") {
      continue;
    }
    const auto offset = candidate.find(character, end);
    if (offset == candidate.npos) {
      return std::nullopt;
    }
    start = std::min(start, offset);
    end = offset + character.size();
    matched += character.size();
  }
  return MatchRank{3, end - start - matched};
}

std::optional<MatchRank> RankCommand(std::string_view query, const ShortcutCommand& command) {
  auto rank = RankMatch(query, command.label);
  auto aliases = command.search_aliases;
  while (!aliases.empty()) {
    const auto separator = aliases.find('|');
    if (const auto alias = RankMatch(query, aliases.substr(0, separator)); alias && (!rank || *alias < *rank)) {
      rank = alias;
    }
    if (separator == aliases.npos) {
      break;
    }
    aliases.remove_prefix(separator + 1);
  }
  const auto group = RankMatch(query, CommandGroup(command));
  if (group && !rank) {
    return MatchRank{4, group->second};
  }
  return rank;
}

struct CommandResult {
  const ShortcutCommand* command;
  CommandState state;
  MatchRank rank;
};

std::vector<CommandResult> FindCommands(std::string_view query, const CommandPaletteOptions& options) {
  const auto normalized = NormalizeSearch(query);
  std::vector<CommandResult> results;
  for (const auto& command : ShortcutCommands()) {
    if (const auto* action = std::get_if<FolioCommand>(&command.command);
        action && *action == FolioCommand::OpenCommandPalette) {
      continue;
    }
    const auto rank = normalized.empty() ? std::optional{MatchRank{}} : RankCommand(normalized, command);
    if (rank) {
      const auto state = options.state(command);
      if (state.enabled || !normalized.empty()) {
        results.push_back({&command, state, *rank});
      }
    }
  }
  std::stable_sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
    return left.rank < right.rank;
  });
  return results;
}

void RevealResult(const ScrollController& scroll, std::size_t index, float row_height) {
  const auto metrics = scroll.Metrics();
  const float top = static_cast<float>(index) * row_height;
  const float bottom = top + row_height;
  if (top < metrics.offset) {
    scroll.ScrollTo(top);
  } else if (bottom > metrics.offset + metrics.viewport_extent) {
    scroll.ScrollTo(bottom - metrics.viewport_extent);
  }
}

View CommandRow(const CommandResult& result, std::span<const KeyCombination> keys, bool selected,
                float height, const ThemeSpec& theme, std::function<void()> run) {
  const auto& colors = theme.colors;
  const auto name = std::string(result.command->label);
  const auto shortcut = keys.empty() ? std::string{} : PreferredShortcutLabel(keys);
  const auto id = "palette-" + std::string(result.command->id);
  std::optional<SemanticCheckedState> checked;
  if (result.state.checked) {
    switch (*result.state.checked) {
    case CheckState::On: checked = SemanticCheckedState::Checked; break;
    case CheckState::Mixed: checked = SemanticCheckedState::Mixed; break;
    case CheckState::Off: checked = SemanticCheckedState::Unchecked; break;
    }
  }
  View status;
  if (!result.state.enabled) {
    status = Text("Unavailable here", TextRole::Label)
        .With(FontSize(11.0F), Foreground(colors.on_surface_variant));
  } else if (result.state.checked && *result.state.checked != CheckState::Off) {
    status = Text(*result.state.checked == CheckState::On ? "✓" : "–", TextRole::Label)
        .With(Foreground(colors.primary));
  }
  const auto shortcut_view = shortcut.empty() ? View{} : View(
      Text(shortcut, TextRole::Label)
          .Style(TextStyle{Font::Monospace(11.0F), colors.on_surface_variant})
          .With(Padding(EdgeInsets::Symmetric(6.0F, 4.0F)),
                Background(colors.surface_container_high), CornerRadius(theme.shapes.extra_small)));
  return Row {
    Column {
      Text(name, TextRole::Label).With(Foreground(result.state.enabled ? colors.on_surface : colors.on_surface_variant)),
      status,
    }.With(Grow(), Spacing(2.0F), CrossAlign(CrossAxisAlignment::Start)),
    shortcut_view,
  }.OnClick(std::move(run)).With(
      Frame{.height = height}, Padding(EdgeInsets::Symmetric(9.0F, 2.0F)), Spacing(7.0F),
      CrossAlign(CrossAxisAlignment::Center), CornerRadius(theme.shapes.small), Focusable{},
      Background(selected && result.state.enabled ? colors.primary_container : Color::Transparent()),
      Enabled(result.state.enabled), PointerCursor(PointerCursorKind::Hand), theme.interactions.indication,
      Semantics{.role = SemanticRole::Button, .label = name,
                .value = shortcut.empty() ? std::nullopt : std::optional<StringVariant>{shortcut},
                .state_description = result.state.enabled ? std::nullopt
                    : std::optional<StringVariant>{"Unavailable in the current context"},
                .identifier = id, .checked = checked, .selected = selected,
                .descendants = SemanticDescendantPolicy::Exclude}
  ).Key(id);
}

} // namespace

[[huxerui::composable]]
View CommandPalette(DialogContext context, CommandPaletteOptions options) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto search = UseState(TextEditingValue{});
  const auto selected = UseState(std::string_view{});
  const auto scroll = UseScrollController();
  const auto completion = UseState(std::make_shared<std::function<void()>>()).Get();
  // Let the dialog finish restoring focus before requesting a new target or opening another layer.
  Lifecycle([completion, tasks = options.window_tasks, closed = options.on_closed] {
    return [completion, tasks, closed] {
      tasks.Post([completion, closed] {
        closed(static_cast<bool>(*completion));
        if (*completion) {
          std::exchange(*completion, {})();
        }
      });
    };
  }, completion);
  const auto query = search->text;
  const auto results = FindCommands(query, options);
  const auto current = std::ranges::find_if(results, [id = selected.Get()](const auto& result) {
    return result.command->id == id;
  });
  const std::size_t index = current == results.end() ? 0 : static_cast<std::size_t>(current - results.begin());
  const auto invoke = [search, completion, context, run = options.invoke](const ShortcutCommand& command) {
    if (search->composition || *completion) {
      return;
    }
    auto after_dismiss = run(command);
    if (after_dismiss && context.Dismiss()) {
      *completion = std::move(after_dismiss);
    }
  };
  const float row_height = compact ? 51.0F : 46.0F;
  const auto on_key = [search, query, selected, scroll, row_height, results, index, invoke](const KeyEvent& event) {
    if (event.type != KeyEventType::Down || event.modifiers.shift || event.modifiers.control || event.modifiers.alt ||
        event.modifiers.meta || search->composition ||
        (event.key != Key::ArrowUp && event.key != Key::ArrowDown && event.key != Key::Enter)) {
      return false;
    }
    if (results.empty() || query != search->text) {
      return true;
    }
    if (event.key == Key::Enter) {
      if (!event.repeat && results[index].state.enabled) {
        invoke(*results[index].command);
      }
      return true;
    }
    const int step = event.key == Key::ArrowDown ? 1 : -1;
    for (auto next = static_cast<std::ptrdiff_t>(index) + step;
         next >= 0 && next < static_cast<std::ptrdiff_t>(results.size()); next += step) {
      const auto& result = results[static_cast<std::size_t>(next)];
      if (result.state.enabled) {
        selected = result.command->id;
        RevealResult(scroll, static_cast<std::size_t>(next), row_height);
        break;
      }
    }
    return true;
  };
  auto field_style = UseEnvironment<TextFieldStyle>();
  field_style.show_label = false;
  field_style.outlined = {
      .background = colors.surface_container_low, .border = colors.outline, .hovered_border = colors.outline,
      .focused_border = colors.primary, .corner_radii = CornerRadii{theme.shapes.medium}, .minimum_height = 44.0F,
  };
  View field = TextField(search).Label("Search commands").Placeholder("Search commands")
      .Variant(TextFieldVariant::Outlined)
      .InputConfiguration({.action = TextInputAction::Search, .autocorrect = false})
      .OnChanged([search, selected, scroll](const TextEditingValue& value) {
        if (search->text != value.text) {
          selected = std::string_view{};
          scroll.ScrollTo(0.0F);
        }
        search = value;
      })
      .On<ViewEvents::KeyIntercept>(on_key)
      .With(options.initial_focus, Grow(), Semantics{.role = SemanticRole::SearchField, .identifier = "palette-query"})
      .Key("palette-query");
  View close = Button("Close").OnClick([context] { context.Dismiss(); })
      .With(Frame{.min_height = 44.0F}, Background(Color::Transparent()),
            Semantics{.label = "Close command palette", .identifier = "palette-close"});
  const auto header = Row {
    ProvideEnvironment(field_style, field),
    close,
  }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center));
  const auto& overrides = UseService<SettingsStore>()->Get().shortcuts;
  Views rows;
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    rows.Add(CommandRow(result, EffectiveShortcuts(overrides, *result.command), i == index, row_height, theme,
                        [invoke, command = result.command] { invoke(*command); }));
  }
  if (results.empty()) {
    rows.Add(Text("No matching commands", TextRole::Label)
        .With(Padding(EdgeInsets::Symmetric(9.0F, 14.0F)), Foreground(colors.on_surface_variant),
              Semantics{.live_region = SemanticLiveRegion::Polite}));
  }
  const auto list = ScrollView(Column {rows}.With(CrossAlign(CrossAxisAlignment::Stretch)))
      .Controller(scroll).With(Frame{.max_height = compact ? 460.0F : 380.0F}, ScrollBar());
  return ScrollableDialogPanel(header, list, {}, theme, compact ? 420.0F : 570.0F)
      .With(Semantics{.label = "Command palette", .identifier = "command-palette"}).Key("command-palette");
}

} // namespace folio

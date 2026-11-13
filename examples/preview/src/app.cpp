#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include "app_resources.h"
#include "appearance.h"
#include "browser.h"
#include "formula.h"
#include "settings.h"
#include "web_url.h"
#include "workbench.h"

using namespace huxerui;
using namespace huxerui::richeditor;
using folio::DialogActions;
using folio::DialogPanel;
using folio::FindField;
using folio::FindPanelState;
using folio::LatexBlock;
using folio::LatexInline;
using folio::latex_type;

namespace l10n = app::strings;

namespace {

constexpr std::string_view sample_markdown = R"markdown(# Field notes

Collect ideas worth keeping. Try **bold**, *italics*, and `inline code`. 中文输入 and emoji 🙂 work here too. Try inline math $x^2 + y^2 = r^2$ in a sentence.

> Keep the detail that makes the story yours.

$$
\sigma = \sqrt{\frac{1}{n}\sum_{i=1}^{n}(x_i-\mu)^2}
$$

| Topic | Note | Status | Next |
| --- | :--- | :---: | ---: |
| **Talk** | 中文输入 | Draft | Add a quote |
| Source | [Reading list](https://github.com/HuxerUI) | Ready | |

## Outline the draft

3. Capture an idea
   - [x] Save a useful detail
   - [ ] Add a note to revisit
4. [ ] Refine the draft

~~~cpp
struct Note {
  const char* title;
  int rank;
};

const Note note{"Trail", 3};
auto pinned = note.rank > 2;
~~~

~~~python
def ready(notes):
    for title, rank in notes:
        if rank > 2:
            yield title

notes = [("Trail", 3)]
for title in ready(notes):
    print(title)
~~~

***

Built with [HuxerUI](https://github.com/HuxerUI).
)markdown";

RichEditorStyle FolioEditorStyle(const ThemeSpec& theme, ViewportClass viewport, float text_size) {
  RichEditorStyle style;
  const float scale = text_size / theme.typography.body_large;
  style.background = theme.colors.surface;
  style.body_font = Font::FromFamily(theme.typography.family, text_size);
  style.code_font = Font::Monospace(14.0F * scale);
  style.heading_font_sizes = std::array{
      viewport == ViewportClass::Compact ? 27.0F : 33.0F, 22.0F, 20.0F, 18.0F, 17.0F, 16.0F};
  for (auto& heading : *style.heading_font_sizes) {
    heading *= scale;
  }
  style.quote_rule_color = theme.colors.primary;
  style.code_block_background = theme.colors.surface_container_highest;
  style.content_padding = viewport == ViewportClass::Expanded ? EdgeInsets::Symmetric(44.0F, 32.0F)
                                                             : EdgeInsets::Symmetric(22.0F, 20.0F);
  style.code_block_padding = EdgeInsets::Symmetric(14.0F, 11.0F);
  style.block_spacing = 12.0F;
  style.quote_rule_width = 2.0F;
  style.inline_code_corner_radius = 5.0F;
  style.inline_code_horizontal_padding = 3.0F;
  style.highlight_corner_radius = 4.0F;
  style.code_block_corner_radius = theme.shapes.medium;
  return style;
}

TextEditingValue DimensionText(float value) {
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << std::setprecision(7) << value;
  return TextEditingValue::FromText(text.str());
}

std::optional<float> ReadDimension(const TextEditingValue& value) {
  if (value.composition) {
    return std::nullopt;
  }
  auto text = value.text;
  std::ranges::replace(text, ',', '.');
  std::istringstream input(std::move(text));
  input.imbue(std::locale::classic());
  float dimension = 0.0F;
  if (input >> dimension && input.eof() && std::isfinite(dimension) && dimension > 0.0F) {
    return dimension;
  }
  return std::nullopt;
}

struct ImageSizeDraft {
  TextEditingValue width;
  TextEditingValue height;
  std::optional<float> preferred_width;
  bool operator==(const ImageSizeDraft&) const = default;
};

[[huxerui::composable]]
View ImageSizeDialog(DialogContext context, RichEditorController editor, Size initial) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const auto draft = UseState(ImageSizeDraft{DimensionText(initial.width), DimensionText(initial.height), {}});
  const float ratio = initial.height / initial.width;
  const auto changed = [draft, ratio](const TextEditingValue& value, bool width_changed) {
    auto next = draft.Get();
    auto& edited = width_changed ? next.width : next.height;
    auto& other = width_changed ? next.height : next.width;
    if (edited.text != value.text || edited.composition != value.composition) {
      next.preferred_width.reset();
      if (const auto dimension = ReadDimension(value)) {
        const float paired = width_changed ? *dimension * ratio : *dimension / ratio;
        if (std::isfinite(paired) && paired > 0.0F) {
          next.preferred_width = width_changed ? *dimension : paired;
          other = DimensionText(paired);
        }
      }
    }
    edited = value;
    draft = std::move(next);
  };
  const auto apply = [context, editor, draft] {
    if (const auto width = draft->preferred_width) {
      editor.Execute(ResizeImage{width});
      context.Dismiss();
    }
  };
  const auto field = [&](bool width, StringVariant label) {
    return TextField(width ? draft->width : draft->height)
        .Label(label)
        .Variant(TextFieldVariant::Outlined)
        .MaxLength(24)
        .InputConfiguration({.type = TextInputType::Decimal, .autocorrect = false})
        .OnChanged([changed, width](const TextEditingValue& value) { changed(value, width); })
        .OnSubmitted(apply)
        .With(Grow());
  };
  return DialogPanel(Column {
    Column {
      Text(l10n::image_size, TextRole::Title),
      Text(l10n::image_proportions, TextRole::Label).With(Foreground(colors.on_surface_variant)),
    }.With(Spacing(4.0F)),
    Row {
      field(true, l10n::width_pixels),
      field(false, l10n::height_pixels),
    }.With(Spacing(10.0F)),
    Text(l10n::images_share_line, TextRole::Label)
        .With(Foreground(colors.on_surface_variant)),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(l10n::apply).OnClick(apply).With(Enabled(draft->preferred_width.has_value())),
      Button(l10n::auto_size).OnClick([context, editor] {
        editor.Execute(ResizeImage{});
        context.Dismiss();
      })),
  }, theme, 380.0F);
}

std::optional<unsigned> ReadListStart(const TextEditingValue& value) {
  if (value.composition) {
    return std::nullopt;
  }
  unsigned start = 0;
  const auto end = value.text.data() + value.text.size();
  const auto result = std::from_chars(value.text.data(), end, start);
  return result.ec == std::errc{} && result.ptr == end ? std::optional(start) : std::nullopt;
}

[[huxerui::composable]]
View ListStartDialog(DialogContext context, RichEditorController editor, ViewHandle focus, unsigned initial) {
  const auto& theme = UseTheme();
  const auto number = std::to_string(initial);
  const auto value = UseState(TextEditingValue{number, {0, static_cast<TextOffset>(number.size())}, {}});
  const auto current = value.Get();
  const auto proposed = ReadListStart(current);
  const auto apply = [context, editor, focus, value] {
    if (const auto start = ReadListStart(value.Get()); start && editor.Execute(SetListStart{*start})) {
      context.Dismiss();
      focus.RequestFocus();
    }
  };
  return DialogPanel(Column {
    Column {
      Text(l10n::list_numbering, TextRole::Title),
      Text(l10n::numbering_hint, TextRole::Label)
          .With(Foreground(theme.colors.on_surface_variant)),
    }.With(Spacing(4.0F)),
    TextField(value)
        .Label(l10n::start_at)
        .Variant(TextFieldVariant::Outlined)
        .MaxLength(std::numeric_limits<unsigned>::digits10 + 1)
        .InputConfiguration({.type = TextInputType::Number, .autocorrect = false})
        .Validation(!proposed && !current.text.empty() && !current.composition
            ? ValidationResult::Invalid(StringVariant::Format(l10n::numbering_range, std::numeric_limits<unsigned>::max()))
            : ValidationResult::None())
        .OnChanged([value](const TextEditingValue& next) { value = next; })
        .OnSubmitted(apply),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(l10n::apply).OnClick(apply).With(Enabled(proposed && *proposed != initial))),
  }, theme, 340.0F);
}

[[huxerui::composable]]
View TextStylePicker(RichEditorController editor, ViewHandle focus, bool available, bool compact) {
  struct Choice {
    StringVariant label;
    StringVariant short_label;
    SetBlockKind command;
  };
  static const std::array choices{
      Choice{l10n::paragraph, l10n::body, {BlockKind::Paragraph}},
      Choice{l10n::heading_1, "H1", {BlockKind::Heading, 1}},
      Choice{l10n::heading_2, "H2", {BlockKind::Heading, 2}},
      Choice{l10n::heading_3, "H3", {BlockKind::Heading, 3}},
      Choice{l10n::heading_4, "H4", {BlockKind::Heading, 4}},
      Choice{l10n::heading_5, "H5", {BlockKind::Heading, 5}},
      Choice{l10n::heading_6, "H6", {BlockKind::Heading, 6}},
      Choice{l10n::code_block, l10n::code, {BlockKind::CodeBlock}},
  };
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  StringVariant label = l10n::style;
  StringVariant description = l10n::text_style;
  for (const auto& choice : choices) {
    const auto state = editor.GetCommandState(choice.command);
    if (state.checked == CheckState::On) {
      label = compact ? choice.short_label : choice.label;
      description = choice.label;
      break;
    }
    if (state.checked == CheckState::Mixed) {
      label = l10n::mixed;
      description = l10n::mixed_text_styles;
    }
  }

  return Button(UseString(label) + " ▾")
      .OnClick([editor, focus, menu, anchor] {
        std::vector<MenuEntry> items;
        items.reserve(choices.size());
        for (const auto& choice : choices) {
          const auto state = editor.GetCommandState(choice.command);
          const bool selected = state.checked == CheckState::On;
          items.emplace_back(MenuItem(choice.label, [editor, focus, command = choice.command, selected] {
            if (selected || editor.Execute(command)) {
              focus.RequestFocus();
            }
          }).Enabled(state.enabled).Checked(selected));
        }
        menu.Show(anchor, std::move(items));
      })
      .With(
          anchor,
          Frame{.min_width = compact ? 76.0F : 98.0F, .min_height = compact ? 44.0F : 30.0F},
          Padding(EdgeInsets::Symmetric(6.0F, 4.0F)),
          Tooltip(l10n::text_style),
          Enabled(available && editor.GetCommandState(choices.front().command).enabled),
          Background(Color::Transparent()),
          Foreground(theme.colors.on_surface),
          Semantics{.label = StringVariant::Format(l10n::text_style_description, UseString(description))}
      );
}

std::string_view LinkAddress(const TextEditingValue& value) {
  constexpr std::string_view spaces = " \t\r\n";
  const auto first = value.text.find_first_not_of(spaces);
  if (value.composition || first == std::string::npos) {
    return {};
  }
  return std::string_view(value.text).substr(first, value.text.find_last_not_of(spaces) - first + 1);
}

[[huxerui::composable]]
View LinkDialog(DialogContext context, RichEditorController editor, ViewHandle focus, std::optional<LinkRange> original) {
  const auto& theme = UseTheme();
  auto initial = TextEditingValue::FromText(original ? original->target : std::string{});
  initial.selection.anchor = 0;
  const auto address = UseState(std::move(initial));
  const auto applied = UseState(false);
  const auto tasks = UseApplicationTaskScope();
  // Restore the editor only after the dialog has released its focus trap.
  Lifecycle([applied, tasks, focus] {
    return [applied, tasks, focus] {
      if (applied.Get()) {
        tasks.Post([focus] { focus.RequestFocus(); });
      }
    };
  }, focus);
  const auto current = LinkAddress(address.Get());
  const auto apply = [context, editor, address, original, applied] {
    const auto value = address.Get();
    const auto target = LinkAddress(value);
    if (!target.empty() && (!original || target != original->target) &&
        editor.Execute(SetLink{std::string(target), original ? original->title : std::string{}})) {
      applied = context.Dismiss();
    }
  };
  return DialogPanel(Column {
    Text(original ? l10n::edit_link : l10n::insert_link, TextRole::Title),
    TextField(address)
        .Label(l10n::link_address)
        .Placeholder("https://example.com")
        .Variant(TextFieldVariant::Outlined)
        .InputConfiguration({.type = TextInputType::Url, .autocorrect = false})
        .OnChanged([address](const TextEditingValue& next) { address = next; })
        .OnSubmitted(apply),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(original ? l10n::apply : l10n::insert).OnClick(apply)
          .With(Enabled(!current.empty() && (!original || current != original->target)))),
  }, theme, 360.0F);
}

using OpenDocumentLink = std::function<void(folio::DocumentLink)>;

bool CanOpenLink(std::string_view target, const OpenDocumentLink& open_document) {
  return folio::ParseWebUrl(target).has_value() || (open_document && folio::ParseDocumentLink(target).has_value());
}

bool OpenLink(std::string_view target, const OpenDocumentLink& open_document, const ToastHandle& toast) {
  if (const auto url = folio::ParseWebUrl(target)) {
    folio::OpenBrowser(*url, toast);
    return true;
  }
  if (open_document) {
    if (auto link = folio::ParseDocumentLink(target)) {
      open_document(std::move(*link));
      return true;
    }
  }
  return false;
}

[[huxerui::composable]]
View LinkTool(RichEditorController editor, ViewHandle focus, bool available, bool compact,
              OpenDocumentLink open_document) {
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto dialog = UseDialog();
  const auto toast = UseToast();
  const auto clipboard = UseApplication().Clipboard();
  const float size = compact ? 44.0F : 30.0F;
  const bool linked = editor.CurrentLink().has_value();
  StringVariant label = linked ? l10n::link_actions : l10n::insert_link;

  return IconButton(app::images::link, label)
      .OnClick([=] {
        const auto current = editor.CurrentLink();
        if (!current) {
          dialog.Show(LinkDialog, editor, focus, std::optional<LinkRange>{});
          return;
        }
        std::vector<MenuEntry> items{
            MenuItem(l10n::edit_link_menu, [=] { dialog.Show(LinkDialog, editor, focus, current); }),
            MenuItem(l10n::copy_link, [clipboard, toast, target = current->target] {
              const bool copied = clipboard->WriteText(target);
              toast.Show(copied ? l10n::link_copied : l10n::link_copy_failed);
            }).Enabled(clipboard && clipboard->IsAvailable() && !current->target.empty()),
            MenuItem(l10n::remove_link, [editor, focus] {
              if (editor.Execute(SetLink{})) {
                focus.RequestFocus();
              }
            }),
        };
        if (CanOpenLink(current->target, open_document)) {
          items.insert(items.begin(), MenuItem(l10n::open_link, [toast, open_document, target = current->target] {
            OpenLink(target, open_document, toast);
          }));
        }
        menu.Show(anchor, std::move(items));
      })
      .With(
          anchor,
          Frame{.min_width = size, .min_height = size},
          Tooltip(label),
          Enabled(available && editor.GetCommandState(SetLink{}).enabled),
          Foreground(linked ? theme.colors.on_primary_container : theme.colors.on_surface_variant),
          Background(linked ? theme.colors.primary_container : Color::Transparent())
      );
}

struct FormulaRequest {
  bool inline_math = false;
  std::optional<CustomBlockData> block;
  std::optional<InlineObjectSelection> object;
};

[[huxerui::composable]]
View FormulaDialog(DialogContext context, RichEditorController editor, FormulaRequest request) {
  const auto& theme = UseTheme();
  const bool editing = request.block || request.object;
  const std::string source = request.object ? request.object->data.payload
      : request.block ? request.block->payload : R"(\frac{a}{b})";
  const auto draft = UseState(TextEditingValue{source, {0, AttributedText(source).Length()}, {}});
  EditorCommand command = InsertCustomBlock{LatexBlock(draft->text)};
  if (request.object) {
    command = ReplaceInlineObject{request.object->target, LatexInline(draft->text), request.object->data};
  } else if (request.inline_math) {
    command = InsertInlineObject{LatexInline(draft->text)};
  } else if (request.block) {
    command = ReplaceCustomBlock{LatexBlock(draft->text), request.block};
  }
  const bool enabled = draft->text.find_first_not_of(" \t\r\n") != std::string::npos &&
      !draft->composition && editor.GetCommandState(command).enabled;
  return DialogPanel(Column {
    Text(editing ? l10n::edit_formula : request.inline_math ? l10n::insert_inline_formula : l10n::insert_display_formula, TextRole::Title),
    TextField(draft)
        .Label(l10n::latex_source)
        .Variant(TextFieldVariant::Outlined)
        .LineLimits(request.inline_math ? TextFieldLineLimits::SingleLine() : TextFieldLineLimits::MultiLine(2, 5))
        .InputConfiguration({.autocorrect = false})
        .OnChanged([draft](const TextEditingValue& value) { draft = value; }),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(editing ? l10n::apply : l10n::insert).OnClick([=] {
        if (editor.Execute(command)) {
          context.Dismiss();
        }
      }).With(Enabled(enabled))),
  }, theme, 400.0F);
}

struct FindMatch {
  std::size_t block_index;
  BlockTextRange target;
};

struct FindResults {
  RichDocument document;
  std::string query;
  std::vector<FindMatch> matches;
};

/// Resumes single-match replacement at the end of the inserted text in the refreshed results.
struct FindResume {
  RichDocument document;
  std::string query;
  std::size_t block_index;
  TextOffset offset;
};

std::vector<FindMatch> FindMatches(std::span<const DecorationBlock> blocks, std::string_view query) {
  std::vector<FindMatch> matches;
  const auto length = AttributedText(std::string(query)).Length();
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    const auto& block = blocks[index].block;
    const std::string_view text = block.Data().text;
    const auto& objects = block.Data().inline_objects;
    auto object = objects.begin();
    std::size_t previous = 0;
    TextOffset offset = 0;
    for (auto match = text.find(query); match != text.npos; match = text.find(query, previous)) {
      offset += AttributedText(std::string(text.substr(previous, match - previous))).Length();
      while (object != objects.end() && object->offset < offset) {
        ++object;
      }
      if (object == objects.end() || object->offset >= offset + length) {
        matches.push_back({index, {block, {offset, offset + length}}});
      }
      offset += length;
      previous = match + query.size();
    }
  }
  return matches;
}

struct FindNavigation {
  std::size_t count;
  std::optional<std::size_t> selected;
  bool ready;
  std::function<void(bool forward)> move;
};

struct FindReplacement {
  State<TextEditingValue> value;
  bool enabled;
  std::function<void(bool all)> apply;
};

void SetFindFieldFocus(State<FindPanelState> find, FindField field, bool focused) {
  find.Update([field, focused](FindPanelState& state) {
    if (focused) {
      state.focused = field;
    } else if (state.focused == field) {
      state.focused = FindField::None;
    }
  });
}

[[huxerui::composable]]
View FindBar(State<TextEditingValue> query, ViewHandle focus, State<FindPanelState> find, FindNavigation navigation,
             FindReplacement replacement, bool read_only, std::function<void()> close) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const bool touch = !UseApplication().SupportsMultipleWindows();
  const float size = touch ? 44.0F : 36.0F;
  const Frame button_size{.width = compact ? 32.0F : size, .height = size};
  const Frame toggle_size{.width = compact ? 36.0F : size, .height = size};
  auto style = UseEnvironment<TextFieldStyle>();
  style.show_label = false;
  style.text_style.font = style.text_style.font.WithSize(theme.typography.body_medium);
  style.placeholder_style.font = style.placeholder_style.font.WithSize(theme.typography.body_medium);
  style.standard = {.border = Color::Transparent(), .hovered_border = Color::Transparent(),
                    .focused_border = Color::Transparent(), .minimum_height = size};
  style.padding = EdgeInsets::Symmetric(compact ? 7.0F : 9.0F, 5.0F);
  style.leading_icon_size = 15.0F;
  style.icon_spacing = 7.0F;

  auto search = TextField(query);
  if (!compact) {
    search = std::move(search).LeadingIcon(app::images::search);
  }
  const auto field = std::move(search)
      .Label(l10n::find_in_document)
      .Placeholder(compact ? l10n::find_text : l10n::find_in_document)
      .Variant(TextFieldVariant::Standard)
      .LineLimits(TextFieldLineLimits::SingleLine())
      .InputConfiguration({.action = TextInputAction::Search, .autocorrect = false})
      .OnChanged([query](const TextEditingValue& value) { query = value; })
      .OnSubmitted([move = navigation.move] { move(true); })
      .On<ViewEvents::FocusChanged>([find](bool value) {
        SetFindFieldFocus(find, FindField::Query, value);
      })
      .On<ViewEvents::KeyIntercept>([move = navigation.move, query](const KeyEvent& event) {
        if (event.type != KeyEventType::Down || event.key != Key::Enter || !event.modifiers.shift ||
            event.modifiers.control || event.modifiers.alt || event.modifiers.meta || query->composition) {
          return false;
        }
        move(false);
        return true;
      })
      .With(focus, Grow(), Semantics{.role = SemanticRole::SearchField, .identifier = "find-query"});
  StringVariant count;
  StringVariant announcement;
  if (!query->text.empty()) {
    if (!navigation.ready) {
      count = "…";
      announcement = l10n::searching;
    } else if (navigation.selected && *navigation.selected < navigation.count) {
      count = std::to_string(*navigation.selected + 1) + (compact ? "/" : " / ") + std::to_string(navigation.count);
      announcement = StringVariant::Format(l10n::match_position, *navigation.selected + 1, navigation.count);
    } else {
      count = navigation.count == 0 && !compact
          ? StringVariant{l10n::no_matches} : StringVariant{std::to_string(navigation.count)};
      announcement = StringVariant::Format(navigation.count == 1 ? l10n::match_count_one : l10n::match_count_many,
                                           navigation.count);
    }
  }
  const bool can_navigate = navigation.count != 0 && !query->composition;
  const auto arrow = [&](const ImageResource& icon, StringVariant label, bool forward) {
    return IconButton(icon, label).OnClick([move = navigation.move, forward] { move(forward); })
        .With(button_size, Focusable(!touch), Enabled(can_navigate), Tooltip(label));
  };
  const auto group = Row {
    ProvideEnvironment(style, field),
    Text(count, TextRole::Label).Align(TextAlign::Center).VerticalAlign(TextVerticalAlign::Center)
        .LineLimit(1).Overflow(TextOverflow::Ellipsis)
        .With(Frame{.height = 18.0F, .min_width = compact ? 24.0F : 72.0F,
                    .max_width = compact ? std::optional{52.0F} : std::nullopt},
              FontSize(11.0F), ClipChildren(), Foreground(colors.on_surface_variant),
              Semantics{.label = announcement, .identifier = "find-count", .live_region = SemanticLiveRegion::Polite,
                        .hidden = query->text.empty()}),
    arrow(app::images::chevron_up, l10n::previous_match, false),
    arrow(app::images::chevron_down, l10n::next_match, true),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Center), Background(colors.surface),
         Border{find->focused == FindField::Query ? colors.primary : colors.outline, 1.0F},
         CornerRadius(theme.shapes.medium));
  const auto replace_open = !read_only && find->replace_open;
  const auto toggle_label = replace_open ? l10n::hide_replacement : l10n::find_and_replace;
  View replace_controls;
  if (replace_open) {
    const auto replacement_field = Row {
      ProvideEnvironment(style, TextField(replacement.value)
          .Label(l10n::replacement_text)
          .Placeholder(l10n::replacement_text)
          .Variant(TextFieldVariant::Standard)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .InputConfiguration({.autocorrect = false})
          .OnChanged([value = replacement.value](const TextEditingValue& next) { value = next; })
          .On<ViewEvents::FocusChanged>([find](bool value) { SetFindFieldFocus(find, FindField::Replacement, value); })
          .With(Grow(), Semantics{.identifier = "replace-text"})),
    }.With(Frame{.height = size}, Background(colors.surface),
           Border{find->focused == FindField::Replacement ? colors.primary : colors.outline, 1.0F},
           CornerRadius(theme.shapes.medium));
    const auto action = [&](StringResource label, bool all) {
      return Button(label).OnClick([apply = replacement.apply, all] { apply(all); })
          .With(Frame{.min_height = size}, Enabled(replacement.enabled));
    };
    const auto buttons = Row {action(l10n::replace, false), action(l10n::replace_all, true)}
        .With(Spacing(5.0F), MainAlign(compact ? MainAxisAlignment::End : MainAxisAlignment::Start),
              CrossAlign(CrossAxisAlignment::Center));
    replace_controls = compact
        ? View(Column {replacement_field, buttons}.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        : View(Row {View(replacement_field).With(Grow()), buttons}
                   .With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center)));
  }
  return Column {
    Row {
      read_only ? View{} : View(IconButton(replace_open ? app::images::chevron_down : app::images::chevron_right,
                                          toggle_label)
          .OnClick([find, focus, touch] {
            const bool restore_query = find->replace_open && find->focused == FindField::Replacement && !touch;
            find.Update([](FindPanelState& state) {
              state.replace_open = !state.replace_open;
              if (!state.replace_open && state.focused == FindField::Replacement) {
                state.focused = FindField::None;
              }
            });
            if (restore_query) {
              focus.RequestFocus();
            }
          }).With(toggle_size, Focusable(!touch), Tooltip(toggle_label),
                  Semantics{.identifier = "toggle-replace", .expanded = replace_open})),
      group,
      IconButton(app::images::close, l10n::close_find).OnClick(close).With(button_size, Tooltip(l10n::close_find)),
    }.With(Spacing(compact ? 3.0F : 5.0F), CrossAlign(CrossAxisAlignment::Center)).Key("query-row"),
    replace_controls ? std::move(replace_controls).Key("replace-controls") : View{},
  }.On<ViewEvents::KeyIntercept>([close, query, value = replacement.value](const KeyEvent& event) {
    if (event.type == KeyEventType::Down && event.key == Key::Escape && !query->composition && !value->composition) {
      close();
      return true;
    }
    return false;
  }).With(Padding(EdgeInsets::Symmetric(compact ? 8.0F : 14.0F, 6.0F)),
          Spacing(5.0F),
          CrossAlign(CrossAxisAlignment::Stretch),
          Background(colors.surface_container));
}

[[huxerui::composable]]
View FormattingBar(RichEditorController editor, ViewHandle focus, bool available, bool can_pick_images,
                   std::function<void(bool, bool)> pick_image, OpenDocumentLink open_document) {
  using namespace app::images;
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto dialog = UseDialog();
  const auto table_anchor = UseViewHandle();
  const auto image_anchor = UseViewHandle();
  const auto list_anchor = UseViewHandle();
  const auto formula_anchor = UseViewHandle();
  const auto more_anchor = UseViewHandle();
  const auto application = UseApplication();
  const auto clipboard = application.Clipboard();
  const auto toast = UseToast();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const float tool_size = compact ? 44.0F : 30.0F;
  const auto tool_frame = Frame{.min_width = tool_size, .min_height = tool_size};
  const auto selected_link = folio::IsMobilePlatform() ? editor.CurrentLink() : std::nullopt;
  const View open_link = selected_link && CanOpenLink(selected_link->target, open_document) ? View(Button(l10n::open_link)
      .OnClick([toast, open_document, target = selected_link->target] { OpenLink(target, open_document, toast); })
      .With(tool_frame, Enabled(available), Background(theme.colors.primary_container),
            Foreground(theme.colors.on_primary_container))) : View{};
  const auto header_state = editor.GetCommandState(ToggleTableHeader{});
  const bool in_table = header_state.checked.has_value();
  const bool in_code = editor.GetCommandState(SetBlockKind{BlockKind::CodeBlock}).checked == CheckState::On;
  const ContinueInParagraph continue_in_paragraph;
  const EditorCommand insert_table = InsertTable{};
  const EditorCommand insert_thematic_break = InsertThematicBreak{};
  const bool can_insert_thematic_break = editor.GetCommandState(insert_thematic_break).enabled;
  const auto run = [editor, focus](const EditorCommand& command) {
    if (editor.Execute(command)) {
      focus.RequestFocus();
    }
  };
  const auto command_item = [editor, run](StringVariant label, EditorCommand command) {
    const auto state = editor.GetCommandState(command);
    auto item = MenuItem(label, [run, command = std::move(command)] { run(command); }).Enabled(state.enabled);
    return state.checked ? std::move(item).Checked(state.checked == CheckState::On) : std::move(item);
  };
  const auto format_tool = [&](const ImageResource& icon, StringVariant label, CommandState state, auto on_click) {
    Color foreground = theme.colors.on_surface_variant;
    Color background = Color::Transparent();
    std::optional<SemanticCheckedState> checked;
    if (state.checked == CheckState::On) {
      foreground = theme.colors.on_primary_container;
      background = theme.colors.primary_container;
      checked = SemanticCheckedState::Checked;
    } else if (state.checked == CheckState::Mixed) {
      foreground = theme.colors.on_tertiary_container;
      background = theme.colors.tertiary_container;
      checked = SemanticCheckedState::Mixed;
    } else if (state.checked) {
      checked = SemanticCheckedState::Unchecked;
    }
    return IconButton(icon, label)
        .OnClick(std::move(on_click))
        .With(
            tool_frame,
            Tooltip(label),
            Enabled(available && state.enabled),
            Foreground(foreground),
            Background(background),
            Semantics{.checked = checked}
        );
  };
  const auto tool = [&](const ImageResource& icon, StringVariant label, EditorCommand command) {
    const auto state = editor.GetCommandState(command);
    return format_tool(icon, label, state, [run, command = std::move(command)] { run(command); });
  };
  const auto menu_tool = [&](const ImageResource& icon, StringVariant label, ViewHandle anchor,
                             bool enabled, bool active, auto on_click) {
    return IconButton(icon, label)
        .OnClick(std::move(on_click))
        .With(
            anchor,
            tool_frame,
            Tooltip(label),
            Enabled(available && enabled),
            Foreground(active ? theme.colors.on_primary_container : theme.colors.on_surface_variant),
            Background(active ? theme.colors.primary_container : Color::Transparent())
        );
  };
  const auto table_label = in_table ? l10n::table_actions : l10n::insert_table;
  const bool image_selected = editor.GetCommandState(DeleteImage{}).enabled;
  const bool can_insert_table = editor.GetCommandState(insert_table).enabled;
  const bool can_insert_image = can_pick_images &&
      editor.GetCommandState(InsertImage{{.source = "attachments/image.jpg"}}).enabled;
  const auto custom = editor.SelectedCustomBlock();
  const auto inline_object = editor.SelectedInlineObject();
  const bool inline_formula = inline_object && inline_object->data.type == latex_type &&
      inline_object->data.version == 1;
  const bool formula_selected = inline_formula || (custom && custom->type == latex_type && custom->version == 1);
  const bool can_insert_formula = editor.GetCommandState(InsertCustomBlock{LatexBlock("x")}).enabled;
  const bool can_insert_inline_formula = editor.GetCommandState(InsertInlineObject{LatexInline("x")}).enabled;
  const bool can_insert_inline_image = can_pick_images && editor.GetCommandState(
      InsertInlineObject{InlineImage({.source = "attachments/image.png"})}).enabled;
  const auto insert_formula = [=] { dialog.Show(FormulaDialog, editor, FormulaRequest{}); };
  const auto insert_inline_formula = [=] { dialog.Show(FormulaDialog, editor, FormulaRequest{.inline_math = true}); };
  const auto edit_formula = [=] {
    dialog.Show(FormulaDialog, editor, FormulaRequest{inline_formula, custom, inline_formula ? inline_object : std::nullopt});
  };
  const auto copy_formula = [=] { clipboard->WriteText(inline_formula ? inline_object->data.payload : custom->payload); };
  const auto selected_size = editor.SelectedImageSize();
  const ToggleList ordered_list{ListKind::Ordered};
  const auto ordered_state = editor.GetCommandState(ordered_list);
  const bool in_numbered_list = ordered_state.checked == CheckState::On;
  const auto list_start = editor.OrderedListStart();
  const auto list_label = in_numbered_list ? l10n::list_numbering : l10n::numbered_list;
  const auto table_entries = [command_item, continue_in_paragraph, header_state] {
    return std::vector<MenuEntry>{
        command_item(l10n::insert_row_above, InsertTableRow{TableInsertSide::Before}),
        command_item(l10n::insert_row_below, InsertTableRow{}),
        command_item(l10n::delete_row, DeleteTableRow{}),
        MenuSection{},
        command_item(l10n::insert_column_before, InsertTableColumn{TableInsertSide::Before}),
        command_item(l10n::insert_column_after, InsertTableColumn{}),
        command_item(l10n::delete_column, DeleteTableColumn{}),
        MenuItem(l10n::column_alignment, {
            command_item(l10n::align_default, SetTableColumnAlignment{TableColumnAlignment::Default}),
            command_item(l10n::align_left, SetTableColumnAlignment{TableColumnAlignment::Left}),
            command_item(l10n::align_center, SetTableColumnAlignment{TableColumnAlignment::Center}),
            command_item(l10n::align_right, SetTableColumnAlignment{TableColumnAlignment::Right}),
        }).Enabled(header_state.enabled),
        MenuSection{},
        command_item(l10n::header_row, ToggleTableHeader{}),
        command_item(l10n::continue_below_table, continue_in_paragraph),
        command_item(l10n::delete_table, DeleteTable{}),
    };
  };
  const auto list_entries = [=] {
    return std::vector<MenuEntry>{
        MenuItem(l10n::set_start_number, [=] {
          if (const auto current = editor.OrderedListStart()) {
            dialog.Show(ListStartDialog, editor, focus, *current);
          }
        }),
        command_item(l10n::restart_at_one, SetListStart{}).Enabled(list_start && *list_start != 1),
        MenuSection{},
        command_item(l10n::increase_list_indent, IndentList{}),
        command_item(l10n::decrease_list_indent, OutdentList{}),
        MenuItem(l10n::remove_numbering, [run, ordered_list] { run(ordered_list); }).Enabled(ordered_state.enabled),
    };
  };
  const auto open_image_size = [editor, dialog] {
    if (const auto size = editor.SelectedImageSize()) {
      dialog.Show(ImageSizeDialog, editor, *size);
    }
  };
  const auto add_after = MenuItem(l10n::add_image_after, [pick_image] { pick_image(false, false); }).Enabled(can_pick_images);
  const auto replace_image = [pick_image] { pick_image(true, false); };
  const auto table_tool = menu_tool(table_grid, table_label, table_anchor, header_state.enabled || can_insert_table,
                                    in_table, [=] {
    if (in_table) {
      menu.Show(table_anchor, table_entries());
    } else {
      run(insert_table);
    }
  });
  const CommandState numbering_state{
      in_numbered_list ? list_start.has_value() : ordered_state.enabled, ordered_state.checked};
  const auto list_tool = format_tool(numbered_list, list_label, numbering_state, [=] {
    if (in_numbered_list) {
      menu.Show(list_anchor, list_entries());
    } else {
      run(ordered_list);
    }
  }).With(list_anchor);
  const auto padding = Padding(EdgeInsets::Symmetric(compact ? 5.0F : 12.0F, 5.0F));
  const auto background = Background(theme.colors.surface);
  const auto more_tool = menu_tool(more, compact && image_selected ? l10n::image_actions : l10n::more_formatting,
                                  more_anchor, true, false, [=] {
    if (compact && image_selected) {
      menu.Show(more_anchor, {command_item(l10n::redo, Redo{}), MenuSection{}, command_item(l10n::delete_image, DeleteImage{})});
      return;
    }
    std::vector<MenuEntry> items;
    if (in_table) {
      items.emplace_back(MenuItem(l10n::table_actions, table_entries()));
    }
    if (list_start) {
      items.emplace_back(MenuItem(l10n::list_numbering, list_entries()));
    }
    if (compact) {
      items.emplace_back(command_item(l10n::redo, Redo{}));
    }
    if (!in_table && !in_code && editor.GetCommandState(continue_in_paragraph).enabled) {
      items.emplace_back(command_item(l10n::continue_in_plain_paragraph, continue_in_paragraph));
    }
    items.emplace_back(MenuItem(l10n::text_formatting, {
        command_item(l10n::strikethrough, ToggleMark{Mark::Strike}),
        command_item(l10n::inline_code, ToggleMark{Mark::Code}),
        command_item(l10n::highlight, ToggleMark{Mark::Highlight}),
        command_item(l10n::clear_formatting, ClearFormatting{}),
    }));
    items.emplace_back(MenuItem(l10n::insert, {
        command_item(l10n::insert_table, insert_table),
        command_item(l10n::insert_thematic_break, insert_thematic_break),
        MenuItem(l10n::insert_image, [pick_image] { pick_image(false, false); }).Enabled(can_insert_image),
        MenuItem(l10n::insert_inline_image, [pick_image] { pick_image(false, true); }).Enabled(can_insert_inline_image),
        MenuItem(l10n::insert_inline_formula_menu, insert_inline_formula).Enabled(can_insert_inline_formula),
        MenuItem(l10n::insert_display_formula_menu, insert_formula).Enabled(can_insert_formula),
    }).Enabled(can_insert_table || can_insert_thematic_break || can_insert_image || can_insert_inline_image ||
               can_insert_formula || can_insert_inline_formula));
    items.emplace_back(MenuItem(l10n::lists_and_quote, {
        command_item(l10n::bulleted_list, ToggleList{ListKind::Unordered}),
        command_item(l10n::numbered_list, ordered_list),
        command_item(l10n::task_list, ToggleTaskMarker{}),
        command_item(l10n::block_quote, ToggleQuote{}),
        command_item(l10n::increase_list_indent, IndentList{}),
        command_item(l10n::decrease_list_indent, OutdentList{}),
    }));
    menu.Show(more_anchor, std::move(items));
  });

  const auto text_tool = [&](StringVariant title, StringVariant label, bool enabled, auto on_click) {
    return Button(title).OnClick(std::move(on_click)).With(
        tool_frame,
        Padding(EdgeInsets::Symmetric(8.0F, 6.0F)),
        Enabled(available && enabled),
        Background(Color::Transparent()),
        Foreground(theme.colors.on_surface),
        Semantics{.label = label}
    );
  };
  const auto text_command = [&](StringVariant title, StringVariant label, EditorCommand command) {
    return text_tool(title, label, editor.GetCommandState(command).enabled,
                     [run, command] { run(command); });
  };
  const auto continue_below = in_table || in_code
      ? text_command(l10n::continue_below, in_table ? l10n::continue_below_table : l10n::continue_below_code,
                     continue_in_paragraph)
      : View{};
  if (compact) {
    const ToggleList unordered_list{ListKind::Unordered};
    const auto unordered_state = editor.GetCommandState(unordered_list);
    const bool show_numbered = ordered_state.checked.value_or(CheckState::Off) != CheckState::Off;
    const bool show_bulleted = unordered_state.checked.value_or(CheckState::Off) != CheckState::Off;
    const bool show_list = show_numbered || show_bulleted;
    const bool in_quote = editor.GetCommandState(ToggleQuote{}).checked == CheckState::On;
    const bool can_link = editor.GetCommandState(SetLink{}).enabled;
    const char* context = "text";
    Views actions;
    if (image_selected) {
      context = "image";
      actions.Add(text_tool(l10n::size, l10n::image_size, selected_size.has_value(), open_image_size));
      actions.Add(text_tool(l10n::replace, l10n::replace_image, can_pick_images, replace_image));
      actions.Add(text_tool(l10n::add_image_short, l10n::add_image_after, can_pick_images, [pick_image] { pick_image(false, false); }));
    } else if (formula_selected) {
      context = "formula";
      actions.Add(text_tool(l10n::edit_formula, l10n::edit_formula, true, edit_formula));
      actions.Add(text_tool(l10n::copy_latex, l10n::copy_latex, clipboard && clipboard->IsAvailable(), copy_formula));
    } else {
      if (continue_below) {
        actions.Add(continue_below);
      }
      if (open_link) {
        actions.Add(open_link);
      }
      if (in_table) {
        context = "table";
        actions.Add(table_tool);
        actions.Add(text_command(l10n::add_row_short, l10n::insert_row_below, InsertTableRow{}));
        actions.Add(text_command(l10n::add_column_short, l10n::insert_column_after, InsertTableColumn{}));
      } else if (in_code) {
        context = "code";
      } else if (show_list) {
        context = show_numbered && show_bulleted ? "mixed-list" : show_numbered ? "numbered-list" : "bulleted-list";
        if (show_numbered) {
          actions.Add(list_tool);
        }
        if (show_bulleted) {
          actions.Add(format_tool(bulleted_list, l10n::bulleted_list, unordered_state, [run, unordered_list] {
            run(unordered_list);
          }));
        }
        actions.Add(tool(outdent, l10n::decrease_list_indent, OutdentList{}));
        actions.Add(tool(indent, l10n::increase_list_indent, IndentList{}));
      } else if (in_quote) {
        context = "quote";
        actions.Add(tool(quote, l10n::block_quote, ToggleQuote{}));
      } else if (can_link) {
        context = "link";
        actions.Add(LinkTool(editor, focus, available, compact, open_document));
      }
      const View text_style = in_table ? View{} : TextStylePicker(editor, focus, available, compact);
      const bool style_first = in_code || (!show_list && !in_quote && !can_link);
      if (text_style && style_first) {
        actions.Add(text_style);
      }
      if (!in_code) {
        actions.Add(tool(bold, l10n::bold, ToggleMark{Mark::Bold}));
        actions.Add(tool(italic, l10n::italic, ToggleMark{Mark::Italic}));
        actions.Add(tool(underline, l10n::underline, ToggleMark{Mark::Underline}));
      }
      if (can_link && (in_table || show_list || in_quote)) {
        actions.Add(LinkTool(editor, focus, available, compact, open_document));
      }
      if (text_style && !style_first) {
        actions.Add(text_style);
      }
    }

    return Row {
      tool(undo, l10n::undo, Undo{}),
      // Mount a fresh scroll position when the selection moves to another tool group.
      ScrollView {
        Row {std::move(actions)}.With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Center))
      }.ScrollAxis(Axis::Horizontal).With(Grow(), ScrollBar()).Key(open_link ? "selected-link" : context),
      more_tool,
    }.With(padding, Spacing(2.0F), CrossAlign(CrossAxisAlignment::Center), background);
  }

  const auto image_tool = menu_tool(image_block, image_selected ? l10n::image_actions : l10n::insert_image, image_anchor,
      image_selected || can_insert_image || can_insert_inline_image, image_selected, [=] {
    if (image_selected) {
      menu.Show(image_anchor, {
          MenuItem(l10n::image_size_menu, open_image_size).Enabled(selected_size.has_value()),
          add_after,
          MenuItem(l10n::replace_image, replace_image).Enabled(can_pick_images),
          MenuSection{},
          command_item(l10n::delete_image, DeleteImage{}),
      });
    } else {
      menu.Show(image_anchor, {
          MenuItem(l10n::insert_image_block, [pick_image] { pick_image(false, false); }).Enabled(can_insert_image),
          MenuItem(l10n::insert_inline_image, [pick_image] { pick_image(false, true); }).Enabled(can_insert_inline_image),
      });
    }
  });
  const auto formula_tool = menu_tool(formula, formula_selected ? l10n::formula_actions : l10n::insert_formula, formula_anchor,
      formula_selected || can_insert_formula || can_insert_inline_formula, formula_selected, [=] {
    if (formula_selected) {
      menu.Show(formula_anchor, {
          MenuItem(l10n::edit_formula_menu, edit_formula),
          MenuItem(l10n::copy_latex, copy_formula).Enabled(clipboard && clipboard->IsAvailable()),
      });
    } else {
      menu.Show(formula_anchor, {
          MenuItem(l10n::inline_formula_menu, insert_inline_formula).Enabled(can_insert_inline_formula),
          MenuItem(l10n::display_formula_menu, insert_formula).Enabled(can_insert_formula),
      });
    }
  });

  const auto separator = Divider(Axis::Vertical).With(Frame{.height = 21.0F});
  return ScrollView {
    Row {
      tool(undo, l10n::undo, Undo{}),
      folio::IsMobilePlatform() ? continue_below : View{},
      open_link,
      tool(redo, l10n::redo, Redo{}),
      separator,
      TextStylePicker(editor, focus, available, compact),
      tool(bold, l10n::bold, ToggleMark{Mark::Bold}),
      tool(italic, l10n::italic, ToggleMark{Mark::Italic}),
      tool(underline, l10n::underline, ToggleMark{Mark::Underline}),
      LinkTool(editor, focus, available, compact, open_document),
      tool(strikethrough, l10n::strikethrough, ToggleMark{Mark::Strike}),
      tool(inline_code, l10n::inline_code, ToggleMark{Mark::Code}),
      tool(highlight, l10n::highlight, ToggleMark{Mark::Highlight}),
      tool(clear_formatting, l10n::clear_formatting, ClearFormatting{}),
      separator,
      tool(bulleted_list, l10n::bulleted_list, ToggleList{ListKind::Unordered}),
      list_tool,
      tool(task_list, l10n::task_list, ToggleTaskMarker{}),
      tool(quote, l10n::block_quote, ToggleQuote{}),
      tool(indent, l10n::increase_list_indent, IndentList{}),
      tool(outdent, l10n::decrease_list_indent, OutdentList{}),
      separator,
      table_tool,
      image_tool,
      formula_tool,
    }.With(padding, Spacing(1.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.ScrollAxis(Axis::Horizontal).With(ScrollBar(), background);
}

} // namespace

namespace folio {

ImportResult LoadSample() { return DocumentMarkdown().Import(sample_markdown); }

[[huxerui::composable]]
View EditorPane(RichEditorController editor, EditorPaneOptions options) {
  const auto focus = options.focus;
  const auto find_focus = UseViewHandle();
  const auto dialog = UseDialog();
  const auto toast = UseToast();
  const auto find_query = UseState(TextEditingValue{});
  const auto replacement_text = UseState(TextEditingValue{});
  const auto find_provider = UseState(std::shared_ptr<const DecorationProvider>{});
  const auto active_provider = UseState(std::shared_ptr<const DecorationProvider>{});
  const auto find_results = UseState(std::shared_ptr<const FindResults>{});
  const auto active_match = UseState(std::optional<std::size_t>{});
  const auto resume_match = UseState(std::optional<FindResume>{});
  const auto tasks = UseTaskScope();
  const auto& theme = UseTheme();
  const auto settings = UseService<SettingsStore>();
  const auto& preferences = settings->Get().editor;
  const auto viewport = UseViewportClass();
  const bool compact = viewport == ViewportClass::Compact;
  const bool read_only = editor.IsReadOnly();
  const auto find = options.find;
  const bool search_open = find->open;
  const bool finding = options.active && search_open;
  Lifecycle([=] {
    if (finding) {
      find_focus.RequestFocus();
    }
  }, finding, find->focus_request);

  const auto query = find_query->text;
  const bool editor_composing = finding && find->replace_open && editor.IsComposing();
  const auto match_color = theme.colors.primary_container;
  Lifecycle([=] {
    if (!search_open || query.empty()) {
      find_provider = nullptr;
      find_results = nullptr;
      active_match = std::nullopt;
      resume_match = std::nullopt;
      return;
    }
    find_provider = std::make_shared<const DecorationProvider>(DecorationProvider{
        .scope = DecorationScope::Document,
        .analyze = [=](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
          auto matches = co_await RunWorker([query](std::vector<DecorationBlock> blocks) {
            return FindMatches(blocks, query);
          }, std::move(request.blocks));
          if (!find->open || find_query->text != query || editor.Document() != *request.document) {
            co_return std::vector<TextDecorationRange>{};
          }
          std::vector<TextDecorationRange> marks;
          marks.reserve(matches.size());
          for (const auto& match : matches) {
            marks.push_back({match.block_index, match.target.range, DecorationKind::Background, match_color});
          }
          const auto& previous = find_results.Get();
          if (!previous || previous->query != query || previous->document != *request.document) {
            active_match = std::nullopt;
          }
          find_results = std::make_shared<const FindResults>(
              FindResults{std::move(*request.document), query, std::move(matches)});
          co_return marks;
        },
    });
  }, search_open, query, match_color);

  const auto results = find_results.Get();
  const bool ready = search_open && results && results->query == query && results->document == editor.Document();
  const std::size_t count = ready ? results->matches.size() : 0;
  const auto selected = active_match.Get();
  const auto active_color = theme.colors.primary;
  Lifecycle([=] {
    if (!finding || !ready || editor_composing || !resume_match.Get()) {
      return;
    }
    tasks.Post([=] {
      const auto& resume = resume_match.Get();
      if (!resume || !find->open || find_results.Get() != results || find_query->text != results->query ||
          editor.Document() != results->document) {
        return;
      }
      if (resume->document == results->document && resume->query == results->query && !results->matches.empty()) {
        const auto next = std::ranges::lower_bound(results->matches, std::pair{resume->block_index, resume->offset},
            {}, [](const FindMatch& match) { return std::pair{match.block_index, match.target.range.start}; });
        const auto match = next == results->matches.end() ? results->matches.begin() : next;
        if (editor.RevealTextRange(match->target.block, match->target.range, ScrollAlignment::Center)) {
          active_match = static_cast<std::size_t>(match - results->matches.begin());
        }
      }
      resume_match = std::nullopt;
    });
  }, finding, ready, results, editor_composing);

  Lifecycle([=] {
    if (!selected || *selected >= count) {
      active_provider = nullptr;
      return;
    }
    const auto& match = results->matches[*selected];
    const auto mark = TextDecorationRange{match.block_index, match.target.range, DecorationKind::Outline, active_color};
    active_provider = std::make_shared<const DecorationProvider>(DecorationProvider{
        .scope = DecorationScope::Document,
        .analyze = [document = results->document, mark](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
          co_return request.document == document ? std::vector{mark} : std::vector<TextDecorationRange>{};
        },
    });
  }, results, count, selected, active_color);

  std::vector<std::shared_ptr<const DecorationProvider>> decorations;
  if (const auto& provider = find_provider.Get(); search_open && provider) {
    decorations.push_back(provider);
  }
  if (const auto& provider = active_provider.Get(); search_open && provider) {
    decorations.push_back(provider);
  }

  View find_bar;
  if (finding) {
    const auto navigate = [editor, results, count, active_match, find_query, resume_match](bool forward) {
      if (count == 0 || find_query->composition || find_query->text != results->query ||
          editor.Document() != results->document) {
        return;
      }
      const auto index = active_match.Get();
      const auto next = index && *index < count
          ? (forward ? (*index + 1) % count : (*index + count - 1) % count)
          : (forward ? 0 : count - 1);
      const auto& match = results->matches[next];
      if (editor.RevealTextRange(match.target.block, match.target.range, ScrollAlignment::Center)) {
        resume_match = std::nullopt;
        active_match = next;
      }
    };
    const bool can_replace = !read_only && options.available && count != 0 && !find_query->composition &&
        !replacement_text->composition && !editor_composing && query != replacement_text->text;
    const auto replace = [editor, results, count, active_match, resume_match, find_query, replacement_text, find,
                          find_results, toast, can_replace](bool all) {
      if (!can_replace || editor.IsReadOnly() || !find->open || !find->replace_open || find_results.Get() != results ||
          find_query->composition || replacement_text->composition ||
          find_query->text != results->query || find_query->text == replacement_text->text ||
          editor.Document() != results->document) {
        return;
      }
      const auto& text = replacement_text->text;
      const auto index = active_match.Get().value_or(0);
      const auto& match = results->matches[index < count ? index : 0];
      std::vector<BlockTextRange> targets;
      if (all) {
        targets.reserve(count);
        for (const auto& item : results->matches) {
          targets.push_back(item.target);
        }
      }
      const auto ranges = all ? std::span<const BlockTextRange>(targets) : std::span(&match.target, 1);
      if (!editor.ReplaceTextRanges(ranges, text)) {
        return;
      }
      active_match = std::nullopt;
      if (all) {
        resume_match = std::nullopt;
        toast.Show(StringVariant::Format(count == 1 ? l10n::replaced_one : l10n::replaced_many, count));
      } else {
        const auto after = match.target.range.start + AttributedText(text).Length();
        resume_match = FindResume{editor.Document(), results->query, match.block_index, after};
      }
    };
    const bool desktop = UseApplication().SupportsMultipleWindows();
    find_bar = FindBar(find_query, find_focus, find, {count, selected, ready, navigate},
        {replacement_text, can_replace, replace}, read_only, [find, focus, desktop] {
          find.Update([](FindPanelState& state) {
            state.open = false;
            state.focused = FindField::None;
          });
          if (desktop) {
            focus.RequestFocus();
          }
        });
  }

  const auto editor_view = RichEditor(editor, {
      .style = FolioEditorStyle(theme, viewport, static_cast<float>(preferences.text_size)),
      .document_directory = options.working_directory,
      .extensions = {.image_resolver = std::move(options.image_resolver),
                     .decorations = std::move(decorations),
                     .custom_blocks = {{std::string(latex_type), FormulaRenderer()}},
                     .inline_objects = {InlineFormulaExtension()}},
      .keymap = settings->Bindings()->editor,
  }).On<RichEditorEvents::LinkActivated>([toast, open_document = options.open_document_link](const LinkRange& link) {
    return OpenLink(link.target, open_document, toast);
  }).On<RichEditorEvents::InlineObjectActivated>([=](const InlineObjectSelection& object) {
    if (!editor.IsReadOnly() && object.data.type == latex_type && object.data.version == 1) {
      dialog.Show(FormulaDialog, editor, FormulaRequest{.inline_math = true, .object = object});
    }
  }).With(focus, Grow(), Enabled(options.available),
          Semantics{.label = l10n::document_editor, .identifier = "article-editor"}).Key("editor");
  const auto paper_frame = compact ? Frame{} : Frame{.width = static_cast<float>(preferences.reading_width)};
  const auto paper_border = Border{theme.colors.outline, compact ? 0.0F : 1.0F};
  const auto paper_corners = CornerRadius(CornerRadii::Top(compact ? 0.0F : theme.shapes.medium));
  const View paper = Column {editor_view}.With(paper_frame, paper_border, paper_corners, ClipChildren(),
      Background(theme.colors.surface), Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  const float margin = viewport == ViewportClass::Expanded ? 24.0F : 12.0F;
  const auto page_insets = compact ? EdgeInsets{} : EdgeInsets{.top = margin, .right = margin, .left = margin};
  View toolbar;
  if (read_only) {
    if (IsMobilePlatform()) {
      if (const auto link = editor.CurrentLink(); link && CanOpenLink(link->target, options.open_document_link)) {
        toolbar = Row {Button(l10n::open_link)
            .OnClick([toast, target = link->target, open_document = options.open_document_link] {
              OpenLink(target, open_document, toast);
            }).With(Enabled(options.available), Frame{.min_height = 44.0F})}
            .With(Padding(EdgeInsets::Symmetric(8.0F, 2.0F)));
      }
    }
  } else {
    toolbar = FormattingBar(editor, focus, options.available, options.can_pick_images,
                            [focus, pick = std::move(options.pick_image)](bool replace, bool inline_image) {
                              pick(focus, replace, inline_image);
                            }, std::move(options.open_document_link));
  }
  return Column {
    toolbar ? std::move(toolbar).Key("formatting") : View{},
    Divider().Key("editor-divider"),
    find_bar ? std::move(find_bar).Key("find-bar") : View{},
    Column {paper}.With(Grow(), Padding(page_insets),
                       CrossAlign(compact ? CrossAxisAlignment::Stretch : CrossAxisAlignment::Center)).Key("paper"),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background));
}

} // namespace folio

View App() {
  const auto& settings = UseService<folio::SettingsStore>()->Get();
  const auto brightness = settings.theme == folio::system_theme ? UseSystemThemeBrightness() : ThemeBrightness::Light;
  const auto dispatch = UseState(folio::WorkbenchKeyHandler{});
  // The application root receives keys even when no child control owns focus.
  return Stack {
    Theme(folio::FolioTheme(folio::ResolveTheme(settings.theme, brightness)), folio::Workbench(dispatch)),
  }.On<ViewEvents::KeyIntercept>([dispatch](const KeyEvent& event) {
    const auto& handler = dispatch.Get();
    return handler && handler(event);
  });
}

const Application application{
    App,
    {
        .window = folio::FolioWindowOptions(),
        .show_debug_overlay = false,
        .application_hooks = {folio::InstallFolio},
        .window_hooks = {folio::InstallWorkbench},
    }
};

#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>
#include <huxerui/richeditor/math.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include "app_resources.h"
#include "attachments.h"
#include "formula.h"

using namespace huxerui;
using namespace huxerui::richeditor;

namespace {

constexpr std::string_view sample_markdown = R"markdown(# Field notes

Collect ideas worth keeping. Try **bold**, *italics*, and `inline code`. 中文输入 and emoji 🙂 work here too. Try inline math $x^2 + y^2 = r^2$ in a sentence.

> Keep the detail that makes the story yours.

$$
\sigma = \sqrt{\frac{1}{n}\sum_{i=1}^{n}(x_i-\mu)^2}
$$

| Topic | Note | Status | Next |
| --- | --- | --- | --- |
| **Talk** | 中文输入 | Draft | Add a quote |
| Source | [Reading list](https://github.com/HuxerUI) | Ready | |

## Outline the draft

3. Capture an idea
   - Save a useful detail
   - Add a note to revisit
4. Refine the draft

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

Built with [HuxerUI](https://github.com/HuxerUI).
)markdown";

RichEditorStyle StudioEditorStyle(const ThemeSpec& theme, bool compact) {
  RichEditorStyle style;
  style.background = theme.colors.surface;
  style.body_font = Font::FromFamily(theme.typography.family, compact ? 16.0F : 17.0F);
  style.code_font = Font::Monospace(14.0F);
  style.heading_font_sizes = std::array{compact ? 30.0F : 36.0F, 22.0F, 20.0F, 18.0F, 17.0F, 16.0F};
  style.code_block_background = theme.colors.surface_container;
  style.content_padding = compact ? EdgeInsets::Symmetric(4.0F, 10.0F) : EdgeInsets::Symmetric(16.0F, 16.0F);
  style.code_block_padding = EdgeInsets::Symmetric(14.0F, 11.0F);
  style.block_spacing = 11.0F;
  style.quote_rule_width = 3.5F;
  style.inline_code_corner_radius = 5.0F;
  style.inline_code_horizontal_padding = 3.0F;
  style.highlight_corner_radius = 4.0F;
  style.code_block_corner_radius = 9.0F;
  return style;
}

std::string IssueMessage(const std::vector<ConversionIssue>& issues) {
  if (issues.empty()) {
    return {};
  }
  std::string result = issues.front().code + " — " + issues.front().message;
  if (issues.size() > 1) {
    result += " (+" + std::to_string(issues.size() - 1) + ")";
  }
  return result;
}

Task<IoResult<std::string>> SaveFolderCopy(FileReference parent, File directory, File staged_file,
                                         RichDocument document, std::string name) {
  auto checked = co_await folio::CheckAttachments(directory, document);
  if (!checked.Succeeded()) {
    co_return IoResult<std::string>::Failure(checked.Error());
  }
  const auto folder_name = folio::UniqueName("Folio-");
  auto destination = co_await parent.CreateDirectoryAsync(folder_name);
  if (!destination.Succeeded()) {
    co_return IoResult<std::string>::Failure(destination.Error());
  }
  auto attachments = co_await folio::CopyAttachments(directory, destination.Value(), std::move(document));
  if (!attachments.Succeeded()) {
    co_return IoResult<std::string>::Failure(attachments.Error());
  }
  auto copied = co_await destination.Value().CopyFileFromAsync(std::move(staged_file), std::move(name));
  if (!copied.Succeeded()) {
    co_return IoResult<std::string>::Failure(copied.Error());
  }
  co_return IoResult<std::string>::Success(folder_name);
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
  const auto field = [&](bool width, const char* label) {
    return TextField(width ? draft->width : draft->height)
        .Label(label)
        .Variant(TextFieldVariant::Outlined)
        .MaxLength(24)
        .InputConfiguration({.type = TextInputType::Decimal, .autocorrect = false})
        .OnChanged([changed, width](const TextEditingValue& value) { changed(value, width); })
        .OnSubmitted(apply)
        .With(Grow());
  };
  const auto action_padding = Padding(EdgeInsets::Symmetric(10.0F, 8.0F));
  return Column {
    Column {
      Text("Image size", TextRole::Title),
      Text("Original proportions are locked.", TextRole::Label).With(Foreground(colors.on_surface_variant)),
    }.With(Spacing(4.0F)),
    Row {
      field(true, "Width (px)"),
      field(false, "Height (px)"),
    }.With(Spacing(10.0F)),
    Text("Adjacent images share a line when they fit.", TextRole::Label)
        .With(Foreground(colors.on_surface_variant)),
    Row {
      Button("Auto size").OnClick([context, editor] {
        editor.Execute(ResizeImage{});
        context.Dismiss();
      }).With(
          Frame{.min_height = 44.0F},
          action_padding,
          Background(Color::Transparent()),
          Foreground(colors.on_surface_variant),
          CornerRadius(theme.shapes.small)
      ),
      Spacer().With(Grow()),
      Button("Cancel").OnClick([context] { context.Dismiss(); }).With(
          Frame{.min_height = 44.0F},
          action_padding,
          Background(colors.surface_container_low),
          Foreground(colors.on_surface),
          Border{.color = colors.surface_container_high},
          CornerRadius(theme.shapes.small)
      ),
      Button("Apply").OnClick(apply).With(
          Frame{.min_height = 44.0F},
          action_padding,
          Enabled(draft->preferred_width.has_value()),
          Background(colors.primary_container),
          Foreground(colors.on_primary_container),
          CornerRadius(theme.shapes.small)
      ),
    }.With(Spacing(6.0F)),
  }.With(
      Frame{.width = 360.0F},
      Padding(18.0F),
      Spacing(14.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.surface),
      Border{.color = colors.surface_container_high},
      CornerRadius(theme.shapes.large)
  );
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
  return Column {
    Column {
      Text("List numbering", TextRole::Title),
      Text("Items count up from the number you enter.", TextRole::Label)
          .With(Foreground(theme.colors.on_surface_variant)),
    }.With(Spacing(4.0F)),
    TextField(value)
        .Label("Start at")
        .Variant(TextFieldVariant::Outlined)
        .MaxLength(std::numeric_limits<unsigned>::digits10 + 1)
        .InputConfiguration({.type = TextInputType::Number, .autocorrect = false})
        .Validation(!proposed && !current.text.empty() && !current.composition
            ? ValidationResult::Invalid("Enter a whole number from 0 to " +
                                        std::to_string(std::numeric_limits<unsigned>::max()))
            : ValidationResult::None())
        .OnChanged([value](const TextEditingValue& next) { value = next; })
        .OnSubmitted(apply),
    Row {
      Spacer().With(Grow()),
      Button("Cancel").OnClick([context] { context.Dismiss(); })
          .With(Background(Color::Transparent()), Foreground(theme.colors.primary)),
      Button("Apply").OnClick(apply).With(Enabled(proposed && *proposed != initial)),
    }.With(Spacing(8.0F)),
  }.With(
      Frame{.width = 340.0F},
      Padding(20.0F),
      Spacing(16.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_high),
      CornerRadius(theme.shapes.extra_large)
  );
}

[[huxerui::composable]]
View TextStylePicker(RichEditorController editor, ViewHandle focus, bool available) {
  struct Choice {
    const char* label;
    const char* short_label;
    SetBlockKind command;
  };
  static const std::array choices{
      Choice{"Paragraph", "Body", {BlockKind::Paragraph}},
      Choice{"Heading 1", "H1", {BlockKind::Heading, 1}},
      Choice{"Heading 2", "H2", {BlockKind::Heading, 2}},
      Choice{"Heading 3", "H3", {BlockKind::Heading, 3}},
      Choice{"Heading 4", "H4", {BlockKind::Heading, 4}},
      Choice{"Heading 5", "H5", {BlockKind::Heading, 5}},
      Choice{"Heading 6", "H6", {BlockKind::Heading, 6}},
      Choice{"Code block", "Code", {BlockKind::CodeBlock}},
  };
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const char* label = "Style";
  const char* description = "Text style";
  for (const auto& choice : choices) {
    const auto state = editor.GetCommandState(choice.command);
    if (state.checked == CheckState::On) {
      label = choice.short_label;
      description = choice.label;
      break;
    }
    if (state.checked == CheckState::Mixed) {
      label = "Mixed";
      description = "Mixed text styles";
    }
  }

  return Button(std::string(label) + " ▾")
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
          Frame{.width = 76.0F, .min_height = 44.0F},
          Padding(EdgeInsets::Symmetric(8.0F, 6.0F)),
          Tooltip("Text style"),
          Enabled(available && editor.GetCommandState(choices.front().command).enabled),
          Background(theme.colors.surface_container_high),
          Foreground(theme.colors.on_surface),
          CornerRadius(theme.shapes.full),
          Semantics{.label = std::string("Text style, ") + description}
      );
}

bool HasLinkAddress(const TextEditingValue& value) {
  return !value.composition && value.text.find_first_not_of(" \t\r\n") != std::string::npos;
}

[[huxerui::composable]]
View LinkDialog(DialogContext context, RichEditorController editor, ViewHandle focus, std::optional<LinkRange> original) {
  const auto& theme = UseTheme();
  auto initial = TextEditingValue::FromText(original ? original->target : std::string{});
  initial.selection.anchor = 0;
  const auto address = UseState(std::move(initial));
  const auto current = address.Get();
  const auto apply = [context, editor, focus, address, title = original ? original->title : std::string{}] {
    const auto value = address.Get();
    if (HasLinkAddress(value) && editor.Execute(SetLink{value.text, title})) {
      context.Dismiss();
      focus.RequestFocus();
    }
  };
  return Column {
    Text(original ? "Edit link" : "Insert link", TextRole::Title),
    TextField(address)
        .Label("Link address")
        .Placeholder("https://example.com")
        .Variant(TextFieldVariant::Outlined)
        .InputConfiguration({.type = TextInputType::Url, .autocorrect = false})
        .OnChanged([address](const TextEditingValue& next) { address = next; })
        .OnSubmitted(apply),
    Row {
      Spacer().With(Grow()),
      Button("Cancel").OnClick([context] { context.Dismiss(); })
          .With(Background(Color::Transparent()), Foreground(theme.colors.primary)),
      Button(original ? "Apply" : "Insert").OnClick(apply)
          .With(Enabled(HasLinkAddress(current) && (!original || current.text != original->target))),
    }.With(Spacing(8.0F)),
  }.With(
      Frame{.width = 360.0F},
      Padding(20.0F),
      Spacing(16.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_high),
      CornerRadius(theme.shapes.extra_large)
  );
}

[[huxerui::composable]]
View LinkTool(RichEditorController editor, ViewHandle focus, bool available) {
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto dialog = UseDialog();
  const auto toast = UseToast();
  const auto clipboard = UseApplication().Clipboard();
  const bool linked = editor.CurrentLink().has_value();
  const char* label = linked ? "Link actions" : "Insert link";

  return IconButton(app::images::link, label)
      .OnClick([=] {
        const auto current = editor.CurrentLink();
        if (!current) {
          dialog.Show(LinkDialog, editor, focus, std::optional<LinkRange>{});
          return;
        }
        menu.Show(anchor, {
            MenuItem("Edit link…", [=] { dialog.Show(LinkDialog, editor, focus, current); }),
            MenuItem("Copy link address", [clipboard, toast, target = current->target] {
              const bool copied = clipboard->WriteText(target);
              toast.Show(copied ? "Link address copied" : "Could not copy the link address");
            }).Enabled(clipboard && clipboard->IsAvailable() && !current->target.empty()),
            MenuItem("Remove link", [editor, focus] {
              if (editor.Execute(SetLink{})) {
                focus.RequestFocus();
              }
            }),
        });
      })
      .With(
          anchor,
          Frame{.min_width = 44.0F, .min_height = 44.0F},
          Tooltip(label),
          Enabled(available && editor.GetCommandState(SetLink{}).enabled),
          Foreground(linked ? theme.colors.on_primary_container : theme.colors.on_surface),
          Background(linked ? theme.colors.primary_container : Color::Transparent()),
          CornerRadius(theme.shapes.full)
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
  return Column {
    Text(editing ? "Edit formula" : request.inline_math ? "Insert inline formula" : "Insert display formula", TextRole::Title),
    TextField(draft)
        .Label("LaTeX source")
        .Variant(TextFieldVariant::Outlined)
        .LineLimits(request.inline_math ? TextFieldLineLimits::SingleLine() : TextFieldLineLimits::MultiLine(2, 5))
        .InputConfiguration({.autocorrect = false})
        .OnChanged([draft](const TextEditingValue& value) { draft = value; }),
    Row {
      Spacer().With(Grow()),
      Button("Cancel").OnClick([context] { context.Dismiss(); }),
      Button(editing ? "Apply" : "Insert").OnClick([=] {
        if (editor.Execute(command)) {
          context.Dismiss();
        }
      }).With(Enabled(enabled), Background(theme.colors.primary_container),
              Foreground(theme.colors.on_primary_container), CornerRadius(theme.shapes.full)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Frame{.width = 400.0F}, Padding(18.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
         Background(theme.colors.surface), CornerRadius(theme.shapes.large));
}

std::vector<TextDecorationRange> FindMatches(std::span<const DecorationBlock> blocks, std::string_view query,
                                            Color color) {
  std::vector<TextDecorationRange> matches;
  const auto length = AttributedText(std::string(query)).Length();
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    const std::string_view text = blocks[index].block.Data().text;
    std::size_t previous = 0;
    TextOffset offset = 0;
    for (auto match = text.find(query); match != text.npos; match = text.find(query, previous)) {
      offset += AttributedText(std::string(text.substr(previous, match - previous))).Length();
      matches.push_back({index, {offset, offset + length}, DecorationKind::Background, color});
      offset += length;
      previous = match + query.size();
    }
  }
  return matches;
}

[[huxerui::composable]]
View FormattingBar(RichEditorController editor, ViewHandle focus, bool available, bool can_pick_images,
                   std::function<void(bool, bool)> pick_image) {
  using namespace app::images;
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto dialog = UseDialog();
  const auto table_anchor = UseViewHandle();
  const auto image_anchor = UseViewHandle();
  const auto list_anchor = UseViewHandle();
  const auto formula_anchor = UseViewHandle();
  const auto more_anchor = UseViewHandle();
  const auto clipboard = UseApplication().Clipboard();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto header_state = editor.GetCommandState(ToggleTableHeader{});
  const bool in_table = header_state.checked.has_value();
  const EditorCommand insert_table = InsertTable{};
  const auto run = [editor, focus](const EditorCommand& command) {
    if (editor.Execute(command)) {
      focus.RequestFocus();
    }
  };
  const auto command_item = [editor, run](const char* label, EditorCommand command) {
    const auto state = editor.GetCommandState(command);
    auto item = MenuItem(label, [run, command = std::move(command)] { run(command); }).Enabled(state.enabled);
    return state.checked ? std::move(item).Checked(state.checked == CheckState::On) : std::move(item);
  };
  const auto tool = [&](const ImageResource& icon, const char* label, EditorCommand command) {
    const auto state = editor.GetCommandState(command);
    Color foreground = theme.colors.on_surface;
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
        .OnClick([run, command = std::move(command)] { run(command); })
        .With(
            Frame{.min_width = 44.0F, .min_height = 44.0F},
            Tooltip(label),
            Enabled(available && state.enabled),
            Foreground(foreground),
            Background(background),
            CornerRadius(theme.shapes.full),
            Semantics{.checked = checked}
        );
  };
  const auto menu_tool = [&](const ImageResource& icon, const char* label, ViewHandle anchor,
                             bool enabled, bool active, auto on_click) {
    return IconButton(icon, label)
        .OnClick(std::move(on_click))
        .With(
            anchor,
            Frame{.min_width = 44.0F, .min_height = 44.0F},
            Tooltip(label),
            Enabled(available && enabled),
            Foreground(active ? theme.colors.on_primary_container : theme.colors.on_surface),
            Background(active ? theme.colors.primary_container : Color::Transparent()),
            CornerRadius(theme.shapes.full)
        );
  };
  const char* table_label = in_table ? "Table actions" : "Insert table";
  const bool image_selected = editor.GetCommandState(DeleteImage{}).enabled;
  const char* image_label = image_selected ? "Image actions" : "Insert image";
  const bool can_insert_table = editor.GetCommandState(insert_table).enabled;
  const bool can_insert_image = can_pick_images &&
      editor.GetCommandState(InsertImage{{.source = "attachments/image.jpg"}}).enabled;
  const auto custom = editor.SelectedCustomBlock();
  const auto inline_object = editor.SelectedInlineObject();
  const bool inline_formula = inline_object && inline_object->data.type == latex_block_type &&
      inline_object->data.version == 1;
  const bool formula_selected = inline_formula || (custom && custom->type == latex_block_type && custom->version == 1);
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
  const char* list_label = in_numbered_list ? "List numbering" : "Numbered list";
  const auto table_entries = [command_item] {
    return std::vector<MenuEntry>{
        command_item("Insert row above", InsertTableRow{TableInsertSide::Before}),
        command_item("Insert row below", InsertTableRow{}),
        command_item("Delete row", DeleteTableRow{}),
        MenuSection{},
        command_item("Insert column before", InsertTableColumn{TableInsertSide::Before}),
        command_item("Insert column after", InsertTableColumn{}),
        command_item("Delete column", DeleteTableColumn{}),
        MenuSection{},
        command_item("Header row", ToggleTableHeader{}),
        command_item("Continue below table", ExitTable{}),
        command_item("Delete table", DeleteTable{}),
    };
  };
  const auto list_entries = [=] {
    return std::vector<MenuEntry>{
        MenuItem("Set start number…", [=] {
          if (const auto current = editor.OrderedListStart()) {
            dialog.Show(ListStartDialog, editor, focus, *current);
          }
        }),
        command_item("Restart at 1", SetListStart{}).Enabled(list_start && *list_start != 1),
        MenuSection{},
        command_item("Increase list indent", IndentList{}),
        command_item("Decrease list indent", OutdentList{}),
        MenuItem("Remove numbering", [run, ordered_list] { run(ordered_list); }),
    };
  };
  const auto open_image_size = [editor, dialog] {
    if (const auto size = editor.SelectedImageSize()) {
      dialog.Show(ImageSizeDialog, editor, *size);
    }
  };
  const auto add_after = MenuItem("Add image after", [pick_image] { pick_image(false, false); }).Enabled(can_pick_images);
  const auto replace_image = [pick_image] { pick_image(true, false); };
  const auto table_tool = menu_tool(table_grid, table_label, table_anchor, header_state.enabled || can_insert_table,
                                    in_table, [=] {
    if (in_table) {
      menu.Show(table_anchor, table_entries());
    } else {
      run(insert_table);
    }
  });
  const auto list_tool = menu_tool(numbered_list, list_label, list_anchor,
                                   ordered_state.enabled && (!in_numbered_list || list_start.has_value()),
                                   in_numbered_list, [=] {
    if (in_numbered_list) {
      menu.Show(list_anchor, list_entries());
    } else {
      run(ordered_list);
    }
  });
  const auto padding = Padding(EdgeInsets::Symmetric(6.0F, 4.0F));
  const auto background = Background(theme.colors.surface_container);
  const auto corners = CornerRadius(theme.shapes.large);

  if (compact) {
    const auto text_tool = [&](const char* title, const char* label, bool enabled, auto on_click) {
      return Button(title).OnClick(std::move(on_click)).With(
          Frame{.min_width = 44.0F, .min_height = 44.0F},
          Padding(EdgeInsets::Symmetric(8.0F, 6.0F)),
          Enabled(available && enabled),
          Background(Color::Transparent()),
          Foreground(theme.colors.on_surface),
          Semantics{.label = label}
      );
    };
    const auto text_command = [&](const char* title, const char* label, EditorCommand command) {
      return text_tool(title, label, editor.GetCommandState(command).enabled,
                       [run, command] { run(command); });
    };
    const ToggleList unordered_list{ListKind::Unordered};
    const bool in_bulleted_list = editor.GetCommandState(unordered_list).checked == CheckState::On;
    const bool in_quote = editor.GetCommandState(ToggleQuote{}).checked == CheckState::On;
    const bool can_link = editor.GetCommandState(SetLink{}).enabled;
    const char* context = "text";
    Views actions;
    if (image_selected) {
      context = "image";
      actions.Add(text_tool("Size", "Image size", selected_size.has_value(), open_image_size));
      actions.Add(text_tool("Replace", "Replace image", can_pick_images, replace_image));
      actions.Add(text_tool("+ Image", "Add image after", can_pick_images, [pick_image] { pick_image(false, false); }));
    } else if (formula_selected) {
      context = "formula";
      actions.Add(text_tool("Edit formula", "Edit formula", true, edit_formula));
      actions.Add(text_tool("Copy LaTeX", "Copy LaTeX", clipboard && clipboard->IsAvailable(), copy_formula));
    } else {
      if (in_table) {
        context = "table";
        actions.Add(table_tool);
        actions.Add(text_command("+ Row", "Insert row below", InsertTableRow{}));
        actions.Add(text_command("+ Column", "Insert column after", InsertTableColumn{}));
        actions.Add(text_command("Below", "Continue below table", ExitTable{}));
      } else if (in_numbered_list || in_bulleted_list) {
        context = in_numbered_list ? "numbered-list" : "bulleted-list";
        actions.Add(in_numbered_list ? list_tool : tool(bulleted_list, "Bulleted list", unordered_list));
        actions.Add(tool(outdent, "Decrease list indent", OutdentList{}));
        actions.Add(tool(indent, "Increase list indent", IndentList{}));
      } else if (in_quote) {
        context = "quote";
        actions.Add(tool(quote, "Block quote", ToggleQuote{}));
      } else if (can_link) {
        context = "link";
        actions.Add(LinkTool(editor, focus, available));
      } else {
        actions.Add(TextStylePicker(editor, focus, available));
      }
      actions.Add(tool(bold, "Bold", ToggleMark{Mark::Bold}));
      actions.Add(tool(italic, "Italic", ToggleMark{Mark::Italic}));
      actions.Add(tool(underline, "Underline", ToggleMark{Mark::Underline}));
      if (can_link && (in_table || in_numbered_list || in_bulleted_list || in_quote)) {
        actions.Add(LinkTool(editor, focus, available));
      }
      if (!in_table && (in_numbered_list || in_bulleted_list || in_quote || can_link)) {
        actions.Add(TextStylePicker(editor, focus, available));
      }
    }

    return Row {
      tool(undo, "Undo", Undo{}),
      // Mount a fresh scroll position when the selection moves to another tool group.
      ScrollView {
        Row {std::move(actions)}.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center))
      }.ScrollAxis(Axis::Horizontal).With(Grow(), ScrollBar()).Key(context),
      menu_tool(more, image_selected ? "Image actions" : "More formatting", more_anchor, true, false, [=] {
        if (image_selected) {
          menu.Show(more_anchor, {command_item("Redo", Redo{}), MenuSection{}, command_item("Delete image", DeleteImage{})});
          return;
        }
        std::vector<MenuEntry> items;
        if (in_table) {
          items.emplace_back(MenuItem("Table actions", table_entries()));
        }
        if (list_start) {
          items.emplace_back(MenuItem("List numbering", list_entries()));
        }
        items.emplace_back(command_item("Redo", Redo{}));
        items.emplace_back(MenuItem("Text formatting", {
            command_item("Strikethrough", ToggleMark{Mark::Strike}),
            command_item("Inline code", ToggleMark{Mark::Code}),
            command_item("Highlight", ToggleMark{Mark::Highlight}),
            command_item("Clear formatting", ClearFormatting{}),
        }));
        items.emplace_back(MenuItem("Insert", {
            command_item("Insert table", insert_table),
            MenuItem("Insert image", [pick_image] { pick_image(false, false); }).Enabled(can_insert_image),
            MenuItem("Insert inline image", [pick_image] { pick_image(false, true); }).Enabled(can_insert_inline_image),
            MenuItem("Insert inline formula…", insert_inline_formula).Enabled(can_insert_inline_formula),
            MenuItem("Insert display formula…", insert_formula).Enabled(can_insert_formula),
        }).Enabled(can_insert_table || can_insert_image || can_insert_inline_image ||
                   can_insert_formula || can_insert_inline_formula));
        items.emplace_back(MenuItem("Lists and quote", {
            command_item("Bulleted list", ToggleList{ListKind::Unordered}),
            command_item("Numbered list", ordered_list),
            command_item("Block quote", ToggleQuote{}),
            command_item("Increase list indent", IndentList{}),
            command_item("Decrease list indent", OutdentList{}),
        }));
        menu.Show(more_anchor, std::move(items));
      }),
    }.With(padding, Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center), background, corners);
  }

  const auto separator = Divider(Axis::Vertical).With(Frame{.height = 22.0F});

  return ScrollView {
    Row {
      tool(undo, "Undo", Undo{}),
      tool(redo, "Redo", Redo{}),
      table_tool,
      menu_tool(image_block, image_label, image_anchor, image_selected || can_insert_image || can_insert_inline_image, image_selected, [=] {
        if (image_selected) {
          menu.Show(image_anchor, {
              MenuItem("Image size…", open_image_size).Enabled(selected_size.has_value()),
              add_after,
              MenuItem("Replace image", replace_image).Enabled(can_pick_images),
              MenuSection{},
              command_item("Delete image", DeleteImage{}),
          });
        } else {
          menu.Show(image_anchor, {
              MenuItem("Insert image block", [pick_image] { pick_image(false, false); }).Enabled(can_insert_image),
              MenuItem("Insert inline image", [pick_image] { pick_image(false, true); }).Enabled(can_insert_inline_image),
          });
        }
      }),
      menu_tool(formula, formula_selected ? "Formula actions" : "Insert formula", formula_anchor,
                formula_selected || can_insert_formula || can_insert_inline_formula, formula_selected, [=] {
        if (formula_selected) {
          menu.Show(formula_anchor, {
              MenuItem("Edit formula…", edit_formula),
              MenuItem("Copy LaTeX", copy_formula).Enabled(clipboard && clipboard->IsAvailable()),
          });
        } else {
          menu.Show(formula_anchor, {
              MenuItem("Inline formula…", insert_inline_formula).Enabled(can_insert_inline_formula),
              MenuItem("Display formula…", insert_formula).Enabled(can_insert_formula),
          });
        }
      }),
      tool(bulleted_list, "Bulleted list", ToggleList{ListKind::Unordered}),
      list_tool,
      separator,
      TextStylePicker(editor, focus, available),
      LinkTool(editor, focus, available),
      tool(bold, "Bold", ToggleMark{Mark::Bold}),
      tool(italic, "Italic", ToggleMark{Mark::Italic}),
      tool(underline, "Underline", ToggleMark{Mark::Underline}),
      tool(strikethrough, "Strikethrough", ToggleMark{Mark::Strike}),
      tool(inline_code, "Inline code", ToggleMark{Mark::Code}),
      tool(highlight, "Highlight", ToggleMark{Mark::Highlight}),
      tool(clear_formatting, "Clear formatting", ClearFormatting{}),
      separator,
      tool(quote, "Block quote", ToggleQuote{}),
      tool(indent, "Increase list indent", IndentList{}),
      tool(outdent, "Decrease list indent", OutdentList{}),
    }.With(padding, Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.ScrollAxis(Axis::Horizontal).With(ScrollBar(), background, corners);
}

[[huxerui::composable]]
View Preview() {
  static const auto imported = MarkdownCodec({.display_math = true, .inline_math = true, .inline_images = true}).Import(sample_markdown);
  const auto editor = UseRichEditor(imported.document.value_or(RichDocument{}));
  const auto focus = UseViewHandle();
  const auto find_focus = UseViewHandle();
  const auto dialog = UseDialog();
  const auto file_menu = UseMenu();
  const auto file_menu_anchor = UseViewHandle();
  const auto tasks = UseTaskScope();
  const auto picker = UseService<FilePicker>();
  const auto status = UseState(imported.document ? std::string("Markdown imported") : IssueMessage(imported.issues));
  const auto busy = UseState(false);
  const auto find_open = UseState(false);
  const auto find_query = UseState(TextEditingValue{});
  const auto find_provider = UseState(std::shared_ptr<const DecorationProvider>{});
  const auto find_count = UseState(std::optional<std::pair<RichDocument, std::size_t>>{});
  const auto focus_after_picker = UseState(false);
  const auto directories = UseApplication().Directories();
  const auto folder = UseState(directories.data_directory);
  const auto directory = folder.Get();
  const auto& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const bool working = busy.Get();
  const bool finding = find_open.Get();
  const bool can_save = picker->CanSaveFiles() || picker->CanOpenDirectories(true);
  Lifecycle([=] {
    if (!working && focus_after_picker.Get()) {
      focus.RequestFocus();
      focus_after_picker = false;
    }
  }, working);
  Lifecycle([=] {
    if (finding) {
      find_focus.RequestFocus();
    }
  }, finding);

  const auto query = find_query->text;
  const auto match_color = theme.colors.tertiary_container;
  Lifecycle([=] {
    find_count = std::nullopt;
    if (!finding || query.empty()) {
      find_provider = nullptr;
      return;
    }
    find_provider = std::make_shared<const DecorationProvider>(DecorationProvider{
        .scope = DecorationScope::Document,
        .analyze = [=](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
          auto matches = co_await RunWorker([query, match_color](std::vector<DecorationBlock> blocks) {
            return FindMatches(blocks, query, match_color);
          }, std::move(request.blocks));
          if (find_open.Get() && find_query->text == query) {
            find_count = std::pair(*request.document, matches.size());
          }
          co_return matches;
        },
    });
  }, finding, query, match_color);
  std::vector<std::shared_ptr<const DecorationProvider>> decoration_providers;
  if (const auto& provider = find_provider.Get()) {
    decoration_providers.push_back(provider);
  }

  const auto pick_image = [=](bool replace, bool inline_image) {
    if (busy.Get()) {
      return;
    }
    busy = true;
    tasks.Launch([=]() -> Task<void> {
      const auto selected = co_await picker->OpenFileAsync({
          .name = "PNG and JPEG images", .extensions = {"png", "jpg", "jpeg"},
          .content_types = {"image/png", "image/jpeg"}});
      if (selected) {
        auto saved = co_await folio::ImportImage(directory, *selected);
        if (saved.Succeeded()) {
          const ImageBlockData image{std::move(saved.Value()), selected->Name(), {}};
          const auto command = replace ? EditorCommand{ReplaceImage{image.source}}
              : inline_image ? EditorCommand{InsertInlineObject{InlineImage(image)}} : EditorCommand{InsertImage{image}};
          status = editor.Execute(command) ? (replace ? "Image replaced" : "Image inserted")
                                           : "Select an image or place the caret in a paragraph";
        } else {
          status = saved.Error().message;
        }
      }
      focus_after_picker = true;
      busy = false;
    });
  };

  const auto import_sample = [=] {
    if (imported.document) {
      editor.ReplaceDocument(*imported.document);
      status = "Markdown imported";
    } else {
      status = IssueMessage(imported.issues);
    }
  };

  const auto save_document = [=](RichDocument snapshot, ExportResult result, SaveFileOptions options) {
    if (busy.Get()) {
      return;
    }
    const auto issues = IssueMessage(result.issues);
    if (!result.text) {
      status = issues;
      return;
    }
    const bool save_folder = folio::HasLocalAttachments(snapshot) || !picker->CanSaveFiles();
    if (save_folder && !picker->CanOpenDirectories(true)) {
      status = "Saving this document requires a writable folder";
      return;
    }

    const auto temporary = directories.temporary_directory;
    const auto staged_file = temporary.Child(folio::UniqueName("folio-") + "-" + options.suggested_name);
    // Prepare the temporary file without suspending so the picker keeps the initiating user activation on Web.
    if (!temporary.CreateDirectories() || !staged_file.WriteString(*result.text)) {
      (void)staged_file.Delete();
      status = "Could not prepare the document";
      return;
    }
    busy = true;
    tasks.Launch([=, snapshot = std::move(snapshot), options = std::move(options)]() -> Task<void> {
      std::string message;
      bool succeeded = false;
      if (save_folder) {
        const auto parent = co_await picker->OpenDirectoryAsync(true);
        if (parent) {
          auto saved = co_await SaveFolderCopy(*parent, directory, staged_file, snapshot, options.suggested_name);
          succeeded = saved.Succeeded();
          message = succeeded ? "Saved " + saved.Value() : saved.Error().message;
        }
      } else {
        succeeded = co_await picker->SaveFileAsync(staged_file, options);
        message = succeeded ? "Document saved" : "Save canceled or unsuccessful";
      }
      (void)co_await staged_file.DeleteAsync();
      if (succeeded && !issues.empty()) {
        message += ". " + issues;
      }
      if (!message.empty()) {
        status = std::move(message);
      }
      busy = false;
    });
  };

  const auto save_json = [=] {
    const auto snapshot = editor.Document();
    save_document(snapshot, JsonCodec().Export(snapshot), {
        .suggested_name = "article.json",
        .filter = {.name = "RichEditor JSON", .extensions = {"json"}, .content_types = {"application/json"}}});
  };

  const auto export_markdown = [=] {
    const auto* codec = &MarkdownCodec({.display_math = true, .inline_math = true, .inline_images = true});
    const auto snapshot = editor.Document();
    auto strict = codec->Export(snapshot);
    const SaveFileOptions options{
        .suggested_name = "article.md",
        .filter = {.name = "Markdown", .extensions = {"md"}, .content_types = {"text/markdown"}}};
    if (strict.text) {
      save_document(snapshot, std::move(strict), options);
      return;
    }
    dialog.Show("Export Markdown with loss?", IssueMessage(strict.issues), "Export with loss", "Cancel", [=] {
      save_document(snapshot, codec->Export(snapshot, LossPolicy::AllowLoss), options);
    });
  };

  const auto new_working_directory = [data = directories.data_directory] {
    return data.Child("documents").Child(folio::UniqueName("document-"));
  };
  const auto finish_open = [=](File working_directory, folio::OpenedDocument opened) {
    folder = std::move(working_directory);
    editor.ReplaceDocument(std::move(opened.document));
    status = opened.missing_images == 0 ? "Document opened"
        : "Opened with " + std::to_string(opened.missing_images) + " missing image(s)";
  };

  const auto open_folder = [=](std::optional<folio::DocumentSource> document) {
    if (busy.Get()) {
      return;
    }
    busy = true;
    tasks.Launch([=, document = std::move(document)]() -> Task<void> {
      const auto selected = co_await picker->OpenDirectoryAsync();
      if (selected) {
        const auto working_directory = new_working_directory();
        auto opened = document ? co_await folio::ReadAttachments(*selected, working_directory, *document)
                               : co_await folio::ReadFolder(*selected, working_directory);
        if (opened.Succeeded()) {
          finish_open(working_directory, std::move(opened.Value()));
        } else {
          status = opened.Error().message;
        }
      }
      busy = false;
    });
  };

  const auto open_file = [=] {
    if (busy.Get()) {
      return;
    }
    busy = true;
    tasks.Launch([=]() -> Task<void> {
      const auto selected = co_await picker->OpenFileAsync({
          .name = "RichEditor documents",
          .extensions = {"json", "md", "markdown"},
          .content_types = {"application/json", "text/markdown", "text/plain"}});
      if (selected) {
        auto imported = co_await folio::ReadDocument(*selected);
        if (!imported.Succeeded()) {
          status = imported.Error().message;
        } else if (!folio::HasLocalAttachments(imported.Value().document)) {
          finish_open(new_working_directory(), {std::move(imported.Value().document)});
        } else if (picker->CanOpenDirectories()) {
          dialog.Show("Locate local images", "Choose the folder containing this document and its local images.",
                      "Choose folder", "Cancel", [=, document = std::move(imported.Value())] {
                        open_folder(document);
                      });
        } else {
          status = "Opening this document's local images requires folder access";
        }
      }
      busy = false;
    });
  };

  const auto& colors = theme.colors;
  View find_bar;
  if (finding) {
    const auto& result = find_count.Get();
    const bool ready = result && result->first == editor.Document();
    const auto count = ready ? result->second : 0;
    const auto match_label = !query.empty() && !ready
        ? std::string("Searching…") : std::to_string(count) + (count == 1 ? " match" : " matches");
    find_bar = Row {
      TextField(find_query).Label("Find in document").Variant(TextFieldVariant::Outlined)
          .OnChanged([find_query](const TextEditingValue& value) { find_query = value; })
          .With(find_focus, Grow()),
      Text(match_label, TextRole::Label)
          .With(Foreground(colors.on_surface_variant)),
      Button("Done").OnClick([=] { find_open = false; }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
  }
  return Column {
    Row {
      Image(app::images::folio)
          .Tint(colors.on_primary_container)
          .With(
              Frame{.width = 38.0F, .height = 38.0F},
              Padding(9.0F),
              Background(colors.primary_container),
              CornerRadius(theme.shapes.small),
              Semantics{.hidden = true}
          ),
      Column {
        Text("Folio", TextRole::Title)
            .Style({Font::System(22.0F).WithWeight(FontWeight::Bold), colors.on_surface})
            .With(Semantics{.role = SemanticRole::Heading, .heading_level = 1}),
        Text("RichEditor studio", TextRole::Label).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
      }.With(Grow(), Spacing(2.0F)),
      Row {
        IconButton(app::images::search, "Find in document")
            .OnClick([=] { find_open = !find_open.Get(); })
            .With(Frame{.min_width = 44.0F, .min_height = 44.0F}, Tooltip("Find in document"), Enabled(!working)),
        IconButton(app::images::save, "Save JSON")
            .OnClick(save_json)
            .With(Frame{.min_width = 44.0F, .min_height = 44.0F}, Tooltip("Save JSON"),
                  Enabled(!working && can_save)),
        IconButton(app::images::more, "File actions")
            .OnClick([=] {
              file_menu.Show(file_menu_anchor, {
                  MenuItem("Open file…", open_file).Enabled(picker->CanOpenFiles()),
                  MenuItem("Open folder…", [=] { open_folder(std::nullopt); }).Enabled(picker->CanOpenDirectories()),
                  MenuItem("Export Markdown…", export_markdown).Enabled(can_save),
                  MenuSection{},
                  MenuItem("Load sample", import_sample),
              });
            })
            .With(file_menu_anchor, Frame{.min_width = 44.0F, .min_height = 44.0F}, Tooltip("File actions"),
                  Enabled(!working)),
      }.With(
          Padding(2.0F),
          CrossAlign(CrossAxisAlignment::Center),
          Background(colors.surface_container),
          CornerRadius(theme.shapes.full)
      ),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    std::move(find_bar),
    FormattingBar(editor, focus, !working, picker->CanOpenFiles(), pick_image),
    RichEditor(editor, {.style = StudioEditorStyle(theme, compact), .document_directory = directory,
                        .extensions = {.decorations = std::move(decoration_providers),
                                       .custom_blocks = {{std::string(latex_block_type), folio::FormulaRenderer()}},
                                       .inline_objects = {folio::InlineFormulaExtension()}}})
        .On<RichEditorEvents::InlineObjectActivated>([=](const InlineObjectSelection& object) {
          if (object.data.type == latex_block_type && object.data.version == 1) {
            dialog.Show(FormulaDialog, editor, FormulaRequest{.inline_math = true, .object = object});
          }
        })
        .With(focus, Grow(), Enabled(!working), Semantics{.label = "Article", .identifier = "article-editor"}),
    Text(working ? "Working…" : status.Get(), TextRole::Label)
        .Align(TextAlign::Center)
        .With(FontSize(12.0F), Foreground(colors.on_surface_variant),
              Semantics{.identifier = "preview-status", .busy = working, .live_region = SemanticLiveRegion::Polite}),
  }.With(
      Padding(EdgeInsets::Symmetric(compact ? 12.0F : 24.0F, compact ? 8.0F : 12.0F)),
      Spacing(8.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      SafeAreaPadding(),
      Background(colors.surface)
  );
}

} // namespace

View App() {
  const auto theme = UseSystemThemeBrightness() == ThemeBrightness::Dark
      ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  return MaterialTheme(theme, Preview());
}

const Application application{
    App,
    {
        .window = {.title = "Lib-RichEditor Preview", .initial_size = {920.0F, 720.0F},
                   .minimum_size = Size{320.0F, 360.0F}},
        .show_debug_overlay = false,
    }
};

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
#include "formula.h"
#include "workbench.h"

using namespace huxerui;
using namespace huxerui::richeditor;
using folio::DialogPanel;
using folio::LatexBlock;
using folio::LatexInline;
using folio::PrimaryAction;
using folio::latex_type;

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

RichEditorStyle FolioEditorStyle(const ThemeSpec& theme, bool compact) {
  RichEditorStyle style;
  style.background = theme.colors.surface;
  style.body_font = Font::FromFamily(theme.typography.family, theme.typography.body_large);
  style.code_font = Font::Monospace(14.0F);
  style.heading_font_sizes = std::array{compact ? 27.0F : 33.0F, 22.0F, 20.0F, 18.0F, 17.0F, 16.0F};
  style.quote_rule_color = theme.colors.primary;
  style.highlight_background = theme.colors.primary_container;
  style.code_block_background = theme.colors.surface_container_highest;
  style.content_padding = compact ? EdgeInsets::Symmetric(22.0F, 20.0F) : EdgeInsets::Symmetric(44.0F, 32.0F);
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
  return DialogPanel(Column {
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
      }).With(Background(Color::Transparent()), Foreground(colors.on_surface_variant)),
      Spacer().With(Grow()),
      Button("Cancel").OnClick([context] { context.Dismiss(); }),
      PrimaryAction(Button("Apply").OnClick(apply).With(Enabled(draft->preferred_width.has_value())), theme),
    }.With(Spacing(6.0F)),
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
      Button("Cancel").OnClick([context] { context.Dismiss(); }),
      PrimaryAction(Button("Apply").OnClick(apply).With(Enabled(proposed && *proposed != initial)), theme),
    }.With(Spacing(8.0F)),
  }, theme, 340.0F);
}

[[huxerui::composable]]
View TextStylePicker(RichEditorController editor, ViewHandle focus, bool available, bool compact) {
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
      label = compact ? choice.short_label : choice.label;
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
          Frame{.width = compact ? 76.0F : 98.0F, .min_height = compact ? 44.0F : 30.0F},
          Padding(EdgeInsets::Symmetric(6.0F, 4.0F)),
          Tooltip("Text style"),
          Enabled(available && editor.GetCommandState(choices.front().command).enabled),
          Background(Color::Transparent()),
          Foreground(theme.colors.on_surface),
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
  return DialogPanel(Column {
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
      Button("Cancel").OnClick([context] { context.Dismiss(); }),
      PrimaryAction(Button(original ? "Apply" : "Insert").OnClick(apply)
          .With(Enabled(HasLinkAddress(current) && (!original || current.text != original->target))), theme),
    }.With(Spacing(8.0F)),
  }, theme, 360.0F);
}

[[huxerui::composable]]
View LinkTool(RichEditorController editor, ViewHandle focus, bool available, bool compact) {
  const auto& theme = UseTheme();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto dialog = UseDialog();
  const auto toast = UseToast();
  const auto clipboard = UseApplication().Clipboard();
  const float size = compact ? 44.0F : 30.0F;
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
      PrimaryAction(Button(editing ? "Apply" : "Insert").OnClick([=] {
        if (editor.Execute(command)) {
          context.Dismiss();
        }
      }).With(Enabled(enabled)), theme),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
  }, theme, 400.0F);
}

struct FindMatch {
  std::size_t block_index;
  Block block;
  TextRange range;
};

struct FindResults {
  RichDocument document;
  std::string query;
  std::vector<FindMatch> matches;
};

std::vector<FindMatch> FindMatches(std::span<const DecorationBlock> blocks, std::string_view query) {
  std::vector<FindMatch> matches;
  const auto length = AttributedText(std::string(query)).Length();
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    const std::string_view text = blocks[index].block.Data().text;
    std::size_t previous = 0;
    TextOffset offset = 0;
    for (auto match = text.find(query); match != text.npos; match = text.find(query, previous)) {
      offset += AttributedText(std::string(text.substr(previous, match - previous))).Length();
      matches.push_back({index, blocks[index].block, {offset, offset + length}});
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
  const auto application = UseApplication();
  const auto clipboard = application.Clipboard();
  const bool compact = !application.SupportsMultipleWindows() && UseViewportClass() == ViewportClass::Compact;
  const float tool_size = compact ? 44.0F : 30.0F;
  const auto tool_frame = Frame{.min_width = tool_size, .min_height = tool_size};
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
        .OnClick([run, command = std::move(command)] { run(command); })
        .With(
            tool_frame,
            Tooltip(label),
            Enabled(available && state.enabled),
            Foreground(foreground),
            Background(background),
            Semantics{.checked = checked}
        );
  };
  const auto menu_tool = [&](const ImageResource& icon, const char* label, ViewHandle anchor,
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
  const char* table_label = in_table ? "Table actions" : "Insert table";
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
  const auto padding = Padding(EdgeInsets::Symmetric(compact ? 5.0F : 12.0F, 5.0F));
  const auto background = Background(theme.colors.surface);
  const auto more_tool = menu_tool(more, compact && image_selected ? "Image actions" : "More formatting",
                                  more_anchor, true, false, [=] {
    if (compact && image_selected) {
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
    if (compact) {
      items.emplace_back(command_item("Redo", Redo{}));
    }
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
  });

  if (compact) {
    const auto text_tool = [&](const char* title, const char* label, bool enabled, auto on_click) {
      return Button(title).OnClick(std::move(on_click)).With(
          tool_frame,
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
        actions.Add(LinkTool(editor, focus, available, compact));
      } else {
        actions.Add(TextStylePicker(editor, focus, available, compact));
      }
      actions.Add(tool(bold, "Bold", ToggleMark{Mark::Bold}));
      actions.Add(tool(italic, "Italic", ToggleMark{Mark::Italic}));
      actions.Add(tool(underline, "Underline", ToggleMark{Mark::Underline}));
      if (can_link && (in_table || in_numbered_list || in_bulleted_list || in_quote)) {
        actions.Add(LinkTool(editor, focus, available, compact));
      }
      if (!in_table && (in_numbered_list || in_bulleted_list || in_quote || can_link)) {
        actions.Add(TextStylePicker(editor, focus, available, compact));
      }
    }

    return Row {
      tool(undo, "Undo", Undo{}),
      // Mount a fresh scroll position when the selection moves to another tool group.
      ScrollView {
        Row {std::move(actions)}.With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Center))
      }.ScrollAxis(Axis::Horizontal).With(Grow(), ScrollBar()).Key(context),
      more_tool,
    }.With(padding, Spacing(2.0F), CrossAlign(CrossAxisAlignment::Center), background);
  }

  const auto image_tool = menu_tool(image_block, image_selected ? "Image actions" : "Insert image", image_anchor,
      image_selected || can_insert_image || can_insert_inline_image, image_selected, [=] {
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
  });
  const auto formula_tool = menu_tool(formula, formula_selected ? "Formula actions" : "Insert formula", formula_anchor,
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
  });

  const auto separator = Divider(Axis::Vertical).With(Frame{.height = 21.0F});
  return ScrollView {
    Row {
      tool(undo, "Undo", Undo{}),
      tool(redo, "Redo", Redo{}),
      separator,
      TextStylePicker(editor, focus, available, compact),
      tool(bold, "Bold", ToggleMark{Mark::Bold}),
      tool(italic, "Italic", ToggleMark{Mark::Italic}),
      tool(underline, "Underline", ToggleMark{Mark::Underline}),
      LinkTool(editor, focus, available, compact),
      tool(strikethrough, "Strikethrough", ToggleMark{Mark::Strike}),
      tool(inline_code, "Inline code", ToggleMark{Mark::Code}),
      tool(highlight, "Highlight", ToggleMark{Mark::Highlight}),
      tool(clear_formatting, "Clear formatting", ClearFormatting{}),
      separator,
      tool(bulleted_list, "Bulleted list", ToggleList{ListKind::Unordered}),
      list_tool,
      tool(quote, "Block quote", ToggleQuote{}),
      tool(indent, "Increase list indent", IndentList{}),
      tool(outdent, "Decrease list indent", OutdentList{}),
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
  const auto focus = UseViewHandle();
  const auto find_focus = UseViewHandle();
  const auto dialog = UseDialog();
  const auto find_query = UseState(TextEditingValue{});
  const auto find_provider = UseState(std::shared_ptr<const DecorationProvider>{});
  const auto active_provider = UseState(std::shared_ptr<const DecorationProvider>{});
  const auto find_results = UseState(std::shared_ptr<const FindResults>{});
  const auto active_match = UseState(std::optional<std::size_t>{});
  const auto& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto find_open = options.find_open;
  const bool search_open = find_open.Get();
  const bool finding = options.active && search_open;
  Lifecycle([=] {
    if (finding) {
      find_focus.RequestFocus();
    }
  }, finding);

  const auto query = find_query->text;
  const auto match_color = theme.colors.tertiary_container;
  Lifecycle([=] {
    if (!search_open || query.empty()) {
      find_provider = nullptr;
      find_results = nullptr;
      active_match = std::nullopt;
      return;
    }
    find_provider = std::make_shared<const DecorationProvider>(DecorationProvider{
        .scope = DecorationScope::Document,
        .analyze = [=](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
          auto matches = co_await RunWorker([query](std::vector<DecorationBlock> blocks) {
            return FindMatches(blocks, query);
          }, std::move(request.blocks));
          if (!find_open.Get() || find_query->text != query || editor.Document() != *request.document) {
            co_return std::vector<TextDecorationRange>{};
          }
          std::vector<TextDecorationRange> marks;
          marks.reserve(matches.size());
          for (const auto& match : matches) {
            marks.push_back({match.block_index, match.range, DecorationKind::Background, match_color});
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
    if (!selected || *selected >= count) {
      active_provider = nullptr;
      return;
    }
    const auto& match = results->matches[*selected];
    const auto mark = TextDecorationRange{match.block_index, match.range, DecorationKind::Outline, active_color};
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
    const bool can_navigate = count != 0 && !find_query->composition;
    const auto navigate = [editor, results, count, active_match, find_focus, find_query](bool forward) {
      if (count == 0 || find_query->composition || find_query->text != results->query ||
          editor.Document() != results->document) {
        return;
      }
      const auto index = active_match.Get();
      const auto next = index && *index < count
          ? (forward ? (*index + 1) % count : (*index + count - 1) % count)
          : (forward ? 0 : count - 1);
      const auto& match = results->matches[next];
      if (editor.RevealTextRange(match.block, match.range)) {
        active_match = next;
      }
      find_focus.RequestFocus();
    };
    auto match_label = !query.empty() && !ready ? std::string("Searching…") : std::to_string(count) + " matches";
    if (selected && *selected < count) {
      match_label = std::to_string(*selected + 1) + " / " + std::to_string(count);
    }
    const auto button_frame = Frame{.min_width = compact ? 44.0F : 32.0F, .min_height = compact ? 44.0F : 32.0F};
    find_bar = Row {
      TextField(find_query).Label(compact ? "Find" : "Find in document").Variant(TextFieldVariant::Outlined)
          .OnChanged([find_query](const TextEditingValue& value) { find_query = value; })
          .OnSubmitted([navigate] { navigate(true); })
          .On<ViewEvents::KeyIntercept>([navigate, find_query](const KeyEvent& event) {
            if (event.type != KeyEventType::Down || event.key != Key::Enter || !event.modifiers.shift ||
                event.modifiers.control || event.modifiers.alt || event.modifiers.meta || find_query->composition) {
              return false;
            }
            navigate(false);
            return true;
          })
          .With(find_focus, Grow()),
      Text(match_label, TextRole::Label).With(Foreground(theme.colors.on_surface_variant),
          Semantics{.live_region = SemanticLiveRegion::Polite}),
      IconButton(app::images::chevron_up, "Previous match").OnClick([navigate] { navigate(false); })
          .With(button_frame, Enabled(can_navigate), Tooltip("Previous match")),
      IconButton(app::images::chevron_down, "Next match").OnClick([navigate] { navigate(true); })
          .With(button_frame, Enabled(can_navigate), Tooltip("Next match")),
      IconButton(app::images::close, "Close find").OnClick([find_open] { find_open = false; })
          .With(button_frame, Tooltip("Close find")),
    }.With(Padding(EdgeInsets::Symmetric(compact ? 8.0F : 14.0F, 7.0F)), Spacing(compact ? 3.0F : 6.0F),
           CrossAlign(CrossAxisAlignment::Center), Background(theme.colors.surface_container));
  }

  const auto editor_view = RichEditor(editor, {
      .style = FolioEditorStyle(theme, compact),
      .document_directory = options.working_directory,
      .extensions = {.image_resolver = std::move(options.image_resolver),
                     .decorations = std::move(decorations),
                     .custom_blocks = {{std::string(latex_type), FormulaRenderer()}},
                     .inline_objects = {InlineFormulaExtension()}},
  }).On<RichEditorEvents::InlineObjectActivated>([=](const InlineObjectSelection& object) {
    if (object.data.type == latex_type && object.data.version == 1) {
      dialog.Show(FormulaDialog, editor, FormulaRequest{.inline_math = true, .object = object});
    }
  }).With(focus, Grow(), Enabled(options.available),
          Semantics{.label = "Document editor", .identifier = "article-editor"}).Key("editor");
  const auto paper_frame = compact ? Frame{} : Frame{.width = 684.0F};
  const auto paper_border = Border{theme.colors.outline, compact ? 0.0F : 1.0F};
  const auto paper_corners = CornerRadius(CornerRadii::Top(compact ? 0.0F : theme.shapes.medium));
  const View paper = Column {editor_view}.With(paper_frame, paper_border, paper_corners, ClipChildren(),
      Background(theme.colors.surface), Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  return Column {
    Column {
      FormattingBar(editor, focus, options.available, options.can_pick_images,
                    [focus, pick = std::move(options.pick_image)](bool replace, bool inline_image) {
                      pick(focus, replace, inline_image);
                    }),
      Divider(),
    }.With(CrossAlign(CrossAxisAlignment::Stretch)).Key("formatting"),
    find_bar ? std::move(find_bar).Key("find-bar") : View{},
    Column {paper}.With(Grow(), Padding(compact ? EdgeInsets{} : EdgeInsets{.top = 20.0F, .right = 24.0F, .left = 24.0F}),
                       CrossAlign(compact ? CrossAxisAlignment::Stretch : CrossAxisAlignment::Center)).Key("paper"),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background));
}

} // namespace folio

View App() {
  return Theme(folio::FolioTheme(UseSystemThemeBrightness()), folio::Workbench());
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

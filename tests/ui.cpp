#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>

#include "../examples/preview/src/formula.h"
#include "../src/editor_session.h"

using namespace huxerui;
using namespace huxerui::richeditor;
using namespace huxerui::testing;
using folio::LatexBlock;
using folio::LatexInline;
using folio::latex_type;

namespace {

std::optional<RichEditorController> controller;
std::optional<State<RichEditorStyle>> appearance;
std::optional<State<std::shared_ptr<const EditorKeymap>>> shortcut_keymap;
std::shared_ptr<const ImageResolver> initial_image_resolver;
std::optional<State<std::shared_ptr<const ImageResolver>>> image_resolver_state;
std::optional<State<std::optional<File>>> image_directory_state;

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void CheckDocumentIndex(const RichDocument& document) {
  using namespace huxerui::richeditor::detail;
  const auto& index = DocumentAccess::Get(document).index;
  auto cursor = index.Leaves();
  BlockPath path;
  TextOffset start = 0;
  std::size_t ordinal = 0;
  const auto visit = [&](const auto& self, std::span<const Block> blocks) -> void {
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      const auto& block = blocks[i];
      path.push_back(i);
      if (block.Data().children.empty()) {
        const auto id = DocumentAccess::Id(block);
        const auto leaf = index.At(ordinal);
        const auto& body = DocumentAccess::Body(block);
        Check(leaf.path == path && leaf.start == start && DocumentAccess::Id(leaf.block) == id &&
                  index.IndexOf(id) == ordinal && index.StartOf(id) == start,
              "Random index lookup agrees with the document tree");
        Check(cursor && cursor->path == path && cursor->start == start && DocumentAccess::Id(cursor->block) == id,
              "Sequential cursor agrees with the document tree");
        Check(PositionAt(document, start).block == id &&
                  PositionAt(document, start + body.Length()).offset == body.Length(),
              "UTF-16 start, end, and separator resolve to the expected block");
        start += body.Length() + 1;
        ++ordinal;
        cursor.Next();
      } else {
        self(self, block.Data().children);
      }
      path.pop_back();
    }
  };
  visit(visit, document.Blocks());
  Check(!cursor && index.LeafCount() == ordinal && index.Length() == start - 1, "Index totals agree with the tree");
}

void CheckIncrementalDocumentIndex() {
  using namespace huxerui::richeditor::detail;
  std::vector<Block> paragraphs;
  for (unsigned i = 0; i < 195; ++i) {
    paragraphs.emplace_back(BlockData{.text = "A🙂 " + std::to_string(i)});
  }
  const auto initial = RichDocument::Create(std::move(paragraphs));
  const auto& index = DocumentAccess::Get(initial).index;
  CheckDocumentIndex(initial);

  for (const auto ordinal : {0u, 63u, 64u, 98u, 194u}) {
    EditorSession editor(initial);
    const auto original = index.At(ordinal);
    const auto start = original.start;
    const TextInputCommand split{.kind = TextInputCommandKind::CommitText,
                                 .target = TextRange{start + 3, start + 3}, .text = "\n"};
    Check(editor.Apply(std::span(&split, 1)), "Split a paragraph near a page boundary");
    const auto inserted = editor.Document();
    const auto& new_index = DocumentAccess::Get(inserted).index;
    const auto id = DocumentAccess::Id(new_index.At(ordinal + 1).block);
    Check(new_index.LeafCount() == index.LeafCount() + 1 && new_index.StartOf(id) == start + 4 &&
              !index.IndexOf(id),
          "A newly allocated ID is visible in the edited snapshot");
    CheckDocumentIndex(inserted);

    const TextInputCommand join{.kind = TextInputCommandKind::CommitText,
                                .target = TextRange{start + 3, start + 4}};
    Check(editor.Apply(std::span(&join, 1)) && editor.Document() == initial &&
              !DocumentAccess::Get(editor.Document()).index.IndexOf(id),
          "Joining paragraphs removes the deleted ID from the new snapshot");
    CheckDocumentIndex(editor.Document());
    Check(editor.Execute(Undo{}) && DocumentAccess::Same(editor.Document(), inserted) &&
              editor.Execute(Redo{}) && editor.Document() == initial && new_index.IndexOf(id) == ordinal + 1,
          "Undo and redo switch snapshots and keep retained indexes valid");
  }

  EditorSession editor(initial);
  const auto start = index.At(64).start;
  const TextInputCommand preedit{.kind = TextInputCommandKind::UpdateComposition,
                                  .target = TextRange{start + 1, start + 3}, .text = "zhong"};
  Check(editor.Apply(std::span(&preedit, 1)) && DocumentAccess::Same(editor.Document(), initial) &&
            editor.CompositionRange() == TextRange{start + 1, start + 6},
        "Preedit uses the visible index and preserves the committed snapshot");
  CheckDocumentIndex(editor.VisibleDocument());
  const TextInputCommand commit{.kind = TextInputCommandKind::CommitText, .text = "中文"};
  Check(editor.Apply(std::span(&commit, 1)) && editor.SelectionOffsets() == TextSelection{start + 3, start + 3},
        "IME commits at the correct UTF-16 offset after replacing a surrogate pair");
  CheckDocumentIndex(editor.Document());
  const TextInputCommand invalid{.kind = TextInputCommandKind::SetSelection,
                                 .selection_after = TextSelection{index.At(65).start + 2, index.At(65).start + 2}};
  Check(!editor.Apply(std::span(&invalid, 1)), "IME rejects a selection that splits a surrogate pair");
  const auto tail_id = DocumentAccess::Id(index.At(194).block);
  const auto removed_id = DocumentAccess::Id(index.At(70).block);
  const TextInputCommand delete_pages{.kind = TextInputCommandKind::CommitText,
                                      .target = TextRange{3, index.At(150).start}};
  Check(editor.Apply(std::span(&delete_pages, 1)), "Delete a selection covering multiple pages");
  const auto& shortened = DocumentAccess::Get(editor.VisibleDocument()).index;
  Check(shortened.LeafCount() == 45 && shortened.IndexOf(tail_id) == 44 && !shortened.IndexOf(removed_id),
        "The following pages shift and the erased IDs disappear");
  CheckDocumentIndex(editor.Document());
  Check(editor.Execute(Undo{}) && DocumentAccess::Get(editor.Document()).index.IndexOf(removed_id) == 70,
        "Undo restores the erased pages and their IDs");

  const auto nested = *MarkdownCodec().Import("- First\n  - Inner\n- Second 🙂\n- Third\n").document;
  EditorSession list(nested);
  const auto& nested_index = DocumentAccess::Get(nested).index;
  const auto moving = nested_index.At(2);
  const auto id = DocumentAccess::Id(moving.block);
  Check(list.Select({moving.start, moving.start}) && list.Execute(IndentList{}),
        "Indent moves an existing item into a different container");
  const auto indented = list.Document();
  const auto& indented_index = DocumentAccess::Get(indented).index;
  Check(indented_index.StartOf(id) == moving.start &&
            indented_index.At(*indented_index.IndexOf(id)).path != moving.path,
        "Moving across containers preserves the block identity and updates the path");
  CheckDocumentIndex(indented);
  Check(list.Execute(Undo{}) && DocumentAccess::Same(list.Document(), nested) &&
            list.Execute(Redo{}) && DocumentAccess::Same(list.Document(), indented),
        "Undo and redo restore both versions of a moved subtree");
  Check(list.Execute(OutdentList{}) && list.Document() == nested, "Outdent restores the tree and coordinates");
  CheckDocumentIndex(list.Document());

  const auto quoted = *MarkdownCodec().Import("> First\n>\n> Move 🙂\n\nMiddle\n\n> Last\n").document;
  const auto& quoted_index = DocumentAccess::Get(quoted).index;
  const auto moved_id = DocumentAccess::Id(quoted_index.At(1).block);
  std::vector<Block> roots(quoted.Blocks().begin(), quoted.Blocks().end());
  auto source = roots.front().Data();
  auto target = roots.back().Data();
  target.children.push_back(source.children.back());
  source.children.pop_back();
  roots.front() = DocumentAccess::WithData(roots.front(), std::move(source));
  roots.back() = DocumentAccess::WithData(roots.back(), std::move(target));
  EditChanges change{DocumentAccess::Get(quoted).max_id};
  const auto reordered = change.Build(quoted, std::move(roots), {{0, {0, 3, 3}}});
  const auto& reordered_index = DocumentAccess::Get(reordered).index;
  Check(reordered_index.IndexOf(moved_id) == 3 && reordered_index.StartOf(moved_id) > quoted_index.StartOf(moved_id),
        "A cross-container move updates the ordinal and offset when the flattened order changes");
  CheckDocumentIndex(reordered);
}

void IncrementalDocumentIndex() {
  const Application app([]() -> View { return Text(""); },
                        {.show_debug_overlay = false, .application_hooks = {[](ApplicationContext&) {
                           CheckIncrementalDocumentIndex();
                         }}});
  UiTest ui(app);
}

View App() {
  auto initial = MarkdownCodec().Import("A🙂\n\nSecond **line**\n\n> Third\n");
  const auto editor = UseRichEditor(*initial.document);
  const auto focus = UseViewHandle();
  controller = editor;
  return Column {
    Button("Focus").OnClick([focus] { Check(focus.RequestFocus(), "ViewHandle request accepted"); }),
    Button("Bold").OnClick([editor, focus] {
      editor.Execute(ToggleMark{Mark::Bold});
      focus.RequestFocus();
    }),
    RichEditor(editor).With(focus, Grow(), Semantics{.label = "Article", .identifier = "editor"}).Key("editor"),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

TextInputApplyResult Send(UiTest& ui, std::initializer_list<TextInputCommand> commands) {
  const auto active = ui.ActiveTextInput();
  Check(active.has_value(), "Editor has an active text input session");
  return ui.SendTextInput({active->session_id, commands});
}

void Accept(UiTest& ui, std::initializer_list<TextInputCommand> commands) {
  Check(Send(ui, commands).result_code == TextInputResultCode::Ok, "Accepted editor input");
  ui.Pump();
}

void Workflow() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {460, 330}});
  ui.Find(UiSelector::Text("Focus")).Tap();
  const auto initial = controller->Document();
  const auto first_session = ui.ActiveTextInput();
  Check(first_session.has_value(), "ViewHandle focuses the RichEditor surface");
  Check(ui.FindSemantics(UiSemanticSelector::Identifier("editor")).Exists(), "Editable semantic node");

  ui.PressKey(Key::ArrowDown);
  Check(ui.ActiveTextInput()->selection.active == 4, "Vertical caret motion uses the next paragraph layout");
  ui.PressKey(Key::ArrowUp);
  Check(ui.ActiveTextInput()->selection.active == 0, "Vertical caret motion returns to the first paragraph");

  Check(Send(ui, {
      {.kind = TextInputCommandKind::CommitText, .text = "x"},
      {.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{3, 3}},
  }).result_code == TextInputResultCode::Rejected, "Reject a batch containing a split surrogate");
  Check(controller->Document() == initial && !controller->GetCommandState(Undo{}).enabled, "Rejected batch is atomic");

  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{7, 1}}});
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  Check(controller->Document() == initial, "Preedit is absent from committed document");
  Check(ui.ActiveTextInput()->composition == TextRange{1, 3}, "Composition range over cross-block selection");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});
  Check(controller->Document() == initial && ui.ActiveTextInput()->selection == TextSelection{7, 1}, "Cancel restores selection and structure");

  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{3, 3}},
              {.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"}});
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"}});
  Check(controller->Document().Blocks()[0].Data().text == "A🙂你好", "Chinese composition committed once");
  Check(controller->Execute(Undo{}) && controller->Document() == initial, "One undo reverts the entire IME commit");
  Check(controller->Execute(Redo{}), "Redo composition");
  ui.Pump();

  const TextSelection across_paragraphs{15, 1};
  const EditorCommand bold = ToggleMark{Mark::Bold};
  const EditorCommand underline = ToggleMark{Mark::Underline};
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = across_paragraphs}});
  Check(controller->GetCommandState(bold).checked == CheckState::Mixed, "Partially bold selection reports Mixed");
  Check(controller->GetCommandState(underline).checked == CheckState::Off, "Unformatted selection reports Off");
  Check(controller->Execute(underline), "Format backward selection across blocks");
  Check(controller->GetCommandState(underline).checked == CheckState::On, "Underlined selection reports On");
  const auto undo = controller->GetCommandState(Undo{});
  Check(undo.enabled && !undo.checked, "An ordinary command reports availability without a check state");
  ui.Pump();
  const auto underlined = controller->Document();
  ui.Find(UiSelector::Text("Bold")).Tap();
  Check(controller->Document() != underlined, "Toolbar uses the existing selection");
  Check(controller->Execute(Undo{}) && controller->Document() == underlined, "Undo only the toolbar command");
  ui.Pump();
  Check(ui.ActiveTextInput()->selection == across_paragraphs, "Undo restores directed selection");

  const auto saved = JsonCodec().Export(controller->Document());
  Check(saved.text && saved.issues.empty(), "The edited document is saved as JSON without loss");
  const auto reopened = JsonCodec().Import(*saved.text);
  Check(reopened.document == underlined && reopened.issues.empty(),
        "Reopening JSON preserves edited styles and structure");
  const auto session_before_open = ui.ActiveTextInput()->session_id;
  controller->ReplaceDocument(*reopened.document);
  ui.Pump();
  Check(!controller->GetCommandState(Undo{}).enabled, "New document clears history");
  Check(ui.ActiveTextInput() && ui.ActiveTextInput()->session_id != session_before_open, "New document rotates input session");
  Check(ui.SendTextInput({session_before_open, {{.kind = TextInputCommandKind::CommitText, .text = "late"}}})
            .result_code == TextInputResultCode::SessionMismatch,
        "Old input session cannot edit a replacement document");

  const auto& codec = MarkdownCodec();
  const auto strict = codec.Export(controller->Document());
  Check(!strict.text && !strict.issues.empty() && strict.issues[0].code == "markdown.underline",
        "Strict export rejects underline");
  const auto lossy = codec.Export(controller->Document(), LossPolicy::AllowLoss);
  Check(lossy.text && !lossy.issues.empty(), "Lossy export requires opt-in");
  const auto markdown = codec.Import(*lossy.text);
  Check(markdown.document && markdown.document->Blocks()[0].Data().text == "A🙂你好" &&
            markdown.document->Blocks()[2].Data().kind == BlockKind::Quote,
        "Markdown retains committed Chinese text and structure");
}

void HeadingAndLinkEditing() {
  const LinkRange guide{{4, 9}, "https://example.com/guide", "Reference"};
  const auto initial = RichDocument::Create({
      Block(BlockData{.kind = BlockKind::Heading, .text = "A🙂 guide", .marks = {{{4, 9}, Mark::Bold}},
                      .links = {guide}, .heading_level = 1}),
      Block(BlockData{.text = "More"}),
  });
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  controller->ReplaceDocument(initial);
  ui.Pump();
  ui.Find(UiSelector::Text("Focus")).Tap();
  const auto select = [&](TextSelection selection) {
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = selection}});
  };

  const SetBlockKind h1{BlockKind::Heading, 1};
  const SetBlockKind h6{BlockKind::Heading, 6};
  const SetBlockKind paragraph{BlockKind::Paragraph};
  Check(controller->GetCommandState(h1).checked == CheckState::On &&
            controller->GetCommandState(h6).checked == CheckState::Off, "Heading status includes the level");
  select({14, 5});
  Check(controller->GetCommandState(h1).checked == CheckState::Mixed &&
            controller->GetCommandState(paragraph).checked == CheckState::Mixed,
        "A selection over a heading and paragraph reports mixed styles");
  Check(controller->Execute(h6) && controller->GetCommandState(h6).checked == CheckState::On &&
            controller->Document().Blocks()[0].Data().links == std::vector{guide} &&
            controller->Execute(Undo{}) && controller->Document() == initial,
        "Choosing a heading level preserves inline content and can be undone");

  select({6, 6});
  Check(controller->CurrentLink() == guide && controller->GetCommandState(SetLink{}).enabled,
        "A caret inside the link exposes its address and can edit it");
  auto edited = guide;
  edited.target = "https://example.com/reference";
  Check(controller->Execute(SetLink{edited.target, edited.title}) &&
            controller->CurrentLink() == edited && controller->Document().Blocks()[0].Data().marks ==
                initial.Blocks()[0].Data().marks && controller->Document().Blocks()[0].Data().text == "A🙂 guide",
        "Editing from the caret changes the full link and preserves the text and marks");
  ui.Pump();
  Check(ui.ActiveTextInput()->selection == TextSelection{6, 6} && controller->Execute(Undo{}) &&
            controller->Document() == initial, "Undo restores the link without moving the caret");
  Check(controller->Execute(SetLink{}) && !controller->CurrentLink() && controller->Execute(Undo{}),
        "Removing a link from its caret is reversible");

  select({9, 9});
  Check(!controller->CurrentLink() && !controller->GetCommandState(SetLink{}).enabled,
        "The end of a link lies outside its half-open range");
  select({5, 7});
  Check(controller->CurrentLink() == guide && controller->Execute(SetLink{edited.target, edited.title}) &&
            controller->CurrentLink() == LinkRange{{5, 7}, edited.target, edited.title} &&
            controller->Document().Blocks()[0].Data().links.size() == 3,
        "Changing a partial link selection still affects only the selected characters");

  select({10, 14});
  Check(!controller->CurrentLink() && controller->Execute(SetLink{edited.target, {}}) &&
            controller->CurrentLink() == LinkRange{{0, 4}, edited.target, {}}, "Insert a link on selected plain text");
  const auto markdown = MarkdownCodec().Export(controller->Document());
  Check(markdown.text && MarkdownCodec().Import(*markdown.text).document == controller->Document(),
        "The edited heading and links survive a strict Markdown round trip");
}

std::vector<LinkRange> activated_links;
std::size_t link_pointer_moves = 0;

View LinkFixture() {
  const auto document = MarkdownCodec().Import("[A link](https://example.com \"Site\") and plain");
  const auto editor = UseRichEditor(*document.document);
  return RichEditor(editor, {.style = {.body_font = Font::Monospace(20), .content_padding = EdgeInsets::All(12),
                                       .link_hover_color = Color::Rgb(166, 193, 217)}})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Move) {
          ++link_pointer_moves;
        }
      })
      .On<RichEditorEvents::LinkActivated>([](const LinkRange& link) {
        activated_links.push_back(link);
        return true;
      }).Key("linked-editor");
}

void LinkActivation() {
  const Application app(LinkFixture, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {400, 240}});
  const auto editor = ui.Find(UiSelector::Key("linked-editor"));
  const auto bounds = editor.One().bounds;
  const Point linked{bounds.x + 27, bounds.y + 24};
  const Point blank{bounds.x + 300, bounds.y + 24};
  const KeyModifiers command{.meta = true};
  const KeyModifiers control{.control = true};
  const auto move = [&](Point point, KeyModifiers modifiers = {}) {
    ui.SendPointer({.type = PointerEventType::Move, .position = point, .modifiers = modifiers});
    ui.Pump();
  };
  const auto has_hover_color = [&] {
    return ui.CaptureSnapshot().ToString().find("green=0.756863") != std::string::npos;
  };
  move(linked);
  Check(!has_hover_color(), "Ordinary hover preserves the normal link foreground");
  move({linked.x + 1, linked.y}, control);
  Check(!ui.ActiveTextInput() && has_hover_color(), "Control-hover works on an unfocused editor with a pointer listener");
  Check(link_pointer_moves == 2, "The editor still delivers each pointer move to the caller once");
  move(blank, control);
  Check(!has_hover_color(), "Moving off the link clears the hover color");

  const auto pointer = [&](PointerEventType type, Point point, KeyModifiers modifiers) {
    ui.SendPointer({.type = type, .pointer_id = 7, .position = point, .device_kind = PointerDeviceKind::Mouse,
                    .changed_button = type == PointerEventType::Down || type == PointerEventType::Up
                        ? PointerButton::Primary : PointerButton::None,
                    .pressed_buttons = type == PointerEventType::Down || type == PointerEventType::Move
                        ? PointerButton::Primary : PointerButton::None,
                    .modifiers = modifiers});
    ui.Pump();
  };
  ui.TapAt(linked, {.device_kind = PointerDeviceKind::Mouse});
  Check(activated_links.empty(), "A normal click places the caret without activating the link");
  move(linked);
  ui.SendKey({.type = KeyEventType::Down, .key = Key::MetaLeft, .modifiers = command});
  ui.Pump();
  Check(has_hover_color(), "Pressing Command over a stationary link updates the color");
  ui.SendKey({.type = KeyEventType::Up, .key = Key::MetaLeft});
  ui.Pump();
  Check(!has_hover_color(), "Releasing Command restores the normal link color");
  ui.Pump(std::chrono::milliseconds(600));
  const TextSelection backward{9, 3};
  editor.SetSelection(backward);
  pointer(PointerEventType::Down, linked, command);
  Check(activated_links.empty(), "A modifier press waits for the click to finish");
  pointer(PointerEventType::Up, linked, command);
  const LinkRange expected{{0, 6}, "https://example.com", "Site"};
  Check(activated_links == std::vector{expected} && ui.ActiveTextInput()->selection == backward,
        "Command-click requests navigation without changing the directed selection");

  ui.Pump(std::chrono::milliseconds(600));
  pointer(PointerEventType::Down, linked, control);
  pointer(PointerEventType::Up, linked, control);
  Check(activated_links == std::vector{expected, expected}, "Control-click requests the same link");
  pointer(PointerEventType::Down, blank, control);
  pointer(PointerEventType::Up, blank, control);
  pointer(PointerEventType::Down, linked, control);
  pointer(PointerEventType::Move, blank, control);
  pointer(PointerEventType::Up, blank, control);
  Check(activated_links.size() == 2 && !ui.ActiveTextInput()->selection.IsCollapsed(),
        "Whitespace and dragging do not navigate, and the drag still selects text");
}

void ListEditing() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {400, 290}});
  const auto original = MarkdownCodec().Import("3. First\n4. Second\n5. Third\n");
  controller->ReplaceDocument(*original.document);
  ui.Pump();
  ui.Find(UiSelector::Text("Focus")).Tap();
  const ToggleList ordered{ListKind::Ordered};
  const ToggleList unordered{ListKind::Unordered};
  Check(controller->GetCommandState(ordered).checked == CheckState::On &&
            controller->GetCommandState(unordered).checked == CheckState::Off, "List commands report the current kind");
  const TextSelection list_selection{1, 8};
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = list_selection}});
  Check(controller->OrderedListStart() == 3 && controller->Execute(SetListStart{9}),
        "Change the starting number of the shared ordered list");
  const auto renumbered = controller->Document();
  const auto json = JsonCodec().Export(renumbered);
  const auto markdown = MarkdownCodec().Export(renumbered);
  ui.Pump();
  Check(renumbered.Blocks()[0].Data().list_start == 9 &&
            renumbered.Blocks()[0].Data().children == original.document->Blocks()[0].Data().children &&
            ui.ActiveTextInput()->selection == list_selection &&
            json.text && JsonCodec().Import(*json.text).document == renumbered &&
            markdown.text && MarkdownCodec().Import(*markdown.text).document == renumbered,
        "Renumbering preserves the items and selection and survives both codecs");
  Check(controller->Execute(Undo{}) && controller->Document() == original.document,
        "Undo restores the previous list numbering");
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{0, 0}}});
  Check(controller->Execute(unordered) && !controller->Document().Blocks()[0].Data().ordered,
        "The unordered list command changes the selected item");
  Check(controller->GetCommandState(unordered).checked == CheckState::On && !controller->OrderedListStart() &&
            !controller->GetCommandState(SetListStart{}).enabled, "A bulleted list has no start number");
  Check(controller->Execute(Undo{}) && controller->Document() == original.document,
        "Undo restores the original list kind");
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{6, 6}}});
  ui.PressKey(Key::Tab);
  Check(controller->Document().Blocks()[0].Data().children[0].Data().children[1].Data().kind == BlockKind::List,
        "Tab indents the current list item");
  Check(controller->OrderedListStart() == 1 && controller->Execute(SetListStart{0}) &&
            controller->OrderedListStart() == 0 && controller->Document().Blocks()[0].Data().list_start == 3,
        "A nested list can start at zero without renumbering its parent");
  Check(controller->Execute(Undo{}) && controller->OrderedListStart() == 1, "Undo restores the nested start");
  ui.Pump();
  ui.PressKey(Key::Tab, {.shift = true});
  Check(controller->Document() == original.document, "Shift+Tab outdents the item and restores the list order");
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{9, 9}}});
  ui.PressKey(Key::Enter);
  Check(controller->Document().Blocks()[0].Data().children.size() == 4, "Enter splits the list item");
  Check(controller->Execute(Undo{}) && controller->Document() == original.document, "Undo restores list split");

  const auto mixed = *MarkdownCodec().Import(
      "7. Parent\n   - First\n   - **Second**\n   - Third\n8. Following\n").document;
  const auto& mixed_index = huxerui::richeditor::detail::DocumentAccess::Get(mixed).index;
  const auto middle = mixed_index.At(2).start;
  const auto first_bullet = mixed_index.At(1).start;
  const auto last_bullet = mixed_index.At(3).start + 5;
  for (const auto selection : {TextSelection{middle + 4, middle + 1}, TextSelection{last_bullet, first_bullet}}) {
    controller->ReplaceDocument(mixed);
    ui.Pump();
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = selection}});
    Check(controller->GetCommandState(unordered).checked == CheckState::On && controller->Execute(unordered),
          "Remove nested bullets from the selected child items");
    const auto unwrapped = controller->Document();
    const auto& outer = unwrapped.Blocks().front().Data();
    Check(outer.ordered && outer.list_start == 7 && outer.children.size() == 2 &&
              outer.children.back() == mixed.Blocks().front().Data().children.back() &&
              controller->GetCommandState(ordered).checked == CheckState::Off &&
              controller->GetCommandState(unordered).checked == CheckState::Off && !controller->OrderedListStart() &&
              !controller->GetCommandState(OutdentList{}).enabled,
          "The unmarked continuation keeps its parent and does not select the parent's numbering");
    CheckDocumentIndex(unwrapped);
    Check(controller->Execute(unordered) && controller->Document() == mixed,
          "Reapplying bullets reunites the sublist without renumbering the outer list");
    ui.Pump();
    Check(ui.ActiveTextInput()->selection == selection &&
              controller->GetCommandState(unordered).checked == CheckState::On,
          "Restoring nested bullets preserves the directed selection and toolbar state");
    Check(controller->Execute(Undo{}) && controller->Document() == unwrapped &&
              controller->Execute(Redo{}) && controller->Document() == mixed,
          "Unwrapping and wrapping preserve the original list through undo and redo");
  }
}

void ListSelectionScope() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  using huxerui::richeditor::detail::DocumentAccess;
  const ToggleList ordered{ListKind::Ordered};
  const ToggleList unordered{ListKind::Unordered};
  const auto verify = [&](std::string_view markdown, std::size_t last, CheckState numbered, CheckState bulleted) {
    const auto original = *MarkdownCodec().Import(markdown).document;
    controller->ReplaceDocument(original);
    ui.Pump();
    const auto end = DocumentAccess::Get(original).index.At(last).start + 1;
    const TextSelection selection{end, 1};
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = selection}});
    Check(controller->GetCommandState(ordered).checked == numbered &&
              controller->GetCommandState(unordered).checked == bulleted && !controller->OrderedListStart(),
          "A cross-level selection reports its styles independently of the available list target");
    for (const auto& command : std::array<EditorCommand, 5>{ordered, unordered, SetListStart{20},
                                                           IndentList{}, OutdentList{}}) {
      Check(!controller->GetCommandState(command).enabled && !controller->Execute(command),
            "A structural list command cannot retarget a cross-level selection to its ancestor");
    }
    Check(controller->Document() == original && ui.ActiveTextInput()->selection == selection &&
              !controller->GetCommandState(Undo{}).enabled,
          "Unavailable commands leave the tree, selection, and history in place");
    Check(controller->Execute(ToggleMark{Mark::Bold}) && controller->Execute(Undo{}) &&
              controller->Document() == original,
          "Text formatting remains available across list levels");
  };
  verify("7. Parent\n   - Child\n8. Tail\n", 1, CheckState::Mixed, CheckState::Mixed);
  verify("7. Parent\n   - Child\n8. Tail\n", 2, CheckState::Mixed, CheckState::Mixed);
  verify("7. Parent\n   1. Child\n8. Tail\n", 1, CheckState::On, CheckState::Off);
}

void ListMarkerOwnership() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  using huxerui::richeditor::detail::BlockAt;
  using huxerui::richeditor::detail::DocumentAccess;
  const ToggleList numbered{ListKind::Ordered};
  const ToggleList bulleted{ListKind::Unordered};
  const auto verify = [&](std::string_view markdown, std::size_t ordinal, bool first_in_list) {
    const auto imported = MarkdownCodec().Import(markdown).document;
    Check(imported.has_value(), "The list fixture imports as strict Markdown");
    const auto saved = JsonCodec().Export(*imported);
    Check(saved.text.has_value(), "The list fixture can be saved as JSON");
    const auto reopened = JsonCodec().Import(*saved.text).document;
    Check(reopened == imported, "JSON preserves the original list ownership");
    const auto leaf = DocumentAccess::Get(*reopened).index.At(ordinal);
    const auto item_path = std::span(leaf.path).first(leaf.path.size() - 1);
    const auto& item = BlockAt(*reopened, item_path);
    controller->ReplaceDocument(*reopened);
    ui.Pump();
    const TextSelection selection{leaf.start + 2, leaf.start + 1};
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = selection}});
    const auto state = controller->GetCommandState(bulleted);
    Check(state.checked == CheckState::On && !state.enabled && !controller->Execute(bulleted) &&
              controller->GetCommandState(numbered).checked == CheckState::Off && !controller->OrderedListStart() &&
              controller->Document() == reopened && ui.ActiveTextInput()->selection == selection,
          "A marker that cannot be removed safely stays checked without claiming an ancestor list");

    if (first_in_list) {
      const TextSelection start{leaf.start, leaf.start};
      Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = start}});
      ui.PressKey(Key::Backspace);
      Accept(ui, {{.kind = TextInputCommandKind::DeleteSurrounding, .delete_before = 1}});
      Check(controller->Document() == reopened && ui.ActiveTextInput()->selection == start &&
                !controller->GetCommandState(Undo{}).enabled,
            "Hardware and IME Backspace cannot unwrap a protected first item or fall through to a text join");
    }

    const auto before_conversion = ui.ActiveTextInput()->selection;
    Check(controller->Execute(numbered), "Changing kind remains available for a protected item");
    const auto converted = controller->Document();
    const auto changed_leaf = DocumentAccess::Get(converted).index.At(ordinal);
    const auto changed_item_path = std::span(changed_leaf.path).first(changed_leaf.path.size() - 1);
    Check(DocumentAccess::SameBlock(BlockAt(converted, changed_item_path), item) &&
              controller->OrderedListStart() == 1 && controller->GetCommandState(SetListStart{7}).enabled &&
              !controller->GetCommandState(numbered).enabled,
          "Changing kind keeps the whole item together and enables numbering independently of removal");
    CheckDocumentIndex(converted);
    Check(controller->Execute(Undo{}) && controller->Document() == reopened &&
              controller->Execute(Redo{}) && controller->Document() == converted,
          "Undo and redo restore the tree around a protected item");
    ui.Pump();
    Check(ui.ActiveTextInput()->selection == before_conversion, "Changing kind preserves the directed selection");
  };
  verify("7. - Child\n8. Tail\n", 0, true);
  verify("7. Parent\n   - Alpha\n   - Child\n     - Grandchild\n   - Omega\n8. Tail\n", 2, false);
  verify("Intro\n\n- Child\n\n  Continuation\n", 1, true);
}

void ListNumbering() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  using huxerui::richeditor::detail::DocumentAccess;
  const ToggleList numbered{ListKind::Ordered};
  const ToggleList bulleted{ListKind::Unordered};
  const auto select = [&](const RichDocument& document, std::size_t ordinal) {
    controller->ReplaceDocument(document);
    ui.Pump();
    const auto start = DocumentAccess::Get(document).index.At(ordinal).start;
    const TextSelection selection{start + 2, start + 1};
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = selection}});
    return selection;
  };
  const auto original = *MarkdownCodec().Import("7. Alpha\n8. Beta\n9. Gamma\n").document;
  for (const auto ordinal : {0u, 1u, 2u}) {
    const auto selection = select(original, ordinal);
    Check(controller->Execute(numbered), "Remove numbering from one item");
    const auto unwrapped = controller->Document();
    Check((ordinal == 0 || unwrapped.Blocks().front().Data().list_start == 7) &&
              (ordinal == 2 || unwrapped.Blocks().back().Data().list_start == 8 + ordinal),
          "Removing one marker preserves the displayed ordinals of its neighbors");
    Check(controller->Execute(numbered) && controller->Document() == original,
          "Reapplying numbering joins the compatible fragments and restores the ordinal");
    CheckDocumentIndex(controller->Document());
    Check(controller->Execute(Undo{}) && DocumentAccess::Same(controller->Document(), unwrapped) &&
              controller->Execute(Redo{}) && controller->Document() == original,
          "Undo restores the split and redo rejoins it in one edit");
    ui.Pump();
    Check(ui.ActiveTextInput()->selection == selection, "Local rejoining preserves the directed selection");
  }
  select(original, 1);
  Check(controller->Execute(bulleted) && controller->Execute(numbered) && controller->Document() == original,
        "Converting the middle item back to numbered uses the same local continuation rule");

  const auto nested = *MarkdownCodec().Import(
      "- Parent\n\n  0. Alpha\n  1. Beta\n  2. Gamma\n\n- Next\n").document;
  select(nested, 1);
  Check(controller->OrderedListStart() == 0 && controller->Execute(numbered) &&
            controller->Execute(numbered) && controller->Document() == nested,
        "Prepending to the remaining sublist restores a valid start at zero");
  CheckDocumentIndex(controller->Document());
  const auto json = JsonCodec().Export(controller->Document());
  const auto markdown = MarkdownCodec().Export(controller->Document());
  Check(json.text && JsonCodec().Import(*json.text).document == nested &&
            markdown.text && MarkdownCodec().Import(*markdown.text).document == nested,
        "A restored numbered sublist round-trips through JSON and strict Markdown");

  const auto groups = *MarkdownCodec().Import("7. Alpha\n\nGap\n\n1. Restart\n\n- Delta\n\n+ Epsilon\n").document;
  select(groups, 1);
  Check(controller->Execute(numbered), "Wrap text between independent list groups");
  const auto joined = controller->Document();
  Check(joined.Blocks().size() == 4 && joined.Blocks()[0].Data().list_start == 7 &&
            joined.Blocks()[0].Data().children.size() == 2 &&
            DocumentAccess::SameBlock(joined.Blocks()[1], groups.Blocks()[2]) &&
            DocumentAccess::SameBlock(joined.Blocks()[2], groups.Blocks()[3]) &&
            DocumentAccess::SameBlock(joined.Blocks()[3], groups.Blocks()[4]),
        "The edit extends the preceding group and preserves the restarted and unrelated groups");

  const auto spacing = *MarkdownCodec().Import("- Tight\n\nGap\n\n- Loose\n\n- Spacing\n").document;
  Check(spacing.Blocks().size() == 3 && spacing.Blocks()[0].Data().tight && !spacing.Blocks()[2].Data().tight,
        "The neighboring groups have different list spacing");
  select(spacing, 1);
  Check(controller->Execute(bulleted) && controller->Document().Blocks().size() == 2 &&
            controller->Document().Blocks()[0].Data().children.size() == 2 &&
            DocumentAccess::SameBlock(controller->Document().Blocks()[1], spacing.Blocks()[2]),
        "Local wrapping preserves the separate group with different spacing");
}

void DeleteSurrounding() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{3, 3}},
              {.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{3, 5}},
              {.kind = TextInputCommandKind::DeleteSurrounding, .delete_before = 1,
               .delete_unit = TextInputUnit::UnicodeCodePoint}});
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});
  Check(controller->Document().Blocks()[0].Data().text == "A", "Cancel keeps the deletion outside composition");
  Check(controller->Execute(Undo{}) && controller->Document().Blocks()[0].Data().text == "A🙂",
        "Surrounding deletion is reversible");
}

void LineEditing() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  const auto replace = [&](std::string_view markdown, TextOffset caret) {
    controller->ReplaceDocument(*MarkdownCodec().Import(markdown).document);
    ui.Pump();
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{caret, caret}}});
  };
  const auto bold = ToggleMark{Mark::Bold};
  replace("# Title", 5);
  Check(controller->Execute(bold), "Enable a pending format at the end of a heading");
  ui.Pump();
  ui.PressKey(Key::Enter);
  const auto heading_and_paragraph = controller->Document();
  Check(heading_and_paragraph.Blocks()[0].Data().kind == BlockKind::Heading &&
            heading_and_paragraph.Blocks()[1].Data().kind == BlockKind::Paragraph &&
            controller->GetCommandState(bold).checked == CheckState::Off,
        "Return starts a plain paragraph after a heading and clears the pending inline format");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "正文"}});
  Check(controller->Document().Blocks()[1].Data().marks.empty(), "Typing into the new paragraph stays plain");

  replace("**Line**", 4);
  ui.PressKey(Key::Enter, {.shift = true});
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "continued"}});
  const auto soft_break = controller->Document();
  Check(soft_break.Blocks().size() == 1 && soft_break.Blocks()[0].Data().text == "Line\ncontinued" &&
            soft_break.Blocks()[0].Data().marks == std::vector<MarkRange>{{{0, 14}, Mark::Bold}},
        "Shift+Return keeps the inline format within the same paragraph");

  replace("# **Title**", 2);
  const auto before_plain_break = controller->Document();
  ui.PressKey(Key::Enter, {.meta = true});
  const auto plain_break = controller->Document();
  Check(plain_break.Blocks().size() == 2 && plain_break.Blocks()[0].Data().kind == BlockKind::Heading &&
            plain_break.Blocks()[0].Data().text == "Ti" && plain_break.Blocks()[1].Data().kind == BlockKind::Paragraph &&
            plain_break.Blocks()[1].Data().text == "tle" &&
            plain_break.Blocks()[1].Data().marks == std::vector<MarkRange>{{{0, 3}, Mark::Bold}} &&
            ui.ActiveTextInput()->selection == TextSelection{3, 3} &&
            controller->GetCommandState(bold).checked == CheckState::Off,
        "Command+Enter splits a heading and preserves the existing suffix formatting");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "中文"}});
  Check(controller->Document().Blocks()[1].Data().text == "中文tle" &&
            controller->Document().Blocks()[1].Data().marks == std::vector<MarkRange>{{{2, 5}, Mark::Bold}},
        "Typing after a plain break does not inherit the suffix's marks");
  Check(controller->Execute(Undo{}) && controller->Document() == plain_break &&
            controller->Execute(Undo{}) && controller->Document() == before_plain_break,
        "The plain break and subsequent typing are separate undoable edits");

  replace("7. Parent\n   - Child\n8. Next\n\n**After**", 8);
  const auto before_continue = controller->Document();
  const auto after_start = huxerui::richeditor::detail::DocumentAccess::Get(before_continue).index.At(3).start;
  ui.PressKey(Key::Enter, {.control = true});
  Check(controller->Document() == before_continue &&
            ui.ActiveTextInput()->selection == TextSelection{after_start, after_start} &&
            controller->GetCommandState(bold).checked == CheckState::Off,
        "Ctrl+Enter exits the outer list to the following paragraph without moving any items");

  replace("> Quote\n\n# Next", 2);
  const auto before_quote_exit = controller->Document();
  Check(controller->Execute(ContinueInParagraph{}), "Continue out of a quote through the editor command");
  ui.Pump();
  const auto quote_exit = controller->Document();
  Check(quote_exit.Blocks().size() == 3 && quote_exit.Blocks()[0] == before_quote_exit.Blocks()[0] &&
            quote_exit.Blocks()[1].Data().kind == BlockKind::Paragraph && quote_exit.Blocks()[1].Data().text.empty() &&
            quote_exit.Blocks()[2] == before_quote_exit.Blocks()[1] &&
            ui.ActiveTextInput()->selection == TextSelection{6, 6} &&
            controller->Execute(Undo{}) && controller->Document() == before_quote_exit,
        "A new paragraph below a quote preserves the quote and is undone in one step");

  replace("3. Item", 4);
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .selection_after = TextSelection{5, 5}, .text = "\n"}});
  const auto list = controller->Document();
  Check(list.Blocks()[0].Data().children.size() == 2, "IME Return creates the next numbered item");
  Accept(ui, {{.kind = TextInputCommandKind::DeleteSurrounding, .delete_before = 1,
               .delete_unit = TextInputUnit::UnicodeCodePoint}});
  const auto left_list = controller->Document();
  Check(left_list.Blocks().size() == 2 && left_list.Blocks()[0].Data().list_start == 3 &&
            left_list.Blocks()[0].Data().children.size() == 1 &&
            left_list.Blocks()[1].Data().kind == BlockKind::Paragraph &&
            ui.ActiveTextInput()->selection == TextSelection{5, 5},
        "IME Backspace leaves the empty item without merging or moving the caret to the previous item");
  Check(controller->Execute(Undo{}) && controller->Document() == list, "Leaving the list is one undoable edit");
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .selection_after = TextSelection{6, 6}, .text = "\n"}});
  Check(controller->Document() == left_list && ui.ActiveTextInput()->selection == TextSelection{5, 5},
        "IME Return on the empty item exits the list at the resulting caret");

  replace("3. Item\n   - Bullet\n4. Tail\n", 4);
  const auto mixed_list = controller->Document();
  const auto& original_item = mixed_list.Blocks()[0].Data().children[0];
  Check(original_item.Data().children.size() == 2 &&
            original_item.Data().children[1].Data().kind == BlockKind::List &&
            !original_item.Data().children[1].Data().ordered, "The fixture has an unordered sublist");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .selection_after = TextSelection{5, 5}, .text = "\n"}});
  const auto split_mixed_list = controller->Document();
  Check(split_mixed_list.Blocks()[0].Data().children[0] == original_item &&
            split_mixed_list.Blocks()[0].Data().children.size() == 3 &&
            ui.ActiveTextInput()->selection == TextSelection{12, 12},
        "Return after the item text places a new sibling below the sublist");
  Accept(ui, {{.kind = TextInputCommandKind::DeleteSurrounding, .delete_before = 1}});
  const auto left_mixed_list = controller->Document();
  Check(left_mixed_list.Blocks().size() == 3 &&
            left_mixed_list.Blocks()[0].Data().children == std::vector{original_item} &&
            left_mixed_list.Blocks()[1].Data().kind == BlockKind::Paragraph &&
            left_mixed_list.Blocks()[2].Data().list_start == 4 &&
            ui.ActiveTextInput()->selection == TextSelection{12, 12},
        "Removing the empty item's marker preserves the sublist and the next original number");
  Check(controller->Execute(Undo{}) && controller->Document() == split_mixed_list &&
            controller->Execute(Redo{}) && controller->Document() == left_mixed_list,
        "Undo and redo preserve the mixed-list structure");

  replace("3. Item\n   - Bullet\n4. Tail\n", 5);
  const auto& original_list = original_item.Data().children[1];
  const auto check_inserted_bullet = [&] {
    const auto current = controller->Document();
    const auto& list = current.Blocks()[0].Data();
    const auto& nested = list.children[0].Data().children[1].Data();
    Check(list.list_start == 3 && list.children.size() == 2 && !nested.ordered && nested.children.size() == 2 &&
              nested.children[0].Data().children[0].Data().text.empty() &&
              nested.children[1] == original_list.Data().children[0] &&
              ui.ActiveTextInput()->selection == TextSelection{5, 5},
          "Return at the first bullet creates a sibling above it with the caret in the new item");
  };
  const auto check_removed_bullet = [&] {
    Check(controller->Document() == mixed_list && ui.ActiveTextInput()->selection == TextSelection{5, 5},
          "Backspace removes the empty first bullet and leaves the original sublist and numbering in place");
    CheckDocumentIndex(controller->Document());
  };
  ui.PressKey(Key::Enter);
  check_inserted_bullet();
  const auto with_empty_bullet = controller->Document();
  ui.PressKey(Key::Enter);
  const auto with_continuation = controller->Document();
  const auto& items = with_continuation.Blocks()[0].Data().children;
  const auto& children = items[0].Data().children;
  Check(items.size() == 2 && children.size() == 3 &&
            children[1].Data().kind == BlockKind::Paragraph && children[1].Data().text.empty() &&
            children[2] == original_list && ui.ActiveTextInput()->selection == TextSelection{5, 5},
        "Enter removes the empty subitem's marker and leaves its siblings under their original parent");
  Check(controller->Execute(Undo{}) && controller->Document() == with_empty_bullet,
        "Undo restores the empty subitem with its marker");
  ui.Pump();
  ui.PressKey(Key::Backspace);
  check_removed_bullet();
  Check(controller->Execute(Undo{}) && controller->Document() == with_empty_bullet &&
            controller->Execute(Undo{}) && controller->Document() == mixed_list &&
            controller->Execute(Redo{}) && controller->Document() == with_empty_bullet &&
            controller->Execute(Redo{}) && controller->Document() == mixed_list,
        "Undo and redo keep the insertion and removal as separate edits");
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .selection_after = TextSelection{6, 6}, .text = "\n"}});
  check_inserted_bullet();
  Accept(ui, {{.kind = TextInputCommandKind::DeleteSurrounding, .delete_before = 1}});
  check_removed_bullet();
  Check(controller->Execute(Undo{}), "Undo an IME Backspace");
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "First"}});
  Check(controller->Document().Blocks()[0].Data().children[0].Data().children[1].Data().children[0]
            .Data().children[0].Data().text == "First", "Typing fills the inserted bullet");

  replace("3. Item\n   - First\n   - Second\n   - Third\n4. Tail\n", 17);
  const ToggleList bulleted{ListKind::Unordered};
  Check(controller->Execute(bulleted), "Unwrap a child item as a continuation");
  const auto before_continuation_break = controller->Document();
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .selection_after = TextSelection{18, 18}, .text = "\n"}});
  const auto continuation_break = controller->Document();
  const auto& siblings = continuation_break.Blocks()[0].Data().children;
  const auto& continuation = siblings[0].Data().children;
  Check(siblings.size() == 2 && continuation.size() == 5 && continuation[2].Data().text == "Second" &&
            continuation[3].Data().kind == BlockKind::Paragraph && continuation[3].Data().text.empty() &&
            continuation[4] == before_continuation_break.Blocks()[0].Data().children[0].Data().children[3] &&
            ui.ActiveTextInput()->selection == TextSelection{18, 18} &&
            controller->GetCommandState(ToggleList{ListKind::Ordered}).checked == CheckState::Off,
        "IME Return splits the continuation inside its existing list item");
  CheckDocumentIndex(continuation_break);
  Check(controller->Execute(Undo{}) && controller->Document() == before_continuation_break &&
            controller->Execute(Redo{}) && controller->Document() == continuation_break,
        "Undo and redo restore a continuation break without introducing another list item");

  replace("3. Alpha beta\n   - Bullet\n4. Tail\n", 6);
  const auto before_join = controller->Document();
  ui.PressKey(Key::Enter);
  ui.PressKey(Key::Backspace);
  Check(controller->Document() == before_join && ui.ActiveTextInput()->selection == TextSelection{6, 6},
        "Joining a split item restores its nested list without leaving a numbered wrapper");

  replace("3. Alpha\n4. Beta\n   - **Bullet**\n5. Tail\n", 0);
  const auto before_delete = controller->Document();
  const TextSelection crossed_items{8, 4};
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = crossed_items}});
  ui.PressKey(Key::Backspace);
  const auto joined = controller->Document();
  const auto expected_join = MarkdownCodec().Import("3. Alphta\n   - **Bullet**\n4. Tail\n").document;
  Check(joined == expected_join && ui.ActiveTextInput()->selection == TextSelection{4, 4},
        "Deleting text across numbered items keeps the bulleted suffix under the joined item");
  CheckDocumentIndex(joined);
  const auto markdown = MarkdownCodec().Export(joined);
  Check(markdown.text && MarkdownCodec().Import(*markdown.text).document == joined,
        "The joined list retains its structure and styles through strict Markdown");
  Check(controller->Execute(Undo{}) && controller->Document() == before_delete,
        "One undo restores the two numbered items and their original sublist");
  ui.Pump();
  Check(ui.ActiveTextInput()->selection == crossed_items, "Undo restores the directed cross-item selection");

  replace("3. Alpha\n4. Removed\n5. Beta\n   - **Bullet**\n6. Tail\n", 0);
  const auto before_composition = controller->Document();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{16, 4}},
              {.kind = TextInputCommandKind::UpdateComposition, .text = "zhong"}});
  Check(controller->Document() == before_composition, "IME preedit retains the committed list");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "中文"}});
  Check(controller->Document() == MarkdownCodec().Import("3. Alph中文ta\n   - **Bullet**\n4. Tail\n").document &&
            ui.ActiveTextInput()->selection == TextSelection{6, 6},
        "IME replacement over multiple items merges the remaining text and keeps the sublist");
  CheckDocumentIndex(controller->Document());
  Check(controller->Execute(Undo{}) && controller->Document() == before_composition,
        "One undo restores a list edited by IME");

  replace("> Note", 4);
  ui.PressKey(Key::Enter);
  ui.PressKey(Key::Backspace);
  const auto quote_and_paragraph = controller->Document();
  Check(quote_and_paragraph.Blocks().size() == 2 &&
            quote_and_paragraph.Blocks()[0].Data().kind == BlockKind::Quote &&
            quote_and_paragraph.Blocks()[1].Data().kind == BlockKind::Paragraph &&
            ui.ActiveTextInput()->selection == TextSelection{5, 5},
        "Backspace in the trailing empty quote paragraph exits the quote in place");
  ui.PressKey(Key::Backspace);
  Check(controller->Document().Blocks().size() == 1 && ui.ActiveTextInput()->selection.active == 4,
        "Backspace at a plain paragraph boundary still joins paragraphs");

  replace("Previous\n\n# Title", 9);
  ui.PressKey(Key::Backspace);
  Check(controller->Document().Blocks().size() == 2 &&
            controller->Document().Blocks()[1].Data().kind == BlockKind::Paragraph &&
            controller->Document().Blocks()[1].Data().text == "Title" &&
            ui.ActiveTextInput()->selection.active == 9,
        "Backspace at the start of a heading removes the heading style in place");
}

void NestedEmptyListEditing() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  ui.Find(UiSelector::Text("Focus")).Tap();
  using huxerui::richeditor::detail::DocumentAccess;
  const auto select = [&](const RichDocument& document, std::size_t ordinal, bool at_end) {
    controller->ReplaceDocument(document);
    ui.Pump();
    const auto leaf = DocumentAccess::Get(document).index.At(ordinal);
    const auto offset = leaf.start + (at_end ? DocumentAccess::Body(leaf.block).Length() : 0);
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{offset, offset}}});
    return offset;
  };

  const auto original = *MarkdownCodec().Import("7. Parent\n\n   - 中文🙂\n\n   Continuation\n8. Tail\n").document;
  const auto offset = select(original, 1, true) + 1;
  const TextSelection empty_item_caret{offset, offset};
  ui.PressKey(Key::Enter);
  const auto with_empty_item = controller->Document();
  Check(ui.ActiveTextInput()->selection == empty_item_caret, "Return creates an empty subitem before the continuation");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText,
               .selection_after = TextSelection{offset + 1, offset + 1}, .text = "\n"}});
  const auto with_empty_continuation = controller->Document();
  const auto& outer = with_empty_continuation.Blocks().front().Data();
  const auto& original_outer = original.Blocks().front().Data();
  const auto& children = outer.children.front().Data().children;
  const auto& original_children = original_outer.children.front().Data().children;
  Check(outer.list_start == 7 && outer.children.size() == 2 &&
            DocumentAccess::SameBlock(outer.children.back(), original_outer.children.back()) &&
            children.size() == 4 && children[1] == original_children[1] &&
            children[2].Data().kind == BlockKind::Paragraph && children[2].Data().text.empty() &&
            DocumentAccess::SameBlock(children[3], original_children[2]) &&
            ui.ActiveTextInput()->selection == empty_item_caret &&
            controller->GetCommandState(ToggleList{ListKind::Ordered}).checked == CheckState::Off,
        "IME Return removes the marker in place and keeps the parent's continuation and numbering");
  CheckDocumentIndex(with_empty_continuation);
  const auto saved = JsonCodec().Export(with_empty_continuation);
  Check(saved.text && JsonCodec().Import(*saved.text).document == with_empty_continuation,
        "JSON round-trips the empty continuation in its original parent");
  Check(controller->Execute(Undo{}) && controller->Document() == with_empty_item &&
            controller->Execute(Redo{}) && controller->Document() == with_empty_continuation &&
            controller->Execute(Undo{}) && controller->Document() == with_empty_item,
        "The change from an empty subitem to a continuation is one undoable edit");
  ui.Pump();
  ui.PressKey(Key::Backspace);
  Check(controller->Document() == original && ui.ActiveTextInput()->selection == empty_item_caret &&
            DocumentAccess::SameBlock(DocumentAccess::Get(controller->Document()).index.At(2).block, original_children[2]),
        "Backspace deletes the empty subitem and places the caret in the following block of the parent");
  CheckDocumentIndex(controller->Document());
  const auto exported = MarkdownCodec().Export(controller->Document());
  Check(exported.text && MarkdownCodec().Import(*exported.text).document == original,
        "The list restored by Backspace round-trips through strict Markdown");

  const auto without_lead = *MarkdownCodec().Import("7. - Child\n8. Tail\n").document;
  select(without_lead, 0, false);
  ui.PressKey(Key::Enter);
  const auto with_first_empty = controller->Document();
  ui.PressKey(Key::Enter);
  Check(controller->Document() == without_lead && ui.ActiveTextInput()->selection == TextSelection{0, 0} &&
            controller->GetCommandState(ToggleList{ListKind::Unordered}).checked == CheckState::On &&
            controller->Execute(Undo{}) && controller->Document() == with_first_empty,
        "An empty first child is removed instead of becoming the numbered parent's lead");

  const auto at_tail = *MarkdownCodec().Import("7. Parent\n   - Child\n8. Tail\n").document;
  select(at_tail, 1, true);
  ui.PressKey(Key::Enter);
  ui.PressKey(Key::Enter);
  const auto outdented = controller->Document();
  const auto& items = outdented.Blocks().front().Data().children;
  Check(items.size() == 3 && items[0] == at_tail.Blocks().front().Data().children[0] &&
            items[1].Data().children.front().Data().text.empty() &&
            items[2] == at_tail.Blocks().front().Data().children[1],
        "Return at the end of the parent still outdents an empty subitem by one level");
}

void CodeEditing() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app);
  const auto original = MarkdownCodec().Import("~~~cpp\ncode\n~~~");
  controller->ReplaceDocument(*original.document);
  ui.Pump();
  ui.Find(UiSelector::Text("Focus")).Tap();
  ui.PressKey(Key::Enter);
  Check(controller->Document().Blocks().size() == 1 &&
            controller->Document().Blocks()[0].Data().text == "\ncode", "Enter inserts a literal code newline");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "\n"}});
  Check(controller->Document().Blocks().size() == 1 &&
            controller->Document().Blocks()[0].Data().text == "\n\ncode", "IME Return inserts a newline in code");
  const auto code = controller->Document();
  ui.PressKey(Key::Enter, {.control = true});
  Check(controller->Document().Blocks().size() == 2 &&
            controller->Document().Blocks()[1].Data().kind == BlockKind::Paragraph, "Ctrl+Enter exits a code block");
  Check(controller->Execute(Undo{}) && controller->Document() == code, "Code exit is reversible");
}

void CodeHighlighting() {
  static std::optional<State<std::shared_ptr<const CodeBlockHighlighting>>> presentation;
  const Application app([]() -> View {
    static const auto initial = MarkdownCodec().Import("~~~custom meta\n🙂value\n~~~\n");
    const auto editor = UseRichEditor(*initial.document);
    const auto state = UseState(std::shared_ptr<const CodeBlockHighlighting>{});
    controller = editor;
    presentation = state;
    return RichEditor(editor, {.extensions = {.code_blocks = state.Get()}});
  }, {.show_debug_overlay = false});
  UiTest ui(app);
  const auto initial = controller->Document();
  const auto has_highlight = [&] {
    return ui.CaptureSnapshot().ToString().find("green=0.796078") != std::string::npos;
  };
  Check(!has_highlight(), "Unrecognized fence displays plain code");
  *presentation = std::make_shared<const CodeBlockHighlighting>(CodeBlockHighlighting{
      .languages = {{"custom", "Custom"}},
      .highlight = [](std::string_view code, std::string_view id, Color) -> std::vector<CodeHighlight> {
        Check(code.starts_with("🙂value") && id == "custom", "Provider receives code and canonical language ID");
        return {{{2, 7}, Color::Rgb(17, 203, 223)}};
      },
  });
  ui.Pump();
  Check(has_highlight() && controller->Document() == initial && !controller->GetCommandState(Undo{}).enabled,
        "Changing providers updates the mounted block without an edit");
  *presentation = std::shared_ptr<const CodeBlockHighlighting>{};
  ui.Pump();
  Check(!has_highlight(), "Clearing the custom provider returns to the built-in catalog");
  presentation.reset();
}

void CodeLanguagePicker() {
  const Application app([]() -> View {
    static const auto initial = [] {
      std::string markdown = "Beginning\n\n~~~python\nprint(1)\n~~~\n\n";
      for (int i = 0; i < 20; ++i) {
        markdown += "Filler paragraph\n\n";
      }
      return MarkdownCodec().Import(markdown + "~~~JSON keep-this\nconst int x = 3;\n~~~\n");
    }();
    const auto editor = UseRichEditor(*initial.document);
    controller = editor;
    return RichEditor(editor);
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {320.0F, 440.0F}});
  const auto initial = controller->Document();
  const auto json_label = ui.FindSemantics(UiSemanticSelector::Label("Code language, JSON"));
  ui.TapAt({16.0F, 24.0F});
  Check(ui.ActiveTextInput() && ui.ActiveTextInput()->selection == TextSelection{0, 0},
        "The caret starts at the beginning of the document");
  ui.SendScroll({.position = {200.0F, 200.0F}, .delta_y = 1000.0F});
  ui.Pump(std::chrono::milliseconds(220));
  const auto bounds = json_label.One().bounds;
  Check(bounds.y >= 0.0F && bounds.y + bounds.height <= 440.0F,
        "Scrolling brings the last code block into view");
  ui.TapAt({bounds.x + bounds.width / 2, bounds.y + bounds.height / 2});
  ui.Pump(std::chrono::milliseconds(220));
  ui.Find(UiSelector::Text("C++")).Tap();
  ui.Pump(std::chrono::milliseconds(220));
  const auto changed = controller->Document();
  Check(std::ranges::equal(changed.Blocks().first(changed.Blocks().size() - 1),
                          initial.Blocks().first(initial.Blocks().size() - 1)) &&
            changed.Blocks().back().Data().text == initial.Blocks().back().Data().text &&
            changed.Blocks().back().Data().fence_info == "cpp keep-this",
        "The picker changes only the target code language and keeps fence metadata");
  const auto cpp_label = ui.FindSemantics(UiSemanticSelector::Label("Code language, C++"));
  Check(cpp_label.Exists() && cpp_label.One().bounds.y == bounds.y &&
            ui.ActiveTextInput() && ui.ActiveTextInput()->selection == TextSelection{0, 0},
        "Switching the language preserves the scroll position and caret");
  Check(controller->Execute(Undo{}) && controller->Document() == initial,
        "Undo restores the original language and metadata");
  ui.Pump();
  json_label.PerformSemanticAction({SemanticActionKind::Activate, {}});
  ui.Pump(std::chrono::milliseconds(220));
  ui.Find(UiSelector::Text("Plain text")).Tap();
  Check(controller->Document().Blocks().back().Data().fence_info == "text keep-this",
        "The accessible picker can switch to plain text");
  ui.Pump(std::chrono::milliseconds(220));
  ui.PressKey(Key::ArrowRight);
  Check(ui.ActiveTextInput() && ui.ActiveTextInput()->selection == TextSelection{1, 1} &&
            ui.FindSemantics(UiSemanticSelector::Label("Code language, Plain text")).One().bounds.y > 440.0F,
        "Keyboard navigation reveals the caret again after the language picker");
}

void PresentationExtensions() {
  using DecorationSources = std::vector<std::shared_ptr<const DecorationProvider>>;
  static std::optional<State<DecorationSources>> decorations;
  static unsigned renders = 0;
  static const auto custom_renderer = std::make_shared<const CustomBlockRenderer>(
      [](const CustomBlockData& block, Color, float) -> std::optional<CustomBlockGraphic> {
        Check(block.type == latex_type, "Custom presenter receives the persistent block");
        ++renders;
        return std::nullopt;
      });
  static const auto code_blocks = std::make_shared<const CodeBlockHighlighting>(CodeBlockHighlighting{
      .languages = {{"notes", "Notes", {"nt"}}, {"script", "Script"}},
      .highlight = [](std::string_view, std::string_view id, Color) -> std::vector<CodeHighlight> {
        Check(id == "notes" || id == "script", "Highlighter receives an ID from the same language catalog");
        return {{{0, 1}, Color::Rgb(17, 203, 223)}};
      },
  });
  const Application app([]() -> View {
    static const auto initial = folio::DocumentMarkdown().Import(
        "A🙂 content\n\n$$\nx^2\n$$\n\n~~~NT keep\na\n~~~\n");
    Check(initial.document.has_value(), "Presentation fixture imports");
    const auto editor = UseRichEditor(*initial.document);
    const auto state = UseState(DecorationSources{});
    decorations = state;
    controller = editor;
    return RichEditor(editor, {.extensions = {.code_blocks = code_blocks, .decorations = state.Get(),
                                              .custom_blocks = {{std::string(latex_type), custom_renderer}}}});
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {320.0F, 440.0F}});
  const auto initial = controller->Document();
  const auto label = ui.FindSemantics(UiSemanticSelector::Label("Code language, Notes"));
  const auto before = label.One().bounds;
  Check(renders > 0 && ui.CaptureSnapshot().ToString().find("green=0.796078") != std::string::npos,
        "The registered alias sets both the header and the syntax colors");
  const auto initial_renders = renders;
  const auto valid = Color::Rgb(17, 177, 223);
  const auto invalid = Color::Rgb(17, 181, 223);
  const auto obsolete = std::make_shared<unsigned>(0);
  *decorations = DecorationSources{
      std::make_shared<const DecorationProvider>(DecorationProvider{
          .scope = DecorationScope::Document,
          .analyze = [obsolete](DecorationRequest) -> Task<std::vector<TextDecorationRange>> {
            ++*obsolete;
            co_await Delay(std::chrono::milliseconds(500));
            ++*obsolete;
            co_return std::vector<TextDecorationRange>{};
          },
      }),
  };
  ui.PumpUntil([&] { return *obsolete != 0; });
  const auto analysis_count = std::make_shared<unsigned>(0);
  *decorations = DecorationSources{
      std::make_shared<const DecorationProvider>(DecorationProvider{
          .scope = DecorationScope::Document,
          .analyze = [initial, valid, invalid, analysis_count](DecorationRequest request)
              -> Task<std::vector<TextDecorationRange>> {
            ++*analysis_count;
            Check(request.document.has_value(), "Document scope receives the snapshot");
            if (request.document == initial) {
              Check(request.blocks.size() == 2 && request.blocks[1].path == std::vector<std::size_t>{2},
                    "Document scope receives ordered paths for the text blocks");
            }
            co_await Delay(std::chrono::milliseconds(40));
            co_return std::vector<TextDecorationRange>{
                {0, {1, 3}, DecorationKind::Background, request.document == initial ? valid : Color::Rgb(17, 171, 223)},
                {0, {2, 3}, DecorationKind::Underline, invalid},
                {7, {0, 1}, DecorationKind::Background, invalid},
            };
          },
      }),
      std::make_shared<const DecorationProvider>(DecorationProvider{
          .analyze = [](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
            std::vector<TextDecorationRange> ranges;
            for (std::size_t index = 0; index < request.blocks.size(); ++index) {
              if (request.blocks[index].block.Data().text.starts_with("A🙂 content")) {
                ranges.push_back({index, {3, 6}, DecorationKind::Underline, Color::Rgb(17, 160, 223)});
              }
            }
            co_return ranges;
          },
      }),
  };
  ui.PumpUntil([&] { return ui.CaptureSnapshot().ToString().find("green=0.694118") != std::string::npos; });
  ui.Pump(std::chrono::milliseconds(500));
  const auto painted = ui.CaptureSnapshot().ToString();
  Check(*obsolete == 1 && painted.find("green=0.694118") != std::string::npos &&
            painted.find("green=0.627451") != std::string::npos &&
            painted.find("green=0.709804") == std::string::npos &&
            label.One().bounds == before && renders == initial_renders && controller->Document() == initial &&
            !controller->GetCommandState(Undo{}).enabled,
        "Only active providers and valid decoration ranges repaint, preserving layout and history");

  ui.TapAt({20.0F, 24.0F});
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  ui.Pump(std::chrono::milliseconds(100));
  Check(*analysis_count == 1, "Selection and provisional IME text do not trigger decoration analysis");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});

  label.PerformSemanticAction({SemanticActionKind::Activate, {}});
  ui.Pump(std::chrono::milliseconds(220));
  ui.Find(UiSelector::Text("Script")).Tap();
  ui.Pump(std::chrono::milliseconds(220));
  ui.PumpUntil([&] { return ui.CaptureSnapshot().ToString().find("green=0.670588") != std::string::npos; });
  Check(controller->Document().Blocks().back().Data().fence_info == "script keep" &&
            ui.CaptureSnapshot().ToString().find("green=0.694118") == std::string::npos,
        "The custom menu keeps extra fence info and drops decorations for the previous snapshot");
  Check(controller->Execute(Undo{}) && controller->Document() == initial, "Custom language changes are undoable");
  ui.Pump();

  const auto bounds = ui.FindSemantics(UiSemanticSelector::Label("x^2")).One().bounds;
  ui.TapAt({bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F});
  Check(controller->SelectedCustomBlock() == LatexBlock("x^2"), "A tap selects the whole formula");
  Check(controller->Execute(ReplaceCustomBlock{LatexBlock("y^3"), LatexBlock("x^2")}) &&
            controller->SelectedCustomBlock() == LatexBlock("y^3"), "Editing a selected formula preserves selection");
  Check(controller->Execute(Undo{}) && controller->Document() == initial, "Formula source edits are undoable");
  ui.Pump();
  ui.PressKey(Key::Backspace);
  Check(controller->Document().Blocks().size() == 2 && controller->Execute(Undo{}) &&
            controller->Document() == initial, "Backspace removes the selected atomic block and undo restores it");
  ui.Pump();
  ui.PressKey(Key::ArrowRight);
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "中文"}});
  Check(controller->Document().Blocks()[1].Data().custom == LatexBlock("x^2") &&
            controller->Document().Blocks()[2].Data().text == "中文",
        "IME input after a formula opens a paragraph and leaves the formula intact");
  decorations.reset();
}

void TableEditing() {
  const Application app(App, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {220.0F, 380.0F}});
  controller->ReplaceDocument(RichDocument{});
  ui.Pump();
  ui.Find(UiSelector::Text("Focus")).Tap();
  Check(!controller->GetCommandState(InsertTable{0, 2}).enabled &&
            !controller->GetCommandState(InsertTable{4097, 1}).enabled,
        "Table insertion rejects invalid sizes");
  Check(controller->Execute(InsertTable{1, 1}) &&
            !controller->GetCommandState(DeleteTableRow{}).enabled &&
            !controller->GetCommandState(DeleteTableColumn{}).enabled,
        "Insert a table with one cell and protect the final row and column");
  Check(controller->Execute(DeleteTable{}) && controller->Document() == RichDocument{} &&
            controller->Execute(Undo{}),
        "Deleting the only table leaves an empty document and is reversible");

  const auto original = MarkdownCodec().Import("| A | B | C | D |\n| --- | --- | --- | --- |\n| X | Y | Z | W |\n");
  Check(original.document.has_value(), "Table editing fixture imports");
  controller->ReplaceDocument(*original.document);
  ui.Pump();
  const auto select = [&](TextOffset anchor, TextOffset active) {
    Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{anchor, active}}});
  };
  const auto cell = [](const RichDocument& document, std::size_t row, std::size_t column) -> const BlockData& {
    return document.Blocks()[0].Data().children[row].Data().children[column].Data().children.front().Data();
  };
  const auto active = [&] { return ui.ActiveTextInput()->selection.active; };

  Check(!controller->GetCommandState(InsertTableRow{TableInsertSide::Before}).enabled &&
            controller->GetCommandState(ToggleTableHeader{}).checked == CheckState::On,
        "The header stays in the first row");
  ui.PressKey(Key::Tab);
  Check(active() == 2, "Tab moves to the next cell");
  ui.PressKey(Key::ArrowDown);
  Check(active() == 10, "Down moves to the same column in the next row");
  ui.PressKey(Key::ArrowUp);
  ui.PressKey(Key::Tab, {.shift = true});
  Check(active() == 0, "Up and Shift+Tab return to the first cell");

  select(14, 14);
  ui.PressKey(Key::Tab);
  const auto appended = controller->Document();
  Check(appended.Blocks()[0].Data().children.size() == 3 && cell(appended, 2, 0).text.empty() && active() == 16,
        "Tab from the last cell adds a row and focuses its first cell");
  Check(controller->Execute(Undo{}) && controller->Document() == original.document, "Undo removes the appended row");
  ui.Pump();
  Check(active() == 14 && controller->Execute(Redo{}) && controller->Document() == appended,
        "Undo restores the original caret and redo restores the appended row");
  ui.Pump();
  Check(active() == 16 && controller->Execute(Undo{}), "Redo restores the caret to the appended row");
  ui.Pump();
  select(0, 0);

  const auto bounds = ui.Find(UiSelector::Key("editor")).One().bounds;
  const Point at_right{bounds.x + 178.0F, bounds.y + 30.0F};
  Check(ui.SendScroll({.position = at_right, .delta_x = 200.0F}).x > 0.0F,
        "Wheel scrolls the overflowing table");
  ui.Pump();
  ui.TapAt(at_right);
  Check(active() == 7, "A click after scrolling hits the fourth cell");
  ui.SendScroll({.position = at_right, .delta_x = -200.0F});
  ui.Pump();
  ui.Drag(at_right, {bounds.x + 30.0F, at_right.y});
  ui.TapAt(at_right);
  Check(active() == 7, "A horizontal touch drag scrolls the table");

  select(2, 2);
  ui.PressKey(Key::Backspace);
  Check(controller->Document() == original.document, "Backspace at a cell boundary preserves the table");
  select(1, 2);
  Check(!controller->GetCommandState(ToggleMark{Mark::Bold}).enabled &&
            !controller->GetCommandState(ToggleQuote{}).enabled &&
            Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "x"}}).result_code ==
                TextInputResultCode::Rejected,
        "Cross-cell text and formatting edits are rejected");
  select(10, 10);
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"}});
  Check(controller->Document() == original.document && !controller->GetCommandState(InsertTableRow{}).enabled,
        "Table structure remains stable during preedit");
  const auto header = controller->GetCommandState(ToggleTableHeader{});
  Check(!header.enabled && header.checked == CheckState::On, "Header state remains observable during preedit");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"}});
  const auto committed = controller->Document();
  Check(cell(committed, 1, 1).text == "你好Y" && cell(committed, 1, 2).text == "Z",
        "IME commit changes just one cell");
  Check(controller->Execute(Undo{}) && controller->Document() == original.document &&
            controller->Execute(Redo{}) && controller->Document() == committed,
        "Undo and redo restore one complete IME commit");
  ui.Pump();

  select(12, 10);
  Check(controller->Execute(ToggleMark{Mark::Bold}), "Format a backward cell selection");
  const auto formatted = controller->Document();
  Check(controller->Execute(InsertTableRow{}) &&
            controller->Execute(InsertTableColumn{TableInsertSide::Before}),
        "Insert a row and column");
  const auto expanded = controller->Document();
  Check(expanded.Blocks()[0].Data().children.size() == 3 && cell(expanded, 1, 2).text == "你好Y" &&
            cell(expanded, 1, 1).text.empty() && cell(expanded, 2, 2).text.empty(),
        "Grid expansion preserves the active cell and creates empty cells");
  Check(controller->Execute(DeleteTableColumn{}) && cell(controller->Document(), 1, 2).text == "Z" &&
            controller->Execute(Undo{}) && controller->Document() == expanded,
        "Column deletion and undo restore the selected cell");
  Check(controller->Execute(DeleteTableRow{}) && cell(controller->Document(), 1, 2).text.empty() &&
            controller->Execute(Undo{}) && controller->Document() == expanded &&
            controller->Execute(Undo{}) && controller->Execute(Undo{}) && controller->Document() == formatted,
        "Row deletion and undo restore the original grid and styles");
  ui.Pump();
  Check(ui.ActiveTextInput()->selection == TextSelection{12, 10}, "Undo restores the backward selection");

  const auto saved = JsonCodec().Export(formatted);
  Check(saved.text.has_value(), "Save the table to JSON");
  const auto reopened = JsonCodec().Import(*saved.text);
  Check(reopened.document == formatted, "Reopen JSON with formatted Chinese text and table structure");
  controller->ReplaceDocument(*reopened.document);
  ui.Pump();
  const auto strict = MarkdownCodec().Export(controller->Document());
  Check(strict.text && MarkdownCodec().Import(*strict.text).document == formatted, "Strict exports the edited table");
  Check(controller->Execute(DeleteTableRow{}) && !controller->Document().Blocks()[0].Data().table_has_header,
        "Deleting the header does not promote the next row");
  Check(!MarkdownCodec().Export(controller->Document()).text &&
            MarkdownCodec().Export(controller->Document(), LossPolicy::AllowLoss).text.has_value(),
        "Headerless Markdown export requires AllowLoss");
  Check(controller->Execute(Undo{}) && controller->Document() == formatted, "Undo restores the header");
  ui.Pump();
  ui.PressKey(Key::Enter);
  Check(cell(controller->Document(), 0, 0).text == "\nA" && controller->Document().Blocks().size() == 1,
        "Enter keeps the line break inside the cell");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "\n"}});
  const auto broken = controller->Document();
  Check(cell(broken, 0, 0).text == "\n\nA" && broken.Blocks().size() == 1,
        "IME Return inserts a newline in the current cell");
  ui.PressKey(Key::Enter, {.control = true});
  Check(controller->Document().Blocks().size() == 2 && controller->Execute(Undo{}) &&
            controller->Document() == broken,
        "Exit creates a paragraph and undo restores the grid");
}

void AppearanceChanges() {
  const Application app([]() -> View {
    static const auto initial = MarkdownCodec().Import(R"markdown(A `first second third fourth fifth sixth`

> First
>
> > Nested
>
> Last

~~~
code
~~~
)markdown");
    Check(initial.document.has_value(), "Appearance fixture imports");
    const auto editor = UseRichEditor(*initial.document);
    const auto focus = UseViewHandle();
    const auto style = UseState(RichEditorStyle{});
    controller = editor;
    appearance = style;
    return Column {
      Button("Focus").OnClick([focus] { Check(focus.RequestFocus(), "Style test can focus the editor"); }),
      RichEditor(editor, {.style = style.Get()}).With(focus, Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {210.0F, 380.0F}});
  ui.Find(UiSelector::Text("Focus")).Tap();
  const auto initial = controller->Document();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "Draft "}});
  const auto committed = controller->Document();
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  const auto before = ui.ActiveTextInput();
  appearance->Update([](RichEditorStyle& style) {
    style.background = Color::Rgb(17, 203, 223);
    style.body_font = Font::System(20.0F);
    style.content_padding = EdgeInsets::All(22.0F);
    style.quote_rule_width = 3.5F;
    style.inline_code_corner_radius = 3.0F;
    style.inline_code_horizontal_padding = 4.0F;
    style.code_block_corner_radius = 9.0F;
  });
  ui.Pump();

  const auto after = ui.ActiveTextInput();
  Check(before && after && before->session_id == after->session_id && before->selection == after->selection &&
            before->composition == after->composition && after->composition && controller->Document() == committed,
        "A style update preserves the input session, selection, preedit, and committed document");
  const auto render = ui.CaptureSnapshot().ToString();
  Check(render.find("green=0.796078") != std::string::npos && render.find("size=20.000000") != std::string::npos,
        "The mounted editor applies the viewport background and font overrides");
  const std::string_view inline_corner = "corner_radius=3.000000";
  const auto first_corner = render.find(inline_corner);
  Check(first_corner != std::string::npos &&
            render.find(inline_corner, first_corner + inline_corner.size()) != std::string::npos &&
            render.find("corner_radius=9.000000") != std::string::npos,
        "Wrapped inline code and fenced code use their configured corner radii");
  const std::string_view quote_corner = "corner_radius=1.750000";
  const auto outer_rule = render.find(quote_corner);
  Check(outer_rule != std::string::npos, "The quote uses its configured rule width");
  const auto inner_rule = render.find(quote_corner, outer_rule + quote_corner.size());
  Check(inner_rule != std::string::npos && render.find(quote_corner, inner_rule + quote_corner.size()) == std::string::npos,
        "Each quote has one continuous rule, including the nested quote");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});
  Check(controller->Execute(Undo{}) && controller->Document() == initial, "Style updates preserve undo history");
  appearance.reset();
}

void EditorShortcuts() {
  const Application app([]() -> View {
    const auto editor = UseRichEditor();
    const auto focus = UseViewHandle();
    const auto bindings = UseState(std::shared_ptr<const EditorKeymap>{});
    const auto query = UseState(TextEditingValue{});
    shortcut_keymap = bindings;
    controller = editor;
    return Column {
      Button("Focus editor").OnClick([focus] { Check(focus.RequestFocus(), "Focus the shortcut fixture"); }),
      TextField(query).Label("Search").OnChanged([query](const TextEditingValue& value) { query = value; }).Key("query"),
      RichEditor(editor, {.keymap = bindings.Get()}).With(focus, Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {480.0F, 340.0F}});
  ui.Find(UiSelector::Text("Focus editor")).Tap();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"},
              {.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{0, 2}}});
  const auto unformatted = controller->Document();
  const auto bold = ToggleMark{Mark::Bold};
  ui.PressKey(Key::B, {.meta = true});
  Check(controller->GetCommandState(bold).checked == CheckState::On, "Command+B is a default alias");
  const auto formatted = controller->Document();
  ui.PressKey(Key::Z, {.control = true});
  Check(controller->Document() == unformatted, "Control+Z undoes formatting");
  ui.PressKey(Key::Y, {.control = true});
  Check(controller->Document() == formatted, "Control+Y remains a redo alias");
  ui.PressKey(Key::Z, {.meta = true});

  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "zhongwen"}});
  const auto composing = ui.ActiveTextInput();
  *shortcut_keymap = std::make_shared<const EditorKeymap>(EditorKeymap{
      {{Key::J, {.control = true}}, bold},
      {{Key::K, {.control = true}}, Undo{}},
      {{Key::L, {.control = true}}, Redo{}},
  });
  ui.Pump();
  const auto updated = ui.ActiveTextInput();
  Check(composing && updated && composing->session_id == updated->session_id && composing->composition &&
            updated->composition == composing->composition && controller->Document() == unformatted,
        "Replacing shortcuts preserves the input session and Chinese preedit");
  ui.PressKey(Key::K, {.control = true});
  Check(!ui.ActiveTextInput()->composition && controller->Document() == unformatted,
        "Remapped Undo cancels the preedit without undoing committed text");
  ui.PressKey(Key::B, {.control = true});
  ui.PressKey(Key::B, {.meta = true});
  Check(controller->Document() == unformatted, "Rebinding removes both old bold aliases");
  ui.PressKey(Key::J, {.control = true});
  Check(controller->Document() == formatted, "The new key executes the existing formatting command");
  ui.PressKey(Key::K, {.control = true});
  Check(controller->Document() == unformatted, "Remapped Undo uses the existing history");
  ui.PressKey(Key::L, {.control = true});
  Check(controller->Document() == formatted, "Remapped Redo uses the existing history");

  ui.Find(UiSelector::Key("query")).Tap();
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "中文"}});
  ui.PressKey(Key::J, {.control = true});
  ui.PressKey(Key::K, {.control = true});
  Check(controller->Document() == formatted, "Editor shortcuts cannot act while a text field owns focus");
  ui.Find(UiSelector::Text("Focus editor")).Tap();
  *shortcut_keymap = std::make_shared<const EditorKeymap>();
  ui.Pump();
  ui.PressKey(Key::Z, {.control = true});
  ui.PressKey(Key::K, {.control = true});
  Check(controller->Document() == formatted, "An empty map clears configurable shortcuts");
  *shortcut_keymap = std::shared_ptr<const EditorKeymap>{};
  ui.Pump();
  ui.PressKey(Key::Z, {.control = true});
  Check(controller->Document() == unformatted, "Resetting the map restores defaults and preserves history");
  controller->ReplaceDocument(RichDocument{});
  ui.Pump();
  ui.PressKey(Key::T, {.shift = true, .meta = true});
  ui.PressKey(Key::R, {.shift = true, .control = true});
  ui.PressKey(Key::Backslash, {.shift = true, .control = true});
  const auto table = controller->Document();
  Check(table.Blocks()[0].Data().kind == BlockKind::Table && table.Blocks()[0].Data().children.size() == 3 &&
            table.Blocks()[0].Data().children[0].Data().children.size() == 3,
        "Default table shortcuts insert a table, row, and column through the editor");
  shortcut_keymap.reset();
}

void InlineCodePadding() {
  const Application app([]() -> View {
    static const auto initial = MarkdownCodec().Import("A`中🙂`.\n");
    const auto editor = UseRichEditor(*initial.document);
    const auto style = UseState(RichEditorStyle{.inline_code_corner_radius = 7.0F});
    controller = editor;
    appearance = style;
    return RichEditor(editor, {.style = style.Get()});
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {240.0F, 160.0F}});
  const auto code_bounds = [&] {
    const auto render = ui.CaptureSnapshot().ToString();
    const auto radius = render.find("corner_radius=7.000000");
    Check(radius != std::string::npos, "Inline code background is drawn");
    const auto field = [&](std::string_view name) {
      const auto offset = render.rfind(name, radius);
      Check(offset != std::string::npos, "Inline code background bounds are available");
      return std::stof(render.substr(offset + name.size()));
    };
    return Rect{field("x="), field("y="), field("width="), field("height=")};
  };
  const auto initial = controller->Document();
  const auto unpadded = code_bounds();
  appearance->Update([](RichEditorStyle& style) { style.inline_code_horizontal_padding = 4.0F; });
  ui.Pump();
  const auto padded = code_bounds();
  Check(padded.width > unpadded.width && padded.height == unpadded.height,
        "Inline code padding reserves horizontal space");

  ui.TapAt({padded.x + 1.0F, padded.y + padded.height / 2.0F});
  const auto active = [&] { return ui.ActiveTextInput()->selection.active; };
  Check(active() == 1, "Tapping the leading inset places the caret at the code start");
  ui.PressKey(Key::ArrowRight);
  Check(active() == 2, "The first code character follows its leading inset");
  ui.PressKey(Key::ArrowRight);
  Check(active() == 4, "The emoji and trailing inset do not add caret stops");
  ui.PressKey(Key::ArrowRight);
  Check(active() == 5, "Arrow keys reach adjacent punctuation");
  ui.PressKey(Key::Home);
  Check(active() == 0, "Home uses document offsets");
  ui.PressKey(Key::End);
  Check(active() == 5, "End uses document offsets");
  ui.TapAt({padded.x + padded.width - 1.0F, padded.y + padded.height / 2.0F});
  Check(active() == 4, "Tapping the trailing inset places the caret at the code end");
  ui.PressKey(Key::ArrowLeft);
  Check(active() == 2, "Left skips the inset and crosses the emoji in one move");
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  Check(ui.ActiveTextInput()->composition == TextRange{2, 4} && controller->Document() == initial,
        "Inline padding stays out of the IME range and document");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你"}});
  Check(controller->Document().Blocks().front().Data().text == "A中你🙂." &&
            controller->Execute(Undo{}) && controller->Document() == initial,
        "IME commit and Undo use the original UTF-16 offsets");
  appearance.reset();
}

void ImageWorkflow() {
  constexpr unsigned char png[]{
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x7b, 0x40, 0xe8, 0xdd, 0x00, 0x00, 0x00,
      0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x98, 0x99, 0x66, 0xec,
      0xd5, 0xa2, 0x0f, 0x00, 0x08, 0x7c, 0x02, 0x30, 0xa0, 0x6b, 0x09, 0x54,
      0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  const auto asset = ImageAsset::CopyEncoded(std::as_bytes(std::span(png)), 0.05F);
  const auto moved_asset = ImageAsset::CopyEncoded(std::as_bytes(std::span(png)), 0.025F);
  std::vector<std::string> loads;
  const auto resolver_for = [&loads](ImageAsset image) {
    return std::make_shared<const ImageResolver>([&loads, image](std::string source) -> Task<IoResult<ImageAsset>> {
      loads.push_back(source);
      if (source == "attachments/missing.jpg") {
        co_return IoResult<ImageAsset>::Failure({IoErrorCode::NotFound, "Missing image"});
      }
      co_return IoResult<ImageAsset>::Success(image);
    });
  };
  initial_image_resolver = resolver_for(asset);
  const auto moved_resolver = resolver_for(moved_asset);
  const Application app([]() -> View {
    const auto editor = UseRichEditor();
    const auto focus = UseViewHandle();
    const auto resolver = UseState(initial_image_resolver);
    const auto directory = UseState(std::optional<File>{});
    image_resolver_state = resolver;
    image_directory_state = directory;
    controller = editor;
    return Column {
      Button("Focus").OnClick([focus] { focus.RequestFocus(); }),
      RichEditor(editor, {.document_directory = directory.Get(), .extensions = {.image_resolver = resolver.Get()}})
          .With(focus, Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {420, 340}});
  ui.Find(UiSelector::Text("Focus")).Tap();
  const ImageBlockData photo{"attachments/photo.png", "现场照片", "Photo"};
  Check(controller->Execute(InsertImage{photo}), "Insert a standalone image");
  const auto image_only = controller->Document();
  Check(image_only.Blocks().size() == 1 && image_only.Blocks()[0].Data().image == photo &&
            MarkdownCodec().Export(image_only).text.has_value(),
        "Inserting an image does not persist an empty paragraph");

  const auto image = ui.FindSemantics(UiSemanticSelector::Label("现场照片"));
  const auto width_is = [&](float width) { return image.Exists() && std::abs(image.One().bounds.width - width) < 0.1F; };
  ui.PumpUntil([&] { return width_is(40.0F); });
  const auto bounds = image.One().bounds;
  ui.TapAt({bounds.x + bounds.width / 2, bounds.y + bounds.height / 2});
  Check(controller->GetCommandState(DeleteImage{}).enabled, "A pointer selects the whole image");
  Check(controller->Execute(ReplaceImage{"attachments/other.jpg"}) &&
            controller->Document().Blocks()[0].Data().image->source == "attachments/other.jpg" &&
            controller->Execute(Undo{}) && controller->Document() == image_only,
        "Undoing replacement restores the reference and image selection");

  ui.Pump();
  ui.PressKey(Key::ArrowRight);
  Check(ui.ActiveTextInput()->selection.active == 1, "Caret can rest after the image");
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .selection_after = TextSelection{3, 3},
               .text = "ni"}});
  Check(controller->Document() == image_only && ui.ActiveTextInput()->composition == TextRange{2, 4} &&
            ui.ActiveTextInput()->selection.active == 4,
        "Preedit after an image accounts for the newly inserted paragraph separator");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});
  Check(controller->Document() == image_only && ui.ActiveTextInput()->selection.active == 1,
        "Cancel restores the image boundary and discards the provisional paragraph");
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"}});
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"}});
  const auto edited = controller->Document();
  Check(edited.Blocks().size() == 2 && edited.Blocks()[0].Data().image == photo &&
            edited.Blocks()[1].Data().text == "你好" && controller->Execute(Undo{}) &&
            controller->Document() == image_only && controller->Execute(Redo{}) && controller->Document() == edited,
        "Committing text after the image and creating its paragraph are one undo step");

  const auto saved = JsonCodec().Export(edited);
  Check(saved.text.has_value(), "Save the image and paragraph to JSON");
  const auto opened = JsonCodec().Import(*saved.text);
  Check(opened.document == edited, "Reopen the saved reference and text");
  *image_resolver_state = moved_resolver;
  controller->ReplaceDocument(*opened.document);
  ui.PumpUntil([&] { return width_is(80.0F); });
  Check(std::ranges::count(loads, photo.source) >= 2, "Opening another root reloads a matching image reference");
  const auto markdown = MarkdownCodec().Export(controller->Document());
  Check(markdown.text && MarkdownCodec().Import(*markdown.text).document == edited,
        "Strict Markdown preserves the reopened image and following text");

  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{0, 1}}});
  Check(controller->Execute(ReplaceImage{"attachments/missing.jpg"}), "Keep an unavailable reference editable");
  ui.PumpUntil([&] { return image.Exists() && image.One().bounds.width > 100.0F; });
  ui.PressKey(Key::Backspace);
  Check(controller->Document().Blocks().size() == 1 && controller->Document().Blocks()[0].Data().text == "你好" &&
            controller->Execute(Undo{}), "Deleting the selected image preserves the paragraph");

  controller->ReplaceDocument(*MarkdownCodec().Import("**AB**").document);
  ui.Pump();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{1, 1}}});
  Check(controller->Execute(InsertImage{photo}), "Insert an image inside a paragraph");
  const auto split = controller->Document();
  Check(split.Blocks().size() == 3 && split.Blocks()[0].Data().text == "A" &&
            split.Blocks()[0].Data().marks == std::vector<MarkRange>{{{0, 1}, Mark::Bold}} &&
            split.Blocks()[2].Data().text == "B" &&
            split.Blocks()[2].Data().marks == std::vector<MarkRange>{{{0, 1}, Mark::Bold}},
        "Inserting an image preserves the formatting on both sides of a split");

  controller->ReplaceDocument(image_only);
  ui.PumpUntil([&] { return width_is(80.0F); });
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{1, 0}}});
  Check(controller->Execute(InsertImage{{"attachments/other.jpg", "Second photo", ""}}) &&
            controller->Document().Blocks()[0].Data().image == photo,
        "Inserting with a backward image selection adds the next image after it");
  const auto second = ui.FindSemantics(UiSemanticSelector::Label("Second photo"));
  ui.PumpUntil([&] { return second.Exists() && second.One().bounds.width == 80.0F; });
  auto first_bounds = image.One().bounds;
  auto second_bounds = second.One().bounds;
  Check(first_bounds.y == second_bounds.y && first_bounds.x + first_bounds.width < second_bounds.x,
        "Adjacent images that fit share a line");
  ui.TapAt({first_bounds.x + first_bounds.width / 2, first_bounds.y + first_bounds.height / 2});
  Check(controller->SelectedImageSize() == Size{80.0F, 40.0F} &&
            controller->Execute(ResizeImage{180.0F}) && controller->SelectedImageSize() == Size{180.0F, 90.0F},
        "An image width command uses the resolved aspect ratio and leaves the image selected");
  ui.PumpUntil([&] { return width_is(180.0F); });
  second_bounds = second.One().bounds;
  ui.TapAt({second_bounds.x + second_bounds.width / 2, second_bounds.y + second_bounds.height / 2});
  Check(ui.ActiveTextInput()->selection.Range() == TextRange{2, 3},
        "Tapping the second image on the same line selects its own block");
  const Point handle{second_bounds.x + second_bounds.width, second_bounds.y + second_bounds.height};
  const auto before_resize = controller->Document();
  ui.Drag(handle, handle + Point{80.0F, 40.0F});
  const auto after_resize = controller->Document();
  Check(after_resize.Blocks()[1].Data().image->width == 160.0F &&
            second.One().bounds.height == 80.0F && controller->Execute(Undo{}) &&
            controller->Document() == before_resize && controller->Execute(Redo{}) &&
            controller->Document() == after_resize,
        "A touch drag resizes proportionally and records one undo entry");
  ui.Pump();
  second_bounds = second.One().bounds;
  PointerEvent pointer{.type = PointerEventType::Down, .pointer_id = 12,
                       .position = {second_bounds.x + second_bounds.width, second_bounds.y + second_bounds.height},
                       .changed_button = PointerButton::Primary, .pressed_buttons = PointerButton::Primary};
  ui.SendPointer(pointer);
  pointer.type = PointerEventType::Move;
  pointer.changed_button = PointerButton::None;
  pointer.position = pointer.position - Point{40.0F, 20.0F};
  ui.SendPointer(pointer);
  ui.Pump();
  Check(second.One().bounds.width == 120.0F && controller->Document() == after_resize,
        "Mouse drag previews the width without mutating the committed document");
  pointer.type = PointerEventType::Cancel;
  pointer.pressed_buttons = PointerButton::None;
  ui.SendPointer(pointer);
  ui.Pump();
  Check(second.One().bounds.width == 160.0F && controller->Document() == after_resize,
        "Canceling a drag restores the committed width");

  const auto sized_json = JsonCodec().Export(after_resize);
  controller->ReplaceDocument(*JsonCodec().Import(*sized_json.text).document);
  ui.SetWindowMetrics({.viewport = {290.0F, 340.0F}});
  ui.PumpUntil([&] { return image.Exists() && second.Exists() && second.One().bounds.y > image.One().bounds.y; });
  Check(image.One().bounds.x == second.One().bounds.x && second.One().bounds.width == 160.0F &&
            controller->Document() == after_resize,
        "Reopening at a narrow width wraps images while retaining their preferred widths");

  const auto fixture = File(std::filesystem::temp_directory_path().u8string()).Child(
      "richeditor-images-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto root = fixture.Child("article");
  const auto attachments = root.Child("attachments");
  const auto photo_file = attachments.Child("photo local.png");
  Check(attachments.CreateDirectories() && photo_file.WriteBytes(std::as_bytes(std::span(png))) &&
            fixture.Child("photo.png").WriteBytes(std::as_bytes(std::span(png))),
        "Create a local attachment fixture");
  *image_directory_state = root;
  const auto local = MarkdownCodec().Import("![Local](<attachments/photo%20local.png>)\n");
  Check(local.document.has_value(), "Relative image fixture imports");
  controller->ReplaceDocument(*local.document);
  const auto local_image = ui.FindSemantics(UiSemanticSelector::Label("Local"));
  ui.PumpUntil([&] { return local_image.Exists() && local_image.One().bounds.width == 80.0F; });
  *image_resolver_state = std::shared_ptr<const ImageResolver>{};
  const auto local_loaded = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return local_image.Exists() && local_image.One().bounds.width == 2.0F;
  };
  ui.PumpUntil(local_loaded);
  const auto outside = MarkdownCodec().Import("![Outside](<%2E%2E/photo.png>)\n");
  controller->ReplaceDocument(*outside.document);
  ui.Pump();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  ui.Pump();
  Check(ui.FindSemantics(UiSemanticSelector::Label("Outside")).One().bounds.width > 2.0F,
        "A parent-directory reference stays a placeholder");
  const auto moved = fixture.Child("moved");
  Check(root.MoveTo(moved), "Move the document directory");
  *image_directory_state = moved;
  const auto reopened = JsonCodec().Import(*JsonCodec().Export(*local.document).text);
  controller->ReplaceDocument(*reopened.document);
  ui.PumpUntil(local_loaded);
  const auto& inline_codec = MarkdownCodec({.inline_images = true});
  const auto inline_doc = inline_codec.Import(
      "| Image | Note |\n| --- | --- |\n| A ![Inline local](<attachments/photo%20local.png>) | B |\n");
  Check(inline_doc.document.has_value(), "Inline image in a table imports");
  const auto inline_json = JsonCodec().Export(*inline_doc.document);
  controller->ReplaceDocument(*JsonCodec().Import(*inline_json.text).document);
  const auto inline_image = ui.FindSemantics(UiSemanticSelector::Label("Inline local"));
  ui.PumpUntil([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return inline_image.Exists() && inline_image.One().bounds.width == 2.0F;
  });
  Check(inline_codec.Export(controller->Document()).text.has_value(),
        "A table inline image reopens from a moved attachment directory and exports strictly");
  Check(fixture.DeleteRecursively(), "Clean up the attachment fixture");
  image_directory_state.reset();
  image_resolver_state.reset();
  initial_image_resolver.reset();
}

void IncrementalBlockDecorations() {
  using Providers = std::vector<std::shared_ptr<const DecorationProvider>>;
  static std::optional<State<Providers>> providers;
  static std::vector<std::vector<std::string>> batches;
  static unsigned retired_starts = 0;
  static unsigned retired_completions = 0;
  static unsigned replacement_starts = 0;
  batches.clear();
  retired_starts = retired_completions = replacement_starts = 0;
  static const auto provider = std::make_shared<const DecorationProvider>(DecorationProvider{
      .analyze = [](DecorationRequest request) -> Task<std::vector<TextDecorationRange>> {
        Check(!request.document, "Block scope analyzes content without a document snapshot");
        std::vector<std::string> texts;
        std::vector<TextDecorationRange> ranges;
        for (std::size_t index = 0; index < request.blocks.size(); ++index) {
          const auto& text = request.blocks[index].block.Data().text;
          texts.push_back(text);
          if (text == "alpha" || text == "Alpha" || text == "beta") {
            const auto green = text == "alpha" ? 177 : text == "Alpha" ? 171 : 160;
            ranges.push_back({index, {0, 1}, DecorationKind::Background, Color::Rgb(17, green, 223)});
          }
        }
        batches.push_back(std::move(texts));
        co_await Delay(std::chrono::milliseconds(250));
        co_return ranges;
      },
  });
  static const auto retired = std::make_shared<const DecorationProvider>(DecorationProvider{
      .scope = DecorationScope::Document,
      .analyze = [](DecorationRequest) -> Task<std::vector<TextDecorationRange>> {
        ++retired_starts;
        co_await Delay(std::chrono::milliseconds(1000));
        ++retired_completions;
        co_return std::vector<TextDecorationRange>{};
      },
  });
  const auto replacement = std::make_shared<const DecorationProvider>(DecorationProvider{
      .analyze = [](DecorationRequest) -> Task<std::vector<TextDecorationRange>> {
        ++replacement_starts;
        co_return std::vector<TextDecorationRange>{};
      },
  });
  const Application app([]() -> View {
    static const auto initial = MarkdownCodec().Import("alpha\n\nbeta\n\ngamma\n");
    const auto editor = UseRichEditor(*initial.document);
    controller = editor;
    const auto state = UseState(Providers{provider, retired});
    providers = state;
    return RichEditor(editor, {.extensions = {.decorations = state.Get()}});
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {320.0F, 440.0F}});
  ui.PumpUntil([] { return batches.size() == 1 && retired_starts == 1; });
  *providers = Providers{replacement, provider};
  ui.PumpUntil([] { return replacement_starts == 1; });
  ui.TapAt({20.0F, 24.0F});
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{0, 1}, .text = "A"}});
  ui.PumpUntil([] { return batches.size() == 2; });
  Check(batches == std::vector<std::vector<std::string>>{{"alpha", "beta", "gamma"}, {"Alpha"}},
        "Replacing and reordering providers preserves a pending batch and sends only the edited block again");
  const auto pending = ui.CaptureSnapshot().ToString();
  Check(pending.find("green=0.627451") != std::string::npos && pending.find("green=0.694118") == std::string::npos,
        "Unchanged decorations stay visible while obsolete decorations are discarded");
  ui.PumpUntil([&] { return ui.CaptureSnapshot().ToString().find("green=0.670588") != std::string::npos; });

  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{5, 5}, .text = "\nnew"}});
  ui.PumpUntil([] { return batches.size() == 3; });
  Check(batches.back() == std::vector<std::string>{"new"} && controller->Document().Blocks()[2].Data().text == "beta",
        "A new paragraph preserves cached decorations when later blocks move, including empty results");
  Check(ui.CaptureSnapshot().ToString().find("green=0.627451") != std::string::npos,
        "A moved block keeps its decoration");
  *providers = Providers{provider};
  ui.Pump(std::chrono::milliseconds(1000));
  Check(retired_completions == 0 && batches.size() == 3 &&
            ui.CaptureSnapshot().ToString().find("green=0.627451") != std::string::npos,
        "Only a removed provider's request is canceled and the remaining provider keeps its painted cache");

  Check(controller->Execute(Undo{}), "Undo the inserted paragraph");
  ui.Pump(std::chrono::milliseconds(300));
  Check(batches.size() == 3, "Undo removes only the inserted paragraph's cache entry");
  Check(controller->Execute(Redo{}), "Redo the inserted paragraph");
  ui.PumpUntil([] { return batches.size() == 4; });
  Check(batches.back() == std::vector<std::string>{"new"}, "Redo analyzes only the restored paragraph");

  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .target = TextRange{0, 1}, .text = "zhong"}});
  ui.Pump(std::chrono::milliseconds(300));
  Check(batches.size() == 4, "Provisional IME text stays out of the incremental queue");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "中"}});
  ui.PumpUntil([] { return batches.size() == 5; });
  Check(batches.back() == std::vector<std::string>{"中lpha"}, "IME commit analyzes only the affected paragraph");
  ui.Pump(std::chrono::milliseconds(300));

  Check(Send(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{0, 1}, .text = "A"}}).result_code ==
            TextInputResultCode::Ok &&
            Send(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{10, 11}, .text = "B"}}).result_code ==
                TextInputResultCode::Ok,
        "Commit to two paragraphs before the next frame");
  ui.PumpUntil([] { return batches.size() == 6; });
  Check(batches.back() == std::vector<std::string>{"Alpha", "Beta"},
        "Coalesced commits analyze both changed blocks in document order");

  controller->ReplaceDocument(*MarkdownCodec().Import("| A | B |\n| --- | --- |\n| C | D |\n").document);
  ui.PumpUntil([] { return batches.size() == 7; });
  Check(batches.back() == std::vector<std::string>{"A", "B", "C", "D"}, "Opening a document initializes its cache");
  ui.Pump(std::chrono::milliseconds(300));
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{2, 2}}});
  Check(controller->Execute(DeleteTableColumn{}), "Delete the second table column");
  ui.Pump(std::chrono::milliseconds(300));
  Check(batches.size() == 7 && controller->Execute(Undo{}), "Removing a column leaves other cells cached");
  ui.PumpUntil([] { return batches.size() == 8; });
  Check(batches.back() == std::vector<std::string>{"B", "D"}, "Undo analyzes only the restored table cells");

  controller->ReplaceDocument(*MarkdownCodec().Import("![image](example.png)\n\nbeta\n").document);
  ui.PumpUntil([] { return batches.size() == 9; });
  Check(batches.back() == std::vector<std::string>{"beta"}, "Analysis skips atomic blocks");
  ui.Pump(std::chrono::milliseconds(300));
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{0, 1}, .text = "replaced"}});
  ui.PumpUntil([] { return batches.size() == 10; });
  Check(batches.back() == std::vector<std::string>{"replaced"}, "An atomic block converted to text enters the queue");
  providers.reset();
}

void InlineObjectWorkflow() {
  using namespace huxerui::richeditor::detail;
  static std::optional<InlineObjectSelection> activation;
  static unsigned renders = 0;
  activation.reset();
  renders = 0;
  const auto& markdown = folio::DocumentMarkdown();
  const Application app([]() -> View {
    static const auto initial = folio::DocumentMarkdown().Import("A🙂 $x^2$ 中文 with `code`.\n\nOther.").document;
    static const auto renderer = std::make_shared<const InlineObjectRenderer>(
        [](InlineObjectData data, InlineObjectRenderContext context) -> Task<std::optional<InlineObjectGraphic>> {
          ++renders;
          co_await Delay(std::chrono::milliseconds(45));
          auto asset = VectorAsset::Create(Size{24, 15}, [](VectorBuilder&) {});
          co_return InlineObjectGraphic{std::move(asset), 24, 11, 4, "Math " + data.payload};
        });
    const auto editor = UseRichEditor(*initial);
    controller = editor;
    return RichEditor(editor, {.style = {.inline_code_horizontal_padding = 4.0F},
                               .extensions = {.inline_objects = {{std::string(latex_type), renderer}}}})
        .On<RichEditorEvents::InlineObjectActivated>([](const InlineObjectSelection& event) {
          activation = event;
        });
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {300.0F, 360.0F}});
  const auto initial = controller->Document();
  ui.TapAt({20.0F, 24.0F});
  const auto resolved = ui.FindSemantics(UiSemanticSelector::Label("Math x^2"));
  ui.PumpUntil([&] { return resolved.Exists(); });
  Check(renders == 1, "Resolve one mounted object");
  resolved.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Check(activation && activation->data == LatexInline("x^2") &&
            ui.ActiveTextInput()->selection.Range() == TextRange{4, 5},
        "Typed activation selects exactly one UTF-16 object");
  const auto original = *activation;
  const auto fragment = controller->SelectedFragment();
  Check(fragment && fragment->content.Blocks()[0].Data().inline_objects.size() == 1 &&
            PlainTextSlice(initial, {4, 5}) == "x^2" &&
            TextSlice(initial, {4, 5}) == object_replacement,
        "Rich copy retains the object and plain copy differs from the IME projection");

  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "zhong"}});
  Check(controller->Document() == initial, "IME preedit does not publish the replaced object");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});
  const auto restored = controller->SelectedInlineObject();
  Check(restored && restored->data == original.data, "Cancel restores the selected object");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{4, 4}, .text = "中"}});
  Check(renders == 1, "Typing before an object reuses its renderer");
  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{5, 5}, .text = "\n"}});
  const auto split = controller->Document();
  Check(split.Blocks().size() == 3 && split.Blocks()[1].Data().inline_objects.size() == 1 &&
            split.Blocks()[1].Data().inline_objects.front().offset == 0 &&
            controller->Execute(ReplaceInlineObject{original.target, LatexInline("x^3"), original.data}),
        "The captured target can replace the object after a paragraph split");
  ui.Pump();
  Check(!controller->Execute(ReplaceInlineObject{original.target, LatexInline("wrong"), original.data}),
        "An expected value rejects a stale edit of the same object");

  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{0, 0}}});
  Check(controller->Execute(InsertFragment{*fragment}), "Paste the rich fragment into another paragraph");
  ui.Pump();
  const auto pasted = controller->Document();
  Check(pasted.Blocks()[0].Data().inline_objects.size() == 1 && pasted.Blocks()[1].Data().inline_objects.size() == 1,
        "Both the original and the pasted object are present");
  const auto& first = pasted.Blocks()[0].Data().inline_objects.front();
  const auto& second = pasted.Blocks()[1].Data().inline_objects.front();
  Check(first.data == LatexInline("x^2") && second.data == LatexInline("x^3") &&
            DocumentAccess::Id(first) != DocumentAccess::Id(second),
        "Paste preserves payload and gives the pasted object a new identity");

  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .target = TextRange{0, 8}}});
  Check(controller->Document().Blocks()[0].Data().inline_objects.empty() &&
            controller->Execute(Undo{}) && controller->Document() == pasted,
        "Cross-paragraph deletion removes both objects and undo restores them");
  ui.Pump();
  const auto json = JsonCodec().Export(controller->Document());
  const auto reopened = JsonCodec().Import(*json.text);
  Check(reopened.document == pasted && markdown.Export(*reopened.document).text,
        "Reopened JSON strictly exports the edited inline formulas");
  controller->ReplaceDocument(*reopened.document);
  Check(!controller->Execute(ReplaceInlineObject{original.target, LatexInline("expired")}),
        "Opening a new document expires captured targets");
  controller->ReplaceDocument(RichDocument::Create({Block(BlockData{
      .text = std::string(object_replacement),
      .inline_objects = {{0, InlineObjectData{"folio.mention", 1, "42", "Unknown person"}}},
  })}));
  ui.PumpUntil([&] { return ui.FindSemantics(UiSemanticSelector::Label("Unknown person")).Exists(); });
  activation.reset();
}

void TallImageScrolling() {
  constexpr unsigned char png[]{
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x2c, 0xe9, 0x40, 0x00, 0x00, 0x00, 0x00,
      0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x98, 0x99, 0x66, 0xcc,
      0x40, 0x02, 0x06, 0x00, 0x5d, 0x47, 0x0e, 0x59, 0x69, 0x68, 0xd0, 0xd5,
      0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  const auto asset = ImageAsset::CopyEncoded(std::as_bytes(std::span(png)), 0.01F);
  initial_image_resolver = std::make_shared<const ImageResolver>([asset](std::string) -> Task<IoResult<ImageAsset>> {
    co_return IoResult<ImageAsset>::Success(asset);
  });
  const Application app([]() -> View {
    const auto editor = UseRichEditor();
    const auto focus = UseViewHandle();
    controller = editor;
    return Column {
      Button("Focus").OnClick([focus] { focus.RequestFocus(); }),
      RichEditor(editor, {.extensions = {.image_resolver = initial_image_resolver}}).With(focus, Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {400.0F, 340.0F}});
  ui.Find(UiSelector::Text("Focus")).Tap();
  Check(controller->Execute(InsertImage{{"attachments/tall.png", "Tall photo", ""}}), "Insert the tall image");
  const auto image = ui.FindSemantics(UiSemanticSelector::Label("Tall photo"));
  ui.PumpUntil([&] { return image.Exists() && image.One().bounds.y < -600.0F; });
  const auto bottom_y = image.One().bounds.y;

  ui.SendScroll({.position = {200.0F, 140.0F}, .delta_y = -40.0F});
  ui.Pump();
  ui.Pump(std::chrono::milliseconds(100));
  Check(image.One().bounds.y > bottom_y + 30.0F, "Wheel scrolling away from the caret survives later frames");
  const float wheel_y = image.One().bounds.y;
  ui.Drag({200.0F, 130.0F}, {200.0F, 210.0F}, {.duration = std::chrono::milliseconds(500), .segments = 20});
  Check(image.One().bounds.y > wheel_y + 50.0F && ui.ActiveTextInput()->selection == TextSelection{1, 1},
        "A touch drag scrolls back without moving the caret");

  Accept(ui, {{.kind = TextInputCommandKind::CommitText, .text = "继续"}});
  ui.PumpUntil([&] { return image.One().bounds.y < bottom_y; });
  Check(controller->Document().Blocks()[1].Data().text == "继续", "Typing reveals the caret below the image again");

  ui.SendScroll({.position = {60.0F, 160.0F}, .delta_y = -120.0F});
  ui.Pump();
  const float browsed_y = image.One().bounds.y;
  ui.TapAt({60.0F, 160.0F});
  ui.Pump(std::chrono::milliseconds(100));
  Check(ui.ActiveTextInput()->selection.Range() == TextRange{0, 1} && image.One().bounds.y == browsed_y,
        "Selecting a tall image leaves the visible portion in place");
  initial_image_resolver.reset();
}

void RevealTextRange() {
  static const auto document = [] {
    std::string markdown;
    for (int i = 0; i < 40; ++i) {
      markdown += i == 20 ? "First match line  \nsecond match line\n\n" : "A paragraph of text\n\n";
    }
    markdown += "尾行🙂 target\n";
    return *MarkdownCodec().Import(markdown).document;
  }();
  const Application app([]() -> View {
    const auto editor = UseRichEditor(document);
    const auto editor_focus = UseViewHandle();
    const auto query_focus = UseViewHandle();
    const auto query = UseState(TextEditingValue{});
    controller = editor;
    return Column {
      Button("Focus editor").OnClick([editor_focus] { editor_focus.RequestFocus(); }),
      Button("Focus query").OnClick([query_focus] { query_focus.RequestFocus(); }),
      TextField(query).Label("Find").With(query_focus),
      RichEditor(editor).With(editor_focus, Grow(), Semantics{.identifier = "reveal-editor"}),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  }, {.show_debug_overlay = false});
  UiTest ui(app, {.viewport = {420.0F, 360.0F}});
  const auto target = controller->Document().Blocks().back();
  const auto editor = ui.FindSemantics(UiSemanticSelector::Identifier("reveal-editor"));
  const auto scroll = editor.Find(UiSemanticSelector::Role(SemanticRole::ScrollView));
  ui.Find(UiSelector::Text("Focus editor")).Tap();
  Accept(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{3, 0}}});
  Accept(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "ni"}});
  Check(!controller->RevealTextRange(target, {5, 11}), "Reveal leaves an active IME composition alone");
  Accept(ui, {{.kind = TextInputCommandKind::CancelComposition}});

  ui.Find(UiSelector::Text("Focus query")).Tap();
  const auto query_session = ui.ActiveTextInput();
  Check(query_session && !editor.One().focused, "The query field owns the text input session");
  Check(!controller->RevealTextRange(target, {3, 4}), "Reveal rejects a range that splits a surrogate pair");
  Check(controller->RevealTextRange(target, {5, 11}), "Reveal accepts a range in an offscreen block");
  ui.Pump();
  Check(scroll.One().scroll->offset > 0.0F, "Reveal scrolls the target into the viewport");
  const auto& middle = document.Blocks()[20];
  const TextRange lines{0, static_cast<TextOffset>(middle.Data().text.size())};
  const auto reveal_at = [&](const Block& block, TextRange range, ScrollAlignment alignment) {
    Check(controller->RevealTextRange(block, range, alignment), "An explicit range alignment is accepted");
    ui.Pump();
    return *scroll.One().scroll;
  };
  const auto start = reveal_at(middle, lines, ScrollAlignment::Start);
  const auto center = reveal_at(middle, lines, ScrollAlignment::Center);
  const auto end = reveal_at(middle, lines, ScrollAlignment::End);
  const auto first_line = reveal_at(middle, {0, 1}, ScrollAlignment::End);
  Check(start.offset > center.offset && center.offset > end.offset &&
            std::abs(2 * center.offset - start.offset - end.offset) < 0.1F && end.offset > first_line.offset,
        "The complete multi-line range aligns to the top, center, and bottom");
  const auto last_start = reveal_at(target, {5, 11}, ScrollAlignment::Start);
  const auto first_end = reveal_at(document.Blocks().front(), {0, 1}, ScrollAlignment::End);
  Check(last_start.offset == last_start.maximum_offset && first_end.offset == 0.0F,
        "Explicit alignment clamps to either end of the document");
  const auto centered = reveal_at(middle, lines, ScrollAlignment::Center);
  ui.SetWindowMetrics({.viewport = {420.0F, 460.0F}});
  ui.Pump();
  const auto expanded = *scroll.One().scroll;
  Check(std::abs((centered.offset + centered.viewport_extent / 2) -
                 (expanded.offset + expanded.viewport_extent / 2)) < 0.1F,
        "Center alignment follows a viewport resize");
  const auto bounds = scroll.One().bounds;
  ui.SendScroll({.position = {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}, .delta_y = 36});
  ui.Pump();
  const auto browsed = scroll.One().scroll->offset;
  ui.SetWindowMetrics({.viewport = {420.0F, 400.0F}});
  ui.Pump();
  Check(browsed != expanded.offset && scroll.One().scroll->offset == browsed,
        "Manual scrolling takes over from explicit alignment");
  Check(ui.ActiveTextInput()->session_id == query_session->session_id && editor.One().text_selection == TextRange{0, 3},
        "Reveal preserves input focus and the editor selection");
  controller->ReplaceDocument(RichDocument{});
  ui.Pump();
  Check(!controller->RevealTextRange(target, {5, 11}), "Reveal rejects a block from a replaced document");
}

} // namespace

int main() {
  try {
    IncrementalDocumentIndex();
    Workflow();
    controller.reset();
    HeadingAndLinkEditing();
    LinkActivation();
    controller.reset();
    ListEditing();
    controller.reset();
    ListSelectionScope();
    controller.reset();
    ListMarkerOwnership();
    controller.reset();
    ListNumbering();
    controller.reset();
    DeleteSurrounding();
    controller.reset();
    LineEditing();
    controller.reset();
    NestedEmptyListEditing();
    controller.reset();
    CodeEditing();
    controller.reset();
    CodeHighlighting();
    controller.reset();
    CodeLanguagePicker();
    controller.reset();
    PresentationExtensions();
    controller.reset();
    InlineObjectWorkflow();
    controller.reset();
    IncrementalBlockDecorations();
    controller.reset();
    TableEditing();
    controller.reset();
    AppearanceChanges();
    controller.reset();
    EditorShortcuts();
    controller.reset();
    InlineCodePadding();
    controller.reset();
    ImageWorkflow();
    controller.reset();
    TallImageScrolling();
    controller.reset();
    RevealTextRange();
    controller.reset();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

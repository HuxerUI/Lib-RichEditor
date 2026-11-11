#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <iostream>
#include <stdexcept>

#include "../examples/preview/src/attachments.h"
#include "../examples/preview/src/web_url.h"
#include "../examples/preview/src/workbench.h"

using namespace huxerui;
using namespace huxerui::testing;
using namespace std::chrono_literals;

View App();

namespace {

class PackageResources final : public PlatformResources {
public:
  explicit PackageResources(std::string root) : root_(std::move(root)) {}

  ResourceConfiguration Configuration() const override { return {}; }
  std::optional<InputStream> OpenRead(std::string_view path) override {
    auto result = File(root_ + "/" + std::string(path)).OpenRead();
    if (!result.Succeeded()) {
      return std::nullopt;
    }
    return std::move(result).Value();
  }

private:
  std::string root_;
};

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Send(UiTest& ui, std::initializer_list<TextInputCommand> commands) {
  const auto active = ui.ActiveTextInput();
  Check(active.has_value(), "A text input client is active");
  Check(ui.SendTextInput({active->session_id, commands}).result_code == TextInputResultCode::Ok,
        "The text input client accepted the commands");
  ui.Pump();
}

void WebUrls() {
  for (const std::string_view source : {"HTTP://example.test/photo.png", "https://example.test/a%20b#details"}) {
    const auto url = folio::ParseWebUrl(source);
    Check(url && url->ToString() == source, "Absolute HTTP and HTTPS URLs keep their original spelling");
  }
  for (const std::string_view source : {"attachments/photo.png", "//example.test/photo.png", "https:///photo.png",
                                        "https:photo.png", "ftp://example.test/photo.png", "https://example.test/%xx"}) {
    Check(!folio::ParseWebUrl(source), "Only valid absolute HTTP and HTTPS URLs can open in the browser");
  }
  const auto remote = folio::Codec(folio::DocumentFormat::Markdown).Import("![Photo](HtTpS://example.test/photo.png)");
  const auto local = folio::Codec(folio::DocumentFormat::Markdown).Import("![Photo](attachments/photo.png)");
  Check(remote.document && !folio::HasLocalAttachments(*remote.document) &&
            local.document && folio::HasLocalAttachments(*local.document),
        "Attachment discovery uses the same HTTP and HTTPS policy as browser links");
}

void PaletteFlow(const char* package_path) {
  const Application app(App, {
      .show_debug_overlay = false,
      .application_hooks = {folio::InstallFolio},
      .window_hooks = {folio::InstallWorkbench},
  });
  UiTest ui(app, {.viewport = {880, 640}, .resource_provider = std::make_shared<PackageResources>(package_path)});
#if defined(__APPLE__)
  const KeyModifiers primary{.meta = true};
#else
  const KeyModifiers primary{.control = true};
#endif
  auto shifted = primary;
  shifted.shift = true;
  const auto palette = ui.Find(UiSelector::Key("command-palette"));
  const auto query = ui.Find(UiSelector::Key("palette-query"));
  const auto editor = ui.Find(UiSelector::Key("editor"));
  const auto by_id = [&](const char* id) { return ui.FindSemantics(UiSemanticSelector::Identifier(id)); };
  const auto wait_for_close = [&] {
    ui.PumpUntil([&] { return !palette.Exists(); });
    ui.Pump();
  };
  const auto open = [&] {
    ui.PressKey(Key::P, shifted);
    ui.PumpUntil([&] { return by_id("palette-query").Exists() && by_id("palette-query").One().focused; });
  };
  const auto tap_semantic = [&](const UiSemanticQuery& node) {
    const auto rect = node.One().bounds;
    ui.TapAt({rect.x + rect.width / 2.0F, rect.y + rect.height / 2.0F});
  };

  ui.PressKey(Key::N, primary);
  editor.Tap();
  editor.EnterText("Alpha\nBeta\nGamma");
  const TextSelection backward{8, 2};
  editor.SetSelection(backward);
  const auto bold = by_id("palette-editor.bold");
  const auto toolbar_bold = ui.FindSemantics(UiSemanticSelector::Label("Bold"));
  open();
  query.EnterText("bld");
  Check(bold.One().enabled && bold.One().selected == true, "Fuzzy search selects Bold");
  ui.PressKey(Key::ArrowDown);
  Check(by_id("palette-editor.bulleted-list").One().selected == true && by_id("palette-query").One().focused,
        "Arrow navigation changes the selected result and keeps focus in the search field");
  ui.PressKey(Key::ArrowUp);
  ui.PressKey(Key::Enter);
  wait_for_close();
  Check(by_id("article-editor").One().focused && ui.ActiveTextInput()->selection == backward,
        "Bold returns focus to the original backward selection across paragraphs");
  Check(toolbar_bold.One().checked == SemanticCheckedState::Checked, "Bold applies to the entire selection");
  ui.PressKey(Key::Z, primary);
  Check(toolbar_bold.One().checked == SemanticCheckedState::Unchecked &&
            ui.ActiveTextInput()->selection == backward && by_id("article-editor").One().value == "Alpha\nBeta\nGamma",
        "One undo removes Bold while preserving the text and selection");

  const auto delete_row = by_id("palette-editor.table.delete-row");
  open();
  query.EnterText("delete row");
  Check(!delete_row.One().enabled, "Delete row is disabled outside a table");
  ui.PressKey(Key::Enter);
  Check(palette.Exists(), "Enter cannot run a disabled result");
  ui.PressKey(Key::Escape);
  wait_for_close();
  editor.SetSelection({16, 16});
  ui.PressKey(Key::T, shifted);
  open();
  query.EnterText("delete row");
  Check(delete_row.One().enabled, "Delete row is available in a table cell");
  ui.PressKey(Key::Escape);
  wait_for_close();

  const auto find = by_id("find-query");
  ui.PressKey(Key::F, {.control = true});
  ui.PumpUntil([&] { return find.Exists() && find.One().focused; });
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"},
            {.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{2, 1}}});
  open();
  Send(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "jiacu"}});
  Check(ui.ActiveTextInput()->composition.has_value(), "The palette keeps the IME preedit active");
  ui.PressKey(Key::Enter);
  Check(palette.Exists(), "Enter during composition does not run a command");
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "加粗"}});
  Check(bold.Exists(), "A committed Chinese alias matches Bold");
  ui.PressKey(Key::Escape);
  wait_for_close();
  Check(find.One().focused && find.One().value == "你好" && ui.ActiveTextInput()->selection == TextSelection{2, 1},
        "Cancel restores the focused Find field and its selection");

  editor.Tap();
  open();
  query.EnterText("find");
  Check(by_id("palette-folio.find").One().selected == true, "The Find command is selected");
  ui.PressKey(Key::Enter);
  wait_for_close();
  ui.PumpUntil([&] { return find.One().focused; });
  Check(find.One().value == "你好", "Running Find focuses its own field with its existing query");

  open();
  query.EnterText("open settings");
  ui.PressKey(Key::Enter);
  ui.PumpUntil([&] { return ui.Find(UiSelector::Key("settings-dialog")).Exists(); });
  tap_semantic(ui.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::Tab), UiSemanticSelector::Label("Keyboard"))));
  ui.Find(UiSelector::Key("keyboard-shortcuts"))
      .ScrollUntil(UiSelector::Key("shortcut-editor.bold"), {.step = {0, 60}}).Tap();
  ui.Pump(300ms);
  ui.PressKey(Key::J, {.control = true});
  tap_semantic(by_id("shortcut-apply"));
  ui.PumpUntil([&] { return !ui.Find(UiSelector::Key("shortcut-capture")).Exists(); });
  ui.Find(UiSelector::Text("Done")).Tap();
  ui.PumpUntil([&] { return !ui.Find(UiSelector::Key("settings-dialog")).Exists(); });
  open();
  query.EnterText("bold");
  Check(bold.One().value == "Ctrl + J", "The palette shows the shortcut recorded in Settings");
  ui.PressKey(Key::Escape);
  wait_for_close();

  ui.SetWindowMetrics({.viewport = {360, 640}});
  ui.Pump();
  ui.Find(UiSelector::Key("file-menu")).Tap();
  tap_semantic(ui.FindSemantics(UiSemanticSelector::Label("Command palette…")));
  ui.PumpUntil([&] { return by_id("palette-query").Exists() && by_id("palette-query").One().focused; });
  Check(ui.ActiveTextInput().has_value(), "A narrow desktop window keeps the menu entry and focuses search");
  ui.PressKey(Key::Escape);
  wait_for_close();

  ui.PressKey(Key::N, primary);
  tap_semantic(by_id("article-editor"));
  const TextSelection link_selection{7, 0};
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "Example"},
            {.kind = TextInputCommandKind::SetSelection, .selection_after = link_selection}});
  tap_semantic(ui.FindSemantics(UiSemanticSelector::Label("Insert link")));
  const auto address = ui.FindSemantics(UiSemanticSelector::Label("Link address"));
  ui.PumpUntil([&] { return address.Exists() && address.One().focused; });
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "  HtTpS://example.org  "}});
  tap_semantic(ui.FindSemantics(UiSemanticSelector::Label("Insert")));
  ui.PumpUntil([&] { return by_id("article-editor").One().focused; });
  Check(ui.ActiveTextInput()->selection == link_selection,
        "Applying a link restores focus to the original directed selection");
  tap_semantic(ui.FindSemantics(UiSemanticSelector::Label("Link actions")));
  Check(ui.FindSemantics(UiSemanticSelector::Label("Open link")).One().enabled,
        "The link menu can open the pasted web address after trimming its outer spaces");
}

} // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "The Folio resource package path is required");
    WebUrls();
    PaletteFlow(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

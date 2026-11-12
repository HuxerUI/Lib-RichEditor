#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "../examples/preview/src/attachments.h"
#include "../examples/preview/src/document_library.h"
#include "../examples/preview/src/web_url.h"
#include "../examples/preview/src/workbench.h"
#include "../examples/preview/src/workspace_store.h"
#include "folio_test_resources.h"

using namespace huxerui;
using namespace huxerui::testing;
using namespace std::chrono_literals;
using folio::test::PackageResources;

View App();

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void WaitForFixture(UiTest& ui, const std::function<bool()>& check) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  do {
    ui.Pump(16ms);
    if (check()) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Check(false, "The fixture operation completed before the timeout");
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

void WorkspaceStorage(const File& directory) {
  using namespace folio;
  const auto folder = directory.Child("Project");
  const auto saved = WorkspaceRecord{
      .active_window = "second",
      .windows = {
          {.id = "first",
           .folders = {{1, "Project", {folder.Path(), true}}},
           .tabs = {{.id = 10, .name = "Outline.md", .location = {folder.Child("Outline.md").Path(), false},
                     .attachments = {folder.Path(), true}, .folder = 1, .relative = {"Outline.md"}},
                    {.id = 12, .name = "Notes.md", .folder = 1, .relative = {"Meeting", "Notes.md"}}},
           .selected = 12, .sidebar_width = 310.0F, .sidebar_visible = false, .showing_outline = true},
          {.id = "second", .folders = {{1, "Shared", {}}},
           .tabs = {{.id = 16, .name = "article.json", .package = "Shared draft"}}, .selected = 16},
      },
      .recent = {{"recent-project", "Project", {folder.Path(), true}, RecentKind::Folder},
                 {"recent-note", "Outline.md", {folder.Child("Outline.md").Path(), false}}},
  };
  WorkspaceStore store(directory);
  Check(store.Get().windows.empty(), "A missing workspace starts empty");
  store.Set(saved);
  Check(!store.WriteFailed() && WorkspaceStore(directory).Get() == saved,
        "Workspace storage preserves ordered windows and tabs, local paths, and opaque grants");

  auto remaining = saved;
  remaining.windows.pop_back();
  remaining.active_window = remaining.windows.front().id;
  store.Set(remaining);
  Check(WorkspaceStore(directory).Get() == remaining && !directory.Child("workspace.json.tmp").Stat().Succeeded(),
        "The saved workspace is replaced and its staging file is removed");

  const auto file = directory.Child("workspace.json");
  auto text = file.ReadString().Value();
  auto legacy = nlohmann::json::parse(text);
  legacy.erase("recent");
  Check(file.WriteString(legacy.dump()), "Write a workspace saved before recent locations existed");
  const auto migrated = WorkspaceStore(directory).Get().recent;
  Check(migrated.size() == 1 && migrated.front().kind == RecentKind::Folder && migrated.front().name == "Project",
        "A previously mounted folder is migrated into the recent list");
  const auto relative = text.find("\"Meeting\"");
  Check(relative != std::string::npos, "The nested folder path is stored as components");
  text.replace(relative, std::string_view("\"Meeting\"").size(), "\"..\"");
  Check(file.WriteString(text) && WorkspaceStore(directory).Get().windows.empty(),
        "A corrupt relative path is rejected");
  Check(file.WriteString(R"({"version":2,"windows":[{"id":"future"}]})") &&
            WorkspaceStore(directory).Get().windows.empty(),
        "An unknown workspace version starts empty");
}

[[huxerui::composable]]
View WorkspaceFixture() {
  const auto window = UseWindow();
  return Column {App().With(Grow()), Button("Request window close").OnClick([window] { window.Close(); })};
}

void WorkspaceRestoreFlow(const File& directory, const char* package_path) {
  using namespace folio;
  const auto document = directory.Child("Notes.md");
  const auto missing = directory.Child("Missing.md");
  Check(directory.CreateDirectories() && document.WriteString("# Restored heading\n\nRestored body."),
        "Write the saved document fixture");
  const WorkspaceRecord initial{
      .active_window = "workbench",
      .windows = {{.id = "workbench", .tabs = {{.id = 1, .name = document.Name(), .location = {document.Path(), false}},
                                               {.id = 2, .name = missing.Name(), .location = {missing.Path(), false}}},
                   .selected = 2}},
  };
  bool seed = true;
  const Application app(WorkspaceFixture, {
      .show_debug_overlay = false,
      .application_hooks = {[&](ApplicationContext& context) {
        auto store = std::make_shared<WorkspaceStore>(directory);
        if (seed) {
          store->Set(initial);
          seed = false;
        }
        InstallFolioWithWorkspace(context, std::move(store));
      }},
      .window_hooks = {InstallWorkbench},
  });
  const auto options = UiTestOptions{.viewport = {360, 640},
                                     .resource_provider = std::make_shared<PackageResources>(package_path)};
  const auto window_count = [&] {
    return nlohmann::json::parse(directory.Child("workspace.json").ReadString().Value()).at("windows").size();
  };
  const auto wait = WaitForFixture;
  {
    UiTest ui(app, options);
    const auto pending = ui.FindSemantics(UiSemanticSelector::Identifier("restore-document"));
    wait(ui, [&] { return pending.Exists(); });
    Check(ui.FindSemantics(UiSemanticSelector::Label("Missing.md, switch document")).Exists(),
          "The selected missing document stays in its tab");
  }
  {
    UiTest ui(app, options);
    const auto pending = ui.FindSemantics(UiSemanticSelector::Identifier("restore-document"));
    wait(ui, [&] { return pending.Exists(); });
    const auto switcher = ui.FindSemantics(UiSemanticSelector::Identifier("document-switcher"));
    const auto center = [](const UiSemanticQuery& target) {
      const auto bounds = target.One().bounds;
      return Point{bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
    };
    ui.TapAt(center(switcher));
    const auto notes = ui.FindSemantics(UiSemanticSelector::Label("Notes.md"));
    wait(ui, [&] { return notes.Exists(); });
    ui.TapAt(center(notes));
    const auto article = ui.FindSemantics(UiSemanticSelector::Identifier("article-editor"));
    wait(ui, [&] { return article.Exists(); });
    Check(article.One().value == "Restored heading\nRestored body.", "The local document reopens with its disk contents");
    ui.Pump(400ms);
    ui.Pump();
    ui.Find(UiSelector::Key("file-menu")).Tap();
    ui.Find(UiSelector::Text("Load sample")).Tap();
    ui.Pump(400ms);
    ui.Pump();

    const auto close = ui.Find(UiSelector::Text("Request window close"));
    const auto cancel = ui.Find(UiSelector::Text("Cancel"));
    const auto discard = ui.Find(UiSelector::Text("Discard"));
    close.Tap();
    wait(ui, [&] { return cancel.Exists(); });
    cancel.Tap();
    wait(ui, [&] { return !cancel.Exists(); });
    Check(window_count() == 1, "Cancel keeps the window in the saved workspace");
    close.Tap();
    wait(ui, [&] { return discard.Exists(); });
    discard.Tap();
    wait(ui, [&] { return window_count() == 0; });
  }
}

void LibraryStorageFlow(const File& directory, const char* package_path) {
  using namespace folio;
  const auto attachments = directory.Child("source").Child("attachments");
  const auto source = *attachments.Parent();
  constexpr std::string_view image = "image bytes";
  Check(attachments.CreateDirectories() && attachments.Child("sample.png").WriteString(image),
        "Prepare the document's local attachment");
  const auto input = Codec(DocumentFormat::Markdown).Import("# Library article\n\n![Sample](attachments/sample.png)");
  Check(input.document.has_value(), "The source document imports in Strict mode");
  const auto encoded = Codec(DocumentFormat::Json).Export(*input.document);
  Check(encoded.text.has_value(), "The managed package uses the JSON codec");
  const auto root = directory.Child("library");
  bool written = false;
  const Application seed([] { return View{}; }, {.application_hooks = {[&](ApplicationContext&) {
    auto library = std::make_shared<DocumentLibrary>(root);
    UseApplicationTaskScope().Launch([&, library]() -> Task<void> {
      const auto package = *library->Package("library-doc");
      const std::vector sources{FileSource(source, FileType::Directory, false)};
      const auto result = co_await WriteLibraryPackage(package, *encoded.text, std::nullopt, sources, *input.document);
      Check(result.Succeeded(), "The first save copies the attachment into the managed package");
      const LibraryEntry parent{"project", "Project", "", true};
      const LibraryEntry nested{"archive", "Archive", "project", true};
      const LibraryEntry reserved{"reserved", "Reserved", "archive", true};
      const LibraryEntry initial{"library-doc", "Article", "", false};
      const LibraryEntry moved{"library-doc", "Planning", "archive", false};
      Check((co_await library->Put(parent)) == LibraryWriteResult::Saved, "The library creates a top-level folder");
      Check((co_await library->Put(nested)) == LibraryWriteResult::Saved, "The library creates a nested folder");
      const auto confirmed = library->Subtree(parent.id);
      Check((co_await library->Put(reserved)) == LibraryWriteResult::Saved, "The library creates a sibling name fixture");
      Check((co_await library->Remove(confirmed)) == LibraryWriteResult::Changed && library->Find(reserved.id),
            "Deletion refuses a confirmation that predates an added descendant");
      Check(!library->CanPlace("project", "archive"), "A folder cannot move into its own descendants");
      Check((co_await library->Put(initial)) == LibraryWriteResult::Saved &&
                (co_await library->Put(moved)) == LibraryWriteResult::Saved,
            "The same document ID can move and be renamed");
      WorkspaceStore workspace(directory);
      workspace.Set({.active_window = "workbench", .windows = {{.id = "workbench",
          .tabs = {{.id = 1, .name = "Article", .library_document = "library-doc"}}, .selected = 1,
          .library_expanded = {"", "project", "archive"}}}});
      written = true;
    });
  }}});
  {
    UiTest ui(seed);
    WaitForFixture(ui, [&] { return written; });
  }
  Check(source.DeleteRecursively(), "The original source and attachment can disappear after import");
  const auto package = root.Child("documents").Child("library-doc");
  Check(package.Child("attachments").Child("sample.png").ReadString().Value() == image &&
            Codec(DocumentFormat::Json).Import(package.Child("article.json").ReadString().Value()).document == input.document,
        "The original JSON and attachment survive folder moves and renaming");
  const Application app(App, {.show_debug_overlay = false,
      .application_hooks = {[&](ApplicationContext& context) { InstallFolioWithWorkspace(context, {}, directory); }},
      .window_hooks = {InstallWorkbench}});
  UiTest ui(app, {.viewport = {360, 640}, .resource_provider = std::make_shared<PackageResources>(package_path)});
  const auto editor = ui.FindSemantics(UiSemanticSelector::Identifier("article-editor"));
  WaitForFixture(ui, [&] { return editor.Exists(); });
  Check(editor.One().value.starts_with("Library article") &&
            ui.FindSemantics(UiSemanticSelector::Label("Planning, switch document")).Exists(),
        "Restart restores the document by ID and resolves its current display name");
  const auto sidebar = ui.FindSemantics(UiSemanticSelector::Identifier("show-sidebar")).One().bounds;
  ui.TapAt({sidebar.x + sidebar.width / 2.0F, sidebar.y + sidebar.height / 2.0F});
  ui.Pump(400ms);
  Check(ui.FindSemantics(UiSemanticSelector::Label("Workspace")).Exists() &&
            !ui.FindSemantics(UiSemanticSelector::Label("My Documents")).Exists() &&
            ui.FindSemantics(UiSemanticSelector::Label("Archive")).Exists() &&
            ui.FindSemantics(UiSemanticSelector::Label("Planning")).Exists(),
        "The workspace root directly contains the previously saved folders and documents");
  const auto rename = ui.FindSemantics(UiSemanticSelector::Label("Actions for Planning")).One().bounds;
  ui.TapAt({rename.x + rename.width / 2.0F, rename.y + rename.height / 2.0F});
  ui.Find(UiSelector::Text("Rename…")).Tap();
  const auto name_field = ui.Find(UiSelector::Key("library-entry-name"));
  const auto name_semantics = ui.FindSemantics(UiSemanticSelector::Identifier("library-entry-name"));
  name_field.Tap();
  name_field.ReplaceText("invalid/name");
  ui.PressKey(Key::Enter);
  Check(name_semantics.One().error == "Enter a valid name." && name_semantics.One().focused,
        "An invalid name keeps the dialog open and shows an inline error");
  name_field.ReplaceText("Reserved");
  ui.PressKey(Key::Enter);
  WaitForFixture(ui, [&] { return name_semantics.One().error.starts_with("A file or folder already uses that name."); });
  Check(name_semantics.One().value == "Reserved" && name_semantics.One().focused,
        "A duplicate name keeps the input and keyboard focus");
  name_field.ReplaceText("Product plan");
  Send(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "jihua"}});
  ui.PressKey(Key::Enter);
  Check(name_field.Exists(), "Enter during IME composition keeps the rename dialog open");
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "计划"}});
  const auto blocked_staging = root.Child("catalog.json.tmp");
  Check(blocked_staging.CreateDirectories() && blocked_staging.Child("fixture").WriteString(""),
        "Block the catalog staging write for the rename fixture");
  ui.PressKey(Key::Enter);
  WaitForFixture(ui, [&] { return name_semantics.One().error.starts_with("Could not write the change"); });
  Check(name_semantics.One().value == "Product plan计划" && name_semantics.One().focused,
        "A storage write failure retains the attempted name and focus");
  Check(blocked_staging.DeleteRecursively(), "Allow the catalog staging write to be retried");
  ui.PressKey(Key::Enter);
  WaitForFixture(ui, [&] {
    return !name_field.Exists() && ui.FindSemantics(UiSemanticSelector::Label("Product plan计划")).Exists();
  });
  const auto renamed = nlohmann::json::parse(root.Child("catalog.json").ReadString().Value());
  Check(std::ranges::any_of(renamed.at("entries"), [](const auto& entry) {
    return entry.at("id") == "library-doc" && entry.at("parent") == "archive" && entry.at("name") == "Product plan计划";
  }), "Enter persists the committed name on the same document in the same folder");
  const auto bounds = ui.FindSemantics(UiSemanticSelector::Label("Archive")).One().bounds;
  const Point touch{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
  ui.SendPointer({.type = PointerEventType::Down, .pointer_id = 7, .position = touch,
                  .device_kind = PointerDeviceKind::Touch, .changed_button = PointerButton::Primary,
                  .pressed_buttons = PointerButton::Primary});
  ui.Pump(800ms);
  ui.SendPointer({.type = PointerEventType::Up, .pointer_id = 7, .position = touch,
                  .device_kind = PointerDeviceKind::Touch, .changed_button = PointerButton::Primary});
  ui.Pump();
  Check(ui.Find(UiSelector::Text("New folder…")).Exists(), "Long pressing a library folder opens its actions on release");
  Check(ui.Find(UiSelector::Text("Import into this folder…")).Exists(), "Each workspace folder has a targeted import action");
  ui.Find(UiSelector::Text("New document")).Tap();
  ui.Pump(400ms);
#if defined(__APPLE__)
  ui.PressKey(Key::S, {.meta = true});
#else
  ui.PressKey(Key::S, {.control = true});
#endif
  WaitForFixture(ui, [&] {
    const auto record = nlohmann::json::parse(root.Child("catalog.json").ReadString().Value());
    return std::ranges::any_of(record.at("entries"), [](const auto& entry) {
      return !entry.at("folder").template get<bool>() && entry.at("id") != "library-doc" && entry.at("parent") == "archive";
    });
  });
}

void LibraryDeleteFlow(const File& directory, const char* package_path) {
  using namespace folio;
  using Json = nlohmann::json;
  const auto library = directory.Child("library");
  const auto catalog = library.Child("catalog.json");
  const auto documents = library.Child("documents");
  const auto external = directory.Child("External.md");
  const auto managed = documents.Child("article");
  const auto attachment = managed.Child("attachments").Child("sample.png");
  const auto orphan = documents.Child("already-deleted");
  const Json entries = Json::array({
      {{"id", "project"}, {"name", "Project"}, {"parent", ""}, {"folder", true}},
      {{"id", "nested"}, {"name", "Nested"}, {"parent", "project"}, {"folder", true}},
      {{"id", "article"}, {"name", "Article"}, {"parent", "nested"}, {"folder", false}},
      {{"id", "keep"}, {"name", "Keep"}, {"parent", ""}, {"folder", true}},
  });
  const auto input = Codec(DocumentFormat::Markdown).Import("# Managed body");
  Check(input.document.has_value(), "Create the managed document fixture");
  const auto encoded = Codec(DocumentFormat::Json).Export(*input.document);
  Check(encoded.text && attachment.Parent()->CreateDirectories() && attachment.WriteString("image bytes") &&
            managed.Child("article.json").WriteString(*encoded.text) &&
            orphan.CreateDirectories() && orphan.Child("article.json").WriteString(*encoded.text) &&
            external.WriteString("# External body") &&
            catalog.WriteString(Json{{"version", 1}, {"entries", entries}, {"pending_deletions", {"already-deleted"}}}.dump()),
        "Write the workspace and a pending package cleanup from a previous launch");
  const WorkspaceLocation external_location{external.Path(), false};
  bool seed = true;
  const Application app(App, {.show_debug_overlay = false,
      .application_hooks = {[&](ApplicationContext& context) {
        auto store = std::make_shared<WorkspaceStore>(directory);
        if (std::exchange(seed, false)) {
          store->Set({.active_window = "workbench", .windows = {{.id = "workbench",
              .tabs = {{.id = 1, .name = "Article", .library_document = "article"},
                       {.id = 2, .name = "External.md", .location = external_location},
                       {.id = 3, .name = "Stale", .library_document = "already-deleted"}},
              .selected = 1, .library_expanded = {"project", "nested", "keep"}}},
              .recent = {{"article", "Article", {}, RecentKind::File, "article"},
                         {"external", "External.md", external_location},
                         {"stale", "Stale", {}, RecentKind::File, "already-deleted"}}});
        }
        InstallFolioWithWorkspace(context, std::move(store), directory);
      }},
      .window_hooks = {InstallWorkbench}});
  const auto options = UiTestOptions{.viewport = {360, 640},
      .resource_provider = std::make_shared<PackageResources>(package_path)};
  {
    UiTest ui(app, options);
    const auto read_catalog = [&] { return Json::parse(catalog.ReadString().Value()); };
    const auto read_workspace = [&] { return Json::parse(directory.Child("workspace.json").ReadString().Value()); };
    const auto by_label = [&](std::string label) { return ui.FindSemantics(UiSemanticSelector::Label(std::move(label))); };
    const auto tap = [&](const UiSemanticQuery& target) {
      const auto bounds = target.One().bounds;
      ui.TapAt({bounds.x + bounds.width / 2, bounds.y + bounds.height / 2});
    };
    const auto editor = ui.FindSemantics(UiSemanticSelector::Identifier("article-editor"));
    WaitForFixture(ui, [&] { return editor.Exists() && !read_catalog().contains("pending_deletions"); });
    const auto restored = read_workspace();
    Check(!orphan.Stat().Succeeded() && restored.at("recent").size() == 2 &&
              restored.at("windows").front().at("tabs").size() == 2,
          "Restart removes stale deleted tabs and history and finishes queued package cleanup");
    tap(editor);
    Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = " Unsaved"}});
    tap(ui.FindSemantics(UiSemanticSelector::Identifier("show-sidebar")));
    ui.Pump(400ms);
    const auto delete_menu = ui.Find(UiSelector::Text("Delete…"));
    tap(by_label("Actions for External.md"));
    Check(ui.Find(UiSelector::Text("Remove from recently opened")).Exists() && !delete_menu.Exists(),
          "External history offers removal without a delete command");
    ui.PressKey(Key::Escape);
    const auto confirm = ui.Find(UiSelector::Text("Delete"));
    const auto article_actions = by_label("Actions for Article").All();
    Check(article_actions.size() == 2, "The managed article is present in the workspace and recent history");
    const auto article_bounds = article_actions.front().bounds;
    ui.TapAt({article_bounds.x + article_bounds.width / 2, article_bounds.y + article_bounds.height / 2});
    delete_menu.Tap();
    WaitForFixture(ui, [&] { return confirm.Exists(); });
    Check(ui.Find(UiSelector::Text("Delete “Article”?")).Exists() &&
              ui.Find(UiSelector::Text("This permanently deletes the document and its attachments\n\n"
                                       "Unsaved changes in open documents will also be discarded")).Exists(),
          "A single document confirmation names the document and warns about its attachment and unsaved changes");
    ui.PressKey(Key::Escape);
    WaitForFixture(ui, [&] { return !confirm.Exists(); });
    const auto open_delete = [&] {
      tap(by_label("Actions for Project"));
      delete_menu.Tap();
      WaitForFixture(ui, [&] { return confirm.Exists(); });
    };
    open_delete();
    const auto cancel = by_label("Cancel");
    Check(cancel.One().focused, "The destructive dialog initially focuses Cancel");
    Check(ui.Find(UiSelector::Text("This permanently deletes the folder and its contents, including attachments\n"
                                   "Documents: 1 · Subfolders: 1\n\n"
                                   "Unsaved changes in open documents will also be discarded\n• Article")).Exists(),
          "The confirmation counts descendants and names the document with unsaved changes");
    ui.PressKey(Key::Enter);
    WaitForFixture(ui, [&] { return !confirm.Exists(); });
    Check(read_catalog().at("entries") == entries && attachment.Stat().Succeeded(),
          "Activating the default Cancel leaves the catalog and attachments intact");

    const auto blocked = library.Child("catalog.json.tmp");
    Check(blocked.CreateDirectories() && blocked.Child("fixture").WriteString(""), "Block the catalog write");
    open_delete();
    confirm.Tap();
    const auto failure = ui.Find(UiSelector::Text("Could not delete"));
    WaitForFixture(ui, [&] { return failure.Exists(); });
    Check(attachment.Stat().Succeeded() && read_workspace().at("windows").front().at("tabs").size() == 2 &&
              read_catalog().at("entries") == entries,
          "A failed catalog write keeps the original file, workspace entry and tab");
    ui.Find(UiSelector::Text("Close")).Tap();
    WaitForFixture(ui, [&] { return !failure.Exists(); });
    Check(blocked.DeleteRecursively(), "Allow the catalog write to be retried");

    open_delete();
    confirm.Tap();
    WaitForFixture(ui, [&] { return !managed.Stat().Succeeded() && !read_catalog().contains("pending_deletions"); });
    const auto saved = read_workspace();
    const auto& window = saved.at("windows").front();
    const auto& recent = saved.at("recent");
    Check(read_catalog().at("entries") == Json::array({entries.back()}) && external.Stat().Succeeded(),
          "Confirming removes the subtree and its attachment while keeping unrelated entries and external files");
    Check(window.at("tabs").size() == 1 && window.at("selected") == 2 &&
              window.at("library_expanded") == Json::array({"keep"}) &&
              recent.size() == 1 && recent.front().at("id") == "external",
          "The workspace clears deleted tabs, history and expansion state and selects the surviving tab");
  }
  {
    UiTest ui(app, options);
    const auto editor = ui.FindSemantics(UiSemanticSelector::Identifier("article-editor"));
    WaitForFixture(ui, [&] { return editor.Exists(); });
    Check(editor.One().value == "External body", "Restart opens the surviving tab");
  }
}

void ExternalHistoryFlow(const File& directory, const char* package_path) {
  using namespace folio;
  const auto project = directory.Child("Project");
  const auto document = project.Child("Notes.md");
  Check(project.CreateDirectories() && document.WriteString("# Notes\n\nAn external document."),
        "Create the external document fixture");
  const WorkspaceLocation folder_location{project.Path(), true};
  const WorkspaceLocation file_location{document.Path(), false};
  bool seed = true;
  const Application app(App, {.show_debug_overlay = false,
      .application_hooks = {[&](ApplicationContext& context) {
        auto store = std::make_shared<WorkspaceStore>(directory);
        if (std::exchange(seed, false)) {
          store->Set({.active_window = "external", .windows = {{.id = "external",
              .folders = {{1, "Project", folder_location, "folder"}},
              .tabs = {{.id = 1, .name = "Notes.md", .location = file_location, .folder = 1,
                        .relative = {"Notes.md"}}}, .selected = 1}},
              .recent = {{"folder", "Project", folder_location, RecentKind::Folder},
                         {"file", "Notes.md", file_location}}});
        }
        InstallFolioWithWorkspace(context, std::move(store), directory);
      }},
      .window_hooks = {InstallWorkbench}});
  const auto options = UiTestOptions{.viewport = {360, 640},
      .resource_provider = std::make_shared<PackageResources>(package_path)};
  const auto tap = [](UiTest& ui, const SemanticNode& node) {
    const auto bounds = node.bounds;
    ui.TapAt({bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F});
  };
  for (const bool first_launch : {true, false}) {
    UiTest ui(app, options);
    const auto by_label = [&](std::string label) { return ui.FindSemantics(UiSemanticSelector::Label(std::move(label))); };
    const auto editor = ui.FindSemantics(UiSemanticSelector::Identifier("article-editor"));
    WaitForFixture(ui, [&] { return editor.Exists(); });
    Check(editor.One().value == "Notes\nAn external document.", "The open external tab survives restart");
    tap(ui, ui.FindSemantics(UiSemanticSelector::Identifier("show-sidebar")).One());
    ui.Pump(400ms);
    const auto locations = by_label("External locations");
    Check(!locations.Exists() && by_label("Project").Count() == 1,
          "Previously mounted folders start in history and remain unmounted");
    if (!first_launch) {
      Check(!by_label("Notes.md").Exists(), "An explicitly removed recent entry stays removed on restart");
      continue;
    }
    tap(ui, by_label("Project").One());
    WaitForFixture(ui, [&] { return locations.Exists() && by_label("Project").Count() == 2; });
    tap(ui, by_label("Actions for Project").All().front());
    ui.Find(UiSelector::Text("New folder…")).Tap();
    const auto external_name = ui.Find(UiSelector::Key("name"));
    external_name.Tap();
    external_name.ReplaceText("Created with Enter");
    ui.PressKey(Key::Enter);
    WaitForFixture(ui, [&] { return !external_name.Exists() && project.Child("Created with Enter").IsDirectory(); });
    tap(ui, by_label("Actions for Project").All().front());
    ui.Find(UiSelector::Text("Remove from external locations")).Tap();
    WaitForFixture(ui, [&] { return !locations.Exists(); });
    Check(by_label("Project").Count() == 1, "Unmounting the folder preserves its recent entry");
    tap(ui, by_label("Actions for Notes.md").One());
    ui.Find(UiSelector::Text("Remove from recently opened")).Tap();
    WaitForFixture(ui, [&] { return !by_label("Notes.md").Exists(); });
    tap(ui, ui.FindSemantics(UiSemanticSelector::Identifier("hide-sidebar")).One());
    ui.Pump(400ms);
    Check(editor.Exists() && document.ReadString().Succeeded(), "Removing a recent item preserves the tab and file");
  }
}

[[huxerui::composable]]
View NestedCodeFixture() {
  using namespace huxerui::richeditor;
  const auto document = MarkdownCodec().Import(
      "> ~~~cpp\n> quoted\n> ~~~\n\n- ~~~cpp\n  listed\n  ~~~\n").document;
  Check(document && document->Blocks().size() == 2 && document->Blocks()[0].Data().kind == BlockKind::Quote &&
            document->Blocks()[1].Data().kind == BlockKind::List,
        "The fixture has a quote and a list enclosing the code blocks");
  const auto editor = UseRichEditor(*document);
  return folio::EditorPane(editor, {.find = UseState(folio::FindPanelState{}), .focus = UseViewHandle(),
                                   .active = true, .available = true, .can_pick_images = false});
}

void CompactCodeTools(const char* package_path) {
  const Application app(NestedCodeFixture, {
      .show_debug_overlay = false,
      .application_hooks = {folio::InstallFolio},
      .window_hooks = {folio::InstallWorkbench},
  });
  UiTest ui(app, {.viewport = {360, 640}, .resource_provider = std::make_shared<PackageResources>(package_path)});
  const auto style = ui.FindSemantics(UiSemanticSelector::Label("Text style, Code block"));
  Check(style.Count() == 1, "A quoted code block has one compact text style picker");
  ui.Find(UiSelector::Key("editor")).Tap();
  Send(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{7, 7}}});
  Check(style.Count() == 1, "A code block in a list has one compact text style picker");
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
  const auto search_bounds = by_id("palette-query").One().bounds;
  const auto close_bounds = by_id("palette-close").One().bounds;
  Check(std::abs(search_bounds.height - close_bounds.height) < 0.5F &&
            std::abs(close_bounds.width - close_bounds.height) < 0.5F &&
            std::abs(search_bounds.y - close_bounds.y) < 0.5F,
        "The palette search field aligns with the square dismiss button");
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

  const auto task_list = by_id("palette-editor.task-list");
  const auto complete_task = by_id("palette-editor.task.complete");
  open();
  query.EnterText("task list");
  Check(task_list.One().enabled && task_list.One().selected == true, "The palette finds the task list command");
  ui.PressKey(Key::Enter);
  wait_for_close();
  const auto first_task = ui.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::Checkbox), UiSemanticSelector::Label("Alpha")));
  Check(first_task.One().checked == SemanticCheckedState::Unchecked && ui.ActiveTextInput()->selection == backward,
        "The task list command creates unchecked items and returns focus to the original selection");
  open();
  query.EnterText("mark task complete");
  Check(complete_task.One().enabled && complete_task.One().selected == true, "Completion is available within tasks");
  ui.PressKey(Key::Enter);
  wait_for_close();
  Check(first_task.One().checked == SemanticCheckedState::Checked && ui.ActiveTextInput()->selection == backward,
        "The palette can complete selected tasks");
  ui.PressKey(Key::Z, primary);
  Check(first_task.One().checked == SemanticCheckedState::Unchecked, "Undo reverses task completion");
  ui.PressKey(Key::Z, primary);
  Check(!first_task.Exists(), "Undo restores the original plain paragraphs");

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
  tap_semantic(by_id("palette-close"));
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

  const auto compact_continue = [&](const char* label, std::string_view text) {
    const auto action = ui.FindSemantics(UiSemanticSelector::Label(label));
    Check(action.Exists() && action.One().enabled, "The compact toolbar offers a way to continue below the block");
    Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = std::string(text)},
              {.kind = TextInputCommandKind::CommitText, .text = "\n"}});
    const auto before = by_id("article-editor").One().value;
    tap_semantic(action);
    Check(!action.Exists() && by_id("article-editor").One().focused && ui.ActiveTextInput().has_value(),
          "Continuing below a block returns focus to the text editor");
    Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "Body"}});
    Check(by_id("article-editor").One().value == before + "\nBody",
          "The compact action places subsequent text below the original block");
  };
  ui.PressKey(Key::N, primary);
  tap_semantic(by_id("article-editor"));
  ui.PressKey(Key::T, shifted);
  compact_continue("Continue below table", "Cell");

  ui.PressKey(Key::N, primary);
  tap_semantic(by_id("article-editor"));
  open();
  query.EnterText("code block");
  ui.PressKey(Key::Enter);
  wait_for_close();
  compact_continue("Continue below code block", "code");

  ui.Find(UiSelector::Key("file-menu")).Tap();
  tap_semantic(ui.FindSemantics(UiSemanticSelector::Label("Load sample")));
  tap_semantic(by_id("article-editor"));
  const auto sample = by_id("article-editor").One().value;
  const auto parent = sample.find("Capture an idea");
  const auto child = sample.find("Save a useful detail");
  Check(parent != sample.npos && child != sample.npos, "The sample contains an ordered item and a bulleted child");
  const TextSelection mixed{AttributedText(sample.substr(0, child + 2)).Length(),
                            AttributedText(sample.substr(0, parent + 1)).Length()};
  Send(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = mixed}});
  for (const auto* label : {"Numbered list", "Bulleted list"}) {
    const auto item = ui.FindSemantics(UiSemanticSelector::Label(label)).One();
    Check(!item.enabled && item.checked == SemanticCheckedState::Mixed,
          "The compact list toolbar displays mixed styles with the structural actions disabled");
  }

  const auto parent_caret = mixed.active;
  Send(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{parent_caret, parent_caret}}});
  const auto numbering = ui.FindSemantics(UiSemanticSelector::Label("List numbering"));
  Check(numbering.One().enabled && numbering.One().checked == SemanticCheckedState::Checked,
        "A protected numbered item keeps its numbering menu available");
  tap_semantic(numbering);
  const auto set_number = ui.FindSemantics(UiSemanticSelector::Label("Set start number…"));
  const auto remove_number = ui.FindSemantics(UiSemanticSelector::Label("Remove numbering"));
  Check(set_number.One().enabled && !remove_number.One().enabled,
        "The numbering menu can change the start while keeping protected markers in place");
  ui.PressKey(Key::Escape);
  ui.PumpUntil([&] { return !remove_number.Exists(); });

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

void OutlineFlow(const char* package_path) {
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
  const auto by_id = [&](const char* id) { return ui.FindSemantics(UiSemanticSelector::Identifier(id)); };
  const auto by_label = [&](const char* label) { return ui.FindSemantics(UiSemanticSelector::Label(label)); };
  const auto tap = [&](const UiSemanticQuery& node) {
    const auto rect = node.One().bounds;
    ui.TapAt({rect.x + rect.width / 2, rect.y + rect.height / 2});
  };
  const auto editor = ui.Find(UiSelector::Key("editor"));
  const auto article = by_id("article-editor");
  const auto scroll = article.Find(UiSemanticSelector::Role(SemanticRole::ScrollView));
  const auto first = by_id("outline-heading-0");
  const auto child = by_id("outline-heading-1");
  const auto nested = by_id("outline-heading-2");
  const auto last = by_id("outline-heading-35");
  const auto end_first = by_id("outline-heading-68");
  const auto end_second = by_id("outline-heading-69");
  const auto scroll_by = [&](float delta) {
    const auto bounds = scroll.One().bounds;
    ui.SendScroll({.position = {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}, .delta_y = delta});
    ui.Pump();
    ui.Pump();
  };

  ui.PressKey(Key::N, primary);
  tap(by_label("Outline"));
  Check(by_label("No headings yet").Exists(), "A new document has an empty outline");

  std::string source = "Repeat\nChild\nNested\n";
  for (int i = 0; i < 32; ++i) {
    source += "A body paragraph\n";
  }
  const auto last_start = static_cast<TextOffset>(source.size());
  source += "Repeat\n";
  for (int i = 0; i < 32; ++i) {
    source += "Another body paragraph\n";
  }
  const auto end_start = static_cast<TextOffset>(source.size());
  source += "Tail\nTail";
  editor.Tap();
  editor.EnterText(source);
  const auto set_kind = [&](TextOffset offset, Key key) {
    editor.SetSelection({offset, offset});
    ui.PressKey(key, primary);
  };
  set_kind(0, Key::Digit1);
  set_kind(7, Key::Digit3);
  set_kind(13, Key::Digit6);
  set_kind(last_start, Key::Digit1);
  set_kind(end_start, Key::Digit2);
  set_kind(end_start + 5, Key::Digit2);
  Check(first.One().label == "Repeat" && last.One().label == "Repeat" &&
            child.One().heading_level == 3 && nested.One().heading_level == 6,
        "The outline keeps duplicate titles and levels that skip intermediate headings");

  editor.SetSelection({6, 6});
  ui.PumpUntil([&] { return first.One().selected == true; });
  Send(ui, {{.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"}});
  Check(first.One().label == "Repeat" && first.One().selected == true,
        "The outline keeps its committed title and highlight during IME preedit");
  Send(ui, {{.kind = TextInputCommandKind::CommitText, .text = "你好"}});
  Check(first.One().label == "Repeat你好", "An IME commit updates the heading title");
  ui.PressKey(Key::Z, primary);
  Check(first.One().label == "Repeat", "Undo restores the original title");
  set_kind(7, Key::Digit0);
  Check(!child.Exists(), "Converting a heading to a paragraph removes its outline entry");
  ui.PressKey(Key::Z, primary);
  Check(child.One().heading_level == 3, "Undo restores the removed heading at its original level");

  const TextSelection backward{10, 2};
  editor.SetSelection(backward);
  tap(last);
  const float wide_last = scroll.One().scroll->offset;
  Check(wide_last > 500 && last.One().selected == true && first.One().selected == false,
        "The second duplicate jumps down and is the only selected outline row");
  tap(first);
  Check(scroll.One().scroll->offset < 50 && first.One().selected == true && last.One().selected == false,
        "The first duplicate independently jumps to the top");
  Check(article.One().text_selection == backward.Range(), "Outline jumps leave the document selection intact");

  scroll_by(240);
  Check(nested.One().selected == true && first.One().selected == false,
        "Scrolling into the body follows the nearest preceding heading");
  tap(end_first);
  const auto bottom = *scroll.One().scroll;
  Check(bottom.offset == bottom.maximum_offset && end_first.One().selected == true,
        "The first tail heading stays highlighted when the jump clamps to the bottom");
  tap(end_second);
  ui.Pump(400ms);
  Check(scroll.One().scroll->offset == bottom.offset && end_second.One().selected == true &&
            end_first.One().selected == false,
        "A second jump at the same offset changes the exact highlighted heading");
  scroll_by(-100);
  Check(last.One().selected == true && end_second.One().selected == false,
        "Reverse scrolling releases the jump target and resumes following the reading line");
  scroll_by(100);
  Check(end_second.One().selected == true, "Natural scrolling to the bottom highlights the final heading");

  ui.SetWindowMetrics({.viewport = {360, 640}});
  ui.Pump();
  editor.Tap();
  editor.SetSelection(backward);
  tap(by_id("show-sidebar"));
  ui.Pump(400ms);
  tap(last);
  ui.Pump(400ms);
  ui.Pump();
  Check(by_id("show-sidebar").One().expanded == false && scroll.One().scroll->offset > 500,
        "A compact outline jump closes the drawer and keeps the requested heading in view");
  Check(article.One().text_selection == backward.Range(), "Closing the drawer preserves the document selection");
  article.PerformSemanticAction({SemanticActionKind::Focus, {}});
  const auto input = ui.ActiveTextInput();
  Check(input && input->selection == backward, "The drawer round trip preserves the selection direction");

  const TextSelection caret{last_start + 10, last_start + 10};
  Send(ui, {{.kind = TextInputCommandKind::SetSelection, .selection_after = caret}});
  tap(by_id("show-sidebar"));
  ui.Pump(400ms);
  Check(last.One().selected == true, "The last jump remains selected after reopening the outline");
  tap(last);
  ui.Pump(400ms);
  Check(by_id("show-sidebar").One().expanded == false && scroll.One().scroll->offset > 500 &&
            article.One().text_selection == caret.Range(),
        "Reactivating the same outline row jumps again without moving the caret");

  ui.SetWindowMetrics({.viewport = {880, 640}});
  ui.Pump();
  editor.Tap();
  editor.SetSelection({last_start + 7, end_start - 1});
  ui.PressKey(Key::Digit2, primary);
  scroll_by(10000);
  ui.PumpUntil([&] { return end_second.Exists() && !end_second.One().offscreen; });
  Check(end_second.One().selected == true, "The sidebar scrolls to show an active heading outside its viewport");
  ui.PressKey(Key::N, primary);
  Check(!first.Exists() && by_label("No headings yet").Exists(), "The outline follows the active document tab");
  const auto active_editor = ui.Find(UiSelector::AllOf(UiSelector::Key("editor"), UiSelector::Enabled(true)));
  active_editor.Tap();
  active_editor.EnterText("An introduction\nFirst chapter");
  ui.PressKey(Key::Digit1, primary);
  Check(child.One().label == "First chapter" && child.One().selected == false,
        "The outline leaves the introduction unselected in a document that fits the viewport");
  tap(by_id("document-switcher"));
  tap(by_label("Untitled, unsaved changes"));
  ui.PumpUntil([&] { return end_second.Exists() && !end_second.One().offscreen; });
  Check(end_second.One().selected == true && end_second.One().label == "Tail",
        "Switching back follows the restored document position");
}

} // namespace

int main(int argc, char** argv) {
  const auto directory = File(std::filesystem::temp_directory_path().u8string()).Child(folio::UniqueName("workspace-test-"));
  try {
    Check(argc == 2, "The Folio resource package path is required");
    WebUrls();
    {
      const Application storage([] { return View{}; }, {
          .application_hooks = {[&](ApplicationContext&) { WorkspaceStorage(directory.Child("store")); }},
      });
      UiTest ui(storage);
    }
    WorkspaceRestoreFlow(directory.Child("flow"), argv[1]);
    LibraryStorageFlow(directory.Child("library-flow"), argv[1]);
    LibraryDeleteFlow(directory.Child("library-delete"), argv[1]);
    ExternalHistoryFlow(directory.Child("external-history"), argv[1]);
    CompactCodeTools(argv[1]);
    PaletteFlow(argv[1]);
    OutlineFlow(argv[1]);
  } catch (const std::exception& error) {
    (void)directory.DeleteRecursively();
    std::cerr << error.what() << '\n';
    return 1;
  }
  (void)directory.DeleteRecursively();
}

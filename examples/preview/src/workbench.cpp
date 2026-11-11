#include "workbench.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

#include "app_resources.h"
#include "appearance.h"
#include "attachments.h"
#include "browser.h"
#include "command_palette.h"
#include "labels.h"
#include "settings.h"

using namespace huxerui;
using namespace huxerui::richeditor;

View App();

namespace l10n = app::strings;

namespace folio {
namespace {

using DocumentId = std::uint64_t;

struct DocumentFile {
  FileReference reference;
  std::optional<FileReference> directory;
  std::optional<File> identity;
};

using Target = std::shared_ptr<DocumentFile>;

bool SameFile(const Target& first, const Target& second) {
  return first == second || (first->identity && first->identity == second->identity);
}

std::string LocationLabel(const DocumentFile& file) {
  if (const auto local = file.reference.AsFile()) {
    return local->Path();
  }
  return file.directory ? file.directory->Name() + "/" + file.reference.Name() : file.reference.Name();
}

struct SavedDocument {
  Target target;
  DocumentFormat format;
  RichDocument snapshot;
  Bytes bytes;
};

struct AttachmentLocation {
  std::vector<FileReference> directories;
  std::shared_ptr<const ImageResolver> resolver;

  AttachmentLocation(const std::optional<File>& working_directory, std::vector<FileReference> directories)
      : directories(std::move(directories)), resolver(AttachmentResolver(working_directory, this->directories)) {}
};

enum class Operation { Idle, PickImage, LocateAttachments, Save, Export };

struct DocumentTab {
  DocumentId id;
  std::string draft_name;
  std::optional<File> working_directory;
  RichDocument initial;
  State<std::shared_ptr<const SavedDocument>> saved;
  State<AttachmentLocation> attachments;
  State<std::optional<RichEditorController>> editor{std::nullopt};
  State<std::optional<ViewHandle>> editor_focus{std::nullopt};
  State<Operation> operation{Operation::Idle};
  State<FindPanelState> find{FindPanelState{}};
  State<std::string> status{std::string{}};
  bool opened_with_attachments;

  DocumentTab(DocumentId id, std::string name, std::optional<File> working_directory, RichDocument initial,
              std::shared_ptr<const SavedDocument> original)
      : id(id), draft_name(std::move(name)), working_directory(std::move(working_directory)),
        initial(std::move(initial)), saved(original),
        attachments(AttachmentLocation(this->working_directory,
            original && original->target->directory ? std::vector{*original->target->directory}
                                                    : std::vector<FileReference>{})),
        opened_with_attachments(original && HasLocalAttachments(this->initial)) {}

  std::string Name() const {
    const auto& original = saved.Get();
    return original ? original->target->reference.Name() : draft_name;
  }

  RichDocument Snapshot() const {
    const auto& controller = editor.Get();
    return controller ? controller->Document() : initial;
  }

  bool Dirty() const {
    static const RichDocument empty;
    const auto& original = saved.Get();
    return Snapshot() != (original ? original->snapshot : empty);
  }

  bool Busy() const { return operation.Get() != Operation::Idle; }

  void SetAttachmentDirectory(FileReference directory) {
    attachments = AttachmentLocation(working_directory, {std::move(directory)});
  }

  void BindAttachmentDirectory(FileReference directory) {
    auto sources = attachments->directories;
    if (const auto local = directory.AsFile()) {
      if (!sources.empty() && sources.front().AsFile() == local) {
        return;
      }
      std::erase_if(sources, [&](const FileReference& source) { return source.AsFile() == local; });
    }
    // Undo can restore a reference that was absent from the saved snapshot.
    sources.insert(sources.begin(), std::move(directory));
    attachments = AttachmentLocation(working_directory, std::move(sources));
  }
};

using Tab = std::shared_ptr<DocumentTab>;

enum class FolderLoad { Unloaded, Loading, Loaded, Failed };

struct FolderEntry;
using Entry = std::shared_ptr<FolderEntry>;

struct FolderEntry {
  std::uint64_t id;
  std::string name;
  std::optional<FileReference> directory;
  Target file;
  std::optional<File> identity;
  State<bool> expanded{false};
  State<FolderLoad> load{FolderLoad::Unloaded};
  State<std::vector<Entry>> children{std::vector<Entry>{}};
  State<std::string> error{std::string{}};
  std::uint64_t load_generation = 0;
};

struct CloseRequest {
  Tab tab;
  bool waiting = true;
};

constexpr float default_sidebar_width = 260.0F;

struct PaneSizes {
  Frame sidebar{.width = default_sidebar_width, .min_width = 150.0F, .max_width = 420.0F};
  Grow sidebar_grow{0.0F};
  Frame documents{.min_width = 160.0F};
  Grow documents_grow{1.0F};
  bool operator==(const PaneSizes&) const = default;
};

struct Workspace {
  std::optional<WindowHandle> window;
  State<std::vector<Tab>> tabs{std::vector<Tab>{}};
  State<DocumentId> selected{0};
  State<std::vector<Entry>> folders{std::vector<Entry>{}};
  State<std::uint64_t> selected_entry{0};
  State<bool> folders_open{true};
  State<bool> recents_open{true};
  State<bool> sidebar_visible{true};
  State<bool> drawer_open{false};
  State<PaneSizes> pane_sizes{PaneSizes{}};
  State<bool> picking{false};
  State<std::shared_ptr<CloseRequest>> closing{std::shared_ptr<CloseRequest>{}};
  State<std::string> status{std::string{}};
  bool allow_close = false;
  bool palette_open = false;
  std::uint64_t next_entry = 0;

  Tab Selected() const {
    const auto id = selected.Get();
    const auto& open = tabs.Get();
    const auto found = std::ranges::find_if(open, [id](const Tab& tab) { return tab->id == id; });
    return found == open.end() ? nullptr : *found;
  }
};

struct FolioState {
  State<std::vector<Target>> recent{std::vector<Target>{}};
  std::vector<std::weak_ptr<Workspace>> windows;
  DocumentId next_document = 0;
  unsigned next_draft = 0;

  void Remember(Target target) {
    recent.Update([target = std::move(target)](auto& entries) {
      std::erase_if(entries, [&](const Target& entry) { return SameFile(entry, target); });
      entries.insert(entries.begin(), target);
      constexpr std::size_t recent_limit = 24;
      if (entries.size() > recent_limit) {
        entries.resize(recent_limit);
      }
    });
  }
};

enum class OpenKind { File, Folder, Package };
enum class WriteKind { Save, SaveAs, Export };
enum class ChildKind { File, Folder };

struct SaveRequest {
  Tab tab;
  std::function<void(bool)> complete;
};

using Save = std::shared_ptr<SaveRequest>;

struct SavePayload {
  RichDocument snapshot;
  DocumentFormat format;
  std::string text;
};

enum class ChoiceTone { Cancel, Secondary, Primary, Destructive };

struct Choice {
  StringVariant label;
  std::function<void()> action;
  ChoiceTone tone = ChoiceTone::Cancel;
};

std::string IssueMessage(const std::vector<ConversionIssue>& issues) {
  if (issues.empty()) {
    return "The document could not be converted.";
  }
  std::string message = issues.front().code + " — " + issues.front().message;
  if (issues.size() > 1) {
    message += " (+" + std::to_string(issues.size() - 1) + ")";
  }
  return message;
}

class WorkbenchActions {
public:
  std::shared_ptr<FolioState> folio;
  std::shared_ptr<Workspace> workspace;
  ApplicationHandle application;
  WindowHandle window;
  std::shared_ptr<FilePicker> picker;
  std::shared_ptr<Resources> resources;
  TaskScope tasks;
  DialogHandle dialog;
  ViewHandle file_menu_focus;
  ViewHandle palette_focus;
  State<bool> sidebar_open;
  bool modal_sidebar;

  bool Available() const { return !workspace->picking.Get() && !workspace->closing.Get(); }
  bool CanExecute(FolioCommand command) const;
  void Execute(FolioCommand command, bool keyboard = false) const;
  void NewDocument(bool sample = false) const;
  void NewWindow() const;
  void PickOpen(OpenKind kind) const;
  void OpenKnown(Target target) const;
  void SetExpanded(const Entry& entry, bool expanded) const;
  void RequestNewChild(Entry folder, ChildKind kind) const;
  void Select(const Tab& tab) const;
  void PickImage(Tab tab, RichEditorController editor, ViewHandle focus, bool replace, bool inline_image) const;
  void LocateAttachments(Tab tab) const;

  void RequestSave(Tab tab, WriteKind kind) const;
  void RequestClose(Tab tab = {}) const;
  bool OnCloseRequest() const;

private:
  void ShowCommandPalette(bool keyboard) const;
  void Status(const Tab& tab, StringVariant message) const;
  void Launch(bool uses_picker, std::function<Task<void>()> action, std::function<void(std::string)> failed = {}) const;
  void AddTab(std::string name, RichDocument snapshot, std::shared_ptr<const SavedDocument> saved = {}) const;
  bool ActivateExisting(const Target& target) const;
  Task<void> OpenTarget(Target target) const;
  Task<Entry> AddFolder(FileReference directory) const;
  void LoadFolder(Entry entry) const;
  void PromptNewChild(Entry folder, ChildKind kind, std::string name = {}, std::string message = {}) const;
  void CreateChild(Entry folder, ChildKind kind, std::string name) const;

  void StartSave(Tab tab, WriteKind kind, std::function<void(bool)> complete = {}) const;
  void FinishSave(const Save& request, bool saved) const;
  void MarkSaved(const Save& request, Target target, SavePayload payload) const;
  void ChooseOutput(Save request, WriteKind kind, DocumentFormat preferred, std::string message = {}) const;
  void PrepareOutput(Save request, WriteKind kind, DocumentFormat format, std::string name) const;
  void ExplainMarkdownLoss(Save request, WriteKind kind, RichDocument snapshot, std::string name,
                           std::string message) const;
  void WriteNew(Save request, WriteKind kind, std::string name, SavePayload payload) const;
  void SaveOriginal(Save request) const;
  void ChooseOriginalFolder(Save request, std::shared_ptr<const SavedDocument> original, SavePayload payload) const;
  Task<void> WriteOriginalFile(Save request, std::shared_ptr<const SavedDocument> original, Target target,
                               SavePayload payload) const;
  void OfferSaveAs(Save request, std::string message) const;

  void ShowChoices(StringVariant title, StringVariant message, std::vector<Choice> choices) const;
  void ContinueClose() const;
  void SaveBeforeClose(std::shared_ptr<const std::vector<Tab>> dirty, std::size_t index) const;
  void FinishClose() const;
  void CancelClose() const;
};

bool WorkbenchActions::CanExecute(FolioCommand command) const {
  if (!Available()) {
    return false;
  }
  const auto tab = workspace->Selected();
  switch (command) {
  case FolioCommand::OpenFile:
    return picker->CanOpenFiles();
  case FolioCommand::Save:
    return tab && !tab->Busy();
  case FolioCommand::SaveAs:
    return tab && !tab->Busy() && picker->CanOpenDirectories(true);
  case FolioCommand::Find:
    return static_cast<bool>(tab);
  case FolioCommand::OpenCommandPalette:
    return !workspace->palette_open;
  case FolioCommand::NewDocument:
  case FolioCommand::ToggleSidebar:
  case FolioCommand::Settings:
    return true;
  }
  return false;
}

void WorkbenchActions::Execute(FolioCommand command, bool keyboard) const {
  if (!CanExecute(command)) {
    return;
  }
  switch (command) {
  case FolioCommand::NewDocument:
    NewDocument();
    break;
  case FolioCommand::OpenFile:
    PickOpen(OpenKind::File);
    break;
  case FolioCommand::Save:
    RequestSave(workspace->Selected(), WriteKind::Save);
    break;
  case FolioCommand::SaveAs:
    RequestSave(workspace->Selected(), WriteKind::SaveAs);
    break;
  case FolioCommand::Find:
    workspace->Selected()->find.Update([](FindPanelState& find) {
      find.open = true;
      ++find.focus_request;
    });
    break;
  case FolioCommand::ToggleSidebar:
    sidebar_open = !sidebar_open.Get();
    break;
  case FolioCommand::Settings:
    if (modal_sidebar) {
      sidebar_open = false;
    }
    dialog.Show(SettingsDialog);
    break;
  case FolioCommand::OpenCommandPalette:
    ShowCommandPalette(keyboard);
    break;
  }
}

void WorkbenchActions::ShowCommandPalette(bool keyboard) const {
  const auto tab = workspace->Selected();
  const auto editor = tab ? tab->editor.Get() : std::nullopt;
  const auto current = [workspace = workspace, window = window, tab] {
    return window.IsOpen() && workspace->Selected() == tab;
  };
  const auto editor_current = [current, tab, editor] {
    return current() && editor && tab->editor.Get() == editor && tab->operation.Get() != Operation::PickImage;
  };
  const auto state = [self = *this, current, editor_current, editor](const ShortcutCommand& entry) -> CommandState {
    if (!current()) {
      return {};
    }
    if (const auto* command = std::get_if<FolioCommand>(&entry.command)) {
      return {self.CanExecute(*command)};
    }
    return self.Available() && editor_current()
        ? editor->GetCommandState(std::get<EditorCommand>(entry.command)) : CommandState{};
  };
  const auto invoke = [self = *this, state, current, editor_current, tab, editor](
      const ShortcutCommand& entry) -> std::function<void()> {
    if (!state(entry).enabled) {
      return {};
    }
    if (const auto* command = std::get_if<FolioCommand>(&entry.command)) {
      return [self, current, command = *command] {
        if (current()) {
          self.Execute(command);
        }
      };
    }
    editor->Execute(std::get<EditorCommand>(entry.command));
    return [self, editor_current, tab] {
      if (self.Available() && editor_current()) {
        if (const auto& focus = tab->editor_focus.Get()) {
          focus->RequestFocus();
        }
      }
    };
  };
  const CommandPaletteOptions options{
      .initial_focus = palette_focus,
      .window_tasks = tasks,
      .state = state,
      .invoke = invoke,
      .on_closed = [workspace = workspace, opener = file_menu_focus, keyboard, current](bool invoked) {
        workspace->palette_open = false;
        if (!invoked && !keyboard && current()) {
          opener.RequestFocus();
        }
      },
  };
  dialog.Show([options](DialogContext context) { return CommandPalette(context, options); },
              {.initial_focus = palette_focus});
  workspace->palette_open = true;
}

void WorkbenchActions::Status(const Tab& tab, StringVariant message) const {
  if (tab) {
    tab->status = resources->GetString(message);
  } else {
    workspace->status = resources->GetString(message);
  }
}

void WorkbenchActions::Launch(bool uses_picker, std::function<Task<void>()> action,
                              std::function<void(std::string)> failed) const {
  if (uses_picker) {
    workspace->picking = true;
  }
  tasks.Launch([self = *this, uses_picker, action = std::move(action), failed = std::move(failed)]() -> Task<void> {
    std::string error;
    try {
      co_await action();
    } catch (const std::exception& exception) {
      error = exception.what();
    }
    if (uses_picker) {
      self.workspace->picking = false;
    }
    if (!error.empty()) {
      if (failed) {
        failed(std::move(error));
      } else {
        self.Status({}, std::move(error));
      }
    }
    self.ContinueClose();
  });
}

void WorkbenchActions::AddTab(std::string name, RichDocument snapshot, std::shared_ptr<const SavedDocument> saved) const {
  std::optional<File> directory;
  try {
    directory = application.Directories().temporary_directory.Child(UniqueName("folio-document-"));
  } catch (const std::logic_error&) {
    // A draft can still be edited on a host without application storage.
  }
  auto tab = std::make_shared<DocumentTab>(++folio->next_document, std::move(name), std::move(directory),
                                           std::move(snapshot), std::move(saved));
  workspace->tabs.Update([&](auto& open) { open.push_back(tab); });
  Select(tab);
}

void WorkbenchActions::NewDocument(bool sample) const {
  if (!Available()) {
    return;
  }
  Status({}, {});
  if (sample) {
    auto imported = LoadSample();
    if (!imported.document) {
      Status({}, IssueMessage(imported.issues));
      return;
    }
    AddTab("Field notes", std::move(*imported.document));
    return;
  }
  const auto number = ++folio->next_draft;
  auto name = resources->GetString(l10n::untitled);
  if (number != 1) {
    name += " " + std::to_string(number);
  }
  AddTab(std::move(name), RichDocument{});
}

void WorkbenchActions::NewWindow() const {
  if (Available() && application.SupportsMultipleWindows()) {
    try {
      application.OpenWindow(App, FolioWindowOptions());
    } catch (const std::exception& error) {
      Status({}, error.what());
    }
  }
}

void WorkbenchActions::Select(const Tab& tab) const {
  workspace->selected = tab->id;
  workspace->drawer_open = false;
  if (const auto& original = tab->saved.Get()) {
    folio->Remember(original->target);
  }
}

bool WorkbenchActions::ActivateExisting(const Target& target) const {
  for (const auto& weak : folio->windows) {
    const auto owner = weak.lock();
    if (!owner) {
      continue;
    }
    for (const auto& tab : owner->tabs.Get()) {
      const auto& original = tab->saved.Get();
      if (original && SameFile(original->target, target)) {
        if (target->directory && tab->attachments->directories.empty()) {
          tab->SetAttachmentDirectory(*target->directory);
        }
        if (owner == workspace) {
          Select(tab);
        } else if (owner->window && owner->window->IsOpen()) {
          owner->selected = tab->id;
          owner->drawer_open = false;
          owner->window->Activate();
          workspace->drawer_open = false;
          folio->Remember(original->target);
        } else {
          continue;
        }
        return true;
      }
    }
  }
  return false;
}

Task<void> WorkbenchActions::OpenTarget(Target target) const {
  if (!target->identity) {
    target->identity = co_await LocalIdentity(target->reference);
  }
  if (ActivateExisting(target)) {
    co_return;
  }
  auto read = co_await ReadDocument(target->reference);
  if (!read.Succeeded()) {
    Status({}, read.Error().message);
    co_return;
  }
  if (ActivateExisting(target)) {
    co_return;
  }
  auto source = std::move(read.Value());
  auto original = std::make_shared<const SavedDocument>(
      SavedDocument{target, source.format, source.document, std::move(source.bytes)});
  AddTab(target->reference.Name(), std::move(source.document), std::move(original));
}

void WorkbenchActions::OpenKnown(Target target) const {
  if (Available()) {
    Status({}, {});
    Launch(true, [self = *this, target = std::move(target)]() -> Task<void> {
      co_await self.OpenTarget(target);
    });
  }
}

void WorkbenchActions::PickOpen(OpenKind kind) const {
  if (!Available()) {
    return;
  }
  Status({}, {});
  Launch(true, [self = *this, kind]() -> Task<void> {
    if (kind == OpenKind::File) {
      const auto file = co_await self.picker->OpenFileAsync({
          .name = self.resources->GetString(l10n::rich_editor_documents), .extensions = {"json", "md", "markdown"},
          .content_types = {"application/json", "text/markdown", "text/plain"},
      });
      if (file) {
        co_await self.OpenTarget(std::make_shared<DocumentFile>(DocumentFile{*file, {}, {}}));
      }
      co_return;
    }
    const auto folder = co_await self.picker->OpenDirectoryAsync();
    if (!folder) {
      co_return;
    }
    if (kind == OpenKind::Folder) {
      co_await self.AddFolder(*folder);
      co_return;
    }
    auto file = co_await FindPackageDocument(*folder);
    if (file.Succeeded()) {
      co_await self.OpenTarget(std::make_shared<DocumentFile>(DocumentFile{std::move(file.Value()), folder, {}}));
    } else {
      self.Status({}, file.Error().message);
    }
  });
}

Task<Entry> WorkbenchActions::AddFolder(FileReference directory) const {
  const auto identity = co_await LocalIdentity(directory);
  if (identity) {
    for (const auto& folder : workspace->folders.Get()) {
      if (identity == folder->identity) {
        if (directory.CanWrite() && !folder->directory->CanWrite()) {
          folder->directory = std::move(directory);
          folder->expanded = true;
          LoadFolder(folder);
        } else {
          SetExpanded(folder, true);
        }
        workspace->folders_open = true;
        workspace->drawer_open = true;
        workspace->sidebar_visible = true;
        co_return folder;
      }
    }
  }
  auto name = directory.Name();
  if (name.empty()) {
    name = identity ? identity->Path() : resources->GetString(l10n::folder);
  }
  auto folder = std::make_shared<FolderEntry>(FolderEntry{
      ++workspace->next_entry, std::move(name), std::move(directory), {}, identity,
  });
  workspace->folders.Update([&](auto& roots) { roots.push_back(folder); });
  workspace->folders_open = true;
  workspace->sidebar_visible = true;
  workspace->drawer_open = true;
  SetExpanded(folder, true);
  co_return folder;
}

void WorkbenchActions::SetExpanded(const Entry& entry, bool expanded) const {
  if (entry->directory) {
    entry->expanded = expanded;
    if (expanded && entry->load.Get() != FolderLoad::Loading) {
      LoadFolder(entry);
    }
  }
}

void WorkbenchActions::LoadFolder(Entry entry) const {
  const auto generation = ++entry->load_generation;
  entry->load = FolderLoad::Loading;
  entry->error = std::string{};
  Launch(false, [self = *this, entry, generation]() -> Task<void> {
    auto children = co_await entry->directory->ListChildrenAsync();
    if (entry->load_generation != generation) {
      co_return;
    }
    if (!children.Succeeded()) {
      entry->error = children.Error().message;
      entry->load = FolderLoad::Failed;
      co_return;
    }
    const auto old_children = entry->children.Get();
    std::vector<Entry> rows;
    for (auto& child : children.Value()) {
      auto name = child.Name();
      if (name.empty() || (child.Type() != FileType::Directory &&
          (child.Type() != FileType::File || !FormatOf(name)))) {
        continue;
      }
      const auto existing = std::ranges::find_if(old_children, [&](const Entry& row) {
        return row->directory && child.Type() == FileType::Directory && row->name == name;
      });
      if (existing != old_children.end()) {
        auto& previous = *(*existing)->directory;
        const auto local = previous.AsFile();
        if (child.CanWrite() || !previous.CanWrite() || !local || child.AsFile() != local) {
          previous = std::move(child);
        }
        rows.push_back(*existing);
      } else {
        auto row = std::make_shared<FolderEntry>(FolderEntry{++self.workspace->next_entry, std::move(name), {}, {}, {}});
        if (child.Type() == FileType::Directory) {
          row->directory = std::move(child);
        } else {
          row->file = std::make_shared<DocumentFile>(DocumentFile{std::move(child), entry->directory, {}});
        }
        rows.push_back(std::move(row));
      }
    }
    std::ranges::sort(rows, [](const Entry& left, const Entry& right) {
      if (left->directory.has_value() != right->directory.has_value()) {
        return left->directory.has_value();
      }
      return left->name < right->name;
    });
    entry->children = std::move(rows);
    entry->load = FolderLoad::Loaded;
  }, [entry, generation](std::string error) {
    if (entry->load_generation == generation) {
      entry->error = std::move(error);
      entry->load = FolderLoad::Failed;
    }
  });
}

void WorkbenchActions::PickImage(Tab tab, RichEditorController editor, ViewHandle focus, bool replace,
                                bool inline_image) const {
  if (!Available() || tab->Busy() || !tab->working_directory) {
    return;
  }
  tab->operation = Operation::PickImage;
  Launch(true, [self = *this, tab, editor, focus, replace, inline_image]() -> Task<void> {
    const auto selected = co_await self.picker->OpenFileAsync({
        .name = self.resources->GetString(l10n::png_jpeg_images), .extensions = {"png", "jpg", "jpeg"},
        .content_types = {"image/png", "image/jpeg"},
    });
    if (selected) {
      auto saved = co_await ImportImage(*tab->working_directory, *selected);
      if (saved.Succeeded()) {
        const ImageBlockData image{std::move(saved.Value()), selected->Name(), {}};
        const auto command = replace ? EditorCommand{ReplaceImage{image.source}}
            : inline_image ? EditorCommand{InsertInlineObject{InlineImage(image)}} : EditorCommand{InsertImage{image}};
        self.Status(tab, editor.Execute(command) ? (replace ? l10n::image_replaced : l10n::image_inserted)
                                                : l10n::select_image_or_paragraph);
      } else {
        self.Status(tab, saved.Error().message);
      }
    }
    tab->operation = Operation::Idle;
    if (self.workspace->selected.Get() == tab->id && !self.workspace->closing.Get()) {
      self.tasks.Post([focus] { focus.RequestFocus(); });
    }
  }, [self = *this, tab](std::string error) {
    tab->operation = Operation::Idle;
    self.Status(tab, std::move(error));
  });
}

void WorkbenchActions::LocateAttachments(Tab tab) const {
  if (!Available() || tab->Busy()) {
    return;
  }
  tab->operation = Operation::LocateAttachments;
  Launch(true, [self = *this, tab]() -> Task<void> {
    if (auto folder = co_await self.picker->OpenDirectoryAsync()) {
      tab->SetAttachmentDirectory(std::move(*folder));
      self.Status(tab, l10n::attachment_folder_linked);
    }
    tab->operation = Operation::Idle;
  }, [self = *this, tab](std::string error) {
    tab->operation = Operation::Idle;
    self.Status(tab, std::move(error));
  });
}

std::string NameForFormat(std::string name, DocumentFormat format) {
  if (FormatOf(name) == format) {
    return name;
  }
  if (FormatOf(name)) {
    name.resize(name.rfind('.'));
  }
  return name + (format == DocumentFormat::Json ? ".json" : ".md");
}

std::string CopyName(std::string name, DocumentFormat format) {
  auto copy = NameForFormat(std::move(name), format);
  copy.insert(copy.rfind('.'), "-copy");
  return copy;
}

[[huxerui::composable]]
View OutputDialog(DialogContext context, StringVariant title, std::string name, DocumentFormat preferred,
                  std::string message, StringVariant action_label,
                  std::function<void(DocumentFormat, std::string)> action, std::function<void()> cancel) {
  const auto& theme = UseTheme();
  const auto format = UseState(preferred);
  const auto filename = UseState(TextEditingValue::FromText(NameForFormat(std::move(name), preferred)));
  const auto choose = [format, filename](DocumentFormat next) {
    if (next != format.Get()) {
      format = next;
      filename = TextEditingValue::FromText(NameForFormat(filename->text, next));
    }
  };
  const auto format_button = [&](StringVariant label, DocumentFormat value) {
    const bool selected = format.Get() == value;
    return Button(label).OnClick([choose, value] { choose(value); }).With(
        Background(selected ? theme.colors.primary_container : theme.colors.surface_container),
        Foreground(selected ? theme.colors.on_primary_container : theme.colors.on_surface),
        Semantics{.selected = selected});
  };
  const auto field = filename.Get();
  const bool valid = !field.composition && ValidDocumentName(field.text, format.Get());
  const auto submit = [=] {
    if (valid) {
      context.Dismiss();
      action(format.Get(), field.text);
    }
  };
  return DialogPanel(Column {
    Text(std::move(title), TextRole::Title),
    message.empty() ? View{} : Text(std::move(message)).With(Foreground(theme.colors.on_surface_variant)),
    Flow {
      format_button("RichEditor JSON", DocumentFormat::Json),
      format_button("Markdown", DocumentFormat::Markdown),
    }.With(Spacing(6.0F)),
    TextField(filename).Label(l10n::file_name).Variant(TextFieldVariant::Outlined)
        .OnChanged([filename](const TextEditingValue& next) { filename = next; })
        .OnSubmitted(submit),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context, cancel] {
        context.Dismiss();
        cancel();
      }),
      Button(std::move(action_label)).OnClick(submit).With(Enabled(valid))),
  }, theme, 420.0F);
}

[[huxerui::composable]]
View ChoiceDialog(DialogContext context, const StringVariant& title, const StringVariant& message,
                  const std::vector<Choice>& choices) {
  const auto& theme = UseTheme();
  const auto cancel_focus = UseViewHandle();
  Lifecycle([cancel_focus] { cancel_focus.RequestFocus(); });
  View cancel;
  View primary;
  View secondary;
  bool destructive = false;
  for (const auto& choice : choices) {
    auto button = Button(choice.label).OnClick([context, action = choice.action] {
      context.Dismiss();
      action();
    });
    switch (choice.tone) {
    case ChoiceTone::Cancel: cancel = std::move(button).With(cancel_focus); break;
    case ChoiceTone::Primary: primary = button; break;
    case ChoiceTone::Secondary:
    case ChoiceTone::Destructive:
      secondary = button;
      destructive = choice.tone == ChoiceTone::Destructive;
      break;
    }
  }
  return ScrollableDialogPanel(
    Text(title, TextRole::Title),
    ScrollView(Text(message).With(Foreground(theme.colors.on_surface_variant)))
        .With(Frame{.max_height = 168.0F}, ScrollBar()),
    Column {
      Divider(),
      DialogActions(theme, cancel, primary, secondary, destructive),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    theme, 440.0F);
}

void WorkbenchActions::ShowChoices(StringVariant title, StringVariant message, std::vector<Choice> choices) const {
  dialog.Show(DialogFactory{[title = std::move(title), message = std::move(message),
                            choices = std::move(choices)](DialogContext context) {
    return ChoiceDialog(context, title, message, choices);
  }}, {.dismiss_on_outside_press = false, .dismiss_on_cancel = false});
}

bool ValidChildName(std::string_view name) {
  return !name.empty() && !name.ends_with('.') && !name.ends_with(' ') &&
      name.find_first_of("\\/:*?\"<>|") == name.npos &&
      std::ranges::none_of(name, [](unsigned char ch) { return ch < 32; });
}

[[huxerui::composable]]
View NewChildDialog(DialogContext context, ChildKind kind, std::string folder, std::string name, std::string message,
                    std::function<void(std::string)> create) {
  const auto& theme = UseTheme();
  const bool file = kind == ChildKind::File;
  auto initial = TextEditingValue::FromText(name.empty()
      ? (file ? UseString(l10n::untitled) + ".md" : UseString(l10n::new_folder)) : name);
  initial.selection.anchor = 0;
  const auto value = UseState(std::move(initial));
  const auto candidate = file && !FormatOf(value->text) ? value->text + ".md" : value->text;
  const bool valid = !value->composition && ValidChildName(value->text) &&
      (!file || ValidDocumentName(candidate, *FormatOf(candidate)));
  const auto submit = [context, create, candidate, valid] {
    if (valid) {
      context.Dismiss();
      create(candidate);
    }
  };
  return DialogPanel(Column {
    Text(file ? l10n::new_file : l10n::new_folder, TextRole::Title),
    Text(StringVariant::Format(l10n::create_in, folder), TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    message.empty() ? View{} : Text(std::move(message), TextRole::Label).With(Foreground(theme.colors.error)),
    TextField(value).Label(file ? l10n::file_name : l10n::folder_name).Variant(TextFieldVariant::Outlined)
        .OnChanged([value](const TextEditingValue& next) { value = next; })
        .OnSubmitted(submit).Key("name"),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(l10n::create).OnClick(submit).With(Enabled(valid))).Key("actions"),
  }, theme, 400.0F);
}

void WorkbenchActions::RequestNewChild(Entry folder, ChildKind kind) const {
  if (!Available() || !folder->directory) {
    return;
  }
  if (folder->directory->CanWrite()) {
    PromptNewChild(std::move(folder), kind);
    return;
  }
  if (!picker->CanOpenDirectories(true)) {
    return;
  }
  ShowChoices(l10n::allow_folder_changes, StringVariant::Format(l10n::choose_writable_folder, folder->name), {
      {l10n::cancel, [] {}},
      {l10n::choose_folder, [self = *this, folder, kind] {
        self.Launch(true, [self, folder, kind]() -> Task<void> {
          const auto granted = co_await self.picker->OpenDirectoryAsync(true);
          if (!granted) {
            co_return;
          }
          const auto expected = co_await LocalIdentity(*folder->directory);
          if (expected && (co_await LocalIdentity(*granted)) != expected) {
            self.Status({}, l10n::choose_sidebar_folder);
            co_return;
          }
          if (expected) {
            folder->directory = *granted;
            self.PromptNewChild(folder, kind);
          } else {
            self.PromptNewChild(co_await self.AddFolder(*granted), kind);
          }
        });
      }, ChoiceTone::Primary},
  });
}

void WorkbenchActions::PromptNewChild(Entry folder, ChildKind kind, std::string name, std::string message) const {
  const auto local = folder->directory->AsFile();
  const auto location = local ? local->Path() : folder->name;
  dialog.Show(DialogFactory{[self = *this, folder = std::move(folder), kind, location,
                            name = std::move(name), message = std::move(message)](DialogContext context) {
    return NewChildDialog(context, kind, location, name, message,
                          [self, folder, kind](std::string value) { self.CreateChild(folder, kind, std::move(value)); });
  }});
}

Task<IoResult<FileReference>> CreateNamedFolder(FileReference parent, const std::string& name) {
  const IoError exists{IoErrorCode::AlreadyExists, "A file or folder already uses that name"};
  if (const auto local = parent.AsFile()) {
    const auto info = co_await local->Child(name).StatAsync();
    if (info.Succeeded()) {
      co_return IoResult<FileReference>::Failure(exists);
    }
    if (info.Error().code != IoErrorCode::NotFound) {
      co_return IoResult<FileReference>::Failure(info.Error());
    }
  } else {
    auto children = co_await parent.ListChildrenAsync();
    if (!children.Succeeded()) {
      co_return IoResult<FileReference>::Failure(children.Error());
    }
    if (std::ranges::any_of(children.Value(), [&](const FileReference& child) { return child.Name() == name; })) {
      co_return IoResult<FileReference>::Failure(exists);
    }
  }
  co_return co_await parent.CreateDirectoryAsync(name);
}

Task<IoResult<FileReference>> CreateEmptyFile(FileReference parent, const std::string& name, File temporary) {
  auto encoded = Codec(*FormatOf(name)).Export(RichDocument{});
  const auto staged = temporary.Child(UniqueName("folio-new-"));
  if (!encoded.text || !(co_await temporary.CreateDirectoriesAsync()) ||
      !(co_await staged.WriteStringAsync(*encoded.text))) {
    (void)co_await staged.DeleteAsync();
    co_return IoResult<FileReference>::Failure({IoErrorCode::Io, "Could not prepare the new document"});
  }
  auto created = co_await parent.CopyFileFromAsync(staged, name, false);
  (void)co_await staged.DeleteAsync();
  co_return created;
}

void WorkbenchActions::CreateChild(Entry folder, ChildKind kind, std::string name) const {
  if (!Available() || !folder->directory || !folder->directory->CanWrite()) {
    return;
  }
  Status({}, {});
  Launch(true, [self = *this, folder = std::move(folder), kind, name = std::move(name)]() -> Task<void> {
    const auto directory = *folder->directory;
    auto created = kind == ChildKind::File
        ? co_await CreateEmptyFile(directory, name, self.application.Directories().temporary_directory)
        : co_await CreateNamedFolder(directory, name);
    if (!created.Succeeded()) {
      if (created.Error().code == IoErrorCode::AlreadyExists) {
        self.PromptNewChild(folder, kind, name, self.resources->GetString(l10n::name_already_used));
      } else {
        self.Status({}, created.Error().message);
      }
      co_return;
    }
    folder->expanded = true;
    self.LoadFolder(folder);
    if (kind == ChildKind::File) {
      co_await self.OpenTarget(std::make_shared<DocumentFile>(
          DocumentFile{std::move(created.Value()), directory, {}}));
    }
  });
}

void WorkbenchActions::RequestSave(Tab tab, WriteKind kind) const {
  if (!tab || !Available() || tab->Busy()) {
    return;
  }
  file_menu_focus.RequestFocus();
  tasks.Post([self = *this, tab = std::move(tab), kind] {
    if (self.Available()) {
      self.StartSave(tab, kind);
    }
  });
}

void WorkbenchActions::StartSave(Tab tab, WriteKind kind, std::function<void(bool)> complete) const {
  if (tab->Busy()) {
    if (complete) {
      complete(false);
    }
    return;
  }
  auto request = std::make_shared<SaveRequest>(SaveRequest{std::move(tab), std::move(complete)});
  request->tab->operation = kind == WriteKind::Export ? Operation::Export : Operation::Save;
  Status({}, {});
  Status(request->tab, {});
  if (kind == WriteKind::Save) {
    SaveOriginal(request);
  } else {
    const auto& original = request->tab->saved.Get();
    const auto preferred = kind == WriteKind::Export ? DocumentFormat::Markdown
        : original ? original->format : DocumentFormat::Markdown;
    ChooseOutput(request, kind, preferred);
  }
}

void WorkbenchActions::FinishSave(const Save& request, bool saved) const {
  request->tab->operation = Operation::Idle;
  if (auto complete = std::exchange(request->complete, {})) {
    tasks.Post([complete = std::move(complete), saved] { complete(saved); });
  }
  ContinueClose();
}

void WorkbenchActions::MarkSaved(const Save& request, Target target, SavePayload payload) const {
  const auto bytes = std::as_bytes(std::span(payload.text));
  request->tab->saved = std::make_shared<const SavedDocument>(SavedDocument{
      target, payload.format, std::move(payload.snapshot), Bytes(bytes.begin(), bytes.end()),
  });
  if (target->directory) {
    request->tab->BindAttachmentDirectory(*target->directory);
  }
  folio->Remember(std::move(target));
  Status(request->tab, StringVariant::Format(l10n::saved_document, request->tab->Name()));
  FinishSave(request, true);
}

void WorkbenchActions::ChooseOutput(Save request, WriteKind kind, DocumentFormat preferred, std::string message) const {
  const auto cancel = [self = *this, request] { self.FinishSave(request, false); };
  if (kind == WriteKind::SaveAs && !picker->CanOpenDirectories(true)) {
    if (!message.empty()) {
      message += "\n\n";
    }
    message += resources->GetString(l10n::save_folder_required);
    std::vector<Choice> choices{{l10n::cancel, cancel}};
    if (picker->CanSaveFiles()) {
      choices.push_back({l10n::export_copy_menu, [self = *this, request, preferred] {
        self.ChooseOutput(request, WriteKind::Export, preferred);
      }, ChoiceTone::Primary});
    }
    ShowChoices(l10n::save_as, std::move(message), std::move(choices));
    return;
  }
  const auto title = kind == WriteKind::Export ? l10n::export_copy : l10n::save_as;
  const auto action = kind == WriteKind::Export ? l10n::export_copy_menu : l10n::choose_folder_and_save;
  auto name = request->tab->Name();
  if (kind == WriteKind::Export) {
    name = CopyName(std::move(name), preferred);
  }
  dialog.Show(DialogFactory{[self = *this, request, kind, preferred, message = std::move(message),
                             title, action, name = std::move(name), cancel](DialogContext context) {
    return OutputDialog(context, title, name, preferred, message, action,
                        [self, request, kind](DocumentFormat format, std::string name) {
                          self.PrepareOutput(request, kind, format, std::move(name));
                        }, cancel);
  }}, {.dismiss_on_outside_press = false, .dismiss_on_cancel = false});
}

void WorkbenchActions::PrepareOutput(Save request, WriteKind kind, DocumentFormat format, std::string name) const {
  auto snapshot = request->tab->Snapshot();
  auto encoded = Codec(format).Export(snapshot);
  if (encoded.text) {
    WriteNew(request, kind, std::move(name), {std::move(snapshot), format, std::move(*encoded.text)});
  } else if (format == DocumentFormat::Markdown) {
    ExplainMarkdownLoss(request, kind, std::move(snapshot), std::move(name), IssueMessage(encoded.issues));
  } else {
    Status(request->tab, IssueMessage(encoded.issues));
    FinishSave(request, false);
  }
}

void WorkbenchActions::ExplainMarkdownLoss(Save request, WriteKind kind, RichDocument snapshot, std::string name,
                                          std::string message) const {
  if (kind != WriteKind::Export) {
    name = CopyName(std::move(name), DocumentFormat::Markdown);
  }
  std::vector<Choice> choices{{l10n::cancel, [self = *this, request] { self.FinishSave(request, false); }}};
  if (kind != WriteKind::Export && picker->CanOpenDirectories(true)) {
    choices.push_back({l10n::save_as_json, [self = *this, request] {
      self.ChooseOutput(request, WriteKind::SaveAs, DocumentFormat::Json);
    }, ChoiceTone::Primary});
  }
  if (picker->CanSaveFiles() || picker->CanOpenDirectories(true)) {
    choices.push_back({l10n::export_with_loss, [self = *this, request, snapshot = std::move(snapshot), name = std::move(name)] {
      const auto& codec = Codec(DocumentFormat::Markdown);
      auto encoded = codec.Export(snapshot, LossPolicy::AllowLoss);
      if (encoded.text) {
        auto copy = codec.Import(*encoded.text, LossPolicy::AllowLoss);
        if (copy.document) {
          self.WriteNew(request, WriteKind::Export, name,
                        {std::move(*copy.document), DocumentFormat::Markdown, std::move(*encoded.text)});
          return;
        }
      }
      self.Status(request->tab, IssueMessage(encoded.issues));
      self.FinishSave(request, false);
    }, ChoiceTone::Destructive});
  }
  ShowChoices(l10n::markdown_cannot_preserve, std::move(message), std::move(choices));
}

void WorkbenchActions::WriteNew(Save request, WriteKind kind, std::string name, SavePayload payload) const {
  const bool export_copy = kind == WriteKind::Export;
  const bool picker_export = export_copy && picker->CanSaveFiles() && !HasLocalAttachments(payload.snapshot);
  if (workspace->picking.Get() || (!picker_export && !picker->CanOpenDirectories(true))) {
    Status(request->tab, l10n::writable_folder_required);
    FinishSave(request, false);
    return;
  }
  request->tab->operation = export_copy ? Operation::Export : Operation::Save;
  if (!request->tab->working_directory) {
    Status(request->tab, l10n::temporary_storage_unavailable);
    FinishSave(request, false);
    return;
  }
  const auto temporary = *request->tab->working_directory->Parent();
  const auto staged = temporary.Child(UniqueName("folio-output-") + (payload.format == DocumentFormat::Json ? ".json" : ".md"));
  const auto failed = [self = *this, request, staged](std::string message) {
    (void)staged.Delete();
    self.Status(request->tab, std::move(message));
    self.FinishSave(request, false);
  };
  if (picker_export) {
    // Present the picker in the same user gesture on Web.
    if (!temporary.CreateDirectories() || !staged.WriteString(payload.text)) {
      failed(resources->GetString(l10n::prepare_export_failed));
      return;
    }
    const bool markdown = payload.format == DocumentFormat::Markdown;
    const SaveFileOptions options{
        .suggested_name = std::move(name),
        .filter = markdown ? FilePickerFilter{.name = "Markdown", .extensions = {"md", "markdown"},
                                               .content_types = {"text/markdown"}}
                           : FilePickerFilter{.name = "RichEditor JSON", .extensions = {"json"},
                                               .content_types = {"application/json"}},
    };
    Launch(true, [self = *this, request, staged, options]() -> Task<void> {
      const bool exported = co_await self.picker->SaveFileAsync(staged, options);
      (void)co_await staged.DeleteAsync();
      self.Status(request->tab, exported ? l10n::exported_copy : l10n::export_unsuccessful);
      self.FinishSave(request, false);
    }, failed);
    return;
  }
  Launch(true, [self = *this, request, temporary, staged, name = std::move(name),
                payload = std::move(payload), export_copy]() mutable -> Task<void> {
    const auto parent = co_await self.picker->OpenDirectoryAsync(true);
    if (!parent) {
      self.FinishSave(request, false);
      co_return;
    }
    if (!(co_await temporary.CreateDirectoriesAsync()) || !(co_await staged.WriteStringAsync(payload.text))) {
      (void)co_await staged.DeleteAsync();
      self.Status(request->tab, l10n::prepare_document_failed);
      self.FinishSave(request, false);
      co_return;
    }
    auto copied = co_await WriteCopy(*parent, staged, name, request->tab->working_directory,
                                    request->tab->attachments->directories, payload.snapshot);
    (void)co_await staged.DeleteAsync();
    if (!copied.Succeeded()) {
      self.Status(request->tab, copied.Error().message);
      self.FinishSave(request, false);
    } else if (export_copy) {
      self.Status(request->tab, StringVariant::Format(l10n::exported_document, copied.Value().directory.Name() + "/" + name));
      self.FinishSave(request, false);
    } else {
      auto target = std::make_shared<DocumentFile>(
          DocumentFile{std::move(copied.Value().file), std::move(copied.Value().directory), {}});
      target->identity = co_await LocalIdentity(target->reference);
      self.MarkSaved(request, std::move(target), std::move(payload));
    }
  }, failed);
}

void WorkbenchActions::SaveOriginal(Save request) const {
  const auto original = request->tab->saved.Get();
  if (!original) {
    ChooseOutput(request, WriteKind::SaveAs, DocumentFormat::Markdown);
    return;
  }
  auto snapshot = request->tab->Snapshot();
  if (snapshot == original->snapshot) {
    Status(request->tab, l10n::document_already_saved);
    FinishSave(request, true);
    return;
  }
  auto encoded = Codec(original->format).Export(snapshot);
  if (!encoded.text) {
    if (original->format == DocumentFormat::Markdown) {
      ExplainMarkdownLoss(request, WriteKind::Save, std::move(snapshot), request->tab->Name(),
                          IssueMessage(encoded.issues));
    } else {
      Status(request->tab, IssueMessage(encoded.issues));
      FinishSave(request, false);
    }
    return;
  }
  SavePayload payload{std::move(snapshot), original->format, std::move(*encoded.text)};
  const auto& target = original->target;
  if (!target->reference.AsFile()) {
    OfferSaveAs(request, resources->GetString(l10n::provider_cannot_replace));
    return;
  }
  if (!target->reference.CanWrite() ||
      ((!target->directory || !target->directory->CanWrite()) &&
       NeedsAttachmentWrite(target->reference, request->tab->attachments->directories,
                            payload.snapshot, original->snapshot))) {
    ChooseOriginalFolder(request, original, std::move(payload));
    return;
  }
  Launch(false, [self = *this, request, original, payload = std::move(payload)]() mutable -> Task<void> {
    co_await self.WriteOriginalFile(request, original, original->target, std::move(payload));
  }, [self = *this, request](std::string error) { self.OfferSaveAs(request, std::move(error)); });
}

void WorkbenchActions::ChooseOriginalFolder(Save request, std::shared_ptr<const SavedDocument> original,
                                           SavePayload payload) const {
  if (!picker->CanOpenDirectories(true)) {
    OfferSaveAs(request, resources->GetString(l10n::original_needs_folder));
    return;
  }
  ShowChoices(l10n::authorize_original_folder,
      StringVariant::Format(l10n::authorize_original_folder_hint, request->tab->Name()), {
      {l10n::cancel, [self = *this, request] { self.FinishSave(request, false); }},
      {l10n::save_as_menu, [self = *this, request] { self.OfferSaveAs(request, {}); }, ChoiceTone::Secondary},
      {l10n::choose_folder, [self = *this, request, original, payload = std::move(payload)] {
        self.Launch(true, [self, request, original, payload]() mutable -> Task<void> {
          const auto folder = co_await self.picker->OpenDirectoryAsync(true);
          if (!folder) {
            self.FinishSave(request, false);
            co_return;
          }
          const auto identity = co_await LocalIdentity(*folder);
          if (!identity || !original->target->identity || original->target->identity->Parent() != identity) {
            self.OfferSaveAs(request, self.resources->GetString(l10n::original_not_in_folder));
            co_return;
          }
          auto file = co_await FindChild(*folder, original->target->reference.Name(), FileType::File);
          if (!file.Succeeded()) {
            self.OfferSaveAs(request, file.Error().message);
            co_return;
          }
          auto target = std::make_shared<DocumentFile>(
              DocumentFile{std::move(file.Value()), folder, original->target->identity});
          co_await self.WriteOriginalFile(request, original, std::move(target), std::move(payload));
        }, [self, request](std::string error) { self.OfferSaveAs(request, std::move(error)); });
      }, ChoiceTone::Primary},
  });
}

Task<void> WorkbenchActions::WriteOriginalFile(Save request, std::shared_ptr<const SavedDocument> original,
                                             Target target, SavePayload payload) const {
  const auto written = co_await WriteOriginal(target->reference, original->bytes, payload.text,
                                              request->tab->working_directory, request->tab->attachments->directories,
                                              target->directory, payload.snapshot, original->snapshot);
  if (!written.Succeeded()) {
    OfferSaveAs(request, written.Error().message);
    co_return;
  }
  MarkSaved(request, std::move(target), std::move(payload));
}

void WorkbenchActions::OfferSaveAs(Save request, std::string message) const {
  const auto& original = request->tab->saved.Get();
  ChooseOutput(request, WriteKind::SaveAs, original ? original->format : DocumentFormat::Markdown, std::move(message));
}

void WorkbenchActions::RequestClose(Tab tab) const {
  if (workspace->closing.Get()) {
    return;
  }
  if (!tab || workspace->selected.Get() == tab->id) {
    file_menu_focus.RequestFocus();
  }
  workspace->closing = std::make_shared<CloseRequest>(CloseRequest{std::move(tab)});
  tasks.Post([self = *this] { self.ContinueClose(); });
}

bool WorkbenchActions::OnCloseRequest() const {
  if (std::exchange(workspace->allow_close, false) || (workspace->tabs->empty() && !workspace->picking.Get())) {
    return false;
  }
  RequestClose();
  return true;
}

void WorkbenchActions::ContinueClose() const {
  const auto request = workspace->closing.Get();
  if (!request || !request->waiting || workspace->picking.Get()) {
    return;
  }
  const auto tabs = request->tab ? std::vector<Tab>{request->tab} : workspace->tabs.Get();
  if (std::ranges::any_of(tabs, [](const Tab& tab) { return tab->Busy(); })) {
    return;
  }
  request->waiting = false;
  auto dirty = std::make_shared<std::vector<Tab>>();
  for (const auto& tab : tabs) {
    if (tab->Dirty()) {
      dirty->push_back(tab);
    }
  }
  if (dirty->empty()) {
    FinishClose();
    return;
  }
  const auto title = request->tab ? l10n::save_before_close : l10n::save_before_close_window;
  std::string message;
  for (const auto& tab : *dirty) {
    if (!message.empty()) {
      message += '\n';
    }
    message += tab->Name();
  }
  ShowChoices(title, std::move(message), {
      {l10n::cancel, [self = *this] { self.CancelClose(); }},
      {dirty->size() == 1 ? l10n::discard : l10n::discard_all, [self = *this] { self.FinishClose(); }, ChoiceTone::Destructive},
      {dirty->size() == 1 ? l10n::save : l10n::save_all, [self = *this, dirty] { self.SaveBeforeClose(dirty, 0); },
       ChoiceTone::Primary},
  });
}

void WorkbenchActions::SaveBeforeClose(std::shared_ptr<const std::vector<Tab>> dirty, std::size_t index) const {
  if (index == dirty->size()) {
    FinishClose();
    return;
  }
  const auto& tab = (*dirty)[index];
  StartSave(tab, WriteKind::Save, [self = *this, dirty, index](bool saved) {
    const auto& current = (*dirty)[index];
    if (!saved || current->Dirty()) {
      self.CancelClose();
      if (saved) {
        self.Status(current, l10n::new_changes_need_saving);
      } else if (!current->status->empty()) {
        self.Status({}, current->Name() + ": " + current->status.Get());
      }
      return;
    }
    self.SaveBeforeClose(dirty, index + 1);
  });
}

void WorkbenchActions::FinishClose() const {
  const auto request = workspace->closing.Get();
  if (const auto& tab = request->tab) {
    auto tabs = workspace->tabs.Get();
    const auto found = std::ranges::find(tabs, tab);
    const auto index = static_cast<std::size_t>(found - tabs.begin());
    if (found != tabs.end()) {
      tabs.erase(found);
      workspace->tabs = tabs;
      if (workspace->selected.Get() == tab->id) {
        if (tabs.empty()) {
          workspace->selected = 0;
        } else {
          Select(tabs[std::min(index, tabs.size() - 1)]);
        }
      }
      if (const auto& directory = tab->working_directory) {
        Launch(false, [directory = *directory]() -> Task<void> {
          (void)co_await directory.DeleteRecursivelyAsync();
        });
      }
    }
    workspace->closing = nullptr;
  } else {
    workspace->allow_close = true;
    window.Close();
  }
}

void WorkbenchActions::CancelClose() const { workspace->closing = nullptr; }

[[huxerui::composable]]
View DocumentPage(WorkbenchActions actions, Tab tab, bool active) {
  const auto editor = UseRichEditor(tab->initial);
  const auto focus = UseViewHandle();
  Lifecycle([tab, editor, focus] {
    tab->editor = editor;
    tab->editor_focus = focus;
  }, editor, focus);
  const auto& attachments = tab->attachments.Get();
  const bool available = active && !actions.workspace->closing.Get() && tab->operation.Get() != Operation::PickImage;
  const bool needs_attachments = active && tab->opened_with_attachments && attachments.directories.empty();
  View notice;
  if (needs_attachments) {
    notice = Row {
      Text(l10n::local_images_need_folder, TextRole::Label).With(Grow()),
      Button(l10n::locate_attachments).OnClick([actions, tab] { actions.LocateAttachments(tab); })
          .With(Enabled(actions.picker->CanOpenDirectories() && actions.Available() && !tab->Busy())),
    }.With(Padding(EdgeInsets::Symmetric(14.0F, 8.0F)), Spacing(8.0F),
           CrossAlign(CrossAxisAlignment::Center), Background(UseTheme().colors.surface_container));
  }
  return Column {
    notice,
    EditorPane(editor, {
        .working_directory = tab->working_directory,
        .image_resolver = attachments.resolver,
        .find = tab->find,
        .focus = focus,
        .active = active,
        .available = available,
        .can_pick_images = tab->working_directory && actions.picker->CanOpenFiles() &&
            actions.Available() && !tab->Busy(),
        .pick_image = [actions, tab, editor](ViewHandle focus, bool replace, bool inline_image) {
          actions.PickImage(tab, editor, focus, replace, inline_image);
        },
    }).Key("editor-pane").With(Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View Glyph(const ImageResource& icon, Color color, float size = control_icon_size) {
  return Image(icon).Tint(color).With(Frame{.width = size, .height = size}, Semantics{.hidden = true});
}

View ClickableRow(View row, const ThemeSpec& theme, Semantics semantics, std::function<void()> activate) {
  semantics.descendants = SemanticDescendantPolicy::Exclude;
  return std::move(row).OnClick(std::move(activate)).With(
      Focusable{}, PointerCursor(PointerCursorKind::Hand), theme.interactions.indication,
      CornerRadius(theme.shapes.small), std::move(semantics));
}

View OpenRow(const WorkbenchActions& actions, const ThemeSpec& theme, OpenKind kind) {
  const bool file = kind == OpenKind::File;
  StringVariant label = file ? l10n::open_file_menu : l10n::open_folder_menu;
  const auto detail = file ? l10n::supported_documents : l10n::browse_folder;
  const bool enabled = file ? actions.CanExecute(FolioCommand::OpenFile)
                            : actions.Available() && actions.picker->CanOpenDirectories();
  const auto& colors = theme.colors;
  auto row = Row {
    Row {Glyph(file ? app::images::file_text : app::images::folder_open, colors.on_surface_variant)}
        .With(Frame{.width = 30.0F, .height = 30.0F}, MainAlign(MainAxisAlignment::Center),
              CrossAlign(CrossAxisAlignment::Center), Background(colors.surface_container),
              CornerRadius(theme.shapes.extra_small)),
    Column {
      Text(label, TextRole::Label),
      Text(detail, TextRole::Label).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
    }.With(Grow(), Spacing(1.0F)),
    Glyph(app::images::chevron_right, colors.on_surface_variant, 13.0F),
  }.With(Frame{.min_height = 51.0F}, Padding(EdgeInsets::Symmetric(5.0F, 3.0F)), Spacing(9.0F),
         CrossAlign(CrossAxisAlignment::Center));
  return ClickableRow(std::move(row), theme, {.role = SemanticRole::Button, .label = label, .hint = detail},
                      [actions, kind] {
                        if (kind == OpenKind::File) {
                          actions.Execute(FolioCommand::OpenFile);
                        } else {
                          actions.PickOpen(kind);
                        }
                      })
      .With(Enabled(enabled), Opacity(enabled ? 1.0F : 0.48F));
}

View EmptyWorkspace(const WorkbenchActions& actions, const ThemeSpec& theme) {
  const auto& colors = theme.colors;
  const bool enabled = actions.CanExecute(FolioCommand::NewDocument);
  auto create = ClickableRow(Row {
    Glyph(app::images::plus, colors.on_primary),
    Text(l10n::new_document, TextRole::Label).With(Foreground(colors.on_primary)),
  }.With(Frame{.min_height = 40.0F}, Padding(EdgeInsets::Symmetric(12.0F, 5.0F)),
         Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)),
      theme, {.role = SemanticRole::Button, .label = l10n::new_document},
      [actions] { actions.Execute(FolioCommand::NewDocument); })
      .With(Enabled(enabled), Opacity(enabled ? 1.0F : 0.48F));
  const View welcome = Column {
    Row {Glyph(app::images::file_pen, colors.on_primary_container, 22.0F)}
        .With(Frame{.width = 42.0F, .height = 42.0F}, MainAlign(MainAxisAlignment::Center),
              CrossAlign(CrossAxisAlignment::Center), Background(colors.primary_container),
              CornerRadius(theme.shapes.medium)),
    Text(l10n::start_writing, TextRole::Title).Style(TextStyle{
        Font::FromFamily(theme.typography.family, theme.typography.headline_small).WithWeight(FontWeight::SemiBold),
        colors.on_surface}),
    PrimaryAction(std::move(create), theme),
    Column {
      Divider(),
      OpenRow(actions, theme, OpenKind::File),
      Divider(),
      OpenRow(actions, theme, OpenKind::Folder),
      Divider(),
    }.With(Frame{.width = 304.0F}, CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Frame{.width = 304.0F}, Spacing(15.0F), CrossAlign(CrossAxisAlignment::Start));
  return Column {welcome}.With(Grow(), Padding(EdgeInsets::Symmetric(18.0F, 10.0F)),
                              MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View DocumentArea(WorkbenchActions actions) {
  const auto tabs = actions.workspace->tabs.Get();
  const auto selected = actions.workspace->selected.Get();
  const auto& theme = UseTheme();
  View content;
  Tab active;
  if (tabs.empty()) {
    content = EmptyWorkspace(actions, theme);
  } else {
    std::vector<View> pages;
    pages.reserve(tabs.size());
    std::size_t index = 0;
    for (const auto& tab : tabs) {
      const bool is_active = tab->id == selected;
      if (is_active) {
        index = pages.size();
        active = tab;
      }
      pages.push_back(DocumentPage(actions, tab, is_active).Key(tab->id));
    }
    content = IndexedPages(std::move(pages), index).With(Grow());
  }
  auto status = actions.workspace->status.Get();
  if (actions.workspace->picking.Get()) {
    status = UseString(l10n::working);
  } else if (active) {
    switch (active->operation.Get()) {
    case Operation::Save: status = UseString(l10n::saving); break;
    case Operation::Export: status = UseString(l10n::exporting); break;
    default:
      if (status.empty()) {
        status = active->status.Get();
      }
      break;
    }
  }
  return Column {
    std::move(content).Key("document-content"),
    StatusBar(std::move(status)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background));
}

View SidebarLabel(const ImageResource& icon, std::string label, Color color) {
  return Row {
    Glyph(icon, color),
    EllipsizedLabel(std::move(label), color).With(Grow()),
  }.With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center));
}

enum class SidebarSection { Folders, Recent, NoFolders, NoRecent };
using SidebarItem = std::variant<SidebarSection, Entry, Target>;

StringResource SidebarSectionLabel(SidebarSection section) {
  switch (section) {
  case SidebarSection::Folders: return l10n::folders;
  case SidebarSection::Recent: return l10n::recent_edits;
  case SidebarSection::NoFolders: return l10n::no_folders;
  case SidebarSection::NoRecent: return l10n::no_recent_documents;
  }
  return l10n::files;
}

std::vector<MenuEntry> FolderMenu(WorkbenchActions actions, const Entry& folder) {
  const bool available = actions.Available();
  const bool can_create = available && (folder->directory->CanWrite() || actions.picker->CanOpenDirectories(true));
  return {
      MenuItem(l10n::new_file_menu, [actions, folder] { actions.RequestNewChild(folder, ChildKind::File); }).Enabled(can_create),
      MenuItem(l10n::new_folder_menu, [actions, folder] { actions.RequestNewChild(folder, ChildKind::Folder); })
          .Enabled(can_create),
      MenuItem(l10n::refresh, [actions, folder] { actions.SetExpanded(folder, true); })
          .Enabled(available && folder->load.Get() != FolderLoad::Loading),
  };
}

[[huxerui::composable]]
View FolderTreeRow(WorkbenchActions actions, Entry entry, bool desktop, float row_height) {
  const auto& colors = UseTheme().colors;
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const bool expanded = entry->expanded.Get();
  std::string label = entry->name;
  if (expanded && entry->load.Get() == FolderLoad::Loading) {
    label += " — loading…";
  } else if (expanded && entry->load.Get() == FolderLoad::Failed) {
    label += " — couldn’t load";
  }
  const auto tooltip = entry->error->empty() ? entry->name : entry->error.Get();
  auto row = Row {
    SidebarLabel(expanded ? app::images::folder_open : app::images::folder, std::move(label), colors.on_surface)
        .With(Grow(), Tooltip(tooltip)),
    desktop ? View{} : IconButton(app::images::more, StringVariant::Format(l10n::actions_for, entry->name))
        .OnClick([actions, entry, menu, anchor] { menu.Show(anchor, FolderMenu(actions, entry)); })
        .With(anchor, Frame{.width = row_height, .height = row_height}, Enabled(actions.Available())),
  }.With(Frame{.height = row_height}, CrossAlign(CrossAxisAlignment::Center));
  if (desktop) {
    return std::move(row).On<ViewEvents::ContextMenuRequested>([actions, entry, menu](Point point) {
      if (actions.Available()) {
        menu.ShowAt(point, FolderMenu(actions, entry));
      }
    });
  }
  return row;
}

[[huxerui::composable]]
View Sidebar(WorkbenchActions actions, bool desktop) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const float row_height = desktop ? 32.0F : 44.0F;
  const auto workspace = actions.workspace;
  const auto selected = workspace->Selected();
  const auto selected_file = selected ? selected->saved.Get() : nullptr;
  const auto selected_entry = workspace->selected_entry.Get();
  const auto is_selected = [selected_file, selected_entry](const Entry& entry) {
    return entry->file && entry->id == selected_entry && selected_file && SameFile(entry->file, selected_file->target);
  };
  const auto open = [actions](const SidebarItem& item) {
    if (const auto* entry = std::get_if<Entry>(&item); entry && (*entry)->file) {
      actions.workspace->selected_entry = (*entry)->id;
      actions.OpenKnown((*entry)->file);
    } else if (const auto* file = std::get_if<Target>(&item)) {
      actions.OpenKnown(*file);
    }
  };
  auto files = TreeView<SidebarItem>({SidebarSection::Folders, SidebarSection::Recent},
      [actions, colors, desktop, row_height, is_selected](const SidebarItem& item) -> View {
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& entry = *node;
          if (entry->directory) {
            return FolderTreeRow(actions, entry, desktop, row_height).Key(entry->id);
          }
          return SidebarLabel(app::images::file_text, entry->name,
                              is_selected(entry) ? colors.on_primary_container : colors.on_surface)
              .With(Tooltip(LocationLabel(*entry->file))).Key(entry->id);
        }
        if (const auto* target = std::get_if<Target>(&item)) {
          const auto& file = *target;
          return SidebarLabel(app::images::file_text, file->reference.Name(), colors.on_surface_variant)
              .With(Tooltip(LocationLabel(*file))).Key(reinterpret_cast<std::uintptr_t>(file.get()));
        }
        const auto section = std::get<SidebarSection>(item);
        const auto label = SidebarSectionLabel(section);
        const auto key = "sidebar-section-" + std::to_string(static_cast<int>(section));
        if (section == SidebarSection::NoFolders || section == SidebarSection::NoRecent) {
          return Text(label, TextRole::Label).With(FontSize(12.0F), Foreground(colors.on_surface_variant)).Key(key);
        }
        const bool folders = section == SidebarSection::Folders;
        return Row {
          Text(folders ? l10n::folders_heading : l10n::recent_edits_heading, TextRole::Label)
              .With(Grow(), FontSize(11.0F), Foreground(colors.on_surface_variant)),
          folders ? IconButton(app::images::plus, l10n::add_folder).OnClick([actions] { actions.PickOpen(OpenKind::Folder); })
              .With(Frame{.width = row_height, .height = row_height},
                    Enabled(actions.picker->CanOpenDirectories() && actions.Available()), Tooltip(l10n::open_folder)) : View{},
        }.With(CrossAlign(CrossAxisAlignment::Center)).Key(key);
      }, [workspace, is_selected](const SidebarItem& item) {
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& entry = *node;
          return TreeItemInfo{.label = entry->name, .expandable = entry->directory.has_value(),
                              .expanded = entry->expanded.Get(),
                              .selected = entry->file ? std::optional(is_selected(entry)) : std::nullopt};
        }
        if (const auto* file = std::get_if<Target>(&item)) {
          return TreeItemInfo{.label = (*file)->reference.Name(), .selected = false};
        }
        const auto section = std::get<SidebarSection>(item);
        const auto label = SidebarSectionLabel(section);
        switch (section) {
        case SidebarSection::Folders:
          return TreeItemInfo{.label = label, .expandable = true, .expanded = workspace->folders_open.Get()};
        case SidebarSection::Recent:
          return TreeItemInfo{.label = label, .expandable = true, .expanded = workspace->recents_open.Get()};
        default: return TreeItemInfo{.label = label, .enabled = false};
        }
      }).Children([actions, workspace](const SidebarItem& item) -> std::vector<SidebarItem> {
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& children = (*node)->children.Get();
          return {children.begin(), children.end()};
        }
        switch (std::get<SidebarSection>(item)) {
        case SidebarSection::Folders: {
          const auto& roots = workspace->folders.Get();
          return roots.empty() ? std::vector<SidebarItem>{SidebarSection::NoFolders}
                               : std::vector<SidebarItem>{roots.begin(), roots.end()};
        }
        case SidebarSection::Recent: {
          const auto& recent = actions.folio->recent.Get();
          return recent.empty() ? std::vector<SidebarItem>{SidebarSection::NoRecent}
                                : std::vector<SidebarItem>{recent.begin(), recent.end()};
        }
        default: return {};
        }
      }).OnExpandedChanged([actions, workspace](const SidebarItem& item, bool expanded) {
        if (const auto* entry = std::get_if<Entry>(&item)) {
          actions.SetExpanded(*entry, expanded);
        } else if (std::get<SidebarSection>(item) == SidebarSection::Folders) {
          workspace->folders_open = expanded;
        } else {
          workspace->recents_open = expanded;
        }
      }).OnSelectionChanged([open](const SidebarItem& item, bool selected) {
        if (selected) {
          open(item);
        }
      }).OnActivated(open)
      .DisclosureIcon(app::images::chevron_right).ItemExtent(row_height).Label(l10n::files)
      .With(Grow(), ScrollBar());
  auto settings = ClickableRow(Row {
    Text(l10n::settings, TextRole::Label).With(Grow()),
    Glyph(app::images::chevron_right, colors.on_surface_variant, 13.0F),
  }.With(Frame{.min_height = row_height}, Padding(EdgeInsets::Symmetric(9.0F, 3.0F)),
         CrossAlign(CrossAxisAlignment::Center)),
      theme, {.role = SemanticRole::Button, .label = l10n::settings, .identifier = "open-settings"},
      [actions] { actions.Execute(FolioCommand::Settings); })
      .With(Enabled(actions.CanExecute(FolioCommand::Settings)));
  return Column {
    Row {
      Text(l10n::files, TextRole::Label).With(Grow()),
      IconButton(app::images::panel_left, l10n::hide_files).OnClick([actions] { actions.Execute(FolioCommand::ToggleSidebar); })
          .With(Frame{.min_width = row_height, .min_height = row_height}, Tooltip(l10n::hide_files),
                Enabled(actions.CanExecute(FolioCommand::ToggleSidebar)),
                Semantics{.identifier = "hide-files", .expanded = true}),
    }.With(Padding(EdgeInsets::Symmetric(8.0F, 2.0F)), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    files,
    Divider(),
    Column {settings}.With(Padding(EdgeInsets::Symmetric(8.0F, 6.0F)), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low));
}

View DirtyDot(const ThemeSpec& theme, bool dirty) {
  return dirty ? Row{}.With(Frame{.width = 6.0F, .height = 6.0F},
      Background(PendingEditsColor(theme.brightness)), CornerRadius(3.0F), Semantics{.hidden = true}) : View{};
}

[[huxerui::composable]]
View TabHeader(WorkbenchActions actions, Tab tab, bool only_tab) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const auto name = tab->Name();
  const bool dirty = tab->Dirty();
  const bool active = actions.workspace->selected.Get() == tab->id;
  const auto label = dirty ? StringVariant::Format(l10n::unsaved_document, name) : StringVariant{name};
  const bool enabled = !actions.workspace->closing.Get();
  const auto foreground = active ? colors.on_primary_container : colors.on_surface_variant;
  View name_row = Row {
    Glyph(app::images::file_text, foreground),
    only_tab ? DirtyDot(theme, dirty) : View{},
    EllipsizedLabel(name, foreground).With(Grow()),
    only_tab ? View{} : DirtyDot(theme, dirty),
  }.With(Frame{.min_height = 30.0F}, Padding(EdgeInsets::Symmetric(6.0F, 2.0F)),
         Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center));
  if (only_tab) {
    name_row = std::move(name_row).With(Semantics{.label = label, .descendants = SemanticDescendantPolicy::Exclude});
  } else {
    name_row = ClickableRow(std::move(name_row), theme, {.role = SemanticRole::Tab, .label = label, .selected = active},
                           [actions, tab] { actions.Select(tab); }).With(Enabled(enabled));
  }
  return Column {
    Row{}.With(Frame{.height = 2.0F}, Background(active ? colors.primary : Color::Transparent())),
    Row {
      std::move(name_row).With(Grow(), Tooltip(name)),
      IconButton(app::images::close, StringVariant::Format(l10n::close_document, name)).OnClick([actions, tab] { actions.RequestClose(tab); })
          .With(Enabled(enabled), Tooltip(StringVariant::Format(l10n::close_document, name))),
    }.With(Grow(), Padding(EdgeInsets::Symmetric(3.0F, 0.0F)), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Frame{.height = 38.0F}, CrossAlign(CrossAxisAlignment::Stretch), ClipChildren(),
         CornerRadius(CornerRadii::Top(theme.shapes.small)),
         Background(active ? colors.primary_container : Color::Transparent()));
}

constexpr float tab_width = 160.0F;
constexpr float single_title_width = 210.0F;

[[huxerui::composable]]
View TabStrip(WorkbenchActions actions) {
  const auto tabs = actions.workspace->tabs.Get();
  const auto selected = actions.workspace->selected.Get();
  const auto scroll = UseScrollController();
  const auto found = std::ranges::find_if(tabs, [selected](const Tab& tab) { return tab->id == selected; });
  const auto index = static_cast<std::size_t>(found - tabs.begin());
  Lifecycle([scroll, index, count = tabs.size()] {
    if (count > 1 && index < count) {
      scroll.ScrollToItem(index, ScrollAlignment::Center);
    }
  }, selected, index, tabs.size());
  if (tabs.empty()) {
    return Spacer().With(Grow());
  }
  if (tabs.size() == 1) {
    return TabHeader(actions, tabs.front(), true).With(Grow());
  }
  return VirtualList(tabs, [actions](const Tab& tab) { return TabHeader(actions, tab, false).Key(tab->id); })
      .ScrollAxis(Axis::Horizontal).ItemExtent(tab_width).Controller(scroll)
      .With(Grow(), Frame{.height = 38.0F}, Semantics{.role = SemanticRole::TabList, .label = l10n::open_documents});
}

[[huxerui::composable]]
View DocumentSwitcher(DialogContext context, WorkbenchActions actions) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  Views rows;
  for (const auto& tab : actions.workspace->tabs.Get()) {
    const auto name = tab->Name();
    const bool selected = actions.workspace->selected.Get() == tab->id;
    const bool dirty = tab->Dirty();
    const auto foreground = selected ? colors.on_primary_container : colors.on_surface;
    auto row = ClickableRow(Row {
      Glyph(app::images::file_text, foreground),
      EllipsizedLabel(name, foreground).With(Grow(), Tooltip(name)),
      DirtyDot(theme, dirty),
    }.With(Frame{.min_height = 44.0F}, Padding(EdgeInsets::Symmetric(9.0F, 4.0F)), Spacing(7.0F),
           CrossAlign(CrossAxisAlignment::Center)),
        theme, {.role = SemanticRole::Button, .label = dirty ? StringVariant::Format(l10n::unsaved_document, name) : StringVariant{name}, .selected = selected},
        [context, actions, tab] {
          context.Dismiss();
          actions.Select(tab);
        }).With(Grow(), Background(selected ? colors.primary_container : Color::Transparent()));
    rows.Add(Row {
      row,
      IconButton(app::images::close, StringVariant::Format(l10n::close_document, name)).OnClick([context, actions, tab] {
        context.Dismiss();
        actions.RequestClose(tab);
      }).With(Frame{.min_width = 44.0F, .min_height = 44.0F}),
    }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)).Key(tab->id));
  }
  return ScrollableDialogPanel(
    Text(l10n::open_documents, TextRole::Title),
    ScrollView(Column {std::move(rows)}.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        .With(Frame{.max_height = 280.0F}, ScrollBar()),
    DialogActions(theme,
      Button(l10n::done).OnClick([context] { context.Dismiss(); }),
      Button(l10n::new_document).OnClick([context, actions] {
        context.Dismiss();
        actions.Execute(FolioCommand::NewDocument);
      }).With(Enabled(actions.CanExecute(FolioCommand::NewDocument)))),
    theme, 360.0F);
}

[[huxerui::composable]]
View WorkbenchBar(WorkbenchActions actions, bool desktop) {
  const auto& theme = UseTheme();
  const auto file_menu = UseMenu();
  const auto workspace = actions.workspace;
  const auto active = workspace->Selected();
  const bool available = actions.Available();
  const bool document_actions = available && active && !active->Busy();
  const auto viewport = UseViewportClass();
  const bool narrow_desktop = desktop && viewport == ViewportClass::Compact;
  const bool compact_mobile = !desktop && viewport == ViewportClass::Compact;
  const float control_size = desktop ? 32.0F : 44.0F;
  const auto icon_frame = Frame{.min_width = control_size, .min_height = control_size};
  const auto& colors = theme.colors;
  View title;
  if (desktop) {
    const auto count = workspace->tabs->size();
    const bool show_new = active && !narrow_desktop;
    const float tabs_width = count == 1 ? single_title_width : tab_width * static_cast<float>(count);
    const View strip = Row {
      TabStrip(actions).Key("tab-strip"),
      show_new ? IconButton(app::images::plus, l10n::new_document).OnClick([actions] {
        actions.Execute(FolioCommand::NewDocument);
      }).With(icon_frame, Tooltip(l10n::new_document), Enabled(actions.CanExecute(FolioCommand::NewDocument))) : View{},
    }.With(Frame{.max_width = tabs_width + (show_new ? control_size : 0.0F)},
           CrossAlign(CrossAxisAlignment::Center), ClipChildren());
    title = Column {strip}.With(Grow(), CrossAlign(CrossAxisAlignment::Start));
  } else if (active) {
    const auto name = active->Name();
    const bool dirty = active->Dirty();
    const auto font = Font::FromFamily(theme.typography.family, theme.typography.body_large)
        .WithWeight(FontWeight::SemiBold);
    constexpr float padding = 8.0F;
    constexpr float spacing = 6.0F;
    constexpr float chevron_size = 12.0F;
    const auto name_width = UseTextMeasurer().MeasureText(name, TextStyle{font, colors.on_surface},
        std::numeric_limits<float>::infinity(), {.wrap = TextWrap::NoWrap}).size.width;
    const float switcher_width = std::max(control_size,
        name_width + 2 * padding + chevron_size + spacing + (dirty ? 6.0F + spacing : 0.0F));
    const auto switcher = ClickableRow(Row {
      EllipsizedLabel(name, colors.on_surface, {}, font).With(Grow()),
      DirtyDot(theme, dirty),
      Glyph(app::images::chevron_down, colors.on_surface_variant, chevron_size),
    }.With(Frame{.min_width = control_size, .max_width = switcher_width, .min_height = control_size},
           Padding(EdgeInsets::Symmetric(padding, 0.0F)), Spacing(spacing), CrossAlign(CrossAxisAlignment::Center)),
        theme, {.role = SemanticRole::Button,
                .label = StringVariant::Format(dirty ? l10n::switch_unsaved_document : l10n::switch_document, name),
                .identifier = "document-switcher"},
        [actions] { actions.dialog.Show(DocumentSwitcher, actions); })
        .With(CornerRadius(control_size / 2.0F), Enabled(!workspace->closing.Get()), Tooltip(name));
    title = Row {switcher}.With(Grow(), CrossAlign(CrossAxisAlignment::Center));
  } else {
    title = Spacer().With(Grow());
  }
  const auto menu = [actions, file_menu, active, available, document_actions, desktop, narrow_desktop] {
    const auto item = [&actions](StringVariant label, FolioCommand command) {
      return MenuItem(label, [actions, command] { actions.Execute(command); }).Enabled(actions.CanExecute(command));
    };
    std::vector<MenuEntry> entries{
        item(l10n::new_document, FolioCommand::NewDocument),
        item(l10n::open_file_menu, FolioCommand::OpenFile),
        MenuItem(l10n::open_folder_menu, [actions] { actions.PickOpen(OpenKind::Folder); })
            .Enabled(available && actions.picker->CanOpenDirectories()),
        MenuItem(l10n::open_package, [actions] { actions.PickOpen(OpenKind::Package); })
            .Enabled(available && actions.picker->CanOpenDirectories()),
        MenuSection{},
        item(l10n::save, FolioCommand::Save),
        item(l10n::save_as_menu, FolioCommand::SaveAs),
        MenuItem(l10n::export_copy_menu, [actions, active] { actions.RequestSave(active, WriteKind::Export); })
            .Enabled(document_actions && (actions.picker->CanSaveFiles() || actions.picker->CanOpenDirectories(true))),
        MenuItem(l10n::locate_attachment_folder, [actions, active] { actions.LocateAttachments(active); })
            .Enabled(document_actions && actions.picker->CanOpenDirectories()),
        MenuSection{},
        MenuItem(l10n::load_sample, [actions] { actions.NewDocument(true); }).Enabled(available),
    };
    if (!IsMobilePlatform()) {
      entries.insert(entries.begin(), {item(l10n::command_palette_menu, FolioCommand::OpenCommandPalette), MenuSection{}});
    }
    if (narrow_desktop && active) {
      entries.emplace_back(item(l10n::find_in_document, FolioCommand::Find));
    }
    if (desktop) {
      entries.emplace_back(MenuItem(l10n::new_window, [actions] { actions.NewWindow(); }).Enabled(available));
    }
    entries.emplace_back(MenuSection{});
    entries.emplace_back(item(l10n::settings_menu, FolioCommand::Settings));
    file_menu.Show(actions.file_menu_focus, std::move(entries));
  };
  const bool files_visible = actions.sidebar_open.Get();
  const auto files_label = files_visible ? l10n::hide_files : l10n::show_files;
  std::vector<View> controls{
    actions.modal_sidebar || !files_visible ? IconButton(app::images::panel_left, files_label)
        .OnClick([actions] { actions.Execute(FolioCommand::ToggleSidebar); })
        .With(icon_frame, Tooltip(files_label), Enabled(actions.CanExecute(FolioCommand::ToggleSidebar)),
              Semantics{.identifier = "show-files", .expanded = files_visible})
        : View{},
    std::move(title).Key("title"),
    active && !narrow_desktop ? IconButton(app::images::search, l10n::find_in_document)
        .OnClick([actions] { actions.Execute(FolioCommand::Find); })
        .With(icon_frame, Tooltip(l10n::find_in_document), Enabled(actions.CanExecute(FolioCommand::Find)))
        .Key("find") : View{},
    active && !narrow_desktop && !compact_mobile ? IconButton(app::images::save, l10n::save)
        .OnClick([actions] { actions.Execute(FolioCommand::Save); })
        .With(icon_frame, Tooltip(l10n::save), Enabled(actions.CanExecute(FolioCommand::Save))).Key("save") : View{},
    IconButton(app::images::more, l10n::file_actions).OnClick(menu)
        .With(actions.file_menu_focus, icon_frame, Tooltip(l10n::file_actions), Enabled(!workspace->closing.Get()))
        .Key("file-menu"),
  };
  auto bar = desktop ? View(WindowTitleBar(std::move(controls))) : View(Row(std::move(controls)));
  bar = std::move(bar).With(Frame{.min_height = desktop ? 44.0F : 54.0F},
      Padding(EdgeInsets::Symmetric(8.0F, 2.0F)), Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center));
  if (!desktop) {
    auto icons = UseEnvironment<IconButtonStyle>();
    icons.minimum_interactive_size = control_size;
    icons.state_layer_size = 36.0F;
    icons.corner_radius = 18.0F;
    bar = ProvideEnvironment(icons, bar);
  }
  return Column {
    bar,
    Divider(),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container));
}

} // namespace

void InstallFolio(ApplicationContext& context) {
  RegisterBrowser(context);
  context.Provide(std::make_shared<FolioState>());
  std::optional<File> directory;
  try {
    directory = UseApplication().Directories().data_directory;
  } catch (const std::logic_error&) {
    // Settings remain available for the session on hosts without application storage.
  }
  context.Provide(std::make_shared<SettingsStore>(std::move(directory)));
}

void InstallWorkbench(WindowContext& context) {
  InstallBrowser(context);
  context.Provide(std::make_shared<Workspace>());
}

WindowOptions FolioWindowOptions() {
  return {.title = "Folio", .initial_size = {920.0F, 720.0F}, .minimum_size = Size{360.0F, 360.0F},
          .content_mode = WindowContentMode::EdgeToEdge,
          .chrome_mode = WindowChromeMode::Custom};
}

[[huxerui::composable]]
View Workbench(State<WorkbenchKeyHandler> dispatch) {
  const auto folio = UseService<FolioState>();
  const auto workspace = UseService<Workspace>();
  const auto window = UseWindow();
  const auto application = UseApplication();
  const auto application_tasks = UseApplicationTaskScope();
  const auto& theme = UseTheme();
  const auto settings = UseService<SettingsStore>();
  const bool desktop = application.SupportsMultipleWindows();
  const bool modal_sidebar = !desktop && UseViewportClass() == ViewportClass::Compact;
  const auto sidebar_open = modal_sidebar ? workspace->drawer_open : workspace->sidebar_visible;
  const WorkbenchActions actions{
      folio, workspace, application, window, UseService<FilePicker>(), UseService<Resources>(),
      UseTaskScope(), UseDialog(), UseViewHandle(),
      UseViewHandle(), sidebar_open, modal_sidebar,
  };
  Lifecycle([folio, workspace, window, application_tasks] {
    workspace->window = window;
    folio->windows.push_back(workspace);
    return [folio, workspace, application_tasks] {
      workspace->window.reset();
      std::erase_if(folio->windows, [workspace](const auto& weak) {
        const auto owner = weak.lock();
        return !owner || owner == workspace;
      });
      std::vector<File> directories;
      for (const auto& tab : workspace->tabs.Get()) {
        if (tab->working_directory) {
          directories.push_back(*tab->working_directory);
        }
      }
      if (!directories.empty()) {
        application_tasks.Launch([directories = std::move(directories)]() -> Task<void> {
          for (const auto& directory : directories) {
            (void)co_await directory.DeleteRecursivelyAsync();
          }
        });
      }
    };
  }, workspace);
  window.OnCloseRequest([actions] { return actions.OnCloseRequest(); }, workspace);

  const auto on_key = [actions, settings](const KeyEvent& event) {
    if (event.type != KeyEventType::Down) {
      return false;
    }
    const auto& bindings = settings->Bindings();
    const auto binding = std::ranges::find(bindings->window, KeyCombination{event.key, event.modifiers},
                                          &WindowKeyBinding::keys);
    if (binding == bindings->window.end()) {
      return false;
    }
    const auto tab = actions.workspace->Selected();
    // Let the search field handle its own editing history before a window command can claim the same key.
    if (tab && tab->find->open && tab->find->field_focused &&
        (event.key == Key::Z || (event.key == Key::Y && !event.modifiers.shift))) {
      return false;
    }
    if (!event.repeat) {
      actions.Execute(binding->command, true);
    }
    return true;
  };
  Lifecycle([dispatch, on_key] { dispatch = on_key; }, theme, modal_sidebar);

  if (desktop) {
    const auto sizes = workspace->pane_sizes.Get();
    const bool visible = workspace->sidebar_visible.Get();
    return Column {
      WorkbenchBar(actions, true),
      SplitLayout {
        visible ? Sidebar(actions, true)
            .With(sizes.sidebar, sizes.sidebar_grow).Key("sidebar") : View{},
        DocumentArea(actions).With(visible ? sizes.documents : Frame{}, visible ? sizes.documents_grow : Grow{})
            .Key("documents"),
      }.OnChanged([workspace, visible](const std::vector<SplitChange>& changes) {
        if (visible) {
          workspace->pane_sizes.Update([&](PaneSizes& sizes) {
            for (const auto& change : changes) {
              if (change.index == 0) {
                sizes.sidebar = change.frame;
                sizes.sidebar_grow = change.grow;
              } else if (change.index == 1) {
                sizes.documents = change.frame;
                sizes.documents_grow = change.grow;
              }
            }
          });
        }
      }).With(Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), SafeAreaPadding(), Background(theme.colors.background));
  }
  const auto content = Column {
    WorkbenchBar(actions, false),
    DocumentArea(actions).With(Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch)).Key("workspace-content");
  auto layout = modal_sidebar || sidebar_open.Get()
      ? DrawerLayout(content, StartDrawer(Sidebar(actions, false)).Open(sidebar_open)
            .OnOpenChanged([sidebar_open](bool open) { sidebar_open = open; }))
      : DrawerLayout(content);
  return std::move(layout).With(SafeAreaPadding(), Background(theme.colors.background));
}

} // namespace folio

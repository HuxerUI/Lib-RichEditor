#include "workbench.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <variant>

#include "app_resources.h"
#include "appearance.h"
#include "attachments.h"
#include "browser.h"
#include "command_palette.h"
#include "document_library.h"
#include "document_links.h"
#include "labels.h"
#include "settings.h"
#include "workspace_store.h"

using namespace huxerui;
using namespace huxerui::richeditor;
using namespace std::chrono_literals;

View App();

namespace l10n = app::strings;

namespace folio {
namespace {

using DocumentId = std::uint64_t;

struct DocumentFile {
  FileSource reference;
  std::optional<FileSource> directory;
  std::optional<File> identity;
  std::shared_ptr<DocumentLibrary> library;
  std::string library_id;

  std::string Name() const {
    if (library) {
      if (const auto entry = library->Find(library_id)) {
        return entry->name;
      }
    }
    return reference.Name();
  }
};

using Target = std::shared_ptr<DocumentFile>;

bool SameFile(const Target& first, const Target& second) {
  return first == second || (first->identity && first->identity == second->identity);
}

std::string LocationLabel(const DocumentFile& file) {
  if (file.library) {
    return file.Name();
  }
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
  std::vector<FileSource> directories;
  std::shared_ptr<const ImageResolver> resolver;
  std::shared_ptr<const ClipboardAttachmentProvider> clipboard_attachments;

  AttachmentLocation(const std::optional<File>& working_directory, std::vector<FileSource> directories)
      : directories(std::move(directories)), resolver(AttachmentResolver(working_directory, this->directories)),
        clipboard_attachments(ClipboardAttachmentsFor(working_directory, this->directories)) {}
};

enum class Operation { Idle, PickImage, LocateAttachments, Save, Export, Delete };

struct FolderEntry;
using Entry = std::shared_ptr<FolderEntry>;

struct DocumentTab {
  DocumentId id;
  std::string draft_name;
  std::optional<File> working_directory;
  RichDocument initial;
  State<std::shared_ptr<const SavedDocument>> saved;
  State<AttachmentLocation> attachments;
  State<Entry> source_folder{Entry{}};
  State<std::optional<RichEditorController>> editor{std::nullopt};
  State<std::optional<ViewHandle>> editor_focus{std::nullopt};
  State<Operation> operation{Operation::Idle};
  State<FindPanelState> find{FindPanelState{}};
  State<std::string> status{std::string{}};
  State<std::optional<std::string>> pending_anchor{std::nullopt};
  State<std::optional<WorkspaceTabRecord>> unresolved{std::nullopt};
  bool initial_read_only = false;
  std::optional<ReadingPosition> reading_position;
  State<bool> restore_reading_position{false};
  std::vector<std::string> source_relative;
  std::optional<std::string> library_folder;
  std::optional<WorkspaceLocation> unavailable_attachments;
  bool opened_with_attachments;

  DocumentTab(DocumentId id, std::string name, std::optional<File> working_directory, RichDocument initial,
              std::shared_ptr<const SavedDocument> original, std::optional<FileSource> attachment_directory)
      : id(id), draft_name(std::move(name)), working_directory(std::move(working_directory)),
        initial(std::move(initial)), saved(original),
        attachments(AttachmentLocation(this->working_directory,
            attachment_directory ? std::vector{std::move(*attachment_directory)} : std::vector<FileSource>{})),
        opened_with_attachments(HasLocalAttachments(this->initial)) {}

  std::string Name() const {
    const auto& original = saved.Get();
    return original ? original->target->Name() : draft_name;
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

  bool IsReadOnly() const {
    const auto& controller = editor.Get();
    return controller ? controller->IsReadOnly() : initial_read_only;
  }

  std::string_view LibraryId() const {
    if (const auto& pending = unresolved.Get()) {
      return pending->library_document;
    }
    const auto& original = saved.Get();
    return original ? std::string_view(original->target->library_id) : std::string_view{};
  }

  void SetAttachmentDirectory(FileSource directory) {
    unavailable_attachments.reset();
    attachments = AttachmentLocation(working_directory, {std::move(directory)});
  }

  void BindAttachmentDirectory(FileSource directory) {
    unavailable_attachments.reset();
    auto sources = attachments->directories;
    if (const auto local = directory.AsFile()) {
      if (!sources.empty() && sources.front().AsFile() == local) {
        return;
      }
      std::erase_if(sources, [&](const FileSource& source) { return source.AsFile() == local; });
    }
    // Undo can restore a reference that was absent from the saved snapshot.
    sources.insert(sources.begin(), std::move(directory));
    attachments = AttachmentLocation(working_directory, std::move(sources));
  }
};

using Tab = std::shared_ptr<DocumentTab>;

enum class FolderLoad { Unloaded, Loading, Loaded, Failed };

struct FolderEntry {
  std::uint64_t id;
  std::string name;
  std::optional<FileSource> directory;
  Target file;
  std::optional<File> identity;
  State<bool> expanded{false};
  State<FolderLoad> load{FolderLoad::Unloaded};
  State<std::vector<Entry>> children{std::vector<Entry>{}};
  State<std::string> error{std::string{}};
  std::uint64_t load_generation = 0;
  std::weak_ptr<FolderEntry> parent;
  State<std::optional<WorkspaceFolderRecord>> unresolved{std::nullopt};
  State<bool> mounted{true};
  std::string recent_id;
};

struct FolderDocumentLocation {
  Entry root;
  std::vector<std::string> path;
};

FolderDocumentLocation FolderLocation(Entry folder, std::string name) {
  std::vector<std::string> path{std::move(name)};
  if (folder) {
    while (auto parent = folder->parent.lock()) {
      path.push_back(folder->name);
      folder = std::move(parent);
    }
    std::ranges::reverse(path);
  }
  return {std::move(folder), std::move(path)};
}

FolderDocumentLocation TabFolderLocation(const Tab& tab, const Target& target) {
  auto location = FolderLocation(tab->source_folder.Get(), target->reference.Name());
  if (location.root && !tab->source_relative.empty()) {
    location.path = tab->source_relative;
  }
  return location;
}

struct RecentItem {
  WorkspaceRecentRecord record;
  Target target;
  std::optional<FileSource> directory;

  std::string Name(const DocumentLibrary* library) const {
    if (library && !record.library_document.empty()) {
      if (const auto entry = library->Find(record.library_document)) {
        return entry->name;
      }
    }
    return record.name;
  }
};

using Recent = std::shared_ptr<RecentItem>;

struct CloseRequest {
  Tab tab;
  bool waiting = true;
};

constexpr float default_sidebar_width = 260.0F;

struct PaneSizes {
  Frame sidebar{.width = default_sidebar_width, .min_width = 180.0F, .max_width = 420.0F};
  Grow sidebar_grow{0.0F};
  Frame documents{.min_width = 160.0F};
  Grow documents_grow{1.0F};
  bool operator==(const PaneSizes&) const = default;
};

struct Workspace {
  std::string id = UniqueName("window-");
  std::optional<WindowHandle> window;
  State<std::vector<Tab>> tabs{std::vector<Tab>{}};
  State<DocumentId> selected{0};
  State<std::vector<Entry>> folders{std::vector<Entry>{}};
  State<bool> files_open{true};
  State<bool> external_open{true};
  State<bool> recents_open{true};
  State<std::vector<std::string>> library_expanded{std::vector<std::string>{}};
  State<bool> sidebar_visible{true};
  State<bool> showing_outline{false};
  State<bool> drawer_open{false};
  State<PaneSizes> pane_sizes{PaneSizes{}};
  State<bool> picking{false};
  State<bool> showing_open_issues{false};
  State<std::shared_ptr<CloseRequest>> closing{std::shared_ptr<CloseRequest>{}};
  State<std::string> status{std::string{}};
  bool allow_close = false;
  bool palette_open = false;
  State<bool> restoring{false};
  std::optional<TaskHandle> reading_position_save;
  std::uint64_t next_entry = 0;

  Tab Selected() const {
    const auto id = selected.Get();
    const auto& open = tabs.Get();
    const auto found = std::ranges::find_if(open, [id](const Tab& tab) { return tab->id == id; });
    return found == open.end() ? nullptr : *found;
  }

  void PruneFolders() const {
    folders.Update([&](auto& roots) {
      std::erase_if(roots, [&](const Entry& root) {
        return !root->mounted.Get() && std::ranges::none_of(tabs.Get(), [&](const Tab& tab) {
          if (const auto& pending = tab->unresolved.Get()) {
            return pending->folder == root->id;
          }
          auto folder = tab->source_folder.Get();
          while (folder && folder != root) {
            folder = folder->parent.lock();
          }
          return folder == root;
        });
      });
    });
  }
};

using LibraryIds = std::set<std::string, std::less<>>;

LibraryIds EntryIds(const std::vector<LibraryEntry>& entries) {
  LibraryIds ids;
  for (const auto& entry : entries) {
    ids.insert(entry.id);
  }
  return ids;
}

template <class Removed>
void PruneLibraryReferences(WorkspaceRecord& record, const Removed& removed) {
  std::erase_if(record.recent, [&](const WorkspaceRecentRecord& item) {
    return !item.library_document.empty() && removed(item.library_document);
  });
  for (auto& window : record.windows) {
    std::erase_if(window.tabs, [&](const WorkspaceTabRecord& tab) {
      return !tab.library_document.empty() && removed(tab.library_document);
    });
    std::erase_if(window.library_expanded, [&](const std::string& id) { return !id.empty() && removed(id); });
    if (std::ranges::find(window.tabs, window.selected, &WorkspaceTabRecord::id) == window.tabs.end()) {
      window.selected = window.tabs.empty() ? 0 : window.tabs.front().id;
    }
  }
}

struct FolioState {
  std::shared_ptr<WorkspaceStore> store;
  std::shared_ptr<DocumentLibrary> library;
  std::optional<File> temporary_directory;
  std::vector<WorkspaceWindowRecord> restore_windows;
  std::optional<WorkspaceWindowRecord> installing;
  State<std::size_t> restoring{0};
  State<bool> deleting_library_entries{false};
  State<std::vector<Recent>> recent{std::vector<Recent>{}};
  State<std::vector<Target>> pending_files{std::vector<Target>{}};
  std::vector<std::weak_ptr<Workspace>> windows;
  State<std::uint64_t> window_revision{0};
  DocumentId next_document = 0;
  unsigned next_draft = 0;

  Target LibraryTarget(std::string_view id) const {
    const auto entry = library ? library->Find(id) : std::nullopt;
    if (!entry || entry->folder) {
      return {};
    }
    const auto package = *library->Package(id);
    const auto file = package.Child("article.json");
    return std::make_shared<DocumentFile>(DocumentFile{FileSource(file, FileType::File, true),
        FileSource(package, FileType::Directory, true), file, library, entry->id});
  }

  WorkspaceLocation Location(const FileSource& source) const {
    const auto file = source.AsFile();
    for (auto parent = file; parent; parent = parent->Parent()) {
      if (parent == temporary_directory) {
        return {};
      }
    }
    return {file ? file->Path() : std::string{}, source.CanWrite()};
  }

  void Persist(const Workspace& workspace, bool activate = false) const {
    WorkspaceWindowRecord window{
        .id = workspace.id, .selected = workspace.selected.Get(),
        .sidebar_width = workspace.pane_sizes->sidebar.width.value_or(default_sidebar_width),
        .sidebar_visible = workspace.sidebar_visible.Get(), .showing_outline = workspace.showing_outline.Get(),
        .files_open = workspace.files_open.Get(), .recents_open = workspace.recents_open.Get(),
        .library_expanded = workspace.library_expanded.Get(),
    };
    for (const auto& folder : workspace.folders.Get()) {
      if (const auto& unresolved = folder->unresolved.Get()) {
        window.folders.push_back(*unresolved);
      } else if (folder->directory) {
        window.folders.push_back({folder->id, folder->name, Location(*folder->directory), folder->recent_id});
      }
    }
    for (const auto& tab : workspace.tabs.Get()) {
      if (const auto& unresolved = tab->unresolved.Get()) {
        window.tabs.push_back(*unresolved);
        continue;
      }
      const auto& original = tab->saved.Get();
      if (!original) {
        continue;
      }
      const auto& target = *original->target;
      if (target.library) {
        window.tabs.push_back({.id = tab->id, .name = tab->Name(), .library_document = target.library_id,
                               .reading_position = tab->reading_position, .read_only = tab->IsReadOnly()});
        continue;
      }
      WorkspaceTabRecord record{.id = tab->id, .name = tab->Name(), .location = Location(target.reference),
                                .reading_position = tab->reading_position, .read_only = tab->IsReadOnly()};
      const auto& attachments = tab->attachments->directories;
      if (!attachments.empty()) {
        record.attachments = Location(attachments.front());
      } else if (tab->unavailable_attachments) {
        record.attachments = *tab->unavailable_attachments;
      }
      auto location = TabFolderLocation(tab, original->target);
      if (location.root) {
        if (std::ranges::find(window.folders, location.root->id, &WorkspaceFolderRecord::id) != window.folders.end()) {
          record.folder = location.root->id;
          record.relative = std::move(location.path);
        }
      } else if (target.directory && !target.directory->AsFile()) {
        record.package = target.directory->Name();
      }
      window.tabs.push_back(std::move(record));
    }
    std::erase_if(window.folders, [&](const WorkspaceFolderRecord& folder) {
      return std::ranges::none_of(window.tabs, [&](const WorkspaceTabRecord& tab) { return tab.folder == folder.id; });
    });
    if (std::ranges::find(window.tabs, window.selected, &WorkspaceTabRecord::id) == window.tabs.end()) {
      window.selected = window.tabs.empty() ? 0 : window.tabs.front().id;
    }
    auto record = store->Get();
    const auto saved = std::ranges::find(record.windows, window.id, &WorkspaceWindowRecord::id);
    if (saved == record.windows.end()) {
      record.windows.push_back(std::move(window));
    } else {
      *saved = std::move(window);
    }
    if (activate || record.active_window.empty()) {
      record.active_window = workspace.id;
    }
    store->Set(std::move(record));
  }

  void FlushReadingPositions(Workspace& workspace) const {
    if (!workspace.reading_position_save) {
      return;
    }
    workspace.reading_position_save->Cancel();
    workspace.reading_position_save.reset();
    // An explicitly closed window has already been removed from the store.
    const auto& windows = store->Get().windows;
    if (!workspace.restoring.Get() &&
        std::ranges::find(windows, workspace.id, &WorkspaceWindowRecord::id) != windows.end()) {
      Persist(workspace);
    }
  }

  void Forget(const Workspace& workspace) const {
    auto record = store->Get();
    std::erase_if(record.windows, [&](const WorkspaceWindowRecord& window) { return window.id == workspace.id; });
    if (record.active_window == workspace.id) {
      record.active_window = record.windows.empty() ? std::string{} : record.windows.back().id;
    }
    store->Set(std::move(record));
  }

  void QueueActivation(const ApplicationActivation& activation) {
    if (const auto* files = std::get_if<FileActivation>(&activation)) {
      pending_files.Update([&](auto& pending) {
        for (const auto& file : files->files) {
          if (file.Type() == FileType::File && FormatOf(file.Name()) == DocumentFormat::Markdown) {
            pending.push_back(std::make_shared<DocumentFile>(DocumentFile{file, {}, {}}));
          }
        }
      });
    }
  }

  std::shared_ptr<Workspace> ActivationWindow() const {
    std::shared_ptr<Workspace> first;
    for (const auto& weak : windows) {
      const auto owner = weak.lock();
      if (!owner || !owner->window || !owner->window->IsOpen()) {
        continue;
      }
      if (owner->window->LifecycleState() == WindowLifecycleState::Active) {
        return owner;
      }
      if (!first) {
        first = owner;
      }
    }
    return first;
  }

  void PersistRecent() const {
    auto saved = store->Get();
    saved.recent.clear();
    for (const auto& item : recent.Get()) {
      auto record = item->record;
      record.name = item->Name(library.get());
      saved.recent.push_back(std::move(record));
    }
    store->Set(std::move(saved));
  }

  std::string Remember(RecentItem item) const {
    recent.Update([&](auto& entries) {
      std::erase_if(entries, [&](const Recent& existing) {
        const auto& prior = existing->record;
        const auto& current = item.record;
        const bool same = (!current.id.empty() && current.id == prior.id) ||
            (!current.library_document.empty() && current.library_document == prior.library_document) ||
            (current.kind == prior.kind && !current.location.path.empty() && current.location.path == prior.location.path) ||
            (item.target && existing->target && SameFile(item.target, existing->target));
        if (same && item.record.id.empty()) {
          item.record.id = prior.id;
        }
        return same;
      });
      if (item.record.id.empty()) {
        item.record.id = UniqueName("recent-");
      }
      entries.insert(entries.begin(), std::make_shared<RecentItem>(item));
      constexpr std::size_t recent_limit = 24;
      if (entries.size() > recent_limit) {
        entries.resize(recent_limit);
      }
    });
    PersistRecent();
    return item.record.id;
  }

  void RemoveRecent(std::string_view id) const {
    recent.Update([&](auto& entries) {
      std::erase_if(entries, [&](const Recent& entry) { return entry->record.id == id; });
    });
    PersistRecent();
  }

  std::vector<Tab> LibraryTabs(const LibraryIds& ids) const {
    std::vector<Tab> result;
    for (const auto& weak : windows) {
      if (const auto owner = weak.lock()) {
        for (const auto& tab : owner->tabs.Get()) {
          if (ids.contains(tab->LibraryId())) {
            result.push_back(tab);
          }
        }
      }
    }
    return result;
  }

  bool LibraryDeleteBusy(const LibraryIds& ids) const {
    if (deleting_library_entries.Get() || restoring.Get() != 0) {
      return true;
    }
    for (const auto& weak : windows) {
      if (const auto owner = weak.lock()) {
        if (owner->picking.Get() || owner->closing.Get()) {
          return true;
        }
        if (std::ranges::any_of(owner->tabs.Get(), [&](const Tab& tab) {
              return tab->Busy() && (ids.contains(tab->LibraryId()) ||
                  (tab->library_folder && ids.contains(*tab->library_folder)));
            })) {
          return true;
        }
      }
    }
    return false;
  }

  std::vector<File> ForgetLibraryEntries(const LibraryIds& ids) const {
    std::vector<File> temporary;
    auto saved = store->Get();
    PruneLibraryReferences(saved, [&](std::string_view id) { return ids.contains(id); });
    for (const auto& weak : windows) {
      const auto owner = weak.lock();
      if (!owner) {
        continue;
      }
      auto open = owner->tabs.Get();
      const auto active = owner->Selected();
      const bool removing_active = active && ids.contains(active->LibraryId());
      const auto replacement = removing_active ? static_cast<std::size_t>(std::count_if(
          open.begin(), std::ranges::find(open, active), [&](const Tab& tab) { return !ids.contains(tab->LibraryId()); }))
          : 0;
      std::erase_if(open, [&](const Tab& tab) {
        if (tab->library_folder && ids.contains(*tab->library_folder)) {
          tab->library_folder.reset();
        }
        if (!ids.contains(tab->LibraryId())) {
          return false;
        }
        if (tab->working_directory) {
          temporary.push_back(*tab->working_directory);
        }
        return true;
      });
      if (removing_active) {
        const auto selected = open.empty() ? 0 : open[std::min(replacement, open.size() - 1)]->id;
        owner->selected = selected;
        const auto window = std::ranges::find(saved.windows, owner->id, &WorkspaceWindowRecord::id);
        if (window != saved.windows.end() &&
            std::ranges::find(window->tabs, selected, &WorkspaceTabRecord::id) != window->tabs.end()) {
          window->selected = selected;
        }
      }
      owner->tabs = std::move(open);
      owner->library_expanded.Update([&](auto& expanded) {
        std::erase_if(expanded, [&](const std::string& id) { return ids.contains(id); });
      });
    }
    recent.Update([&](auto& entries) {
      std::erase_if(entries, [&](const Recent& item) { return ids.contains(item->record.library_document); });
    });
    store->Set(std::move(saved));
    return temporary;
  }
};

enum class OpenKind { File, Folder, Package, Import };
struct OpenRequest {
  OpenKind kind;
  std::string import_parent;
  std::string recent_id;
};
enum class WriteKind { Save, SaveAs, Export, Library };
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

struct OpenFailure {
  Target target;
  std::string message;
  std::vector<ConversionIssue> issues;
  std::optional<RichDocument> lossy_copy;
  std::optional<std::string> import_parent;
};

struct OpenedDocument {
  std::shared_ptr<Workspace> owner;
  Tab tab;
};

struct LinkDestination {
  std::vector<std::string> path;
  std::optional<std::string> anchor;
};

using OpenResult = std::variant<OpenedDocument, OpenFailure>;
using LoadedDocument = std::variant<std::shared_ptr<const SavedDocument>, OpenFailure>;

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
  TaskScope application_tasks;
  DialogHandle dialog;
  ViewHandle file_menu_focus;
  ViewHandle palette_focus;
  State<bool> sidebar_open;
  bool modal_sidebar;

  bool Available() const {
    return !folio->deleting_library_entries.Get() && !workspace->picking.Get() && !workspace->closing.Get() &&
        !workspace->showing_open_issues.Get();
  }
  bool CanExecute(FolioCommand command) const;
  void Execute(FolioCommand command, bool keyboard = false) const;
  void NewDocument(bool sample = false, std::optional<std::string> library_folder = {}) const;
  void OpenLibraryDocument(std::string id) const;
  void SetLibraryExpanded(std::string id, bool expanded) const;
  void RenameLibraryEntry(std::string id) const;
  void NewLibraryFolder(std::string parent) const;
  void ChooseLibraryDestination(std::string id) const;
  void MoveLibraryEntry(std::string id, std::string parent) const;
  void ConfirmLibraryDelete(std::string id) const;
  void JumpToHeading(const Tab& tab, const Block& block, TextOffset length) const;
  void RevealLinkedHeading(const Tab& tab, const RichEditorController& editor, const std::string& anchor) const;
  void RecordReadingPosition(const Tab& tab, std::optional<ReadingPosition> position) const;
  void FollowDocumentLink(Tab tab, DocumentLink link) const;
  void NewWindow() const;
  void PickOpen(OpenKind kind) const { PickOpen(OpenRequest{kind}); }
  void PickOpen(OpenRequest request) const;
  void OpenRecent(Recent item) const;
  void RemoveExternalFolder(const Entry& folder) const;
  void OpenKnown(Target target, Entry source_folder = {}) const;
  void OpenActivated() const;
  void RestoreWorkspace() const;
  void RestoreWindows() const;
  void RelocateTab(Tab tab) const;
  void RelocateFolder(Entry folder) const;
  void SetExpanded(const Entry& entry, bool expanded) const;
  void RequestNewChild(Entry folder, ChildKind kind) const;
  void Select(const Tab& tab) const;
  void ToggleReadingMode(const Tab& tab) const;
  void PickImage(Tab tab, RichEditorController editor, ViewHandle focus, bool replace, bool inline_image) const;
  void LocateAttachments(Tab tab) const;

  void RequestSave(Tab tab, WriteKind kind) const;
  void RequestClose(Tab tab = {}) const;
  bool OnCloseRequest() const;

private:
  void ShowCommandPalette(bool keyboard) const;
  void Status(const Tab& tab, StringVariant message) const;
  void Launch(bool uses_picker, std::function<Task<void>()> action, std::function<void(std::string)> failed = {}) const;
  Tab AddTab(std::string name, RichDocument snapshot, std::shared_ptr<const SavedDocument> saved = {},
             Entry source_folder = {}, std::optional<FileSource> attachment_directory = {},
             std::optional<std::string> library_folder = {}, std::vector<std::string> relative = {}) const;
  void EditLibraryName(LibraryEntry entry, bool creating) const;
  void ReportLibraryDeleteFailure(StringVariant reason) const;
  void DeleteLibraryEntries(std::vector<LibraryEntry> expected, std::set<DocumentId> confirmed_dirty) const;
  std::optional<OpenedDocument> ActivateExisting(const Target& target, const Entry& source_folder,
                                                  const std::vector<std::string>& relative) const;
  Task<OpenResult> OpenTarget(Target target, const Entry& source_folder,
                             const std::vector<std::string>& relative) const;
  Task<void> OpenTargets(std::vector<Target> targets, Entry source_folder = {}, RecentKind kind = RecentKind::File,
                         std::string recent_id = {}, std::optional<LinkDestination> link = {}) const;
  Task<void> OpenLinkedDocument(Tab tab, LinkDestination destination, FolderDocumentLocation source,
                               FileSource directory, bool offer_folder) const;
  void ChooseLinkFolder(Tab tab, LinkDestination destination, FolderDocumentLocation source) const;
  Task<void> OpenPicked(OpenRequest request) const;
  Task<void> OpenDirectory(FileSource directory, OpenKind kind, std::string recent_id) const;
  Task<void> RestoreTab(Tab tab, std::optional<FileSource> directory = {}) const;
  Task<void> RestoreFolderTabs(Entry folder) const;
  Task<void> FillRestoredTab(Tab tab, Target target, Entry folder) const;
  void FinishRestore() const;
  void ShowOpenFailures(std::vector<OpenFailure> failures) const;
  Task<Entry> AddFolder(FileSource directory, std::string recent_id = {}) const;
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
  void SaveToLibrary(Save request) const;
  void ChooseOriginalFolder(Save request, std::shared_ptr<const SavedDocument> original, SavePayload payload) const;
  Task<void> WriteOriginalFile(Save request, std::shared_ptr<const SavedDocument> original, Target target,
                               SavePayload payload) const;
  void OfferSaveAs(Save request, std::string message) const;

  void ShowChoices(StringVariant title, StringVariant message, std::vector<Choice> choices,
                   bool dismiss_on_cancel = false) const;
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
    return tab && !tab->unresolved.Get() && !tab->Busy();
  case FolioCommand::SaveAs: {
    if (!tab || tab->unresolved.Get() || tab->Busy()) {
      return false;
    }
    const auto& saved = tab->saved.Get();
    const bool exports = IsMobilePlatform() || (saved && saved->target->library);
    return picker->CanOpenDirectories(true) || (exports && picker->CanSaveFiles());
  }
  case FolioCommand::Find:
    return tab && !tab->unresolved.Get();
  case FolioCommand::FindReplace:
    return tab && !tab->unresolved.Get() && !tab->IsReadOnly();
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
  case FolioCommand::SaveAs: {
    const auto tab = workspace->Selected();
    const auto& saved = tab->saved.Get();
    RequestSave(tab, IsMobilePlatform() || (saved && saved->target->library) ? WriteKind::Export : WriteKind::SaveAs);
    break;
  }
  case FolioCommand::Find:
  case FolioCommand::FindReplace:
    workspace->Selected()->find.Update([command](FindPanelState& find) {
      find.open = true;
      if (command == FolioCommand::FindReplace) {
        find.replace_open = true;
      }
      ++find.focus_request;
    });
    break;
  case FolioCommand::ToggleSidebar:
    sidebar_open = !sidebar_open.Get();
    if (!modal_sidebar) {
      folio->Persist(*workspace);
    }
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

void WorkbenchActions::JumpToHeading(const Tab& tab, const Block& block, TextOffset length) const {
  if (!Available() || workspace->Selected() != tab) {
    return;
  }
  const auto& editor = tab->editor.Get();
  if (!editor || !editor->RevealTextRange(block, {0, length}, ScrollAlignment::Start)) {
    Status(tab, l10n::outline_jump_unavailable);
    return;
  }
  tab->restore_reading_position = false;
  if (modal_sidebar) {
    sidebar_open = false;
  }
}

void WorkbenchActions::RevealLinkedHeading(const Tab& tab, const RichEditorController& editor,
                                            const std::string& anchor) const {
  if (workspace->Selected() != tab || tab->pending_anchor.Get() != anchor) {
    return;
  }
  const auto headings = DocumentHeadings(editor.Document());
  const auto heading = std::ranges::find(headings, anchor, &DocumentHeading::anchor);
  if (heading == headings.end()) {
    Status(tab, l10n::linked_heading_missing);
  } else if (!editor.RevealTextRange(heading->block, {0, heading->length}, ScrollAlignment::Start)) {
    Status(tab, l10n::outline_jump_unavailable);
  } else {
    tab->restore_reading_position = false;
    Status(tab, {});
  }
  tab->pending_anchor = std::nullopt;
}

void WorkbenchActions::RecordReadingPosition(const Tab& tab, std::optional<ReadingPosition> position) const {
  if (workspace->Selected() != tab || tab->pending_anchor.Get() || tab->restore_reading_position.Get() ||
      workspace->closing.Get() || tab->reading_position == position) {
    return;
  }
  tab->reading_position = position;
  // Coalesce changes while scrolling and keep the latest position in memory between writes.
  if (tab->saved.Get() && !workspace->reading_position_save) {
    workspace->reading_position_save = tasks.Launch([folio = folio, workspace = workspace]() -> Task<void> {
      co_await Delay(750ms);
      workspace->reading_position_save.reset();
      if (!workspace->restoring.Get()) {
        folio->Persist(*workspace);
      }
    });
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

std::optional<File> NewWorkingDirectory(const ApplicationHandle& application) {
  try {
    return application.Directories().temporary_directory.Child(UniqueName("folio-document-"));
  } catch (const std::logic_error&) {
    return std::nullopt;
  }
}

Tab WorkbenchActions::AddTab(std::string name, RichDocument snapshot, std::shared_ptr<const SavedDocument> saved,
                             Entry source_folder, std::optional<FileSource> attachment_directory,
                             std::optional<std::string> library_folder, std::vector<std::string> relative) const {
  if (saved) {
    attachment_directory = saved->target->directory;
  }
  const bool read_only = IsMobilePlatform() && saved && !saved->target->library;
  auto tab = std::make_shared<DocumentTab>(++folio->next_document, std::move(name), NewWorkingDirectory(application),
                                           std::move(snapshot), std::move(saved), std::move(attachment_directory));
  tab->initial_read_only = read_only;
  tab->source_folder = std::move(source_folder);
  tab->source_relative = std::move(relative);
  tab->library_folder = std::move(library_folder);
  workspace->tabs.Update([&](auto& open) { open.push_back(tab); });
  Select(tab);
  return tab;
}

void WorkbenchActions::NewDocument(bool sample, std::optional<std::string> library_folder) const {
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
  AddTab(std::move(name), RichDocument{}, {}, {}, {}, std::move(library_folder));
}

void WorkbenchActions::OpenLibraryDocument(std::string id) const {
  if (auto target = folio->LibraryTarget(id)) {
    OpenKnown(std::move(target));
  }
}

void WorkbenchActions::SetLibraryExpanded(std::string id, bool expanded) const {
  if (id.empty()) {
    workspace->files_open = expanded;
  } else {
    workspace->library_expanded.Update([&](auto& ids) {
      std::erase(ids, id);
      if (expanded) {
        ids.push_back(std::move(id));
      }
    });
  }
  folio->Persist(*workspace);
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
  folio->Persist(*workspace, !workspace->restoring.Get() && folio->restoring.Get() == 0);
}

void WorkbenchActions::ToggleReadingMode(const Tab& tab) const {
  const auto controller = tab->editor.Get();
  if (!Available() || workspace->Selected() != tab || !controller || tab->Busy()) {
    return;
  }
  const bool read_only = !controller->IsReadOnly();
  controller->SetReadOnly(read_only);
  if (read_only && tab->find->replace_open) {
    tab->find.Update([](FindPanelState& find) {
      find.replace_open = false;
      if (find.focused == FindField::Replacement) {
        find.focused = FindField::None;
      }
    });
  }
  folio->Persist(*workspace);
}

std::optional<OpenedDocument> WorkbenchActions::ActivateExisting(
    const Target& target, const Entry& source_folder, const std::vector<std::string>& relative) const {
  const auto location = relative.empty() ? FolderLocation(source_folder, target->reference.Name())
                                         : FolderDocumentLocation{source_folder, relative};
  for (const auto& weak : folio->windows) {
    const auto owner = weak.lock();
    if (!owner) {
      continue;
    }
    for (const auto& tab : owner->tabs.Get()) {
      const auto& original = tab->saved.Get();
      if (!original) {
        continue;
      }
      const auto same_location = [&] {
        if (!location.root) {
          return false;
        }
        const auto existing = TabFolderLocation(tab, original->target);
        return existing.root == location.root && existing.path == location.path;
      };
      if (SameFile(original->target, target) || same_location()) {
        if (target->directory && tab->attachments->directories.empty()) {
          tab->SetAttachmentDirectory(*target->directory);
        }
        if (owner == workspace) {
          if (source_folder) {
            tab->source_folder = source_folder;
            tab->source_relative = relative;
          }
          Select(tab);
        } else if (owner->window && owner->window->IsOpen()) {
          owner->selected = tab->id;
          owner->drawer_open = false;
          owner->window->Activate();
          workspace->drawer_open = false;
          folio->Persist(*owner, true);
        } else {
          continue;
        }
        return OpenedDocument{owner, tab};
      }
    }
  }
  return {};
}

Task<LoadedDocument> LoadDocument(Target target, bool offer_lossy_copy = false) {
  if (!target->identity) {
    target->identity = co_await LocalIdentity(target->reference);
  }
  auto read = co_await ReadDocument(target->reference);
  if (!read.Succeeded()) {
    co_return OpenFailure{target, read.Error().message};
  }
  auto source = std::move(read.Value());
  const auto& codec = Codec(source.format);
  auto imported = codec.Import(source.Text());
  if (!imported.document) {
    OpenFailure failure{target, {}, std::move(imported.issues)};
    if (offer_lossy_copy && source.format == DocumentFormat::Markdown) {
      // The offered copy must come from the same bytes that produced the diagnostics.
      auto lossy = codec.Import(source.Text(), LossPolicy::AllowLoss);
      if (lossy.document) {
        failure.lossy_copy = std::move(lossy.document);
        failure.issues = std::move(lossy.issues);
      }
    }
    co_return failure;
  }
  co_return std::make_shared<const SavedDocument>(
      SavedDocument{target, source.format, *imported.document, std::move(source.bytes)});
}

void SetRestoredDocument(const Tab& tab, std::shared_ptr<const SavedDocument> original, const Entry& folder) {
  tab->initial = original->snapshot;
  tab->opened_with_attachments = HasLocalAttachments(tab->initial);
  tab->saved = original;
  if (original->target->directory) {
    tab->SetAttachmentDirectory(*original->target->directory);
  }
  tab->source_folder = folder;
  if (folder) {
    tab->source_relative = tab->unresolved.Get()->relative;
  }
  tab->status = std::string{};
  tab->unresolved = std::nullopt;
}

Task<OpenResult> WorkbenchActions::OpenTarget(Target target, const Entry& source_folder,
                                             const std::vector<std::string>& relative) const {
  if (!target->identity) {
    target->identity = co_await LocalIdentity(target->reference);
  }
  if (auto opened = ActivateExisting(target, source_folder, relative)) {
    co_return *opened;
  }
  auto loaded = co_await LoadDocument(target, true);
  if (auto* failure = std::get_if<OpenFailure>(&loaded)) {
    co_return std::move(*failure);
  }
  if (auto opened = ActivateExisting(target, source_folder, relative)) {
    co_return *opened;
  }
  const auto original = std::get<std::shared_ptr<const SavedDocument>>(std::move(loaded));
  if (const auto path = target->reference.AsFile()) {
    for (const auto& weak : folio->windows) {
      const auto owner = weak.lock();
      if (!owner || !owner->window || !owner->window->IsOpen()) {
        continue;
      }
      const auto& tabs = owner->tabs.Get();
      const auto match = std::ranges::find_if(tabs, [&](const Tab& tab) {
        const auto& pending = tab->unresolved.Get();
        return pending && (pending->location.path == path->Path() ||
            (target->identity && pending->location.path == target->identity->Path()));
      });
      if (match != tabs.end()) {
        const auto folder = owner == workspace ? source_folder : Entry{};
        SetRestoredDocument(*match, original, folder);
        if (folder && !relative.empty()) {
          (*match)->source_relative = relative;
        }
        owner->selected = (*match)->id;
        owner->drawer_open = false;
        owner->window->Activate();
        folio->Persist(*owner, true);
        co_return OpenedDocument{owner, *match};
      }
    }
  }
  co_return OpenedDocument{workspace, AddTab(target->Name(), original->snapshot, original, source_folder,
                                             {}, {}, relative)};
}

Task<void> WorkbenchActions::OpenTargets(std::vector<Target> targets, Entry source_folder, RecentKind kind,
                                          std::string recent_id, std::optional<LinkDestination> link) const {
  static const std::vector<std::string> no_relative_path;
  std::shared_ptr<Workspace> last_owner;
  std::vector<OpenFailure> failures;
  for (const auto& target : targets) {
    try {
      auto result = co_await OpenTarget(target, source_folder, link ? link->path : no_relative_path);
      if (auto* opened = std::get_if<OpenedDocument>(&result)) {
        last_owner = std::move(opened->owner);
        if (link) {
          opened->tab->pending_anchor = link->anchor;
          opened->tab->status = std::string{};
        }
        const auto& recent_target = link ? opened->tab->saved.Get()->target : target;
        const bool package = kind == RecentKind::Package && recent_target->directory;
        const auto& source = package ? *recent_target->directory : recent_target->reference;
        folio->Remember({{recent_id, package ? source.Name() : recent_target->Name(),
                          recent_target->library ? WorkspaceLocation{} : folio->Location(source),
                          kind, recent_target->library_id}, recent_target, {}});
      } else {
        failures.push_back(std::get<OpenFailure>(std::move(result)));
      }
    } catch (const std::exception& error) {
      failures.push_back({target, error.what()});
    }
  }
  if (!failures.empty()) {
    window.Activate();
    ShowOpenFailures(std::move(failures));
  } else if (last_owner && last_owner->window && last_owner->window->IsOpen()) {
    last_owner->window->Activate();
  }
}

void WorkbenchActions::FollowDocumentLink(Tab tab, DocumentLink link) const {
  if (!Available() || workspace->Selected() != tab) {
    return;
  }
  if (link.path.empty()) {
    tab->pending_anchor = std::move(link.anchor);
    return;
  }
  const auto& original = tab->saved.Get();
  if (!original || original->target->library) {
    Status(tab, l10n::relative_link_needs_source);
    return;
  }
  auto source = TabFolderLocation(tab, original->target);
  if (!source.root && original->target->identity) {
    // A separately opened local file can already be inside a mounted directory.
    for (const auto& root : workspace->folders.Get()) {
      if (!root->mounted.Get() || !root->identity) {
        continue;
      }
      auto file = original->target->identity;
      std::vector<std::string> path;
      while (file && file != root->identity) {
        path.push_back(file->Name());
        file = file->Parent();
      }
      if (file && !path.empty() && (!source.root || path.size() < source.path.size())) {
        std::ranges::reverse(path);
        source = {root, std::move(path)};
      }
    }
  }
  auto directory = source.root ? source.root->directory : original->target->directory;
  if (!directory && !source.root) {
    if (const auto local = original->target->reference.AsFile(); local && local->Parent()) {
      directory = FileSource(*local->Parent(), FileType::Directory, false);
    }
  }
  auto path = ResolveDocumentLinkPath(source.path, link);
  if (!path) {
    Status(tab, l10n::relative_link_outside_folder);
    return;
  }
  LinkDestination destination{std::move(*path), std::move(link.anchor)};
  if (!directory) {
    ChooseLinkFolder(tab, std::move(destination), std::move(source));
    return;
  }
  Launch(false, [self = *this, tab, destination = std::move(destination), source = std::move(source),
                 directory = std::move(*directory)] {
    return self.OpenLinkedDocument(tab, destination, source, directory, true);
  });
}

Task<void> WorkbenchActions::OpenLinkedDocument(Tab tab, LinkDestination destination, FolderDocumentLocation source,
                                                FileSource directory, bool offer_folder) const {
  const auto original = tab->saved.Get();
  auto entry = co_await FindFolderFile(directory, destination.path);
  if (workspace->Selected() != tab || tab->saved.Get() != original) {
    co_return;
  }
  if (!entry.Succeeded()) {
    if (offer_folder && entry.Error().code == IoErrorCode::PermissionDenied) {
      ChooseLinkFolder(tab, std::move(destination), std::move(source));
    } else {
      Status(tab, entry.Error().code == IoErrorCode::NotFound ? l10n::linked_document_missing : l10n::open_link_failed);
    }
    co_return;
  }
  if (!source.root) {
    const auto identity = co_await LocalIdentity(directory);
    if (workspace->Selected() != tab || tab->saved.Get() != original) {
      co_return;
    }
    const auto& roots = workspace->folders.Get();
    const auto existing = std::ranges::find_if(roots, [&](const Entry& root) {
      return identity && root->identity == identity;
    });
    if (existing != roots.end() && (*existing)->directory) {
      source.root = *existing;
    } else {
      // Retain the link's directory without mounting it in the file sidebar.
      source.root = std::make_shared<FolderEntry>(
          FolderEntry{++workspace->next_entry, directory.Name(), directory, {}, identity});
      source.root->mounted = false;
      workspace->folders.Update([&](auto& roots) { roots.push_back(source.root); });
    }
  }
  if (!tab->source_folder.Get()) {
    tab->source_relative = source.path;
    tab->source_folder = source.root;
    folio->Persist(*workspace);
  }
  auto file = std::move(entry.Value());
  auto target = std::make_shared<DocumentFile>(DocumentFile{std::move(file.file), std::move(file.parent), {}});
  std::vector<Target> targets{std::move(target)};
  co_await OpenTargets(std::move(targets), source.root, RecentKind::File, {}, std::move(destination));
}

void WorkbenchActions::ChooseLinkFolder(Tab tab, LinkDestination destination, FolderDocumentLocation source) const {
  if (!Available()) {
    return;
  }
  if (!picker->CanOpenDirectories()) {
    Status(tab, l10n::linked_document_needs_folder);
    return;
  }
  const auto label = source.root ? source.root->name : tab->Name();
  const auto hint = source.root ? l10n::link_grant_hint : l10n::link_folder_hint;
  ShowChoices(l10n::linked_document_needs_folder, StringVariant::Format(hint, label), {
      {l10n::cancel, [] {}},
      {l10n::choose_folder, [self = *this, tab, destination = std::move(destination), source = std::move(source)] {
        self.Launch(true, [self, tab, destination, source]() -> Task<void> {
          const auto original = tab->saved.Get();
          const auto selected = co_await self.picker->OpenDirectoryAsync();
          if (!selected || !original || self.workspace->Selected() != tab || tab->saved.Get() != original) {
            co_return;
          }
          FileSource directory(*selected);
          auto file = co_await FindFolderFile(directory, source.path);
          bool matches = false;
          if (file.Succeeded()) {
            const auto identity = co_await LocalIdentity(file.Value().file);
            if (original->target->identity && identity) {
              matches = original->target->identity == identity;
            } else {
              // Opaque picker references expose no comparable identity. Check the saved source bytes.
              const auto bytes = co_await file.Value().file.ReadBytesAsync();
              matches = bytes.Succeeded() && bytes.Value() == original->bytes;
            }
          }
          if (self.workspace->Selected() != tab || tab->saved.Get() != original) {
            co_return;
          }
          if (!matches) {
            self.Status(tab, l10n::link_folder_mismatch);
            co_return;
          }
          if (source.root) {
            const auto identity = co_await LocalIdentity(directory);
            source.root->directory = directory;
            source.root->identity = identity;
            source.root->unresolved = std::nullopt;
            source.root->load = FolderLoad::Unloaded;
            if (source.root->expanded.Get()) {
              self.SetExpanded(source.root, true);
            }
            self.folio->Persist(*self.workspace);
          }
          co_await self.OpenLinkedDocument(tab, destination, source, directory, false);
        });
      }, ChoiceTone::Primary},
  }, true);
}

void WorkbenchActions::OpenKnown(Target target, Entry source_folder) const {
  if (Available()) {
    Status({}, {});
    Launch(true, [self = *this, target = std::move(target), source_folder = std::move(source_folder)]() -> Task<void> {
      co_await self.OpenTargets(std::vector{target}, source_folder);
    });
  }
}

void WorkbenchActions::OpenActivated() const {
  if (folio->restoring.Get() || folio->pending_files->empty() || !Available() || folio->ActivationWindow() != workspace) {
    return;
  }
  auto files = folio->pending_files.Get();
  folio->pending_files = std::vector<Target>{};
  Status({}, {});
  Launch(true, [self = *this, files = std::move(files)]() mutable -> Task<void> {
    co_await self.OpenTargets(std::move(files));
  });
}

Task<std::optional<FileSource>> OpenWorkspacePath(WorkspaceLocation location, FileType type) {
  if (location.path.empty()) {
    co_return std::nullopt;
  }
  const File file(location.path);
  const auto info = co_await file.StatAsync();
  if (!info.Succeeded() || info.Value().type != type) {
    co_return std::nullopt;
  }
  co_return FileSource(file, type, location.writable);
}

Task<void> WorkbenchActions::FillRestoredTab(Tab tab, Target target, Entry folder) const {
  auto loaded = co_await LoadDocument(std::move(target));
  if (!tab->unresolved.Get() || std::ranges::find(workspace->tabs.Get(), tab) == workspace->tabs->end()) {
    co_return;
  }
  if (auto* original = std::get_if<std::shared_ptr<const SavedDocument>>(&loaded)) {
    SetRestoredDocument(tab, *original, folder);
    folio->Persist(*workspace);
  } else {
    const auto& failure = std::get<OpenFailure>(loaded);
    Status(tab, failure.message.empty() ? IssueMessage(failure.issues) : failure.message);
  }
}

Task<void> WorkbenchActions::RestoreTab(Tab tab, std::optional<FileSource> directory) const {
  const auto pending = tab->unresolved.Get();
  if (!pending) {
    co_return;
  }
  if (!pending->library_document.empty()) {
    if (auto target = folio->LibraryTarget(pending->library_document)) {
      co_await FillRestoredTab(tab, std::move(target), {});
    }
    co_return;
  }
  const auto& roots = workspace->folders.Get();
  const auto root = std::ranges::find_if(roots, [&](const Entry& folder) { return folder->id == pending->folder; });
  const auto folder = root == roots.end() ? Entry{} : *root;
  if (!directory) {
    auto local = co_await OpenWorkspacePath(pending->location, FileType::File);
    if (local) {
      auto attachments = co_await OpenWorkspacePath(pending->attachments, FileType::Directory);
      if (!attachments && pending->attachments.path.empty()) {
        if (const auto parent = local->AsFile()->Parent()) {
          const bool writable = folder && folder->directory && folder->directory->CanWrite();
          attachments = FileSource(*parent, FileType::Directory, writable);
        }
      } else if (!attachments) {
        tab->unavailable_attachments = pending->attachments;
      }
      co_await FillRestoredTab(tab, std::make_shared<DocumentFile>(DocumentFile{*local, attachments, {}}), folder);
      co_return;
    }
    directory = folder ? folder->directory : std::nullopt;
  }
  if (!directory || pending->relative.empty()) {
    co_return;
  }
  auto entry = co_await FindFolderFile(*directory, pending->relative);
  if (!entry.Succeeded()) {
    Status(tab, entry.Error().message);
    co_return;
  }
  auto file = std::move(entry.Value());
  co_await FillRestoredTab(tab, std::make_shared<DocumentFile>(
      DocumentFile{std::move(file.file), std::move(file.parent), {}}), folder);
}

void WorkbenchActions::FinishRestore() const {
  if (!workspace->restoring.Get()) {
    return;
  }
  workspace->restoring = false;
  folio->Persist(*workspace);
  folio->restoring = folio->restoring.Get() - 1;
  if (folio->restoring.Get() == 0) {
    const auto& active = folio->store->Get().active_window;
    for (const auto& weak : folio->windows) {
      if (const auto owner = weak.lock(); owner && owner->id == active && owner->window && owner->window->IsOpen()) {
        owner->window->Activate();
        break;
      }
    }
  }
}

void WorkbenchActions::RestoreWorkspace() const {
  if (!workspace->restoring.Get()) {
    return;
  }
  Launch(false, [self = *this]() -> Task<void> {
    const auto folders = self.workspace->folders.Get();
    for (const auto& folder : folders) {
      const auto pending = folder->unresolved.Get();
      if (pending) {
        auto directory = co_await OpenWorkspacePath(pending->location, FileType::Directory);
        if (directory && (co_await directory->AsFile()->ListChildrenAsync()).Succeeded()) {
          folder->directory = std::move(directory);
          folder->identity = co_await LocalIdentity(*folder->directory);
          folder->unresolved = std::nullopt;
        }
      }
    }
    const auto tabs = self.workspace->tabs.Get();
    for (const auto& tab : tabs) {
      co_await self.RestoreTab(tab);
    }
    self.FinishRestore();
  }, [self = *this](std::string error) {
    self.Status({}, std::move(error));
    self.FinishRestore();
  });
}

void WorkbenchActions::RestoreWindows() const {
  auto pending = std::exchange(folio->restore_windows, {});
  for (auto& record : pending) {
    folio->installing = std::move(record);
    try {
      application.OpenWindow(App, FolioWindowOptions());
    } catch (const std::exception& error) {
      folio->installing.reset();
      folio->restoring = folio->restoring.Get() - 1;
      Status({}, error.what());
    }
  }
}

void WorkbenchActions::RelocateTab(Tab tab) const {
  const auto pending = tab->unresolved.Get();
  if (!Available() || !pending || !pending->library_document.empty() || workspace->restoring.Get()) {
    return;
  }
  const auto& roots = workspace->folders.Get();
  const auto root = std::ranges::find_if(roots, [&](const Entry& folder) { return folder->id == pending->folder; });
  if (root != roots.end() && (*root)->unresolved.Get()) {
    RelocateFolder(*root);
    return;
  }
  Launch(true, [self = *this, tab, pending]() -> Task<void> {
    if (!pending->package.empty()) {
      const auto folder = co_await self.picker->OpenDirectoryAsync();
      if (!folder) {
        co_return;
      }
      auto file = co_await FindPackageDocument(*folder);
      if (!file.Succeeded()) {
        self.Status(tab, file.Error().message);
        co_return;
      }
      co_await self.FillRestoredTab(tab, std::make_shared<DocumentFile>(
          DocumentFile{std::move(file.Value()), folder, {}}), {});
      co_return;
    }
    const auto file = co_await self.picker->OpenFileAsync({
        .name = self.resources->GetString(l10n::rich_editor_documents), .extensions = {"json", "md", "markdown"},
        .content_types = {"application/json", "text/markdown", "text/plain"},
    });
    if (file) {
      co_await self.FillRestoredTab(tab, std::make_shared<DocumentFile>(DocumentFile{*file, {}, {}}), {});
    }
  });
}

void WorkbenchActions::RelocateFolder(Entry folder) const {
  const auto pending = folder->unresolved.Get();
  if (!Available() || !pending || workspace->restoring.Get() || !picker->CanOpenDirectories()) {
    return;
  }
  Launch(true, [self = *this, folder, writable = pending->location.writable]() -> Task<void> {
    const auto reference = co_await self.picker->OpenDirectoryAsync(writable && self.picker->CanOpenDirectories(true));
    if (!reference) {
      co_return;
    }
    folder->directory = *reference;
    folder->name = reference->Name().empty() ? folder->name : reference->Name();
    folder->identity = co_await LocalIdentity(*folder->directory);
    folder->unresolved = std::nullopt;
    if (folder->mounted.Get()) {
      self.SetExpanded(folder, true);
    }
    co_await self.RestoreFolderTabs(folder);
  });
}

Task<void> WorkbenchActions::RestoreFolderTabs(Entry folder) const {
  const auto tabs = workspace->tabs.Get();
  for (const auto& tab : tabs) {
    const auto& record = tab->unresolved.Get();
    if (record && record->folder == folder->id) {
      co_await RestoreTab(tab, *folder->directory);
    }
  }
}

void WorkbenchActions::PickOpen(OpenRequest request) const {
  if (!Available()) {
    return;
  }
  Status({}, {});
  Launch(true, [self = *this, request = std::move(request)] { return self.OpenPicked(request); });
}

Task<void> WorkbenchActions::OpenPicked(OpenRequest request) const {
  if (request.kind == OpenKind::File || request.kind == OpenKind::Import) {
    const auto file = co_await picker->OpenFileAsync({
        .name = resources->GetString(l10n::rich_editor_documents), .extensions = {"json", "md", "markdown"},
        .content_types = {"application/json", "text/markdown", "text/plain"},
    });
    if (!file) {
      co_return;
    }
    auto target = std::make_shared<DocumentFile>(DocumentFile{*file, {}, {}});
    if (request.kind == OpenKind::File) {
      co_await OpenTargets(std::vector{std::move(target)}, {}, RecentKind::File, std::move(request.recent_id));
      co_return;
    }
    if (const auto local = file->AsFile(); local && local->Parent()) {
      target->directory = FileSource(*local->Parent(), FileType::Directory, false);
    }
    auto loaded = co_await LoadDocument(target, true);
    if (auto* failure = std::get_if<OpenFailure>(&loaded)) {
      failure->import_parent = std::move(request.import_parent);
      ShowOpenFailures({std::move(*failure)});
    } else {
      const auto& original = std::get<std::shared_ptr<const SavedDocument>>(loaded);
      auto tab = AddTab(target->Name(), original->snapshot, {}, {}, target->directory, std::move(request.import_parent));
      StartSave(std::move(tab), WriteKind::Library);
    }
    co_return;
  }
  if (const auto directory = co_await picker->OpenDirectoryAsync()) {
    co_await OpenDirectory(*directory, request.kind, std::move(request.recent_id));
  }
}

Task<void> WorkbenchActions::OpenDirectory(FileSource directory, OpenKind kind, std::string recent_id) const {
  if (kind == OpenKind::Folder) {
    const auto folder = co_await AddFolder(std::move(directory), std::move(recent_id));
    const auto& source = *folder->directory;
    folder->recent_id = folio->Remember({{folder->recent_id, folder->name, folio->Location(source), RecentKind::Folder},
                                        {}, source});
    co_return;
  }
  auto file = co_await FindPackageDocument(directory);
  if (file.Succeeded()) {
    co_await OpenTargets(std::vector{std::make_shared<DocumentFile>(
        DocumentFile{std::move(file.Value()), directory, {}})}, {}, RecentKind::Package, std::move(recent_id));
  } else {
    Status({}, file.Error().message);
  }
}

OpenKind RecentOpenKind(RecentKind kind) {
  switch (kind) {
  case RecentKind::File: return OpenKind::File;
  case RecentKind::Folder: return OpenKind::Folder;
  case RecentKind::Package: return OpenKind::Package;
  }
  return OpenKind::File;
}

void WorkbenchActions::OpenRecent(Recent item) const {
  if (!Available()) {
    return;
  }
  Status({}, {});
  Launch(true, [self = *this, item = std::move(item)]() -> Task<void> {
    const auto& record = item->record;
    const auto kind = RecentOpenKind(record.kind);
    if (!record.library_document.empty()) {
      if (auto target = self.folio->LibraryTarget(record.library_document)) {
        co_await self.OpenTargets(std::vector{std::move(target)}, {}, record.kind, record.id);
      } else {
        self.Status({}, l10n::library_document_unavailable);
      }
      co_return;
    }
    if (item->target) {
      co_await self.OpenTargets(std::vector{item->target}, {}, record.kind, record.id);
      co_return;
    }
    auto source = item->directory ? item->directory
        : co_await OpenWorkspacePath(record.location, kind == OpenKind::File ? FileType::File : FileType::Directory);
    if (source) {
      if (kind == OpenKind::File) {
        co_await self.OpenTargets(std::vector{std::make_shared<DocumentFile>(DocumentFile{*source, {}, {}})},
                                  {}, record.kind, record.id);
      } else {
        co_await self.OpenDirectory(*source, kind, record.id);
      }
      co_return;
    }
    co_await self.OpenPicked({.kind = kind, .recent_id = record.id});
  });
}

void WorkbenchActions::RemoveExternalFolder(const Entry& folder) const {
  if (Available()) {
    folder->mounted = false;
    workspace->PruneFolders();
  }
}

Task<Entry> WorkbenchActions::AddFolder(FileSource directory, std::string recent_id) const {
  const auto identity = co_await LocalIdentity(directory);
  workspace->external_open = true;
  workspace->drawer_open = true;
  workspace->sidebar_visible = true;
  for (const auto folder : workspace->folders.Get()) {
    if ((identity && identity == folder->identity) || (!recent_id.empty() && recent_id == folder->recent_id)) {
      folder->mounted = true;
      if (!recent_id.empty()) {
        folder->recent_id = std::move(recent_id);
      }
      if (!folder->directory || !identity || (directory.CanWrite() && !folder->directory->CanWrite())) {
        if (auto name = directory.Name(); !name.empty()) {
          folder->name = std::move(name);
        }
        folder->directory = std::move(directory);
        folder->identity = identity;
        folder->unresolved = std::nullopt;
        folder->load = FolderLoad::Unloaded;
      }
      SetExpanded(folder, true);
      co_await RestoreFolderTabs(folder);
      co_return folder;
    }
  }
  auto name = directory.Name();
  if (name.empty()) {
    name = identity ? identity->Path() : resources->GetString(l10n::folder);
  }
  auto folder = std::make_shared<FolderEntry>(FolderEntry{
      ++workspace->next_entry, std::move(name), std::move(directory), {}, identity,
  });
  folder->recent_id = std::move(recent_id);
  workspace->folders.Update([&](auto& roots) { roots.push_back(folder); });
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
        row->parent = entry;
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
  if (!Available() || tab->Busy() || editor.IsReadOnly() || !tab->working_directory) {
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
      self.folio->Persist(*self.workspace);
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

using DialogSubmission = std::function<void()>;

template <class Prepare>
auto SubmitDialog(Prepare prepare, std::optional<TaskScope> text_input_tasks = {}) {
  return [prepare, text_input_tasks] {
    if (auto submit = prepare()) {
      // Finish the text-input event before dismissing the dialog or opening another layer.
      if (text_input_tasks) {
        text_input_tasks->Post(std::move(submit));
      } else {
        submit();
      }
    }
  };
}

[[huxerui::composable]]
View OutputDialog(DialogContext context, StringVariant title, std::string name, DocumentFormat preferred,
                  std::string message, StringVariant action_label,
                  std::function<void(DocumentFormat, std::string)> action, std::function<void()> cancel) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
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
  const bool valid = !filename->composition && ValidDocumentName(filename->text, format.Get());
  const auto prepare = [context, format, filename, action]() -> DialogSubmission {
    const auto selected = format.Get();
    const auto& field = filename.Get();
    if (field.composition || !ValidDocumentName(field.text, selected)) {
      return {};
    }
    return [context, action, selected, name = field.text] {
      if (context.Dismiss()) {
        action(selected, name);
      }
    };
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
        .OnSubmitted(SubmitDialog(prepare, tasks)).Key("output-name"),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context, cancel] {
        context.Dismiss();
        cancel();
      }),
      Button(std::move(action_label)).OnClick(SubmitDialog(prepare)).With(Enabled(valid))),
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

void WorkbenchActions::ShowChoices(StringVariant title, StringVariant message, std::vector<Choice> choices,
                                   bool dismiss_on_cancel) const {
  dialog.Show(DialogFactory{[title = std::move(title), message = std::move(message),
                            choices = std::move(choices)](DialogContext context) {
    return ChoiceDialog(context, title, message, choices);
  }}, {.dismiss_on_outside_press = false, .dismiss_on_cancel = dismiss_on_cancel});
}

void WorkbenchActions::ShowOpenFailures(std::vector<OpenFailure> failures) const {
  std::string message;
  for (const auto& failure : failures) {
    if (!message.empty()) {
      message += "\n\n";
    }
    message += failure.target->reference.Name();
    if (failure.issues.empty()) {
      message += '\n';
      message += failure.message.empty() ? resources->GetString(l10n::document_conversion_failed) : failure.message;
    }
    for (const auto& issue : failure.issues) {
      message += "\n• " + issue.message;
      if (!issue.block_path.empty()) {
        std::string path;
        for (const auto index : issue.block_path) {
          if (!path.empty()) {
            path += '.';
          }
          path += std::to_string(index + 1);
        }
        message += " (" + resources->GetString(StringVariant::Format(l10n::source_block, path)) + ")";
      }
    }
    if (failure.lossy_copy && failures.size() > 1) {
      message += "\n" + resources->GetString(l10n::lossy_copy_available);
    }
  }
  const auto finish = [self = *this] {
    self.workspace->showing_open_issues = false;
    self.ContinueClose();
  };
  std::vector<Choice> choices{{l10n::cancel, finish}};
  std::erase_if(failures, [](const OpenFailure& failure) { return !failure.lossy_copy; });
  if (!failures.empty()) {
    StringVariant label = l10n::open_lossy_copy;
    if (failures.size() > 1) {
      label = StringVariant::Format(l10n::open_lossy_copies, failures.size());
    }
    choices.push_back({std::move(label), [self = *this, finish, copies = std::move(failures)] {
      for (const auto& copy : copies) {
        self.AddTab(CopyName(copy.target->reference.Name(), DocumentFormat::Markdown),
                    *copy.lossy_copy, {}, {}, copy.target->directory, copy.import_parent);
      }
      finish();
    }, ChoiceTone::Primary});
  }
  ShowChoices(l10n::open_document_failed, std::move(message), std::move(choices));
  workspace->showing_open_issues = true;
}

bool ValidChildName(std::string_view name) {
  return !name.empty() && !name.ends_with('.') && !name.ends_with(' ') &&
      name.find_first_of("\\/:*?\"<>|") == name.npos &&
      std::ranges::none_of(name, [](unsigned char ch) { return ch < 32; });
}

StringVariant LibraryWriteMessage(LibraryWriteResult result) {
  switch (result) {
  case LibraryWriteResult::Saved: return {};
  case LibraryWriteResult::NameInUse: return l10n::name_already_used;
  case LibraryWriteResult::Busy: return l10n::library_write_busy;
  case LibraryWriteResult::Unavailable: return l10n::library_entry_unavailable;
  case LibraryWriteResult::Changed: return l10n::library_delete_changed;
  case LibraryWriteResult::WriteFailed: return l10n::library_change_failed;
  }
  return {};
}

[[huxerui::composable]]
View LibraryNameDialog(DialogContext context, StringVariant title, StringVariant label, StringVariant action,
                       std::string name, State<bool> saving,
                       std::function<Task<LibraryWriteResult>(std::string)> apply, std::function<void()> on_saved) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto application_tasks = UseApplicationTaskScope();
  const auto focus = UseViewHandle();
  auto initial = TextEditingValue::FromText(std::move(name));
  initial.selection.anchor = 0;
  const auto value = UseState(std::move(initial));
  const auto result = UseState(LibraryWriteResult::Saved);
  const bool valid = !value->composition && ValidChildName(value->text);
  auto validation = ValidationResult::None();
  if (saving.Get()) {
    validation = ValidationResult::Pending(l10n::saving);
  } else if (!value->composition && !valid) {
    validation = ValidationResult::Invalid(l10n::invalid_entry_name);
  } else if (result.Get() != LibraryWriteResult::Saved) {
    validation = ValidationResult::Invalid(LibraryWriteMessage(result.Get()));
  }
  const auto prepare = [context, tasks, application_tasks, value, saving, result, focus, apply,
                        on_saved]() -> DialogSubmission {
    const auto& current = value.Get();
    if (saving.Get() || current.composition || !ValidChildName(current.text)) {
      return {};
    }
    return [context, tasks, application_tasks, saving, result, focus, apply, on_saved, name = current.text] {
      if (saving.Get()) {
        return;
      }
      saving = true;
      // Complete the catalog write even if the window closes while the write is in flight.
      application_tasks.Launch([context, tasks, saving, result, focus, apply, on_saved, name]() -> Task<void> {
        auto written = LibraryWriteResult::WriteFailed;
        try {
          written = co_await apply(name);
        } catch (const std::exception&) {
        }
        tasks.Post([context, saving, result, focus, on_saved, written] {
          saving = false;
          if (written == LibraryWriteResult::Saved) {
            if (context.Dismiss()) {
              on_saved();
            }
          } else {
            result = written;
            focus.RequestFocus();
          }
        });
      });
    };
  };
  return DialogPanel(Column {
    Text(title, TextRole::Title),
    TextField(value).Label(label).Variant(TextFieldVariant::Outlined)
        .Validation(validation).InputConfiguration({.read_only = saving.Get()})
        .OnChanged([value, result](const TextEditingValue& next) {
          if (next.text != value->text) {
            result = LibraryWriteResult::Saved;
          }
          value = next;
        }).OnSubmitted(SubmitDialog(prepare, tasks))
        .With(focus, Semantics{.identifier = "library-entry-name"}).Key("library-entry-name"),
    DialogActions(theme, Button(l10n::cancel).OnClick([context] { context.Dismiss(); }).With(Enabled(!saving.Get())),
                  Button(action).OnClick(SubmitDialog(prepare)).With(Enabled(valid && !saving.Get()))).Key("actions"),
  }, theme, 400.0F);
}

void WorkbenchActions::EditLibraryName(LibraryEntry entry, bool creating) const {
  const State<bool> saving{false};
  const auto id = std::make_shared<LayerId>();
  const auto apply = [library = folio->library, entry, creating](std::string name) -> Task<LibraryWriteResult> {
    auto current = creating ? std::optional(entry) : library->Find(entry.id);
    if (!current) {
      co_return LibraryWriteResult::Unavailable;
    }
    current->name = std::move(name);
    co_return co_await library->Put(std::move(*current));
  };
  const auto on_saved = [self = *this, parent = entry.parent, creating] {
    if (creating) {
      self.SetLibraryExpanded(parent, true);
    }
  };
  *id = dialog.Show(DialogFactory{[entry = std::move(entry), creating, saving, apply, on_saved](DialogContext context) {
    return LibraryNameDialog(context, creating ? l10n::new_folder : l10n::library_rename,
                             entry.folder ? l10n::folder_name : l10n::file_name, creating ? l10n::create : l10n::save,
                             entry.name, saving, apply, on_saved);
  }}, {.on_dismiss_request = [dialog = dialog, id, saving] {
    if (!saving.Get()) {
      dialog.Dismiss(*id);
    }
  }});
}

void WorkbenchActions::NewLibraryFolder(std::string parent) const {
  if (Available() && folio->library) {
    EditLibraryName({UniqueName("folder-"), resources->GetString(l10n::new_folder), std::move(parent), true}, true);
  }
}

void WorkbenchActions::RenameLibraryEntry(std::string id) const {
  if (Available()) {
    if (const auto entry = folio->library->Find(id)) {
      EditLibraryName(*entry, false);
    }
  }
}

void WorkbenchActions::MoveLibraryEntry(std::string id, std::string parent) const {
  const auto library = folio->library;
  if (!Available() || !library || !library->CanPlace(id, parent)) {
    return;
  }
  Launch(false, [self = *this, library, id = std::move(id), parent = std::move(parent)]() -> Task<void> {
    auto entry = library->Find(id);
    if (!entry) {
      self.Status({}, l10n::library_entry_unavailable);
      co_return;
    }
    if (entry->parent == parent) {
      co_return;
    }
    entry->parent = parent;
    const auto result = co_await library->Put(std::move(*entry));
    if (result == LibraryWriteResult::Saved) {
      self.SetLibraryExpanded(parent, true);
    } else {
      self.Status({}, LibraryWriteMessage(result));
    }
  });
}

void WorkbenchActions::ConfirmLibraryDelete(std::string id) const {
  if (!Available() || !folio->library) {
    return;
  }
  auto expected = folio->library->Subtree(id);
  if (expected.empty()) {
    return;
  }
  const auto ids = EntryIds(expected);
  if (folio->LibraryDeleteBusy(ids)) {
    ReportLibraryDeleteFailure(l10n::library_write_busy);
    return;
  }
  const auto& root = expected.front();
  auto title = StringVariant::Format(l10n::library_delete_title, root.name);
  std::string message;
  if (!root.folder) {
    message = resources->GetString(l10n::library_delete_document);
  } else if (expected.size() == 1) {
    message = resources->GetString(l10n::library_delete_empty_folder);
  } else {
    const auto documents = std::ranges::count(expected, false, &LibraryEntry::folder);
    const auto folders = expected.size() - static_cast<std::size_t>(documents) - 1;
    message = resources->GetString(StringVariant::Format(l10n::library_delete_folder, documents, folders));
  }
  std::set<DocumentId> confirmed_dirty;
  std::string dirty_names;
  for (const auto& tab : folio->LibraryTabs(ids)) {
    if (tab->Dirty()) {
      confirmed_dirty.insert(tab->id);
      if (root.folder) {
        dirty_names += "\n• " + tab->Name();
      }
    }
  }
  if (!confirmed_dirty.empty()) {
    message += "\n\n" + resources->GetString(l10n::library_delete_unsaved) + dirty_names;
  }
  ShowChoices(std::move(title), std::move(message), {
      {l10n::cancel, [] {}},
      {l10n::library_delete, [self = *this, expected = std::move(expected),
                             confirmed_dirty = std::move(confirmed_dirty)]() mutable {
        self.DeleteLibraryEntries(std::move(expected), std::move(confirmed_dirty));
      }, ChoiceTone::Destructive},
  }, true);
}

void WorkbenchActions::ReportLibraryDeleteFailure(StringVariant reason) const {
  ShowChoices(l10n::library_delete_failed, std::move(reason), {{l10n::close, [] {}}}, true);
}

void WorkbenchActions::DeleteLibraryEntries(std::vector<LibraryEntry> expected,
                                            std::set<DocumentId> confirmed_dirty) const {
  const auto ids = EntryIds(expected);
  if (!Available() || folio->LibraryDeleteBusy(ids)) {
    ReportLibraryDeleteFailure(l10n::library_write_busy);
    return;
  }
  auto tabs = folio->LibraryTabs(ids);
  if (std::ranges::any_of(tabs, [&](const Tab& tab) { return tab->Dirty() && !confirmed_dirty.contains(tab->id); })) {
    ReportLibraryDeleteFailure(l10n::library_delete_changed);
    return;
  }
  auto name = expected.front().name;
  folio->deleting_library_entries = true;
  for (const auto& tab : tabs) {
    tab->operation = Operation::Delete;
  }
  application_tasks.Launch([self = *this, expected = std::move(expected), ids, tabs = std::move(tabs),
                            name = std::move(name)]() mutable -> Task<void> {
    auto result = LibraryWriteResult::WriteFailed;
    std::string failure;
    try {
      result = co_await self.folio->library->Remove(std::move(expected));
    } catch (const std::exception& error) {
      failure = error.what();
    }
    if (result != LibraryWriteResult::Saved) {
      for (const auto& tab : tabs) {
        tab->operation = Operation::Idle;
      }
      self.folio->deleting_library_entries = false;
      self.ReportLibraryDeleteFailure(failure.empty() ? LibraryWriteMessage(result) : StringVariant(std::move(failure)));
      co_return;
    }
    const auto temporary = self.folio->ForgetLibraryEntries(ids);
    self.folio->deleting_library_entries = false;
    bool cleaned = false;
    try {
      cleaned = co_await self.folio->library->CollectDeletedPackages();
    } catch (const std::exception&) {
      // The catalog retains the pending packages for the next launch.
    }
    self.Status({}, cleaned ? StringVariant::Format(l10n::library_deleted, name)
                            : StringVariant(l10n::library_delete_cleanup_pending));
    for (const auto& directory : temporary) {
      (void)co_await directory.DeleteRecursivelyAsync();
    }
  });
}

[[huxerui::composable]]
View MoveLibraryDialog(DialogContext context, WorkbenchActions actions, std::string id,
                       std::vector<std::pair<std::string, std::string>> destinations) {
  const auto& theme = UseTheme();
  Views rows;
  for (const auto& [folder, label] : destinations) {
    rows.Add(Button(label).OnClick([context, actions, id, folder] {
      context.Dismiss();
      actions.MoveLibraryEntry(id, folder);
    }).With(Frame{.min_height = 38.0F}).Key(folder));
  }
  return ScrollableDialogPanel(
      Text(l10n::library_move_to, TextRole::Title),
      ScrollView(Column {std::move(rows)}.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
          .With(Frame{.max_height = 300.0F}, ScrollBar()),
      DialogActions(theme, Button(l10n::cancel).OnClick([context] { context.Dismiss(); }), {}), theme, 420.0F);
}

void WorkbenchActions::ChooseLibraryDestination(std::string id) const {
  const auto library = folio->library;
  const auto entry = library->Find(id);
  if (!Available() || !entry) {
    return;
  }
  std::vector<std::pair<std::string, std::string>> destinations;
  for (const auto& folder : library->Entries()) {
    if (folder.folder && folder.id != entry->parent && library->CanPlace(id, folder.id)) {
      auto path = folder.name;
      for (auto parent = library->Find(folder.parent); parent; parent = library->Find(parent->parent)) {
        path = parent->name + " / " + path;
      }
      destinations.emplace_back(folder.id, std::move(path));
    }
  }
  std::ranges::sort(destinations, {}, &std::pair<std::string, std::string>::second);
  if (!entry->parent.empty()) {
    destinations.insert(destinations.begin(), {"", resources->GetString(l10n::workspace)});
  }
  if (!destinations.empty()) {
    dialog.Show(MoveLibraryDialog, *this, std::move(id), std::move(destinations));
  }
}

[[huxerui::composable]]
View NewChildDialog(DialogContext context, ChildKind kind, std::string folder, std::string name, std::string message,
                    std::function<void(std::string)> create) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const bool file = kind == ChildKind::File;
  auto initial = TextEditingValue::FromText(name.empty()
      ? (file ? UseString(l10n::untitled) + ".md" : UseString(l10n::new_folder)) : name);
  initial.selection.anchor = 0;
  const auto value = UseState(std::move(initial));
  const auto child_name = [file](const TextEditingValue& field) -> std::optional<std::string> {
    if (field.composition || !ValidChildName(field.text)) {
      return {};
    }
    auto candidate = file && !FormatOf(field.text) ? field.text + ".md" : field.text;
    return !file || ValidDocumentName(candidate, *FormatOf(candidate)) ? std::optional(std::move(candidate)) : std::nullopt;
  };
  const auto prepare = [context, create, value, child_name]() -> DialogSubmission {
    auto candidate = child_name(value.Get());
    if (!candidate) {
      return {};
    }
    return [context, create, name = std::move(*candidate)] {
      if (context.Dismiss()) {
        create(name);
      }
    };
  };
  return DialogPanel(Column {
    Text(file ? l10n::new_file : l10n::new_folder, TextRole::Title),
    Text(StringVariant::Format(l10n::create_in, folder), TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    message.empty() ? View{} : Text(std::move(message), TextRole::Label).With(Foreground(theme.colors.error)),
    TextField(value).Label(file ? l10n::file_name : l10n::folder_name).Variant(TextFieldVariant::Outlined)
        .OnChanged([value](const TextEditingValue& next) { value = next; })
        .OnSubmitted(SubmitDialog(prepare, tasks)).Key("name"),
    DialogActions(theme,
      Button(l10n::cancel).OnClick([context] { context.Dismiss(); }),
      Button(l10n::create).OnClick(SubmitDialog(prepare)).With(Enabled(child_name(value.Get()).has_value()))).Key("actions"),
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

Task<IoResult<FileSource>> CreateNamedFolder(FileSource parent, const std::string& name) {
  const IoError exists{IoErrorCode::AlreadyExists, "A file or folder already uses that name"};
  if (const auto local = parent.AsFile()) {
    const auto info = co_await local->Child(name).StatAsync();
    if (info.Succeeded()) {
      co_return IoResult<FileSource>::Failure(exists);
    }
    if (info.Error().code != IoErrorCode::NotFound) {
      co_return IoResult<FileSource>::Failure(info.Error());
    }
  } else {
    auto children = co_await parent.ListChildrenAsync();
    if (!children.Succeeded()) {
      co_return IoResult<FileSource>::Failure(children.Error());
    }
    if (std::ranges::any_of(children.Value(), [&](const FileSource& child) { return child.Name() == name; })) {
      co_return IoResult<FileSource>::Failure(exists);
    }
  }
  co_return co_await parent.CreateDirectoryAsync(name);
}

Task<IoResult<FileSource>> CreateEmptyFile(FileSource parent, const std::string& name, File temporary) {
  auto encoded = Codec(*FormatOf(name)).Export(RichDocument{});
  const auto staged = temporary.Child(UniqueName("folio-new-"));
  if (!encoded.text || !(co_await temporary.CreateDirectoriesAsync()) ||
      !(co_await staged.WriteStringAsync(*encoded.text))) {
    (void)co_await staged.DeleteAsync();
    co_return IoResult<FileSource>::Failure({IoErrorCode::Io, "Could not prepare the new document"});
  }
  auto created = co_await parent.CopyFileFromAsync(staged, name);
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
      co_await self.OpenTargets(std::vector{std::make_shared<DocumentFile>(
          DocumentFile{std::move(created.Value()), directory, {}})}, folder);
    }
  });
}

void WorkbenchActions::RequestSave(Tab tab, WriteKind kind) const {
  if (!tab || tab->unresolved.Get() || !Available() || tab->Busy()) {
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
  if (kind == WriteKind::Library) {
    SaveToLibrary(request);
  } else if (kind == WriteKind::Save) {
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
  const auto& original = request->tab->saved.Get();
  if (original && !SameFile(original->target, target)) {
    request->tab->source_folder = nullptr;
    request->tab->source_relative.clear();
  }
  request->tab->saved = std::make_shared<const SavedDocument>(SavedDocument{
      target, payload.format, std::move(payload.snapshot), Bytes(bytes.begin(), bytes.end()),
  });
  if (target->directory) {
    request->tab->BindAttachmentDirectory(*target->directory);
  }
  folio->Persist(*workspace);
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
  if (kind != WriteKind::Export && folio->library && IsMobilePlatform()) {
    choices.push_back({l10n::library_save_copy, [self = *this, request] { self.SaveToLibrary(request); }, ChoiceTone::Primary});
  } else if (kind != WriteKind::Export && picker->CanOpenDirectories(true)) {
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
    if (folio->library) {
      SaveToLibrary(request);
    } else {
      ChooseOutput(request, WriteKind::SaveAs, DocumentFormat::Markdown);
    }
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

void WorkbenchActions::SaveToLibrary(Save request) const {
  const auto library = folio->library;
  if (!library) {
    Status(request->tab, l10n::library_save_failed);
    FinishSave(request, false);
    return;
  }
  auto snapshot = request->tab->Snapshot();
  auto encoded = Codec(DocumentFormat::Json).Export(snapshot);
  if (!encoded.text) {
    Status(request->tab, IssueMessage(encoded.issues));
    FinishSave(request, false);
    return;
  }
  const auto original = request->tab->saved.Get();
  auto sources = request->tab->attachments->directories;
  if (sources.empty() && original) {
    if (const auto local = original->target->reference.AsFile(); local && local->Parent()) {
      sources.emplace_back(*local->Parent(), FileType::Directory, false);
    }
  }
  auto title = request->tab->Name();
  if (FormatOf(title)) {
    title.resize(title.rfind('.'));
  }
  auto entry = LibraryEntry{UniqueName("document-"), std::move(title), request->tab->library_folder.value_or("")};
  const auto package = *library->Package(entry.id);
  const auto failed = [self = *this, request](std::string message) {
    self.Status(request->tab, std::move(message));
    self.FinishSave(request, false);
  };
  Launch(false, [self = *this, request, library, entry = std::move(entry), package, sources = std::move(sources),
                 payload = SavePayload{std::move(snapshot), DocumentFormat::Json, std::move(*encoded.text)},
                 failed]() mutable -> Task<void> {
    const auto saved = co_await WriteLibraryPackage(package, payload.text, request->tab->working_directory,
                                                     std::move(sources), payload.snapshot);
    if (!saved.Succeeded()) {
      (void)co_await package.DeleteRecursivelyAsync();
      failed(saved.Error().message);
      co_return;
    }
    entry.name = library->UniqueTitle(std::move(entry.name), entry.parent);
    const auto written = co_await library->Put(entry);
    if (written != LibraryWriteResult::Saved) {
      (void)co_await package.DeleteRecursivelyAsync();
      failed(self.resources->GetString(LibraryWriteMessage(written)));
      co_return;
    }
    self.MarkSaved(request, self.folio->LibraryTarget(entry.id), std::move(payload));
  }, failed);
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
  const auto written = target->library
      ? co_await WriteLibraryPackage(*target->directory->AsFile(), payload.text, request->tab->working_directory,
                                     request->tab->attachments->directories, payload.snapshot, original->snapshot)
      : co_await WriteOriginal(target->reference, original->bytes, payload.text, request->tab->working_directory,
                               request->tab->attachments->directories, target->directory, payload.snapshot, original->snapshot);
  if (!written.Succeeded()) {
    OfferSaveAs(request, written.Error().message);
    co_return;
  }
  MarkSaved(request, std::move(target), std::move(payload));
}

void WorkbenchActions::OfferSaveAs(Save request, std::string message) const {
  const auto& original = request->tab->saved.Get();
  if (original && original->target->library) {
    Status(request->tab, std::move(message));
    FinishSave(request, false);
    return;
  }
  if (folio->library && IsMobilePlatform()) {
    const auto format = original ? original->format : DocumentFormat::Json;
    ShowChoices(l10n::library_save_copy, std::move(message), {
        {l10n::cancel, [self = *this, request] { self.FinishSave(request, false); }},
        {l10n::export_copy_menu, [self = *this, request, format] { self.ChooseOutput(request, WriteKind::Export, format); },
         ChoiceTone::Secondary},
        {l10n::library_save_copy, [self = *this, request] { self.SaveToLibrary(request); }, ChoiceTone::Primary},
    });
    return;
  }
  ChooseOutput(request, WriteKind::SaveAs, original ? original->format : DocumentFormat::Markdown, std::move(message));
}

void WorkbenchActions::RequestClose(Tab tab) const {
  if (folio->deleting_library_entries.Get() || workspace->closing.Get()) {
    return;
  }
  if (!tab || workspace->selected.Get() == tab->id) {
    file_menu_focus.RequestFocus();
  }
  workspace->closing = std::make_shared<CloseRequest>(CloseRequest{std::move(tab)});
  tasks.Post([self = *this] { self.ContinueClose(); });
}

bool WorkbenchActions::OnCloseRequest() const {
  if (std::exchange(workspace->allow_close, false)) {
    return false;
  }
  RequestClose();
  return true;
}

void WorkbenchActions::ContinueClose() const {
  const auto request = workspace->closing.Get();
  if (!request || !request->waiting || workspace->picking.Get() || workspace->showing_open_issues.Get()) {
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
    workspace->PruneFolders();
    folio->Persist(*workspace);
  } else {
    folio->Forget(*workspace);
    workspace->allow_close = true;
    window.Close();
  }
}

void WorkbenchActions::CancelClose() const { workspace->closing = nullptr; }

void TrackReadingPosition(const WorkbenchActions& actions, const Tab& tab, const RichEditorController& editor, bool active) {
  constexpr float reading_line = 24.0F;
  const auto location = active ? editor.ViewportLocation(reading_line) : std::nullopt;
  const auto anchor = tab->pending_anchor.Get();
  const bool restore = tab->restore_reading_position.Get();
  const bool ready = location.has_value();
  const bool restoring_workspace = actions.workspace->restoring.Get();
  Lifecycle([actions, tab, editor, anchor, restore, ready, restoring_workspace, reading_line] {
    if (ready && !restoring_workspace && (anchor || restore)) {
      actions.tasks.Post([actions, tab, editor, anchor, reading_line] {
        if (actions.workspace->Selected() != tab || tab->pending_anchor.Get() != anchor ||
            !editor.ViewportLocation(reading_line)) {
          return;
        }
        if (anchor) {
          actions.RevealLinkedHeading(tab, editor, *anchor);
        } else if (tab->restore_reading_position.Get()) {
          RestoreReadingPosition(editor, *tab->reading_position);
          tab->restore_reading_position = false;
        }
      });
    }
  }, anchor, restore, ready, restoring_workspace);

  const bool capture = ready && !restoring_workspace && !anchor && !restore;
  const auto position = capture ? CaptureReadingPosition(editor.Document(), location->root_block_index) : std::nullopt;
  Lifecycle([actions, tab, capture, position] {
    if (capture) {
      actions.tasks.Post([actions, tab, position] { actions.RecordReadingPosition(tab, position); });
    }
  }, capture, position);
}

[[huxerui::composable]]
View DocumentPage(WorkbenchActions actions, Tab tab, bool active) {
  const auto editor = UseRichEditor(tab->initial);
  const auto focus = UseViewHandle();
  Lifecycle([tab, editor, focus] {
    editor.SetReadOnly(tab->initial_read_only);
    tab->editor = editor;
    tab->editor_focus = focus;
  }, editor, focus);
  TrackReadingPosition(actions, tab, editor, active);
  const auto& attachments = tab->attachments.Get();
  const auto operation = tab->operation.Get();
  const bool available = active && !actions.workspace->closing.Get() && operation != Operation::PickImage &&
      operation != Operation::Delete;
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
        .clipboard_attachments = attachments.clipboard_attachments,
        .find = tab->find,
        .focus = focus,
        .active = active,
        .available = available,
        .can_pick_images = tab->working_directory && actions.picker->CanOpenFiles() &&
            actions.Available() && !tab->Busy(),
        .pick_image = [actions, tab, editor](ViewHandle focus, bool replace, bool inline_image) {
          actions.PickImage(tab, editor, focus, replace, inline_image);
        },
        .open_document_link = [actions, tab](DocumentLink link) { actions.FollowDocumentLink(tab, std::move(link)); },
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
      if (const auto& pending = tab->unresolved.Get()) {
        const bool restoring = actions.workspace->restoring.Get();
        const bool needs_folder = !pending->package.empty() || std::ranges::any_of(
            actions.workspace->folders.Get(), [&](const Entry& folder) {
              return folder->id == pending->folder && folder->unresolved.Get();
            });
        pages.push_back(Column {
          Text(restoring ? l10n::workspace_loading : pending->library_document.empty()
              ? l10n::workspace_reconnect_file : l10n::library_document_unavailable),
          restoring || !pending->library_document.empty() ? View{} : Button(l10n::workspace_relocate)
              .OnClick([actions, tab] { actions.RelocateTab(tab); })
              .With(Enabled(actions.Available() && (needs_folder ? actions.picker->CanOpenDirectories()
                                                                : actions.picker->CanOpenFiles())),
                    Semantics{.identifier = "restore-document"}),
        }.With(Grow(), Padding(18.0F), Spacing(12.0F), MainAlign(MainAxisAlignment::Center),
               CrossAlign(CrossAxisAlignment::Center)).Key(tab->id));
      } else {
        pages.push_back(DocumentPage(actions, tab, is_active).Key(tab->id));
      }
    }
    content = IndexedPages(std::move(pages), index).With(Grow());
  }
  auto status = actions.workspace->status.Get();
  if (actions.folio->deleting_library_entries.Get()) {
    status = UseString(l10n::library_deleting);
  } else if (actions.workspace->picking.Get()) {
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
  if (status.empty() && actions.folio->store->WriteFailed()) {
    status = UseString(l10n::workspace_write_failed);
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

enum class SidebarSection { Workspace, External, Recent, EmptyWorkspace, NoRecent };
using SidebarItem = std::variant<SidebarSection, Entry, Recent, LibraryEntry>;

StringResource SidebarSectionLabel(SidebarSection section) {
  switch (section) {
  case SidebarSection::Workspace: return l10n::workspace;
  case SidebarSection::External: return l10n::external_locations;
  case SidebarSection::Recent: return l10n::recent_opened;
  case SidebarSection::EmptyWorkspace: return l10n::no_files_or_folders;
  case SidebarSection::NoRecent: return l10n::no_recent_documents;
  }
  return l10n::files;
}

bool IsListed(const Entry& entry, const Workspace& workspace) {
  const auto parent = entry->parent.lock();
  const auto& siblings = parent ? parent->children.Get() : workspace.folders.Get();
  return std::ranges::find(siblings, entry) != siblings.end() && (parent ? IsListed(parent, workspace) : entry->mounted.Get());
}

Entry FolderFileRow(const Tab& tab, const Workspace& workspace) {
  const auto& saved = tab->saved.Get();
  const auto& folder = tab->source_folder.Get();
  if (!saved || !folder || !IsListed(folder, workspace)) {
    return {};
  }
  const auto local = saved->target->reference.AsFile();
  const auto& children = folder->children.Get();
  const auto found = std::ranges::find_if(children, [&](const Entry& entry) {
    return entry->file && (SameFile(entry->file, saved->target) || (local && entry->file->reference.AsFile() == local));
  });
  return found == children.end() ? Entry{} : *found;
}

std::vector<MenuEntry> FolderMenu(WorkbenchActions actions, const Entry& folder) {
  const bool available = actions.Available();
  const bool can_create = available && (folder->directory->CanWrite() || actions.picker->CanOpenDirectories(true));
  std::vector<MenuEntry> menu{
      MenuItem(l10n::new_file_menu, [actions, folder] { actions.RequestNewChild(folder, ChildKind::File); }).Enabled(can_create),
      MenuItem(l10n::new_folder_menu, [actions, folder] { actions.RequestNewChild(folder, ChildKind::Folder); })
          .Enabled(can_create),
      MenuItem(l10n::refresh, [actions, folder] { actions.SetExpanded(folder, true); })
          .Enabled(available && folder->load.Get() != FolderLoad::Loading),
  };
  if (folder->parent.expired()) {
    menu.emplace_back(MenuSection{});
    menu.emplace_back(MenuItem(l10n::remove_external_folder, [actions, folder] { actions.RemoveExternalFolder(folder); }));
  }
  return menu;
}

[[huxerui::composable]]
View SidebarActionRow(View label, std::string name, bool desktop, float row_height, bool enabled,
                      std::function<std::vector<MenuEntry>()> entries, bool root = false) {
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto open_menu = [enabled, entries, menu](Point point) {
    if (enabled) {
      menu.ShowAt(point, entries());
    }
  };
  auto row = Row {
    View(label).With(Grow()),
    desktop && !root ? View{} : IconButton(root ? app::images::plus : app::images::more,
                                             StringVariant::Format(l10n::actions_for, name))
        .OnClick([menu, anchor, entries] { menu.Show(anchor, entries()); })
        .With(anchor, Frame{.width = row_height, .height = row_height}, Enabled(enabled)),
  }.With(Frame{.height = row_height}, CrossAlign(CrossAxisAlignment::Center))
      .On<ViewEvents::ContextMenuRequested>(open_menu);
  if (!desktop) {
    return std::move(row).With(LongPressGesture{}).On<LongPressEvents::Ended>([open_menu](const LongPressEvent& event) {
      open_menu(event.window_position);
    });
  }
  return row;
}

[[huxerui::composable]]
View FolderTreeRow(WorkbenchActions actions, Entry entry, bool desktop, float row_height) {
  const auto& colors = UseTheme().colors;
  const bool expanded = entry->expanded.Get();
  std::string label = entry->name;
  if (expanded && entry->load.Get() == FolderLoad::Loading) {
    label += " — loading…";
  } else if (expanded && entry->load.Get() == FolderLoad::Failed) {
    label += " — couldn’t load";
  }
  auto content = SidebarLabel(expanded ? app::images::folder_open : app::images::folder, std::move(label), colors.on_surface);
  if (desktop) {
    content = std::move(content).With(Tooltip(entry->error->empty() ? entry->name : entry->error.Get()));
  }
  return SidebarActionRow(std::move(content), entry->name, desktop, row_height, actions.Available(),
                          [actions, entry] { return FolderMenu(actions, entry); });
}

std::vector<MenuEntry> LibraryMenu(WorkbenchActions actions, const LibraryEntry& entry) {
  std::vector<MenuEntry> menu;
  if (entry.folder) {
    menu.emplace_back(MenuItem(l10n::new_document, [actions, id = entry.id] { actions.NewDocument(false, id); }));
    menu.emplace_back(MenuItem(l10n::new_folder_menu, [actions, id = entry.id] { actions.NewLibraryFolder(id); }));
    menu.emplace_back(MenuItem(l10n::library_import_here, [actions, id = entry.id] {
      actions.PickOpen({.kind = OpenKind::Import, .import_parent = id});
    }).Enabled(actions.picker->CanOpenFiles()));
  }
  if (!entry.id.empty()) {
    if (entry.folder) {
      menu.emplace_back(MenuSection{});
    }
    menu.emplace_back(MenuItem(l10n::library_rename_menu, [actions, id = entry.id] { actions.RenameLibraryEntry(id); }));
    const auto& library = actions.folio->library;
    const bool can_move = !entry.parent.empty() || std::ranges::any_of(library->Entries(), [&](const LibraryEntry& folder) {
      return folder.folder && library->CanPlace(entry.id, folder.id);
    });
    menu.emplace_back(MenuItem(l10n::library_move_menu, [actions, id = entry.id] {
      actions.ChooseLibraryDestination(id);
    }).Enabled(can_move));
    menu.emplace_back(MenuSection{});
    menu.emplace_back(MenuItem(l10n::library_delete_menu, [actions, id = entry.id] {
      actions.ConfirmLibraryDelete(id);
    }));
  }
  return menu;
}

struct LibraryTransfer {
  std::string id;
};

[[huxerui::composable]]
View LibraryTreeRow(WorkbenchActions actions, LibraryEntry entry, bool desktop, float row_height, bool selected) {
  const auto& colors = UseTheme().colors;
  const auto drop_target = UseState(false);
  const bool expanded = entry.id.empty() ? actions.workspace->files_open.Get()
      : std::ranges::find(actions.workspace->library_expanded.Get(), entry.id) != actions.workspace->library_expanded->end();
  const auto& icon = entry.folder ? (expanded ? app::images::folder_open : app::images::folder) : app::images::file_text;
  auto label = SidebarLabel(icon, entry.name, selected ? colors.on_primary_container : colors.on_surface);
  if (desktop) {
    label = std::move(label).With(Tooltip(entry.name));
  }
  auto row = SidebarActionRow(std::move(label), entry.name, desktop, row_height, actions.Available(),
      [actions, entry] { return LibraryMenu(actions, entry); }, entry.id.empty())
      .With(Background(drop_target.Get() ? colors.primary_container : Color::Transparent()));
  if (!desktop) {
    return row;
  }
  if (!entry.id.empty()) {
    row = std::move(row).With(DragSource(LibraryTransfer{entry.id}));
  }
  if (entry.folder) {
    row = std::move(row).With(DropTarget::Accepts<LibraryTransfer>([actions, entry](const LibraryTransfer& item) {
      const auto& library = actions.folio->library;
      const auto source = library->Find(item.id);
      return actions.Available() && source && source->parent != entry.id && library->CanPlace(item.id, entry.id);
    })).On<DropEvents<LibraryTransfer>::Entered>([drop_target](const LibraryTransfer&, const DropEvent&) {
      drop_target = true;
    }).On<DropEvents<LibraryTransfer>::Exited>([drop_target](const LibraryTransfer&, const DropEvent&) {
      drop_target = false;
    }).On<DropEvents<LibraryTransfer>::Dropped>([actions, entry, drop_target](const LibraryTransfer& item, const DropEvent&) {
      drop_target = false;
      actions.MoveLibraryEntry(item.id, entry.id);
    });
  }
  return row;
}

std::vector<SidebarItem> LibraryChildren(const std::shared_ptr<DocumentLibrary>& library, std::string_view parent) {
  if (!library) {
    return {};
  }
  std::vector<LibraryEntry> children;
  for (const auto& entry : library->Entries()) {
    if (entry.parent == parent) {
      children.push_back(entry);
    }
  }
  std::ranges::sort(children, [](const LibraryEntry& first, const LibraryEntry& second) {
    return first.folder != second.folder ? first.folder : first.name < second.name;
  });
  return {children.begin(), children.end()};
}

[[huxerui::composable]]
View RecentTreeRow(WorkbenchActions actions, Recent item, bool desktop, float row_height) {
  const auto& colors = UseTheme().colors;
  const auto name = item->Name(actions.folio->library.get());
  const auto& icon = item->record.kind == RecentKind::File ? app::images::file_text : app::images::folder;
  auto label = SidebarLabel(icon, name, colors.on_surface_variant);
  if (desktop) {
    label = std::move(label).With(Tooltip(item->record.location.path.empty() ? name : item->record.location.path));
  }
  return SidebarActionRow(std::move(label), name, desktop, row_height, actions.Available(), [actions, item] {
    const auto& record = item->record;
    std::vector<MenuEntry> menu;
    if (record.library_document.empty()) {
      const auto kind = RecentOpenKind(record.kind);
      const bool can_locate = kind == OpenKind::File ? actions.picker->CanOpenFiles() : actions.picker->CanOpenDirectories();
      menu.emplace_back(MenuItem(l10n::workspace_relocate, [actions, kind, id = record.id] {
        actions.PickOpen({.kind = kind, .recent_id = id});
      }).Enabled(can_locate));
    }
    menu.emplace_back(MenuItem(l10n::remove_recent, [actions, id = record.id] { actions.folio->RemoveRecent(id); }));
    return menu;
  });
}

[[huxerui::composable]]
View FilesSidebar(WorkbenchActions actions, bool desktop, float row_height) {
  const auto& colors = UseTheme().colors;
  const auto workspace = actions.workspace;
  const auto selected = workspace->Selected();
  const auto selected_entry = selected ? FolderFileRow(selected, *workspace) : Entry{};
  const auto saved = selected ? selected->saved.Get() : nullptr;
  const auto library_selection = saved ? saved->target->library_id : std::string{};
  const auto library = actions.folio->library;
  const auto workspace_name = UseString(l10n::workspace);
  std::vector<SidebarItem> roots{SidebarSection::Workspace};
  if (std::ranges::any_of(workspace->folders.Get(), [](const Entry& folder) { return folder->mounted.Get(); })) {
    roots.emplace_back(SidebarSection::External);
  }
  roots.emplace_back(SidebarSection::Recent);
  const auto open = [actions](const SidebarItem& item) {
    if (const auto* entry = std::get_if<Entry>(&item); entry && (*entry)->file) {
      actions.OpenKnown((*entry)->file, (*entry)->parent.lock());
    } else if (const auto* recent = std::get_if<Recent>(&item)) {
      actions.OpenRecent(*recent);
    } else if (const auto* entry = std::get_if<LibraryEntry>(&item); entry && !entry->folder) {
      actions.OpenLibraryDocument(entry->id);
    }
  };
  return TreeView<SidebarItem>(std::move(roots),
      [actions, colors, desktop, row_height, selected_entry, library_selection, workspace_name, library](const SidebarItem& item) -> View {
        if (const auto* entry = std::get_if<LibraryEntry>(&item)) {
          return LibraryTreeRow(actions, *entry, desktop, row_height, !entry->folder && entry->id == library_selection)
              .Key("library-" + entry->id);
        }
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& entry = *node;
          if (entry->directory) {
            return FolderTreeRow(actions, entry, desktop, row_height).Key(entry->id);
          }
          return SidebarLabel(app::images::file_text, entry->name,
                              entry == selected_entry ? colors.on_primary_container : colors.on_surface)
              .With(Tooltip(LocationLabel(*entry->file))).Key(entry->id);
        }
        if (const auto* recent = std::get_if<Recent>(&item)) {
          return RecentTreeRow(actions, *recent, desktop, row_height).Key((*recent)->record.id);
        }
        const auto section = std::get<SidebarSection>(item);
        if (section == SidebarSection::Workspace && library) {
          return LibraryTreeRow(actions, {.name = workspace_name, .folder = true}, desktop, row_height, false)
              .Key("workspace-root");
        }
        const bool heading = section == SidebarSection::Workspace || section == SidebarSection::External ||
            section == SidebarSection::Recent;
        return Text(SidebarSectionLabel(section), TextRole::Label)
            .With(FontSize(heading ? 11.0F : 12.0F), Foreground(colors.on_surface_variant))
            .Key("sidebar-section-" + std::to_string(static_cast<int>(section)));
      }, [workspace, selected_entry, library_selection, library](const SidebarItem& item) {
        if (const auto* entry = std::get_if<LibraryEntry>(&item)) {
          const auto& expanded = workspace->library_expanded.Get();
          return TreeItemInfo{.label = entry->name, .expandable = entry->folder,
                              .expanded = std::ranges::find(expanded, entry->id) != expanded.end(),
                              .selected = entry->folder ? std::nullopt : std::optional(entry->id == library_selection)};
        }
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& entry = *node;
          return TreeItemInfo{.label = entry->name, .expandable = entry->directory.has_value(),
                              .expanded = entry->expanded.Get(),
                              .selected = entry->file ? std::optional(entry == selected_entry) : std::nullopt};
        }
        if (const auto* recent = std::get_if<Recent>(&item)) {
          return TreeItemInfo{.label = (*recent)->Name(library.get()), .selected = false};
        }
        const auto section = std::get<SidebarSection>(item);
        const auto label = SidebarSectionLabel(section);
        switch (section) {
        case SidebarSection::Workspace:
          return TreeItemInfo{.label = label, .expandable = true, .expanded = workspace->files_open.Get()};
        case SidebarSection::External:
          return TreeItemInfo{.label = label, .expandable = true, .expanded = workspace->external_open.Get()};
        case SidebarSection::Recent:
          return TreeItemInfo{.label = label, .expandable = true, .expanded = workspace->recents_open.Get()};
        default:
          return TreeItemInfo{.label = label, .enabled = false};
        }
      }).Children([actions, workspace, library](const SidebarItem& item) -> std::vector<SidebarItem> {
        if (const auto* parent = std::get_if<LibraryEntry>(&item)) {
          return LibraryChildren(library, parent->id);
        }
        if (const auto* node = std::get_if<Entry>(&item)) {
          const auto& children = (*node)->children.Get();
          return {children.begin(), children.end()};
        }
        if (const auto* section = std::get_if<SidebarSection>(&item)) {
          switch (*section) {
          case SidebarSection::Workspace: {
            auto children = LibraryChildren(library, {});
            return children.empty() ? std::vector<SidebarItem>{SidebarSection::EmptyWorkspace} : std::move(children);
          }
          case SidebarSection::External: {
            std::vector<SidebarItem> children;
            for (const auto& folder : workspace->folders.Get()) {
              if (folder->mounted.Get()) {
                children.emplace_back(folder);
              }
            }
            return children;
          }
          case SidebarSection::Recent: {
            const auto& recent = actions.folio->recent.Get();
            return recent.empty() ? std::vector<SidebarItem>{SidebarSection::NoRecent}
                                  : std::vector<SidebarItem>{recent.begin(), recent.end()};
          }
          default:
            break;
          }
        }
        return {};
      }).OnExpandedChanged([actions, workspace](const SidebarItem& item, bool expanded) {
        if (const auto* entry = std::get_if<LibraryEntry>(&item)) {
          actions.SetLibraryExpanded(entry->id, expanded);
        } else if (const auto* entry = std::get_if<Entry>(&item)) {
          actions.SetExpanded(*entry, expanded);
        } else if (const auto* section = std::get_if<SidebarSection>(&item)) {
          if (*section == SidebarSection::External) {
            workspace->external_open = expanded;
            return;
          }
          if (*section == SidebarSection::Workspace) {
            workspace->files_open = expanded;
          } else {
            workspace->recents_open = expanded;
          }
          actions.folio->Persist(*workspace);
        }
      }).OnSelectionChanged([open](const SidebarItem& item, bool selected) {
        if (selected) {
          open(item);
        }
      }).OnActivated(open)
      .DisclosureIcon(app::images::chevron_right).ItemExtent(row_height).Label(l10n::files)
      .With(Grow(), ScrollBar());
}

struct OutlineHeading : DocumentHeading {
  std::size_t depth;
};

struct Outline {
  std::vector<OutlineHeading> headings;
  std::optional<std::size_t> active;
};

std::vector<OutlineHeading> DocumentOutline(const RichDocument& document) {
  std::vector<OutlineHeading> headings;
  std::array<unsigned, 6> ancestors{};
  std::size_t depth = 0;
  const auto blocks = document.Blocks();
  for (auto& heading : DocumentHeadings(document)) {
    if (&heading.block.Data() != &blocks[heading.root_index].Data()) {
      continue;
    }
    const auto level = heading.block.Data().heading_level;
    // Skipped levels still nest one row beneath the closest preceding ancestor.
    while (depth > 0 && ancestors[depth - 1] >= level) {
      --depth;
    }
    headings.push_back({std::move(heading), depth});
    ancestors[depth++] = level;
  }
  return headings;
}

View OutlineSectionIcon(Color color) {
  return Canvas([color](PaintContext& paint, Size) {
    paint.DrawRect({2.0F, 3.0F, 12.0F, 2.5F}, color, CornerRadii{1.25F});
    paint.DrawRect({2.0F, 7.5F, 9.0F, 1.5F}, color, CornerRadii{0.75F});
    paint.DrawRect({2.0F, 11.0F, 7.0F, 1.5F}, color, CornerRadii{0.75F});
  }).With(Frame{.width = 16.0F, .height = 16.0F}, Semantics{.hidden = true});
}

View OutlineGuides(std::size_t depth, std::size_t next_depth, float row_height, Color guide, Color branch) {
  constexpr float indent = 11.0F;
  return Canvas([depth, next_depth, guide, branch](PaintContext& paint, Size size) {
    const float middle = size.height / 2.0F;
    for (std::size_t level = 0; level < depth; ++level) {
      const float x = 14.0F + indent * static_cast<float>(level);
      const float bottom = level < next_depth ? size.height : middle + 0.5F;
      paint.DrawRect({x, 0.0F, 1.0F, bottom}, guide);
      if (level + 1 == depth) {
        paint.DrawRect({x, middle - 0.5F, size.width - x - 2.0F, 1.0F}, branch);
      }
    }
  }).With(Frame{.width = 23.0F + indent * static_cast<float>(depth), .height = row_height}, Semantics{.hidden = true});
}

[[huxerui::composable]]
View OutlineList(WorkbenchActions actions, Tab tab, bool desktop, float row_height,
                 std::shared_ptr<Outline> outline) {
  const auto theme = UseTheme();
  const auto scroll = UseScrollController();
  const auto menu = UseMenu();
  const auto toast = UseToast();
  const auto clipboard = UseApplication().Clipboard();
  const auto& editor = tab->editor.Get();
  constexpr float reading_line = 32.0F;
  const auto location = editor ? editor->ViewportLocation(reading_line) : std::nullopt;
  if (location) {
    const auto& headings = outline->headings;
    const auto root = location->revealed_root_block_index.value_or(
        location->at_end ? headings.back().root_index : location->root_block_index);
    const auto next = std::ranges::upper_bound(headings, root, {}, &OutlineHeading::root_index);
    outline->active = next == headings.begin() ? std::nullopt
        : std::optional(static_cast<std::size_t>(next - headings.begin() - 1));
  }
  // Keep the last valid highlight while IME preedit owns the layout of this snapshot.
  const auto active = outline->active;
  const bool visible = !actions.modal_sidebar || actions.sidebar_open.Get();
  Lifecycle([scroll, active, row_height, visible] {
    if (!active || !visible) {
      return;
    }
    const auto metrics = scroll.Metrics();
    const float top = row_height * static_cast<float>(*active);
    if (top < metrics.offset) {
      scroll.ScrollToItem(*active, ScrollAlignment::Start);
    } else if (top + row_height > metrics.offset + metrics.viewport_extent) {
      scroll.ScrollToItem(*active, ScrollAlignment::End);
    }
  }, active, row_height, visible);

  return VirtualList(outline->headings.size(),
      [actions, tab, desktop, row_height, theme, outline, active, menu, toast, clipboard](std::size_t row) {
    const auto& headings = outline->headings;
    const auto& heading = headings[row];
    const bool selected = active == row;
    const bool root = heading.depth == 0;
    const auto& colors = theme.colors;
    const auto foreground = selected ? colors.on_primary_container : colors.on_surface;
    auto font = Font::FromFamily(theme.typography.family, theme.typography.label_large);
    if (root || selected) {
      font = font.WithWeight(FontWeight::SemiBold);
    }
    auto label = EllipsizedLabel(heading.title, foreground, {}, font).With(Grow());
    if (desktop) {
      label = std::move(label).With(Tooltip(heading.title));
    }
    const auto next_depth = row + 1 < headings.size() ? headings[row + 1].depth : 0;
    const auto id = "outline-heading-" + std::to_string(heading.root_index);
    auto item = ClickableRow(Row {
      root ? View{} : OutlineGuides(heading.depth, next_depth, row_height, colors.outline,
                                    selected ? colors.primary : colors.outline),
      Row {
        root ? OutlineSectionIcon(selected ? colors.primary : colors.on_surface_variant) : View{},
        std::move(label),
      }.With(Grow(), Frame{.height = row_height - 6.0F}, Padding(EdgeInsets::Symmetric(6.0F, 0.0F)), Spacing(7.0F),
             CrossAlign(CrossAxisAlignment::Center), CornerRadius(theme.shapes.small),
             Background(selected ? colors.primary_container : Color::Transparent())),
    }.With(Frame{.height = row_height}, Padding(EdgeInsets::Symmetric(8.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center)),
        theme, {.role = SemanticRole::Button, .label = heading.title, .identifier = id,
                .selected = selected, .heading_level = heading.block.Data().heading_level},
        [actions, tab, block = heading.block, length = heading.length] {
          actions.JumpToHeading(tab, block, length);
        });
    if (!heading.anchor.empty() && clipboard && clipboard->SupportsSynchronousText()) {
      const auto show_menu = [menu, clipboard, toast, link = "#" + heading.anchor](Point point) {
        menu.ShowAt(point, {MenuItem(l10n::copy_heading_link, [clipboard, toast, link] {
          toast.Show(clipboard->WriteText(link) ? l10n::link_copied : l10n::link_copy_failed);
        })});
      };
      item = std::move(item).On<ViewEvents::ContextMenuRequested>(show_menu);
      if (!desktop) {
        item = std::move(item).With(LongPressGesture{}).On<LongPressEvents::Ended>(
            [show_menu](const LongPressEvent& event) { show_menu(event.window_position); });
      }
    }
    return std::move(item).Key(reinterpret_cast<std::uintptr_t>(&heading.block.Data()));
  }).ItemExtent(row_height).Controller(scroll).With(Grow(), ScrollBar(), Semantics{.label = l10n::outline});
}

[[huxerui::composable]]
View OutlineSidebar(WorkbenchActions actions, Tab tab, bool desktop, float row_height) {
  auto headings = tab ? DocumentOutline(tab->Snapshot()) : std::vector<OutlineHeading>{};
  if (headings.empty()) {
    const auto color = UseTheme().colors.on_surface_variant;
    return Column {
      Row {OutlineSectionIcon(color), Text(l10n::no_headings, TextRole::Label).With(Foreground(color))}
          .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(Grow(), Padding(12.0F));
  }
  return OutlineList(actions, tab, desktop, row_height,
                     std::make_shared<Outline>(Outline{std::move(headings), std::nullopt})).Key(tab->id);
}

[[huxerui::composable]]
View Sidebar(WorkbenchActions actions, bool desktop) {
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;
  const float row_height = desktop ? 32.0F : 44.0F;
  const auto workspace = actions.workspace;
  const bool outline = workspace->showing_outline.Get();
  auto tabs_style = UseEnvironment<TabsStyle>();
  tabs_style.background = Color::Transparent();
  tabs_style.label_style = TextStyle{
      Font::FromFamily(theme.typography.family, desktop ? theme.typography.body_small : theme.typography.label_large)
          .WithWeight(FontWeight::Medium), colors.on_surface_variant};
  tabs_style.selected_label = colors.primary;
  tabs_style.indicator = colors.primary;
  tabs_style.indicator_sizing = TabIndicatorSizing::Content;
  tabs_style.divider_height = 0.0F;
  tabs_style.item_padding = EdgeInsets::Symmetric(4.0F, 3.0F);
  tabs_style.minimum_height = row_height;
  tabs_style.expand_items = true;
  const auto section_switch = Tabs({l10n::files, l10n::outline}, outline ? 1U : 0U)
      .OnChanged([actions](std::size_t index) {
        actions.workspace->showing_outline = index == 1;
        actions.folio->Persist(*actions.workspace);
      }).With(Grow());
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
      ProvideEnvironment(tabs_style, section_switch),
      IconButton(app::images::panel_left, l10n::hide_sidebar)
          .OnClick([actions] { actions.Execute(FolioCommand::ToggleSidebar); })
          .With(Frame{.min_width = row_height, .min_height = row_height}, Tooltip(l10n::hide_sidebar),
                Enabled(actions.CanExecute(FolioCommand::ToggleSidebar)),
                Semantics{.identifier = "hide-sidebar", .expanded = true}),
    }.With(Padding(EdgeInsets::Symmetric(8.0F, 2.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    outline ? OutlineSidebar(actions, workspace->Selected(), desktop, row_height)
            : FilesSidebar(actions, desktop, row_height),
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
  const bool document_actions = available && active && !active->unresolved.Get() && !active->Busy();
  const auto original = active ? active->saved.Get() : nullptr;
  const bool managed = original && original->target->library;
  const bool can_save_to_library = actions.folio->library && document_actions && original && !managed;
  const auto viewport = UseViewportClass();
  const bool narrow_desktop = desktop && viewport == ViewportClass::Compact;
  const bool compact_mobile = !desktop && viewport == ViewportClass::Compact;
  const float control_size = desktop ? 32.0F : 44.0F;
  const auto icon_frame = Frame{.min_width = control_size, .min_height = control_size};
  const auto& colors = theme.colors;
  const bool read_only = active && active->IsReadOnly();
  const auto mode = read_only ? l10n::reading_mode : l10n::editing_mode;
  const auto switch_mode = read_only ? l10n::switch_to_editing : l10n::switch_to_reading;
  View mode_button;
  if (active && !active->unresolved.Get()) {
    const float radius = control_size / 2.0F;
    auto indication = theme.interactions.indication;
    indication.geometry = {.layer_size = Size{control_size, control_size}, .clip_corner_radii = CornerRadii{radius}};
    mode_button = IconButton(read_only ? app::images::file_text : app::images::file_pen, switch_mode)
        .OnClick([actions, active] { actions.ToggleReadingMode(active); })
        .With(Frame{.width = control_size, .height = control_size}, CornerRadius(radius), indication,
              Enabled(document_actions && active->editor.Get().has_value()), Tooltip(switch_mode),
              Foreground(read_only ? colors.on_primary_container : colors.on_surface_variant),
              Background(read_only ? colors.primary_container : Color::Transparent()),
              Semantics{.state_description = mode, .identifier = "toggle-reading-mode"})
        .On<ViewEvents::PointerIntercept>([active](const PointerEvent& event) {
          // A mouse press can move focus before Click, which would otherwise commit the preedit.
          if (event.type == PointerEventType::Down && event.changed_button == PointerButton::Primary) {
            if (const auto editor = active->editor.Get(); editor && editor->IsComposing()) {
              editor->Execute(Undo{});
            }
          }
          return false;
        })
        .Key("reading-mode");
  }
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
  const auto menu = [actions, file_menu, active, available, document_actions, managed, can_save_to_library,
                     desktop, narrow_desktop] {
    const auto item = [&actions](StringVariant label, FolioCommand command) {
      return MenuItem(label, [actions, command] { actions.Execute(command); }).Enabled(actions.CanExecute(command));
    };
    std::vector<MenuEntry> entries{
        item(l10n::new_document, FolioCommand::NewDocument),
        item(l10n::open_file_menu, FolioCommand::OpenFile),
    };
    if (actions.folio->library) {
      entries.emplace_back(MenuItem(l10n::library_import_menu, [actions] { actions.PickOpen(OpenKind::Import); })
          .Enabled(available && actions.picker->CanOpenFiles()));
    }
    entries.insert(entries.end(), {
        MenuItem(l10n::open_folder_menu, [actions] { actions.PickOpen(OpenKind::Folder); })
            .Enabled(available && actions.picker->CanOpenDirectories()),
        MenuItem(l10n::open_package, [actions] { actions.PickOpen(OpenKind::Package); })
            .Enabled(available && actions.picker->CanOpenDirectories()),
        MenuSection{},
        item(l10n::save, FolioCommand::Save),
    });
    if (!IsMobilePlatform() && !managed) {
      entries.emplace_back(item(l10n::save_as_menu, FolioCommand::SaveAs));
    }
    if (can_save_to_library) {
      entries.emplace_back(MenuItem(l10n::library_save_copy, [actions, active] { actions.RequestSave(active, WriteKind::Library); }));
    }
    entries.insert(entries.end(), {
        MenuItem(l10n::export_copy_menu, [actions, active] { actions.RequestSave(active, WriteKind::Export); })
            .Enabled(document_actions && (actions.picker->CanSaveFiles() || actions.picker->CanOpenDirectories(true))),
        MenuItem(l10n::locate_attachment_folder, [actions, active] { actions.LocateAttachments(active); })
            .Enabled(document_actions && actions.picker->CanOpenDirectories()),
        MenuSection{},
        MenuItem(l10n::load_sample, [actions] { actions.NewDocument(true); }).Enabled(available),
    });
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
  const bool sidebar_visible = actions.sidebar_open.Get();
  const auto sidebar_label = sidebar_visible ? l10n::hide_sidebar : l10n::show_sidebar;
  std::vector<View> controls{
    actions.modal_sidebar || !sidebar_visible ? IconButton(app::images::panel_left, sidebar_label)
        .OnClick([actions] { actions.Execute(FolioCommand::ToggleSidebar); })
        .With(icon_frame, Tooltip(sidebar_label), Enabled(actions.CanExecute(FolioCommand::ToggleSidebar)),
              Semantics{.identifier = "show-sidebar", .expanded = sidebar_visible})
        : View{},
    std::move(title).Key("title"),
    std::move(mode_button),
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
  InstallFolioWithWorkspace(context, nullptr);
}

void InstallFolioWithWorkspace(ApplicationContext& context, std::shared_ptr<WorkspaceStore> store,
                               std::optional<File> data_directory) {
  RegisterBrowser(context);
  const auto folio = std::make_shared<FolioState>();
  context.Provide(folio);
  const auto application = UseApplication();
  if (const auto& startup = application.StartupActivation()) {
    folio->QueueActivation(*startup);
  }
  context.OnActivation([folio](ApplicationActivation activation) { folio->QueueActivation(activation); });
  try {
    if (!data_directory) {
      data_directory = application.Directories().data_directory;
    }
    folio->temporary_directory = application.Directories().temporary_directory;
  } catch (const std::logic_error&) {
    // Settings remain available for the session on hosts without application storage.
  }
  if (data_directory) {
    folio->library = std::make_shared<DocumentLibrary>(data_directory->Child("library"));
  }
  folio->store = store ? std::move(store) : std::make_shared<WorkspaceStore>(data_directory);
  if (const auto& library = folio->library; library && library->HasSavedCatalog()) {
    auto record = folio->store->Get();
    PruneLibraryReferences(record, [&](std::string_view id) { return !library->Find(id); });
    folio->store->Set(std::move(record));
    UseApplicationTaskScope().Launch([library]() -> Task<void> { (void)co_await library->CollectDeletedPackages(); });
  }
  const auto& saved = folio->store->Get();
  std::vector<Recent> recent;
  for (const auto& item : saved.recent) {
    recent.push_back(std::make_shared<RecentItem>(RecentItem{item, {}, {}}));
  }
  folio->recent = std::move(recent);
  if (!saved.windows.empty()) {
    const auto active = std::ranges::find(saved.windows, saved.active_window, &WorkspaceWindowRecord::id);
    const auto& initial = active == saved.windows.end() ? saved.windows.front() : *active;
    folio->installing = initial;
    for (const auto& window : saved.windows) {
      if (window.id != initial.id && application.SupportsMultipleWindows()) {
        folio->restore_windows.push_back(window);
      }
      for (const auto& tab : window.tabs) {
        folio->next_document = std::max(folio->next_document, tab.id);
      }
    }
    folio->restoring = 1 + folio->restore_windows.size();
  }
  context.Provide(std::make_shared<SettingsStore>(std::move(data_directory)));
}

void InstallWorkbench(WindowContext& context) {
  InstallBrowser(context);
  const auto folio = UseService<FolioState>();
  auto workspace = std::make_shared<Workspace>();
  if (auto record = std::exchange(folio->installing, std::nullopt)) {
    workspace->id = record->id;
    workspace->restoring = true;
    workspace->files_open = record->files_open;
    workspace->recents_open = record->recents_open;
    workspace->library_expanded = std::move(record->library_expanded);
    workspace->library_expanded.Update([](auto& ids) { std::erase(ids, std::string{}); });
    workspace->sidebar_visible = record->sidebar_visible;
    workspace->showing_outline = record->showing_outline;
    workspace->pane_sizes.Update([&](PaneSizes& sizes) {
      sizes.sidebar.width = std::clamp(record->sidebar_width, *sizes.sidebar.min_width, *sizes.sidebar.max_width);
    });
    std::vector<Entry> folders;
    for (auto& root : record->folders) {
      if (std::ranges::none_of(record->tabs, [&](const WorkspaceTabRecord& tab) { return tab.folder == root.id; })) {
        continue;
      }
      auto folder = std::make_shared<FolderEntry>(FolderEntry{root.id, root.name, {}, {}, {}});
      workspace->next_entry = std::max(workspace->next_entry, root.id);
      folder->mounted = false;
      folder->recent_id = root.recent_id;
      folder->unresolved = std::move(root);
      folders.push_back(std::move(folder));
    }
    workspace->folders = std::move(folders);
    std::vector<Tab> tabs;
    for (auto& item : record->tabs) {
      auto tab = std::make_shared<DocumentTab>(item.id, item.name, NewWorkingDirectory(UseApplication()),
                                               RichDocument{}, nullptr, std::nullopt);
      tab->reading_position = item.reading_position;
      tab->restore_reading_position = item.reading_position.has_value();
      tab->initial_read_only = item.read_only;
      tab->unresolved = std::move(item);
      tabs.push_back(std::move(tab));
    }
    if (!tabs.empty()) {
      const auto selected = std::ranges::find_if(tabs, [&](const Tab& tab) { return tab->id == record->selected; });
      workspace->selected = selected == tabs.end() ? tabs.front()->id : (*selected)->id;
    }
    workspace->tabs = std::move(tabs);
  }
  context.Provide(std::move(workspace));
}

WindowOptions FolioWindowOptions() {
  return {.title = DRAFTSPAN_APP_NAME, .initial_size = {920.0F, 720.0F}, .minimum_size = Size{360.0F, 360.0F},
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
      UseTaskScope(), application_tasks, UseDialog(), UseViewHandle(),
      UseViewHandle(), sidebar_open, modal_sidebar,
  };
  Lifecycle([folio, workspace, window, application_tasks] {
    workspace->window = window;
    folio->windows.push_back(workspace);
    folio->window_revision = folio->window_revision.Get() + 1;
    if (!workspace->restoring.Get()) {
      folio->Persist(*workspace);
    }
    return [folio, workspace, application_tasks] {
      folio->FlushReadingPositions(*workspace);
      if (workspace->restoring.Get()) {
        workspace->restoring = false;
        folio->restoring = folio->restoring.Get() - 1;
      }
      workspace->window.reset();
      std::erase_if(folio->windows, [workspace](const auto& weak) {
        const auto owner = weak.lock();
        return !owner || owner == workspace;
      });
      folio->window_revision = folio->window_revision.Get() + 1;
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
  Lifecycle([actions] {
    actions.tasks.Post([actions] {
      actions.RestoreWindows();
      actions.RestoreWorkspace();
    });
  }, workspace);
  const auto window_state = window.LifecycleState();
  Lifecycle([folio, workspace, window_state] {
    if (window_state == WindowLifecycleState::Active && folio->restoring.Get() == 0) {
      folio->Persist(*workspace, true);
    } else if (window_state == WindowLifecycleState::Background) {
      folio->FlushReadingPositions(*workspace);
    }
  }, window_state);
  const bool can_open_activated = !folio->pending_files->empty() && folio->restoring.Get() == 0 && actions.Available();
  Lifecycle([actions, can_open_activated] {
    if (can_open_activated) {
      actions.tasks.Post([actions] { actions.OpenActivated(); });
    }
  }, can_open_activated, folio->window_revision.Get(), window_state);
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
    // The active find or replacement field owns its editing history.
    if (tab && tab->find->open && tab->find->focused != FindField::None &&
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
      }.OnChanged([actions, workspace, visible](const std::vector<SplitChange>& changes) {
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
          actions.folio->Persist(*workspace);
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
            .OnOpenChanged([actions, sidebar_open, modal_sidebar](bool open) {
              sidebar_open = open;
              if (!modal_sidebar) {
                actions.folio->Persist(*actions.workspace);
              }
            }))
      : DrawerLayout(content);
  return std::move(layout).With(SafeAreaPadding(), Background(theme.colors.background));
}

} // namespace folio

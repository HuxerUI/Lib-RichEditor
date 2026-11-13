#include "workspace_store.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

#include <huxerui/app.h>
#include <nlohmann/json.hpp>

using namespace huxerui;

namespace folio {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceLocation, path, writable)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceFolderRecord, id, name, location, recent_id)

void to_json(nlohmann::json& json, const std::optional<ReadingPosition>& position) {
  if (position) {
    json = {{"block_index", position->block_index}, {"text_fingerprint", position->text_fingerprint}};
  } else {
    json = nullptr;
  }
}

void from_json(const nlohmann::json& json, std::optional<ReadingPosition>& position) {
  position.reset();
  if (!json.is_object()) {
    return;
  }
  const auto index = json.find("block_index");
  const auto fingerprint = json.find("text_fingerprint");
  if (index != json.end() && index->is_number_unsigned() &&
      fingerprint != json.end() && fingerprint->is_number_unsigned()) {
    position = ReadingPosition{index->get<std::uint64_t>(), fingerprint->get<std::uint64_t>()};
  }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceTabRecord, id, name, location, attachments, folder, relative,
                                               package, library_document, reading_position, read_only)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceWindowRecord, id, folders, tabs, selected, sidebar_width,
                                               sidebar_visible, showing_outline, files_open, recents_open, library_expanded)
NLOHMANN_JSON_SERIALIZE_ENUM(RecentKind, {{RecentKind::File, "file"}, {RecentKind::Folder, "folder"},
                                        {RecentKind::Package, "package"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceRecentRecord, id, name, location, kind, library_document)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkspaceRecord, active_window, windows, recent)

namespace {

constexpr auto workspace_file = "workspace.json";
constexpr auto staged_file = "workspace.json.tmp";

bool ValidComponent(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find('/') == name.npos && name.find('\0') == name.npos;
}

bool ValidLocation(const WorkspaceLocation& location) {
  const auto& path = location.path;
  return path.empty() || (path.find('\0') == path.npos && std::filesystem::path(std::u8string_view(
      reinterpret_cast<const char8_t*>(path.data()), path.size())).is_absolute());
}

WorkspaceRecord Load(const std::optional<File>& directory) {
  if (!directory) {
    return {};
  }
  const auto text = directory->Child(workspace_file).ReadString();
  if (!text.Succeeded()) {
    return {};
  }
  try {
    const auto json = nlohmann::json::parse(text.Value());
    if (json.value("version", 0) != 1) {
      return {};
    }
    auto record = json.get<WorkspaceRecord>();
    std::set<std::string> windows;
    std::set<std::uint64_t> tabs;
    constexpr auto max_id = std::numeric_limits<std::uint64_t>::max();
    for (const auto& window : record.windows) {
      if (window.id.empty() || !windows.insert(window.id).second || !std::isfinite(window.sidebar_width)) {
        return {};
      }
      std::set<std::uint64_t> folders;
      for (const auto& folder : window.folders) {
        if (folder.id == 0 || folder.id == max_id || folder.name.empty() || !folders.insert(folder.id).second ||
            !ValidLocation(folder.location)) {
          return {};
        }
      }
      for (const auto& tab : window.tabs) {
        if (tab.id == 0 || tab.id == max_id || tab.name.empty() || !tabs.insert(tab.id).second ||
            !ValidLocation(tab.location) || !ValidLocation(tab.attachments) ||
            (tab.folder && (!folders.contains(tab.folder) || tab.relative.empty())) ||
            !std::ranges::all_of(tab.relative, ValidComponent)) {
          return {};
        }
      }
    }
    if (!record.windows.empty() && !windows.contains(record.active_window)) {
      record.active_window = record.windows.front().id;
    }
    if (!json.contains("recent")) {
      // Previously mounted folders move to history when mounts become temporary.
      for (auto& window : record.windows) {
        for (auto& folder : window.folders) {
          const auto existing = std::ranges::find_if(record.recent, [&](const WorkspaceRecentRecord& item) {
            return !folder.location.path.empty() && item.location.path == folder.location.path;
          });
          if (existing != record.recent.end()) {
            folder.recent_id = existing->id;
          } else {
            folder.recent_id = window.id + "-folder-" + std::to_string(folder.id);
            record.recent.push_back({folder.recent_id, folder.name, folder.location, RecentKind::Folder});
          }
        }
      }
    }
    std::set<std::string> recent_ids;
    std::erase_if(record.recent, [&](const WorkspaceRecentRecord& item) {
      return item.id.empty() || item.name.empty() || !ValidLocation(item.location) || !recent_ids.insert(item.id).second;
    });
    constexpr std::size_t recent_limit = 24;
    if (record.recent.size() > recent_limit) {
      record.recent.resize(recent_limit);
    }
    return record;
  } catch (const nlohmann::json::exception&) {
    return {};
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  }
}

std::string Encode(const WorkspaceRecord& record) {
  nlohmann::json json = record;
  json["version"] = 1;
  return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

#if defined(__EMSCRIPTEN__)
Task<void> WriteWorkspace(File directory, State<WorkspaceRecord> record, State<bool> writing, State<bool> failed) {
  const auto staged = directory.Child(staged_file);
  const auto destination = directory.Child(workspace_file);
  while (true) {
    const auto snapshot = record.Get();
    const bool written = (co_await directory.CreateDirectoriesAsync()) &&
        (co_await staged.WriteStringAsync(Encode(snapshot))) && (co_await staged.MoveToAsync(destination, true));
    if (!written) {
      (void)co_await staged.DeleteAsync();
    }
    failed = !written;
    if (snapshot == record.Get()) {
      writing = false;
      co_return;
    }
  }
}
#else
bool WriteWorkspace(const File& directory, const WorkspaceRecord& record) {
  const auto staged = directory.Child(staged_file);
  if (directory.CreateDirectories() && staged.WriteString(Encode(record)) &&
      staged.MoveTo(directory.Child(workspace_file), true)) {
    return true;
  }
  (void)staged.Delete();
  return false;
}
#endif

} // namespace

WorkspaceStore::WorkspaceStore(std::optional<File> directory) : directory_(std::move(directory)), record_(Load(directory_)) {}

void WorkspaceStore::Set(WorkspaceRecord record) {
  if (record == record_.Get() && !write_failed_.Get()) {
    return;
  }
  record_ = std::move(record);
  if (!directory_) {
    return;
  }
#if defined(__EMSCRIPTEN__)
  if (!writing_.Get()) {
    writing_ = true;
    UseApplicationTaskScope().Launch(WriteWorkspace(*directory_, record_, writing_, write_failed_));
  }
#else
  write_failed_ = !WriteWorkspace(*directory_, record_.Get());
#endif
}

} // namespace folio

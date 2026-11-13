#pragma once

#include <cstdint>

#include <huxerui/file.h>
#include <huxerui/state.h>

#include "reading_position.h"

namespace folio {

/// A local path with the access mode of the original selection. An empty path requires a picker grant.
struct WorkspaceLocation {
  std::string path;
  bool writable = false;
  bool operator==(const WorkspaceLocation&) const = default;
};

struct WorkspaceFolderRecord {
  std::uint64_t id = 0;
  std::string name;
  WorkspaceLocation location;
  std::string recent_id;
  bool operator==(const WorkspaceFolderRecord&) const = default;
};

struct WorkspaceTabRecord {
  std::uint64_t id = 0;
  std::string name;
  WorkspaceLocation location;
  WorkspaceLocation attachments;
  /// A mounted folder and the document's path components beneath it, including the filename.
  std::uint64_t folder = 0;
  std::vector<std::string> relative;
  /// Name of a standalone document package that must be selected again.
  std::string package;
  /// An app-owned document can be reopened by ID under the current application directory.
  std::string library_document;
  /// Last reading position in this tab, if captured in a committed layout.
  std::optional<ReadingPosition> reading_position;
  bool operator==(const WorkspaceTabRecord&) const = default;
};

struct WorkspaceWindowRecord {
  std::string id;
  /// Directory references kept for restoring open tabs that were opened from them.
  std::vector<WorkspaceFolderRecord> folders;
  std::vector<WorkspaceTabRecord> tabs;
  std::uint64_t selected = 0;
  float sidebar_width = 260.0F;
  bool sidebar_visible = true;
  bool showing_outline = false;
  bool files_open = true;
  bool recents_open = true;
  std::vector<std::string> library_expanded;
  bool operator==(const WorkspaceWindowRecord&) const = default;
};

enum class RecentKind { File, Folder, Package };

struct WorkspaceRecentRecord {
  std::string id;
  std::string name;
  WorkspaceLocation location;
  RecentKind kind = RecentKind::File;
  std::string library_document;
  bool operator==(const WorkspaceRecentRecord&) const = default;
};

struct WorkspaceRecord {
  std::string active_window;
  std::vector<WorkspaceWindowRecord> windows;
  std::vector<WorkspaceRecentRecord> recent;
  bool operator==(const WorkspaceRecord&) const = default;
};

/// Stores window and file locations in the application directory, independently of document contents.
class WorkspaceStore {
public:
  explicit WorkspaceStore(std::optional<huxerui::File> directory);

  const WorkspaceRecord& Get() const { return record_.Get(); }
  bool WriteFailed() const { return write_failed_.Get(); }
  void Set(WorkspaceRecord record);

private:
  std::optional<huxerui::File> directory_;
  huxerui::State<WorkspaceRecord> record_;
  huxerui::State<bool> write_failed_{false};
#if defined(__EMSCRIPTEN__)
  huxerui::State<bool> writing_{false};
#endif
};

} // namespace folio

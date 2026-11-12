#pragma once

#include <huxerui/file.h>
#include <huxerui/state.h>

namespace folio {

/// The empty parent ID identifies the root of the workspace document tree.
struct LibraryEntry {
  std::string id;
  std::string name;
  std::string parent;
  bool folder = false;
  bool operator==(const LibraryEntry&) const = default;
};

enum class LibraryWriteResult { Saved, NameInUse, Busy, Unavailable, WriteFailed };

/// Owns the shared folder tree. Each document package is stored under its stable ID.
class DocumentLibrary {
public:
  explicit DocumentLibrary(huxerui::File directory);

  const std::vector<LibraryEntry>& Entries() const;
  std::optional<LibraryEntry> Find(std::string_view id) const;
  std::optional<huxerui::File> Package(std::string_view id) const;
  bool CanPlace(std::string_view id, std::string_view parent) const;
  std::string UniqueTitle(std::string name, std::string_view parent) const;

  /// Writes a new catalog before publishing a tree change to the workspace.
  huxerui::Task<LibraryWriteResult> Put(LibraryEntry entry);

private:
  huxerui::File directory_;
  huxerui::State<std::optional<std::vector<LibraryEntry>>> entries_;
  bool writing_ = false;
};

} // namespace folio

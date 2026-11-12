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

enum class LibraryWriteResult { Saved, NameInUse, Busy, Unavailable, Changed, WriteFailed };

/// Owns the shared folder tree. Each document package is stored under its stable ID.
class DocumentLibrary {
public:
  explicit DocumentLibrary(huxerui::File directory);

  const std::vector<LibraryEntry>& Entries() const;
  /// Checks for a readable, persisted catalog before reconciling saved workspace references.
  bool HasSavedCatalog() const;
  std::optional<LibraryEntry> Find(std::string_view id) const;
  std::optional<huxerui::File> Package(std::string_view id) const;
  /// Returns the selected entry followed by its descendants in breadth-first order.
  std::vector<LibraryEntry> Subtree(std::string_view id) const;
  bool CanPlace(std::string_view id, std::string_view parent) const;
  std::string UniqueTitle(std::string name, std::string_view parent) const;

  /// Writes a new catalog before publishing a tree change to the workspace.
  huxerui::Task<LibraryWriteResult> Put(LibraryEntry entry);
  /// Commits removal of an unchanged subtree and records its packages for physical cleanup.
  huxerui::Task<LibraryWriteResult> Remove(std::vector<LibraryEntry> expected);
  /// Purges committed deletions and retains unsuccessful entries for a later retry.
  huxerui::Task<bool> CollectDeletedPackages();

private:
  struct Catalog {
    std::vector<LibraryEntry> entries;
    std::vector<std::string> pending_deletions;
    bool operator==(const Catalog&) const = default;
  };

  static std::optional<Catalog> ReadCatalog(const huxerui::File& directory);
  huxerui::Task<LibraryWriteResult> WriteCatalog(Catalog catalog);

  huxerui::File directory_;
  huxerui::State<std::optional<Catalog>> catalog_;
  bool writing_ = false;
};

} // namespace folio

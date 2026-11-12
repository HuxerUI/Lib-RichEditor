#include "document_library.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

using namespace huxerui;

namespace folio {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LibraryEntry, id, name, parent, folder)

namespace {

constexpr auto catalog_file = "catalog.json";

struct CatalogWriteGuard {
  explicit CatalogWriteGuard(bool& writing) : writing(writing) { writing = true; }
  ~CatalogWriteGuard() { writing = false; }
  bool& writing;
};

bool ValidId(std::string_view id) {
  return !id.empty() && std::ranges::all_of(id, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}

bool CanPlace(const std::vector<LibraryEntry>& entries, std::string_view id, std::string_view parent) {
  for (std::size_t remaining = entries.size(); !parent.empty(); --remaining) {
    if (remaining == 0 || parent == id) {
      return false;
    }
    const auto folder = std::ranges::find(entries, parent, &LibraryEntry::id);
    if (folder == entries.end() || !folder->folder) {
      return false;
    }
    parent = folder->parent;
  }
  return true;
}

} // namespace

std::optional<DocumentLibrary::Catalog> DocumentLibrary::ReadCatalog(const File& directory) {
  const auto text = directory.Child(catalog_file).ReadString();
  if (!text.Succeeded()) {
    return text.Error().code == IoErrorCode::NotFound ? std::optional(Catalog{}) : std::nullopt;
  }
  try {
    const auto json = nlohmann::json::parse(text.Value());
    if (json.at("version") != 1) {
      return {};
    }
    auto entries = json.at("entries").get<std::vector<LibraryEntry>>();
    std::set<std::string> ids;
    for (const auto& entry : entries) {
      if (!ValidId(entry.id) || entry.name.empty() || !ids.insert(entry.id).second ||
          !folio::CanPlace(entries, entry.id, entry.parent)) {
        return {};
      }
    }
    auto pending = json.value("pending_deletions", std::vector<std::string>{});
    if (std::ranges::any_of(pending, [&](const std::string& id) { return !ValidId(id) || !ids.insert(id).second; })) {
      return {};
    }
    return Catalog{std::move(entries), std::move(pending)};
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

DocumentLibrary::DocumentLibrary(File directory) : directory_(std::move(directory)), catalog_(ReadCatalog(directory_)) {}

bool DocumentLibrary::HasSavedCatalog() const {
  return catalog_.Get().has_value() && directory_.Child(catalog_file).IsFile();
}

const std::vector<LibraryEntry>& DocumentLibrary::Entries() const {
  static const std::vector<LibraryEntry> empty;
  const auto& catalog = catalog_.Get();
  return catalog ? catalog->entries : empty;
}

std::optional<LibraryEntry> DocumentLibrary::Find(std::string_view id) const {
  const auto& entries = Entries();
  const auto entry = std::ranges::find(entries, id, &LibraryEntry::id);
  return entry == entries.end() ? std::nullopt : std::optional(*entry);
}

std::optional<File> DocumentLibrary::Package(std::string_view id) const {
  return ValidId(id) ? std::optional(directory_.Child("documents").Child(id)) : std::nullopt;
}

std::vector<LibraryEntry> DocumentLibrary::Subtree(std::string_view id) const {
  auto root = Find(id);
  if (!root) {
    return {};
  }
  std::vector<LibraryEntry> result{std::move(*root)};
  if (result.front().folder) {
    std::unordered_map<std::string_view, std::vector<const LibraryEntry*>> children;
    for (const auto& entry : Entries()) {
      children[entry.parent].push_back(&entry);
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
      if (const auto next = children.find(result[index].id); next != children.end()) {
        for (const auto* child : next->second) {
          result.push_back(*child);
        }
      }
    }
  }
  return result;
}

bool DocumentLibrary::CanPlace(std::string_view id, std::string_view parent) const {
  return folio::CanPlace(Entries(), id, parent);
}

std::string DocumentLibrary::UniqueTitle(std::string name, std::string_view parent) const {
  const auto taken = [&](std::string_view title) {
    return std::ranges::any_of(Entries(), [&](const LibraryEntry& entry) {
      return entry.parent == parent && entry.name == title;
    });
  };
  auto title = name;
  for (unsigned suffix = 2; taken(title); ++suffix) {
    title = name + " " + std::to_string(suffix);
  }
  return title;
}

Task<LibraryWriteResult> DocumentLibrary::Put(LibraryEntry entry) {
  if (writing_) {
    co_return LibraryWriteResult::Busy;
  }
  const auto& current = catalog_.Get();
  if (!current || !ValidId(entry.id) || entry.name.empty() || !CanPlace(entry.id, entry.parent) ||
      std::ranges::find(current->pending_deletions, entry.id) != current->pending_deletions.end()) {
    co_return LibraryWriteResult::Unavailable;
  }
  auto catalog = *current;
  auto& entries = catalog.entries;
  if (std::ranges::any_of(entries, [&](const LibraryEntry& other) {
        return other.id != entry.id && other.parent == entry.parent && other.name == entry.name;
      })) {
    co_return LibraryWriteResult::NameInUse;
  }
  if (const auto existing = std::ranges::find(entries, entry.id, &LibraryEntry::id); existing != entries.end()) {
    if (existing->folder != entry.folder) {
      co_return LibraryWriteResult::Unavailable;
    }
    if (*existing == entry) {
      co_return LibraryWriteResult::Saved;
    }
    *existing = std::move(entry);
  } else {
    entries.push_back(std::move(entry));
  }
  const CatalogWriteGuard guard(writing_);
  co_return co_await WriteCatalog(std::move(catalog));
}

Task<LibraryWriteResult> DocumentLibrary::Remove(std::vector<LibraryEntry> expected) {
  if (writing_) {
    co_return LibraryWriteResult::Busy;
  }
  const auto& current = catalog_.Get();
  if (!current || expected.empty()) {
    co_return LibraryWriteResult::Unavailable;
  }
  if (Subtree(expected.front().id) != expected) {
    co_return LibraryWriteResult::Changed;
  }
  auto catalog = *current;
  std::set<std::string_view> removed;
  for (const auto& entry : expected) {
    removed.insert(entry.id);
    if (!entry.folder) {
      catalog.pending_deletions.push_back(entry.id);
    }
  }
  std::erase_if(catalog.entries, [&](const LibraryEntry& entry) { return removed.contains(entry.id); });
  const CatalogWriteGuard guard(writing_);
  co_return co_await WriteCatalog(std::move(catalog));
}

Task<bool> DocumentLibrary::CollectDeletedPackages() {
  const auto& current = catalog_.Get();
  if (!current || current->pending_deletions.empty()) {
    co_return true;
  }
  if (writing_) {
    co_return false;
  }
  const CatalogWriteGuard guard(writing_);
  auto catalog = *current;
  std::vector<std::string> remaining;
  for (const auto& id : catalog.pending_deletions) {
    const auto package = *Package(id);
    if (!co_await package.DeleteRecursivelyAsync()) {
      const auto info = co_await package.StatAsync();
      if (info.Succeeded() || info.Error().code != IoErrorCode::NotFound) {
        remaining.push_back(id);
      }
    }
  }
  if (remaining == catalog.pending_deletions) {
    co_return false;
  }
  const bool complete = remaining.empty();
  catalog.pending_deletions = std::move(remaining);
  const auto result = co_await WriteCatalog(std::move(catalog));
  co_return complete && result == LibraryWriteResult::Saved;
}

Task<LibraryWriteResult> DocumentLibrary::WriteCatalog(Catalog catalog) {
  const auto staged = directory_.Child("catalog.json.tmp");
  auto json = nlohmann::json{{"version", 1}, {"entries", catalog.entries}};
  if (!catalog.pending_deletions.empty()) {
    json["pending_deletions"] = catalog.pending_deletions;
  }
  if (!(co_await directory_.CreateDirectoriesAsync()) ||
      !(co_await staged.WriteStringAsync(json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) ||
      !(co_await staged.MoveToAsync(directory_.Child(catalog_file), true))) {
    (void)co_await staged.DeleteAsync();
    co_return LibraryWriteResult::WriteFailed;
  }
  catalog_ = std::move(catalog);
  co_return LibraryWriteResult::Saved;
}

} // namespace folio

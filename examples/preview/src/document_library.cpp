#include "document_library.h"

#include <algorithm>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

using namespace huxerui;

namespace folio {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LibraryEntry, id, name, parent, folder)

namespace {

constexpr auto catalog_file = "catalog.json";

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

std::optional<std::vector<LibraryEntry>> ReadCatalog(const File& directory) {
  const auto text = directory.Child(catalog_file).ReadString();
  if (!text.Succeeded()) {
    return text.Error().code == IoErrorCode::NotFound ? std::optional(std::vector<LibraryEntry>{}) : std::nullopt;
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
          !CanPlace(entries, entry.id, entry.parent)) {
        return {};
      }
    }
    return entries;
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

} // namespace

DocumentLibrary::DocumentLibrary(File directory) : directory_(std::move(directory)), entries_(ReadCatalog(directory_)) {}

const std::vector<LibraryEntry>& DocumentLibrary::Entries() const {
  static const std::vector<LibraryEntry> empty;
  const auto& entries = entries_.Get();
  return entries ? *entries : empty;
}

std::optional<LibraryEntry> DocumentLibrary::Find(std::string_view id) const {
  const auto& entries = Entries();
  const auto entry = std::ranges::find(entries, id, &LibraryEntry::id);
  return entry == entries.end() ? std::nullopt : std::optional(*entry);
}

std::optional<File> DocumentLibrary::Package(std::string_view id) const {
  return ValidId(id) ? std::optional(directory_.Child("documents").Child(id)) : std::nullopt;
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
  if (!entries_.Get() || !ValidId(entry.id) || entry.name.empty() || !CanPlace(entry.id, entry.parent)) {
    co_return LibraryWriteResult::Unavailable;
  }
  auto entries = Entries();
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
  writing_ = true;
  struct WriteGuard {
    bool& writing;
    ~WriteGuard() { writing = false; }
  } guard{writing_};
  const auto staged = directory_.Child("catalog.json.tmp");
  const auto json = nlohmann::json{{"version", 1}, {"entries", entries}};
  if (!(co_await directory_.CreateDirectoriesAsync()) ||
      !(co_await staged.WriteStringAsync(json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) ||
      !(co_await staged.MoveToAsync(directory_.Child(catalog_file), true))) {
    (void)co_await staged.DeleteAsync();
    co_return LibraryWriteResult::WriteFailed;
  }
  entries_ = std::move(entries);
  co_return LibraryWriteResult::Saved;
}

} // namespace folio

#include "attachments.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <huxerui/richeditor/format.h>

using namespace huxerui;
using namespace huxerui::richeditor;

namespace folio {
namespace {

bool External(std::string_view source) {
  const auto uri = Uri::Parse(source);
  if (!uri || !uri->Authority() || uri->Authority()->empty()) {
    return false;
  }
  const auto matches = [&](std::string_view expected) {
    return std::ranges::equal(uri->Scheme(), expected, [](char actual, char lower) {
      return actual == lower || actual == lower - ('a' - 'A');
    });
  };
  return matches("https") || matches("http");
}

std::optional<std::vector<std::string>> RelativePath(std::string_view source) {
  if (source.empty() || source.front() == '/' || source.find_first_of("\\:?#") != source.npos) {
    return std::nullopt;
  }
  const auto hex = [](char value) {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    return value >= 'A' && value <= 'F' ? value - 'A' + 10 : -1;
  };
  std::vector<std::string> parts;
  while (!source.empty()) {
    const auto slash = source.find('/');
    const auto part = source.substr(0, slash);
    std::string decoded;
    for (std::size_t i = 0; i < part.size(); ++i) {
      char ch = part[i];
      if (ch == '%') {
        if (part.size() - i < 3 || hex(part[i + 1]) < 0 || hex(part[i + 2]) < 0) {
          return std::nullopt;
        }
        ch = static_cast<char>(hex(part[i + 1]) * 16 + hex(part[i + 2]));
        i += 2;
      }
      if (static_cast<unsigned char>(ch) < 32 || ch == '/' || ch == '\\' || ch == ':') {
        return std::nullopt;
      }
      decoded += ch;
    }
    if (decoded == "..") {
      if (parts.empty()) {
        return std::nullopt;
      }
      parts.pop_back();
    } else if (!decoded.empty() && decoded != ".") {
      try {
        (void)AttributedText(decoded);
      } catch (const std::invalid_argument&) {
        return std::nullopt;
      }
      parts.push_back(std::move(decoded));
    }
    if (slash == source.npos) {
      break;
    }
    source.remove_prefix(slash + 1);
  }
  if (parts.empty() || parts.front() == "article.json" || parts.front() == "article.md" ||
      parts.front() == "article.json.tmp" || parts.front() == "article.md.tmp") {
    return std::nullopt;
  }
  return parts;
}

File ChildFile(File directory, const std::vector<std::string>& path) {
  for (const auto& part : path) {
    directory = directory.Child(part);
  }
  return directory;
}

Task<IoResult<FileReference>> FindChild(FileReference directory, std::string name, FileType type) {
  auto children = co_await directory.ListChildrenAsync();
  if (!children.Succeeded()) {
    co_return IoResult<FileReference>::Failure(children.Error());
  }
  for (auto& child : children.Value()) {
    if (child.Type() == type && child.Name() == name) {
      co_return IoResult<FileReference>::Success(std::move(child));
    }
  }
  co_return IoResult<FileReference>::Failure({IoErrorCode::NotFound, "Missing " + name});
}

Task<IoResult<FileReference>> FindAttachment(FileReference directory, std::vector<std::string> path) {
  for (std::size_t i = 0; i < path.size(); ++i) {
    auto child = co_await FindChild(directory, path[i], i + 1 == path.size() ? FileType::File : FileType::Directory);
    if (!child.Succeeded()) {
      co_return child;
    }
    directory = std::move(child.Value());
  }
  co_return IoResult<FileReference>::Success(std::move(directory));
}

std::set<std::string> LocalReferences(const RichDocument& document) {
  std::set<std::string> sources;
  for (const auto& block : document.Blocks()) {
    const auto& image = block.Data().image;
    if (image && !External(image->source)) {
      sources.insert(image->source);
    }
  }
  return sources;
}

} // namespace

std::string UniqueName(std::string_view prefix) {
  static std::atomic<unsigned> serial{0};
  return std::string(prefix) + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
}

Task<IoResult<std::string>> ImportImage(File directory, FileReference source) {
  auto bytes = co_await source.ReadBytesAsync();
  if (!bytes.Succeeded()) {
    co_return IoResult<std::string>::Failure(bytes.Error());
  }
  ImageFormat format;
  try {
    format = ImageAsset::CopyEncoded(bytes.Value()).Format();
  } catch (const std::invalid_argument&) {
    co_return IoResult<std::string>::Failure({IoErrorCode::InvalidEncoding, "Choose a PNG or JPEG image"});
  }
  const auto attachments = directory.Child("attachments");
  const auto name = UniqueName("image-") + (format == ImageFormat::Png ? ".png" : ".jpg");
  if (!co_await attachments.CreateDirectoriesAsync()) {
    co_return IoResult<std::string>::Failure({IoErrorCode::Io, "Could not create the attachments folder"});
  }
  const auto temporary = attachments.Child(name + ".tmp");
  if (!(co_await temporary.WriteBytesAsync(std::move(bytes.Value()))) ||
      !(co_await temporary.MoveToAsync(attachments.Child(name), false))) {
    (void)co_await temporary.DeleteAsync();
    co_return IoResult<std::string>::Failure({IoErrorCode::Io, "Could not save the image attachment"});
  }
  co_return IoResult<std::string>::Success("attachments/" + name);
}

bool HasLocalAttachments(const RichDocument& document) {
  return !LocalReferences(document).empty();
}

Task<IoResult<DocumentSource>> ReadDocument(FileReference source) {
  auto name = source.Name();
  std::ranges::transform(name, name.begin(), [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
  });
  const bool markdown = name.ends_with(".md") || name.ends_with(".markdown");
  if (!markdown && !name.ends_with(".json")) {
    co_return IoResult<DocumentSource>::Failure({IoErrorCode::Unsupported, "Choose a JSON or Markdown document"});
  }
  auto contents = co_await source.ReadStringAsync();
  if (!contents.Succeeded()) {
    co_return IoResult<DocumentSource>::Failure(contents.Error());
  }
  auto imported = (markdown ? MarkdownCodec({.display_math = true}) : JsonCodec()).Import(contents.Value());
  if (!imported.document) {
    auto message = imported.issues.empty() ? std::string("Could not open the document") : imported.issues.front().message;
    co_return IoResult<DocumentSource>::Failure({IoErrorCode::InvalidEncoding, std::move(message)});
  }
  co_return IoResult<DocumentSource>::Success({std::move(*imported.document), markdown});
}

Task<IoResult<OpenedDocument>> ReadFolder(FileReference source, File working_directory) {
  auto entry = co_await FindChild(source, "article.json", FileType::File);
  if (!entry.Succeeded() && entry.Error().code == IoErrorCode::NotFound) {
    entry = co_await FindChild(source, "article.md", FileType::File);
  }
  if (!entry.Succeeded()) {
    co_return IoResult<OpenedDocument>::Failure(entry.Error());
  }
  auto imported = co_await ReadDocument(std::move(entry.Value()));
  if (!imported.Succeeded()) {
    co_return IoResult<OpenedDocument>::Failure(imported.Error());
  }
  co_return co_await ReadAttachments(std::move(source), std::move(working_directory), std::move(imported.Value()));
}

Task<IoResult<OpenedDocument>> ReadAttachments(FileReference source, File working_directory, DocumentSource imported) {
  std::unordered_map<std::string, std::string> copied;
  std::set<std::vector<std::string>> imported_paths;
  std::size_t missing_images = 0;
  for (const auto& reference : LocalReferences(imported.document)) {
    const auto path = RelativePath(reference);
    if (!path) {
      ++missing_images;
      continue;
    }
    if (!imported.markdown && !imported_paths.insert(*path).second) {
      continue;
    }
    auto attachment = co_await FindAttachment(source, *path);
    if (!attachment.Succeeded()) {
      ++missing_images;
      continue;
    }
    if (imported.markdown) {
      auto saved = co_await ImportImage(working_directory, std::move(attachment.Value()));
      if (saved.Succeeded()) {
        copied.emplace(reference, std::move(saved.Value()));
      } else if (saved.Error().code == IoErrorCode::InvalidEncoding) {
        ++missing_images;
      } else {
        co_return IoResult<OpenedDocument>::Failure(saved.Error());
      }
    } else {
      const auto file = ChildFile(working_directory, *path);
      if (!(co_await file.Parent()->CreateDirectoriesAsync()) ||
          !(co_await attachment.Value().ImportToAsync(file))) {
        co_return IoResult<OpenedDocument>::Failure({IoErrorCode::Io, "Could not import " + reference});
      }
    }
  }
  auto document = std::move(imported.document);
  if (!copied.empty()) {
    auto blocks = std::vector<Block>(document.Blocks().begin(), document.Blocks().end());
    for (auto& block : blocks) {
      if (block.Data().image && copied.contains(block.Data().image->source)) {
        auto data = block.Data();
        data.image->source = copied.at(data.image->source);
        block = Block(std::move(data));
      }
    }
    document = RichDocument::Create(std::move(blocks));
  }
  co_return IoResult<OpenedDocument>::Success({std::move(document), missing_images});
}

Task<IoResult<void>> CheckAttachments(File directory, RichDocument document) {
  for (const auto& reference : LocalReferences(document)) {
    const auto path = RelativePath(reference);
    if (!path) {
      co_return IoResult<void>::Failure({IoErrorCode::Unsupported, "Invalid attachment reference " + reference});
    }
    const auto info = co_await ChildFile(directory, *path).StatAsync();
    if (!info.Succeeded() || info.Value().type != FileType::File) {
      co_return IoResult<void>::Failure({IoErrorCode::NotFound, "Missing attachment " + reference});
    }
  }
  co_return IoResult<void>::Success();
}

Task<IoResult<void>> CopyAttachments(File directory, FileReference destination, RichDocument document) {
  // Several lexical references can normalize to the same portable destination.
  std::set<std::vector<std::string>> copied;
  for (const auto& reference : LocalReferences(document)) {
    const auto path = RelativePath(reference);
    if (!path) {
      co_return IoResult<void>::Failure({IoErrorCode::Unsupported, "Invalid attachment reference " + reference});
    }
    if (!copied.insert(*path).second) {
      continue;
    }
    auto folder = destination;
    for (std::size_t i = 0; i + 1 < path->size(); ++i) {
      auto child = co_await folder.CreateDirectoryAsync((*path)[i]);
      if (!child.Succeeded()) {
        co_return IoResult<void>::Failure(child.Error());
      }
      folder = std::move(child.Value());
    }
    auto saved = co_await folder.CopyFileFromAsync(ChildFile(directory, *path), path->back());
    if (!saved.Succeeded()) {
      co_return IoResult<void>::Failure({saved.Error().code, "Could not copy attachment " + reference});
    }
  }
  co_return IoResult<void>::Success();
}

} // namespace folio

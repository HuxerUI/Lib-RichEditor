#include "attachments.h"
#include "formula.h"
#include "web_url.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::richeditor;

namespace folio {
namespace {

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
  if (parts.empty()) {
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

std::filesystem::path SystemPath(const File& file) {
  const auto path = file.Path();
  return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

bool WithinDirectory(const File& directory, const File& child) {
  std::error_code error;
  const auto root = std::filesystem::canonical(SystemPath(directory), error);
  if (error) {
    return false;
  }
  const auto resolved = std::filesystem::canonical(SystemPath(child), error);
  if (error) {
    return false;
  }
  const auto relative = resolved.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

Task<IoResult<FileReference>> FindAttachment(FileReference directory, std::vector<std::string> path) {
  const auto root = directory.AsFile();
  for (std::size_t i = 0; i < path.size(); ++i) {
    auto child = co_await FindChild(directory, path[i], i + 1 == path.size() ? FileType::File : FileType::Directory);
    if (!child.Succeeded()) {
      co_return child;
    }
    directory = std::move(child.Value());
    if (const auto local = directory.AsFile(); root && local &&
        !(co_await RunWorker(WithinDirectory, *root, *local))) {
      co_return IoResult<FileReference>::Failure({IoErrorCode::PermissionDenied, "Attachment is outside its folder"});
    }
  }
  co_return IoResult<FileReference>::Success(std::move(directory));
}

std::set<std::string> LocalReferences(const RichDocument& document) {
  std::set<std::string> sources;
  const auto add = [&](const std::string& source) {
    if (!ParseWebUrl(source)) {
      sources.insert(source);
    }
  };
  const auto visit = [&](const auto& self, std::span<const Block> blocks) -> void {
    for (const auto& block : blocks) {
      const auto& data = block.Data();
      if (data.image) {
        add(data.image->source);
      }
      for (const auto& object : data.inline_objects) {
        for (const auto& attachment : object.data.attachments) {
          add(attachment.source);
        }
      }
      self(self, data.children);
    }
  };
  visit(visit, document.Blocks());
  return sources;
}

using AttachmentFile = std::variant<File, FileReference>;

Task<IoResult<AttachmentFile>> ResolveAttachment(std::optional<File> working_directory,
                                                const std::vector<FileReference>& directories,
                                                const std::vector<std::string>& path) {
  if (working_directory) {
    const auto staged = ChildFile(*working_directory, path);
    const auto info = co_await staged.StatAsync();
    if (info.Succeeded()) {
      if (info.Value().type != FileType::File) {
        co_return IoResult<AttachmentFile>::Failure({IoErrorCode::IsDirectory, "Attachment is not a file"});
      }
      co_return IoResult<AttachmentFile>::Success(staged);
    }
    if (info.Error().code != IoErrorCode::NotFound) {
      co_return IoResult<AttachmentFile>::Failure(info.Error());
    }
  }
  for (const auto& directory : directories) {
    auto child = co_await FindAttachment(directory, path);
    if (child.Succeeded()) {
      co_return IoResult<AttachmentFile>::Success(std::move(child.Value()));
    }
    if (child.Error().code != IoErrorCode::NotFound) {
      co_return IoResult<AttachmentFile>::Failure(child.Error());
    }
  }
  co_return IoResult<AttachmentFile>::Failure(
      {IoErrorCode::NotFound, directories.empty() ? "Choose the attachment folder" : "Attachment is missing"});
}

struct AttachmentCopy {
  std::string reference;
  std::vector<std::string> path;
  AttachmentFile source;
};

Task<IoResult<std::vector<AttachmentCopy>>> PrepareAttachments(
    std::optional<File> working_directory, const std::vector<FileReference>& directories, const RichDocument& document,
    std::string document_name, const RichDocument* baseline = nullptr) {
  const auto existing = baseline ? LocalReferences(*baseline) : std::set<std::string>{};
  std::vector<AttachmentCopy> copies;
  std::set<std::vector<std::string>> paths;
  for (const auto& reference : LocalReferences(document)) {
    auto path = RelativePath(reference);
    if (!path || std::ranges::equal(path->front(), document_name, {}, LowerAscii, LowerAscii)) {
      if (existing.contains(reference)) {
        continue;
      }
      co_return IoResult<std::vector<AttachmentCopy>>::Failure(
          {IoErrorCode::Unsupported, "Invalid attachment path " + reference});
    }
    if (!paths.insert(*path).second) {
      continue;
    }
    auto source = co_await ResolveAttachment(working_directory, directories, *path);
    if (!source.Succeeded()) {
      if (source.Error().code == IoErrorCode::NotFound && existing.contains(reference)) {
        continue;
      }
      co_return IoResult<std::vector<AttachmentCopy>>::Failure(
          {source.Error().code, "Could not read " + reference + ". " + source.Error().message});
    }
    copies.push_back({reference, std::move(*path), std::move(source.Value())});
  }
  co_return IoResult<std::vector<AttachmentCopy>>::Success(std::move(copies));
}

IoResult<void> CheckOriginal(const File& target, const Bytes& expected) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(SystemPath(target), error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return IoResult<void>::Failure({IoErrorCode::Unsupported, "The original is no longer an ordinary local file"});
  }
  const auto current = target.ReadBytes();
  if (!current.Succeeded()) {
    return IoResult<void>::Failure(current.Error());
  }
  if (current.Value() != expected) {
    return IoResult<void>::Failure({IoErrorCode::AlreadyExists, "The original file changed outside Folio"});
  }
  return IoResult<void>::Success();
}

IoResult<void> CommitLocalFile(const File& target, std::span<const std::byte> content, const Bytes* expected) {
  const auto parent = target.Parent();
  if (!parent) {
    return IoResult<void>::Failure({IoErrorCode::Unsupported, "The file has no parent directory"});
  }
  const auto staged = parent->Child(UniqueName(".folio-") + ".tmp");
  auto written = IoResult<void>::Success();
  {
    auto output = staged.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
    if (!output.Succeeded()) {
      return IoResult<void>::Failure(output.Error());
    }
    if (!content.empty()) {
      written = output.Value().WriteAt(0, content);
    }
    if (written.Succeeded()) {
      written = output.Value().Close();
    }
  }
  if (!written.Succeeded()) {
    (void)staged.Delete();
    return written;
  }
  if (expected) {
    const auto checked = CheckOriginal(target, *expected);
    if (!checked.Succeeded()) {
      (void)staged.Delete();
      return checked;
    }
  }
  if (!staged.MoveTo(target, expected != nullptr)) {
    (void)staged.Delete();
    return IoResult<void>::Failure({IoErrorCode::Io, "Could not commit " + target.Name()});
  }
  return IoResult<void>::Success();
}

Task<IoResult<void>> CopyAttachments(FileReference destination, std::vector<AttachmentCopy> copies, bool in_place) {
  const auto root = destination.AsFile();
  for (auto& copy : copies) {
    auto folder = destination;
    for (std::size_t index = 0; index + 1 < copy.path.size(); ++index) {
      const auto& name = copy.path[index];
      auto child = co_await folder.CreateDirectoryAsync(name);
      if (!child.Succeeded()) {
        co_return IoResult<void>::Failure(child.Error());
      }
      folder = std::move(child.Value());
      if (const auto local = folder.AsFile(); root && local &&
          !(co_await RunWorker(WithinDirectory, *root, *local))) {
        co_return IoResult<void>::Failure(
            {IoErrorCode::PermissionDenied, "Cannot copy " + copy.reference + " outside the destination folder"});
      }
    }
    const auto name = copy.path.back();
    if (in_place) {
      auto existing = co_await FindAttachment(folder, {name});
      if (!existing.Succeeded() && existing.Error().code != IoErrorCode::NotFound) {
        co_return IoResult<void>::Failure(existing.Error());
      }
      if (existing.Succeeded()) {
        if (const auto* reference = std::get_if<FileReference>(&copy.source)) {
          const auto from = reference->AsFile();
          const auto to = existing.Value().AsFile();
          if (from && from == to) {
            continue;
          }
        }
      }
      auto bytes = co_await std::visit([](const auto& file) { return file.ReadBytesAsync(); }, copy.source);
      if (!bytes.Succeeded()) {
        co_return IoResult<void>::Failure(bytes.Error());
      }
      if (existing.Succeeded()) {
        const auto current = co_await existing.Value().ReadBytesAsync();
        if (!current.Succeeded() || current.Value() != bytes.Value()) {
          co_return IoResult<void>::Failure(
              {IoErrorCode::AlreadyExists, "An attachment already exists at " + copy.reference});
        }
        continue;
      }
      const auto local = folder.AsFile();
      if (!local) {
        co_return IoResult<void>::Failure({IoErrorCode::Unsupported, "Cannot safely add " + copy.reference});
      }
      auto saved = co_await RunWorker([](File file, Bytes content) {
        return CommitLocalFile(file, content, nullptr);
      }, local->Child(name), std::move(bytes.Value()));
      if (!saved.Succeeded()) {
        co_return IoResult<void>::Failure(saved.Error());
      }
      continue;
    }
    auto saved = co_await std::visit([&](const auto& file) {
      return folder.CopyFileFromAsync(file, name);
    }, copy.source);
    if (!saved.Succeeded()) {
      co_return IoResult<void>::Failure({saved.Error().code, "Could not copy " + copy.reference});
    }
  }
  co_return IoResult<void>::Success();
}

} // namespace

std::string UniqueName(std::string_view prefix) {
  static std::atomic<unsigned> serial{0};
  return std::string(prefix) + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
}

std::optional<DocumentFormat> FormatOf(std::string_view name) {
  auto lower = std::string(name);
  std::ranges::transform(lower, lower.begin(), LowerAscii);
  if (lower.ends_with(".md") || lower.ends_with(".markdown")) {
    return DocumentFormat::Markdown;
  }
  if (lower.ends_with(".json")) {
    return DocumentFormat::Json;
  }
  return std::nullopt;
}

const DocumentCodec& Codec(DocumentFormat format) {
  return format == DocumentFormat::Markdown ? DocumentMarkdown() : JsonCodec();
}

bool ValidDocumentName(std::string_view name, DocumentFormat format) {
  const auto extension = name.rfind('.');
  return extension != name.npos && extension != 0 && FormatOf(name) == format &&
      name.find_first_of("\\/:*?\"<>|") == name.npos &&
      std::ranges::none_of(name, [](unsigned char ch) { return ch < 32; });
}

std::shared_ptr<const ImageResolver> AttachmentResolver(std::optional<File> working_directory,
                                                       std::vector<FileReference> directories) {
  if (!working_directory && directories.empty()) {
    return {};
  }
  return std::make_shared<const ImageResolver>(
      [working_directory = std::move(working_directory), directories = std::move(directories)](
          std::string source) -> Task<IoResult<ImageAsset>> {
        const auto path = RelativePath(source);
        if (!path) {
          co_return IoResult<ImageAsset>::Failure({IoErrorCode::Unsupported, "Unsupported image reference"});
        }
        auto file = co_await ResolveAttachment(working_directory, directories, *path);
        if (!file.Succeeded()) {
          co_return IoResult<ImageAsset>::Failure(file.Error());
        }
        auto bytes = co_await std::visit([](const auto& entry) { return entry.ReadBytesAsync(); }, file.Value());
        if (!bytes.Succeeded()) {
          co_return IoResult<ImageAsset>::Failure(bytes.Error());
        }
        try {
          co_return IoResult<ImageAsset>::Success(ImageAsset::CopyEncoded(bytes.Value()));
        } catch (const std::invalid_argument&) {
          co_return IoResult<ImageAsset>::Failure({IoErrorCode::InvalidEncoding, "Image must be PNG or JPEG"});
        }
      });
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

bool NeedsAttachmentWrite(const FileReference& target, const std::vector<FileReference>& directories,
                          const RichDocument& document, const RichDocument& baseline) {
  const auto current = LocalReferences(document);
  if (current.empty()) {
    return false;
  }
  if (!directories.empty()) {
    const auto path = target.AsFile();
    const auto source = directories.front().AsFile();
    if (!path || !source || source != path->Parent()) {
      return true;
    }
  }
  const auto previous = LocalReferences(baseline);
  return !std::ranges::includes(previous, current);
}

Task<IoResult<DocumentSource>> ReadDocument(FileReference source) {
  const auto format = FormatOf(source.Name());
  if (!format) {
    co_return IoResult<DocumentSource>::Failure({IoErrorCode::Unsupported, "Choose a JSON or Markdown document"});
  }
  auto contents = co_await source.ReadBytesAsync();
  if (!contents.Succeeded()) {
    co_return IoResult<DocumentSource>::Failure(contents.Error());
  }
  const auto& bytes = contents.Value();
  std::string_view text;
  if (!bytes.empty()) {
    text = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }
  if (text.starts_with("\xef\xbb\xbf")) {
    text.remove_prefix(3);
  }
  try {
    (void)AttributedText(std::string(text));
  } catch (const std::invalid_argument&) {
    co_return IoResult<DocumentSource>::Failure({IoErrorCode::InvalidEncoding, "The document is not valid UTF-8"});
  }
  auto imported = Codec(*format).Import(text);
  if (!imported.document) {
    auto message = imported.issues.empty() ? std::string("Could not open the document") : imported.issues.front().message;
    co_return IoResult<DocumentSource>::Failure({IoErrorCode::InvalidEncoding, std::move(message)});
  }
  co_return IoResult<DocumentSource>::Success({std::move(*imported.document), *format, std::move(contents.Value())});
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

Task<IoResult<FileReference>> FindPackageDocument(FileReference directory) {
  auto entry = co_await FindChild(directory, "article.json", FileType::File);
  if (!entry.Succeeded() && entry.Error().code == IoErrorCode::NotFound) {
    co_return co_await FindChild(std::move(directory), "article.md", FileType::File);
  }
  co_return entry;
}

Task<std::optional<File>> LocalIdentity(FileReference reference) {
  auto local = reference.AsFile();
  if (!local) {
    co_return std::nullopt;
  }
  co_return co_await RunWorker([](File file) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(SystemPath(file), error);
    return error ? file : File(resolved.generic_u8string());
  }, *local);
}

Task<IoResult<void>> WriteOriginal(FileReference target, const Bytes& expected, std::string text,
                                  std::optional<File> working_directory, std::vector<FileReference> attachments,
                                  std::optional<FileReference> destination, RichDocument document,
                                  const RichDocument& baseline) {
  const auto local = target.AsFile();
  if (!target.CanWrite() || !local) {
    co_return IoResult<void>::Failure({IoErrorCode::PermissionDenied, "The original does not support safe replacement"});
  }
  if (NeedsAttachmentWrite(target, attachments, document, baseline)) {
    if (!destination || !destination->CanWrite()) {
      co_return IoResult<void>::Failure({IoErrorCode::PermissionDenied, "Choose the original folder with write access"});
    }
    const auto directory = co_await LocalIdentity(*destination);
    const auto original = co_await LocalIdentity(target);
    if (!directory || !original || original->Parent() != directory) {
      co_return IoResult<void>::Failure({IoErrorCode::PermissionDenied, "Choose the folder containing the original"});
    }
    const auto checked = co_await RunWorker(CheckOriginal, *local, expected);
    if (!checked.Succeeded()) {
      co_return checked;
    }
    if (attachments.empty()) {
      attachments.push_back(*destination);
    }
    auto copies = co_await PrepareAttachments(working_directory, attachments, document, target.Name(), &baseline);
    if (!copies.Succeeded()) {
      co_return IoResult<void>::Failure(copies.Error());
    }
    const auto copied = co_await CopyAttachments(*destination, std::move(copies.Value()), true);
    if (!copied.Succeeded()) {
      co_return copied;
    }
  }
  co_return co_await RunWorker([](File file, Bytes previous, std::string content) {
    return CommitLocalFile(file, std::as_bytes(std::span(content)), &previous);
  }, *local, expected, std::move(text));
}

Task<IoResult<DocumentCopy>> WriteCopy(FileReference parent, File staged_file, std::string name,
                                     std::optional<File> working_directory, std::vector<FileReference> attachments,
                                     RichDocument document) {
  auto copies = co_await PrepareAttachments(working_directory, attachments, document, name);
  if (!copies.Succeeded()) {
    co_return IoResult<DocumentCopy>::Failure(copies.Error());
  }
  auto destination = std::move(parent);
  if (!copies.Value().empty()) {
    auto created = co_await destination.CreateDirectoryAsync(UniqueName("Folio-"));
    if (!created.Succeeded()) {
      co_return IoResult<DocumentCopy>::Failure(created.Error());
    }
    destination = std::move(created.Value());
    const auto copied = co_await CopyAttachments(destination, std::move(copies.Value()), false);
    if (!copied.Succeeded()) {
      co_return IoResult<DocumentCopy>::Failure(
          {copied.Error().code, destination.Name() + ": " + copied.Error().message});
    }
  }
  auto saved = co_await destination.CopyFileFromAsync(std::move(staged_file), std::move(name));
  if (!saved.Succeeded()) {
    co_return IoResult<DocumentCopy>::Failure(saved.Error());
  }
  co_return IoResult<DocumentCopy>::Success({std::move(saved.Value()), std::move(destination)});
}

} // namespace folio

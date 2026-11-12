#include "file_source.h"

using namespace huxerui;

namespace folio {

FileSource::FileSource(FileReference reference)
    : source_(std::move(reference)), type_(Reference()->Type()), writable_(Reference()->CanWrite()) {}

FileSource::FileSource(File file, FileType type, bool writable)
    : source_(std::move(file)), type_(type), writable_(writable) {}

std::string FileSource::Name() const {
  return std::visit([](const auto& source) { return source.Name(); }, source_);
}

FileType FileSource::Type() const { return type_; }
bool FileSource::CanWrite() const { return writable_; }

std::optional<File> FileSource::AsFile() const {
  if (const auto* reference = Reference()) {
    return reference->AsFile();
  }
  return std::get<File>(source_);
}

const FileReference* FileSource::Reference() const { return std::get_if<FileReference>(&source_); }

Task<IoResult<Bytes>> FileSource::ReadBytesAsync() const {
  return std::visit([](const auto& source) { return source.ReadBytesAsync(); }, source_);
}

Task<IoResult<std::vector<FileSource>>> FileSource::ListChildrenAsync() const {
  return ListChildren(*this);
}

Task<IoResult<std::vector<FileSource>>> FileSource::ListChildren(FileSource directory) {
  std::vector<FileSource> children;
  if (const auto* reference = directory.Reference()) {
    auto entries = co_await reference->ListChildrenAsync();
    if (!entries.Succeeded()) {
      co_return IoResult<std::vector<FileSource>>::Failure(entries.Error());
    }
    for (auto& entry : entries.Value()) {
      children.emplace_back(std::move(entry));
    }
  } else {
    auto entries = co_await std::get<File>(directory.source_).ListChildrenAsync();
    if (!entries.Succeeded()) {
      co_return IoResult<std::vector<FileSource>>::Failure(entries.Error());
    }
    for (auto& entry : entries.Value()) {
      const auto info = co_await entry.StatAsync();
      if (info.Succeeded()) {
        children.emplace_back(std::move(entry), info.Value().type, directory.CanWrite());
      }
    }
  }
  co_return IoResult<std::vector<FileSource>>::Success(std::move(children));
}

Task<IoResult<FileSource>> FileSource::CreateDirectoryAsync(std::string name) const {
  return CreateDirectory(*this, std::move(name));
}

Task<IoResult<FileSource>> FileSource::CreateDirectory(FileSource directory, std::string name) {
  if (const auto* reference = directory.Reference()) {
    auto created = co_await reference->CreateDirectoryAsync(std::move(name));
    co_return created.Succeeded() ? IoResult<FileSource>::Success(std::move(created.Value()))
                                  : IoResult<FileSource>::Failure(created.Error());
  }
  auto child = std::get<File>(directory.source_).Child(name);
  if (directory.CanWrite() && co_await child.CreateDirectoriesAsync()) {
    co_return IoResult<FileSource>::Success(FileSource(std::move(child), FileType::Directory, true));
  }
  co_return IoResult<FileSource>::Failure({IoErrorCode::Io, "Could not create the folder"});
}

Task<IoResult<FileSource>> FileSource::CopyFileFromAsync(File source, std::string name) const {
  return CopyFileFrom(*this, std::move(source), std::move(name));
}

Task<IoResult<FileSource>> FileSource::CopyFileFrom(FileSource directory, File source, std::string name) {
  if (const auto* reference = directory.Reference()) {
    auto copied = co_await reference->CopyFileFromAsync(std::move(source), std::move(name), false);
    co_return copied.Succeeded() ? IoResult<FileSource>::Success(std::move(copied.Value()))
                                 : IoResult<FileSource>::Failure(copied.Error());
  }
  auto child = std::get<File>(directory.source_).Child(name);
  if (directory.CanWrite() && co_await source.CopyToAsync(child, false)) {
    co_return IoResult<FileSource>::Success(FileSource(std::move(child), FileType::File, true));
  }
  const auto info = co_await child.StatAsync();
  co_return IoResult<FileSource>::Failure(info.Succeeded()
      ? IoError{IoErrorCode::AlreadyExists, "A file or folder already uses that name"}
      : IoError{IoErrorCode::Io, "Could not create the file"});
}

} // namespace folio

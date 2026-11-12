#pragma once

#include <variant>

#include <huxerui/file.h>

namespace folio {

/// Access through a live picker grant or a local path reopened with system permissions.
class FileSource {
public:
  FileSource(huxerui::FileReference reference);
  FileSource(huxerui::File file, huxerui::FileType type, bool writable);

  std::string Name() const;
  huxerui::FileType Type() const;
  bool CanWrite() const;
  /// Returns the local path when the source exposes one. The path does not carry the picker grant.
  std::optional<huxerui::File> AsFile() const;
  const huxerui::FileReference* Reference() const;
  huxerui::Task<huxerui::IoResult<huxerui::Bytes>> ReadBytesAsync() const;
  huxerui::Task<huxerui::IoResult<std::vector<FileSource>>> ListChildrenAsync() const;
  huxerui::Task<huxerui::IoResult<FileSource>> CreateDirectoryAsync(std::string name) const;
  huxerui::Task<huxerui::IoResult<FileSource>> CopyFileFromAsync(huxerui::File source, std::string name) const;

private:
  static huxerui::Task<huxerui::IoResult<std::vector<FileSource>>> ListChildren(FileSource directory);
  static huxerui::Task<huxerui::IoResult<FileSource>> CreateDirectory(FileSource directory, std::string name);
  static huxerui::Task<huxerui::IoResult<FileSource>> CopyFileFrom(FileSource directory, huxerui::File source,
                                                                std::string name);
  std::variant<huxerui::FileReference, huxerui::File> source_;
  huxerui::FileType type_;
  bool writable_;
};

} // namespace folio

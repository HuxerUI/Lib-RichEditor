#pragma once

#include <huxerui/file.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

#include "file_source.h"

namespace folio {

enum class DocumentFormat { Json, Markdown };

struct DocumentSource {
  DocumentFormat format;
  huxerui::Bytes bytes;

  /// Validated UTF-8 view for the codec, excluding the optional byte order mark.
  std::string_view Text() const;
};

struct DocumentCopy {
  huxerui::FileReference file;
  huxerui::FileReference directory;
};

std::string UniqueName(std::string_view prefix);
std::optional<DocumentFormat> FormatOf(std::string_view name);
const huxerui::richeditor::DocumentCodec& Codec(DocumentFormat format);
std::shared_ptr<const huxerui::richeditor::ImageResolver> AttachmentResolver(
    std::optional<huxerui::File> working_directory, std::vector<FileSource> directories);
huxerui::Task<huxerui::IoResult<std::string>> ImportImage(huxerui::File directory, huxerui::FileReference source);
bool HasLocalAttachments(const huxerui::richeditor::RichDocument& document);
bool NeedsAttachmentWrite(const FileSource& target,
                          const std::vector<FileSource>& directories,
                          const huxerui::richeditor::RichDocument& document,
                          const huxerui::richeditor::RichDocument& baseline);
huxerui::Task<huxerui::IoResult<DocumentSource>> ReadDocument(FileSource source);
huxerui::Task<huxerui::IoResult<FileSource>> FindChild(
    FileSource directory, std::string name, huxerui::FileType type);

struct FolderFile {
  FileSource file;
  FileSource parent;
};

/// Finds a relative file through the directory grant, rejecting local symlinks that leave the root.
huxerui::Task<huxerui::IoResult<FolderFile>> FindFolderFile(FileSource directory, std::vector<std::string> path);
huxerui::Task<huxerui::IoResult<FileSource>> FindPackageDocument(FileSource directory);
huxerui::Task<std::optional<huxerui::File>> LocalIdentity(FileSource reference);
bool ValidDocumentName(std::string_view name, DocumentFormat format);

huxerui::Task<huxerui::IoResult<void>> WriteOriginal(
    FileSource target, const huxerui::Bytes& expected, std::string text,
    std::optional<huxerui::File> working_directory,
    std::vector<FileSource> attachments, std::optional<FileSource> destination,
    huxerui::richeditor::RichDocument document, const huxerui::richeditor::RichDocument& baseline);
huxerui::Task<huxerui::IoResult<DocumentCopy>> WriteCopy(
    huxerui::FileReference parent, huxerui::File staged_file, std::string name,
    std::optional<huxerui::File> working_directory,
    std::vector<FileSource> attachments, huxerui::richeditor::RichDocument document);
huxerui::Task<huxerui::IoResult<void>> WriteLibraryPackage(
    huxerui::File directory, std::string text, std::optional<huxerui::File> working_directory,
    std::vector<FileSource> attachments, huxerui::richeditor::RichDocument document,
    std::optional<huxerui::richeditor::RichDocument> baseline = {});

} // namespace folio

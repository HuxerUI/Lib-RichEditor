#pragma once

#include <huxerui/file.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

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
    std::optional<huxerui::File> working_directory, std::vector<huxerui::FileReference> directories);
huxerui::Task<huxerui::IoResult<std::string>> ImportImage(huxerui::File directory, huxerui::FileReference source);
bool HasLocalAttachments(const huxerui::richeditor::RichDocument& document);
bool NeedsAttachmentWrite(const huxerui::FileReference& target,
                          const std::vector<huxerui::FileReference>& directories,
                          const huxerui::richeditor::RichDocument& document,
                          const huxerui::richeditor::RichDocument& baseline);
huxerui::Task<huxerui::IoResult<DocumentSource>> ReadDocument(huxerui::FileReference source);
huxerui::Task<huxerui::IoResult<huxerui::FileReference>> FindChild(
    huxerui::FileReference directory, std::string name, huxerui::FileType type);
huxerui::Task<huxerui::IoResult<huxerui::FileReference>> FindPackageDocument(huxerui::FileReference directory);
huxerui::Task<std::optional<huxerui::File>> LocalIdentity(huxerui::FileReference reference);
bool ValidDocumentName(std::string_view name, DocumentFormat format);

huxerui::Task<huxerui::IoResult<void>> WriteOriginal(
    huxerui::FileReference target, const huxerui::Bytes& expected, std::string text,
    std::optional<huxerui::File> working_directory,
    std::vector<huxerui::FileReference> attachments, std::optional<huxerui::FileReference> destination,
    huxerui::richeditor::RichDocument document, const huxerui::richeditor::RichDocument& baseline);
huxerui::Task<huxerui::IoResult<DocumentCopy>> WriteCopy(
    huxerui::FileReference parent, huxerui::File staged_file, std::string name,
    std::optional<huxerui::File> working_directory,
    std::vector<huxerui::FileReference> attachments, huxerui::richeditor::RichDocument document);

} // namespace folio

#pragma once

#include <huxerui/file.h>
#include <huxerui/richeditor.h>

namespace folio {

struct DocumentSource {
  huxerui::richeditor::RichDocument document;
  bool markdown = false;
};

struct OpenedDocument {
  huxerui::richeditor::RichDocument document;
  std::size_t missing_images = 0;
};

std::string UniqueName(std::string_view prefix);
huxerui::Task<huxerui::IoResult<std::string>> ImportImage(huxerui::File directory, huxerui::FileReference source);
bool HasLocalAttachments(const huxerui::richeditor::RichDocument& document);
huxerui::Task<huxerui::IoResult<DocumentSource>> ReadDocument(huxerui::FileReference source);
huxerui::Task<huxerui::IoResult<OpenedDocument>> ReadFolder(huxerui::FileReference source, huxerui::File working_directory);
// Copy referenced attachments from a separately selected directory for an already decoded document.
huxerui::Task<huxerui::IoResult<OpenedDocument>> ReadAttachments(huxerui::FileReference source,
                                                               huxerui::File working_directory,
                                                               DocumentSource imported);
huxerui::Task<huxerui::IoResult<void>> CheckAttachments(huxerui::File directory,
                                                      huxerui::richeditor::RichDocument document);
huxerui::Task<huxerui::IoResult<void>> CopyAttachments(huxerui::File directory, huxerui::FileReference destination,
                                                     huxerui::richeditor::RichDocument document);

} // namespace folio

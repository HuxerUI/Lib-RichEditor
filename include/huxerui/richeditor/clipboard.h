#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <huxerui/clipboard.h>

namespace huxerui::richeditor {

/// Attachment bytes keyed by their original document references.
using ClipboardAttachments = std::map<std::string, Bytes, std::less<>>;

/// Owns the resources staged for one paste until the editor accepts it.
class StagedClipboardAttachments final {
public:
  /// Maps each original reference to its reference in the destination document.
  using References = std::map<std::string, std::string, std::less<>>;

  /// Supplies the new references and an optional nonthrowing rollback action.
  /// Rollback can run on the application thread or a worker when a pending paste is discarded.
  explicit StagedClipboardAttachments(References references, std::function<void()> rollback = {});
  ~StagedClipboardAttachments();

  StagedClipboardAttachments(const StagedClipboardAttachments&) = delete;
  StagedClipboardAttachments& operator=(const StagedClipboardAttachments&) = delete;
  StagedClipboardAttachments(StagedClipboardAttachments&& other) noexcept;
  StagedClipboardAttachments& operator=(StagedClipboardAttachments&& other) noexcept;

  const References& DestinationReferences() const noexcept { return references_; }

  /// Keeps the resources available to the destination, including across undo and redo.
  void Commit() noexcept;

private:
  void Rollback() noexcept;

  References references_;
  std::function<void()> rollback_;
};

/// Optional application storage integration for the mounted editor's clipboard actions.
///
/// Absolute HTTP and HTTPS references with an authority are carried as URLs. Other resource references
/// travel with their bytes. Keep the provider instance stable while its storage context stays the same.
struct ClipboardAttachmentProvider {
  /// Reads one value per requested reference from the source document's storage. The request contains
  /// distinct references. max_bytes counts only the sum of the returned byte buffers.
  std::function<Task<IoResult<ClipboardAttachments>>(std::vector<std::string> references, std::size_t max_bytes)> read;

  /// Stages the supplied bytes and returns a destination reference for every entry. Treat the original
  /// references as identifiers from the clipboard. Use the supplied bytes as the source of resource data.
  std::function<Task<IoResult<StagedClipboardAttachments>>(ClipboardAttachments)> stage;
};

/// Reason the editor could not publish a copy or cut.
enum class ClipboardWriteFailureReason {
  AttachmentUnavailable, ///< An attachment could not be read or the returned set was incomplete.
  TooLarge,               ///< The required representations exceed the editor's size limits.
  UnsupportedFormat,      ///< The available formats cannot preserve the selection for a cut.
  PublicationRejected,    ///< The system clipboard rejected the publication.
};

struct ClipboardWriteFailure {
  TextEditingAction action; ///< Copy or Cut.
  ClipboardWriteFailureReason reason;
};

} // namespace huxerui::richeditor

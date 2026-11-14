#pragma once

#include <huxerui/clipboard.h>
#include <huxerui/richeditor.h>

namespace huxerui::richeditor::detail {

inline constexpr char fragment_clipboard_type[] = "application/vnd.huxerui.richeditor.fragment";
inline constexpr std::size_t fragment_clipboard_limit = 16 * 1024 * 1024;
inline constexpr std::size_t clipboard_text_limit = 16 * 1024 * 1024;

struct ClipboardFragment {
  DocumentFragment fragment;
  ClipboardAttachments attachments;
};

std::vector<std::string> ClipboardAttachmentReferences(const RichDocument& document);
bool IsPlainClipboardSelection(const DocumentFragment& fragment);
ClipboardItem ClipboardRepresentations(std::string plain, const std::optional<DocumentFragment>& fragment);
DocumentFragment FragmentFromAttributedText(const AttributedText& text);
std::optional<Bytes> EncodeClipboardFragment(const DocumentFragment& fragment);
std::optional<std::size_t> ClipboardAttachmentBudget(std::size_t fragment_size, std::span<const std::string> references);
bool HasAllClipboardAttachments(std::span<const std::string> references, const ClipboardAttachments& attachments);
bool AppendClipboardAttachments(Bytes& payload, const ClipboardAttachments& attachments);
std::optional<ClipboardFragment> DecodeClipboardFragment(std::span<const std::byte> payload);
std::optional<DocumentFragment> RebaseClipboardFragment(
    DocumentFragment fragment, const StagedClipboardAttachments::References& references);
bool SupportsClipboardFragmentWrite();

} // namespace huxerui::richeditor::detail

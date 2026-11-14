#include "editor_clipboard.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include <huxerui/richeditor/format.h>

#include "document_internal.h"

namespace huxerui::richeditor {

StagedClipboardAttachments::StagedClipboardAttachments(References references, std::function<void()> rollback)
    : references_(std::move(references)), rollback_(std::move(rollback)) {}

StagedClipboardAttachments::~StagedClipboardAttachments() { Rollback(); }

StagedClipboardAttachments::StagedClipboardAttachments(StagedClipboardAttachments&& other) noexcept
    : references_(std::move(other.references_)), rollback_(std::exchange(other.rollback_, nullptr)) {}

StagedClipboardAttachments& StagedClipboardAttachments::operator=(StagedClipboardAttachments&& other) noexcept {
  if (this != &other) {
    Rollback();
    references_ = std::move(other.references_);
    rollback_ = std::exchange(other.rollback_, nullptr);
  }
  return *this;
}

void StagedClipboardAttachments::Commit() noexcept { rollback_ = nullptr; }

void StagedClipboardAttachments::Rollback() noexcept {
  if (auto rollback = std::exchange(rollback_, nullptr)) {
    rollback();
  }
}

namespace detail {
namespace {

#if defined(__EMSCRIPTEN__)
EM_JS(int, CanWriteWebClipboardFormat, (const char* mime), {
  const item = globalThis.ClipboardItem;
  return typeof item?.supports === "function" && item.supports("web " + UTF8ToString(mime));
});
#endif

constexpr unsigned fragment_version = 1;
constexpr unsigned leading_partial_flag = 1;
constexpr unsigned trailing_partial_flag = 2;
constexpr std::size_t attachment_limit = 1024;
constexpr std::size_t attachment_length_bytes = 8;

void AppendLength(Bytes& payload, std::size_t size) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    payload.push_back(static_cast<std::byte>((size >> shift) & 0xff));
  }
}

std::optional<std::span<const std::byte>> TakeField(std::span<const std::byte>& payload) {
  if (payload.size() < 4) {
    return std::nullopt;
  }
  std::size_t size = 0;
  for (unsigned i = 0; i < 4; ++i) {
    size |= std::to_integer<std::size_t>(payload[i]) << (8 * i);
  }
  payload = payload.subspan(4);
  if (size > payload.size()) {
    return std::nullopt;
  }
  auto field = payload.first(size);
  payload = payload.subspan(size);
  return field;
}

std::string_view AsText(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

TextSpanStyle ClipboardStyle(Mark marks) {
  TextSpanStyle style;
  if ((marks & Mark::Bold) != Mark::None) {
    style.font_weight = FontWeight::Bold;
  }
  if ((marks & Mark::Italic) != Mark::None) {
    style.font_slant = FontSlant::Italic;
  }
  auto decoration = TextDecoration::None;
  if ((marks & Mark::Underline) != Mark::None) {
    decoration = decoration | TextDecoration::Underline;
  }
  if ((marks & Mark::Strike) != Mark::None) {
    decoration = decoration | TextDecoration::StrikeThrough;
  }
  if (decoration != TextDecoration::None) {
    style.decoration = decoration;
  }
  return style;
}

Mark ClipboardMarks(const TextSpanStyle& style) {
  auto marks = Mark::None;
  if (style.font_weight.value_or(FontWeight::Regular) >= FontWeight::SemiBold) {
    marks = marks | Mark::Bold;
  }
  if (style.font_slant == FontSlant::Italic) {
    marks = marks | Mark::Italic;
  }
  const auto decoration = style.decoration.value_or(TextDecoration::None);
  if ((decoration & TextDecoration::Underline) != TextDecoration::None) {
    marks = marks | Mark::Underline;
  }
  if ((decoration & TextDecoration::StrikeThrough) != TextDecoration::None) {
    marks = marks | Mark::Strike;
  }
  return marks;
}

AttributedText ClipboardText(const RichDocument& document, std::string plain) {
  const auto& index = DocumentAccess::Get(document).index;
  std::vector<TextStyleRange> styles;
  std::vector<TextLinkRange> links;
  TextOffset offset = 0;
  auto leaves = index.Leaves(0);
  for (std::size_t i = 0; i < index.LeafCount(); ++i, leaves.Next()) {
    const auto& data = leaves->block.Data();
    auto length = DocumentAccess::Body(leaves->block).Length();
    if (IsAtomicBlock(data.kind)) {
      length = AttributedText(data.image ? data.image->alt_text : data.custom ? data.custom->fallback_text : "---").Length();
    }

    std::vector<std::pair<TextOffset, TextOffset>> object_offsets;
    TextOffset extra = 0;
    for (const auto& object : data.inline_objects) {
      extra += AttributedText(object.data.fallback_text).Length() - 1;
      object_offsets.emplace_back(object.offset + 1, extra);
    }
    const auto remap = [&](TextOffset position) {
      const auto next = std::ranges::upper_bound(object_offsets, position, {}, &std::pair<TextOffset, TextOffset>::first);
      return offset + position + (next == object_offsets.begin() ? 0 : std::prev(next)->second);
    };
    for (const auto& mark : data.marks) {
      const auto style = ClipboardStyle(mark.marks);
      if (style != TextSpanStyle{}) {
        styles.push_back({{remap(mark.range.start), remap(mark.range.end)}, style});
      }
    }
    for (const auto& link : data.links) {
      if (auto uri = Uri::Parse(link.target)) {
        links.push_back({{remap(link.range.start), remap(link.range.end)}, std::move(*uri)});
      }
    }
    offset += length + extra + 1;
  }
  return AttributedText::FromRanges(std::move(plain), std::move(styles), std::move(links));
}

bool IsRemoteResource(std::string_view source) {
  const auto uri = Uri::Parse(source);
  if (!uri || uri->Authority().value_or("").empty()) {
    return false;
  }
  const auto lower_ascii = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
  return std::ranges::equal(uri->Scheme(), std::string_view("http"), {}, lower_ascii) ||
         std::ranges::equal(uri->Scheme(), std::string_view("https"), {}, lower_ascii);
}

template <class Data, class Visit>
void VisitBlockReferences(Data& data, const Visit& visit) {
  if (data.image) {
    visit(data.image->source);
  }
  for (auto& object : data.inline_objects) {
    for (auto& attachment : object.data.attachments) {
      visit(attachment.source);
    }
  }
}

void CollectAttachmentReferences(std::span<const Block> blocks, std::set<std::string, std::less<>>& references) {
  for (const auto& block : blocks) {
    const auto& data = block.Data();
    VisitBlockReferences(data, [&](const std::string& source) {
      if (!IsRemoteResource(source)) {
        references.insert(source);
      }
    });
    CollectAttachmentReferences(data.children, references);
  }
}

Block RebaseClipboardBlock(const Block& block, const StagedClipboardAttachments::References& references) {
  auto data = block.Data();
  VisitBlockReferences(data, [&](std::string& source) {
    if (const auto found = references.find(source); found != references.end()) {
      source = found->second;
    }
  });
  for (auto& child : data.children) {
    child = RebaseClipboardBlock(child, references);
  }
  return Block(std::move(data));
}

bool ReserveAttachmentReference(std::size_t& remaining, std::string_view reference) {
  if (reference.empty() || remaining < attachment_length_bytes ||
      reference.size() > remaining - attachment_length_bytes) {
    return false;
  }
  remaining -= attachment_length_bytes + reference.size();
  return true;
}

} // namespace

std::vector<std::string> ClipboardAttachmentReferences(const RichDocument& document) {
  std::set<std::string, std::less<>> references;
  CollectAttachmentReferences(document.Blocks(), references);
  return {references.begin(), references.end()};
}

bool IsPlainClipboardSelection(const DocumentFragment& fragment) {
  return std::ranges::all_of(fragment.content.Blocks(), [](const Block& block) {
    const auto& data = block.Data();
    return data.kind == BlockKind::Paragraph && data.marks.empty() && data.links.empty() &&
           data.inline_objects.empty() && data.text.find('\n') == std::string::npos;
  });
}

ClipboardItem ClipboardRepresentations(std::string plain, const std::optional<DocumentFragment>& fragment) {
  try {
    if (fragment) {
      return ClipboardItem::FromAttributedText(ClipboardText(fragment->content, plain));
    }
  } catch (const std::invalid_argument&) {
  } catch (const std::length_error&) {
  }
  return {.text = std::move(plain)};
}

DocumentFragment FragmentFromAttributedText(const AttributedText& text) {
  BlockData data{.text = text.PlainText()};
  for (const auto& range : text.StyleRanges()) {
    if (const auto marks = ClipboardMarks(range.style); marks != Mark::None) {
      data.marks.push_back({range.range, marks});
    }
  }
  for (const auto& range : text.LinkRanges()) {
    data.links.push_back({range.range, range.target.ToString(), {}});
  }
  return {RichDocument::Create({Block(std::move(data))}), true, true};
}

std::optional<Bytes> EncodeClipboardFragment(const DocumentFragment& fragment) {
  // The version and boundary flags precede length-prefixed JSON, then pairs of attachment names and bytes.
  const auto encoded = JsonCodec().Export(fragment.content);
  constexpr std::size_t header_size = 6;
  if (!encoded.text || encoded.text->size() > fragment_clipboard_limit - header_size) {
    return std::nullopt;
  }
  const unsigned flags = (fragment.leading_partial ? leading_partial_flag : 0) |
                         (fragment.trailing_partial ? trailing_partial_flag : 0);
  Bytes payload;
  payload.reserve(header_size + encoded.text->size());
  payload.push_back(static_cast<std::byte>(fragment_version));
  payload.push_back(static_cast<std::byte>(flags));
  AppendLength(payload, encoded.text->size());
  const auto json = std::as_bytes(std::span(*encoded.text));
  payload.insert(payload.end(), json.begin(), json.end());
  return payload;
}

std::optional<std::size_t> ClipboardAttachmentBudget(std::size_t fragment_size, std::span<const std::string> references) {
  if (fragment_size > fragment_clipboard_limit || references.size() > attachment_limit) {
    return std::nullopt;
  }
  auto remaining = fragment_clipboard_limit - fragment_size;
  for (const auto& reference : references) {
    if (!ReserveAttachmentReference(remaining, reference)) {
      return std::nullopt;
    }
  }
  return remaining;
}

bool HasAllClipboardAttachments(std::span<const std::string> references, const ClipboardAttachments& attachments) {
  return references.size() == attachments.size() &&
      std::ranges::all_of(references, [&](const std::string& reference) { return attachments.contains(reference); });
}

bool AppendClipboardAttachments(Bytes& payload, const ClipboardAttachments& attachments) {
  if (attachments.size() > attachment_limit || payload.size() > fragment_clipboard_limit) {
    return false;
  }
  auto remaining = fragment_clipboard_limit - payload.size();
  for (const auto& [source, bytes] : attachments) {
    if (!ReserveAttachmentReference(remaining, source) || bytes.size() > remaining) {
      return false;
    }
    remaining -= bytes.size();
  }
  payload.reserve(fragment_clipboard_limit - remaining);
  for (const auto& [source, bytes] : attachments) {
    AppendLength(payload, source.size());
    const auto name = std::as_bytes(std::span(source));
    payload.insert(payload.end(), name.begin(), name.end());
    AppendLength(payload, bytes.size());
    payload.insert(payload.end(), bytes.begin(), bytes.end());
  }
  return true;
}

std::optional<ClipboardFragment> DecodeClipboardFragment(std::span<const std::byte> payload) {
  if (payload.size() < 6 || payload.size() > fragment_clipboard_limit ||
      std::to_integer<unsigned>(payload[0]) != fragment_version) {
    return std::nullopt;
  }
  const auto flags = std::to_integer<unsigned>(payload[1]);
  if ((flags & ~(leading_partial_flag | trailing_partial_flag)) != 0) {
    return std::nullopt;
  }
  payload = payload.subspan(2);
  const auto json = TakeField(payload);
  if (!json) {
    return std::nullopt;
  }
  auto imported = JsonCodec().Import(AsText(*json));
  if (!imported.document) {
    return std::nullopt;
  }
  const auto references = ClipboardAttachmentReferences(*imported.document);
  if (references.size() > attachment_limit || (!payload.empty() && references.empty())) {
    return std::nullopt;
  }
  ClipboardAttachments attachments;
  while (!payload.empty()) {
    if (attachments.size() == references.size()) {
      return std::nullopt;
    }
    const auto name = TakeField(payload);
    const auto bytes = TakeField(payload);
    if (!name || !bytes || !std::ranges::binary_search(references, AsText(*name)) ||
        !attachments.try_emplace(std::string(AsText(*name)), bytes->begin(), bytes->end()).second) {
      return std::nullopt;
    }
  }
  if (attachments.size() != references.size()) {
    return std::nullopt;
  }
  return ClipboardFragment{{std::move(*imported.document), (flags & leading_partial_flag) != 0,
                            (flags & trailing_partial_flag) != 0}, std::move(attachments)};
}

std::optional<DocumentFragment> RebaseClipboardFragment(
    DocumentFragment fragment, const StagedClipboardAttachments::References& references) {
  const auto sources = ClipboardAttachmentReferences(fragment.content);
  if (sources.size() != references.size() ||
      !std::ranges::all_of(sources, [&](const std::string& source) { return references.contains(source); })) {
    return std::nullopt;
  }
  try {
    std::vector<Block> blocks;
    blocks.reserve(fragment.content.Blocks().size());
    for (const auto& block : fragment.content.Blocks()) {
      blocks.push_back(RebaseClipboardBlock(block, references));
    }
    fragment.content = RichDocument::Create(std::move(blocks));
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  return fragment;
}

bool SupportsClipboardFragmentWrite() {
#if defined(__EMSCRIPTEN__)
  return CanWriteWebClipboardFormat(fragment_clipboard_type);
#else
  return true;
#endif
}

} // namespace detail
} // namespace huxerui::richeditor

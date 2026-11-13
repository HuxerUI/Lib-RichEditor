#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace huxerui::richeditor {
class RichDocument;
class RichEditorController;
}

namespace folio {

/// A top-level block near the viewport top and a fingerprint of its leading text, when present.
/// The fingerprint helps locate a displaced block without copying document text into the workspace.
struct ReadingPosition {
  std::uint64_t block_index;
  std::uint64_t text_fingerprint;
  bool operator==(const ReadingPosition&) const = default;
};

/// Captures a position in the same committed snapshot as the viewport query. The first block needs no restoration.
std::optional<ReadingPosition> CaptureReadingPosition(const huxerui::richeditor::RichDocument& document,
                                                      std::size_t block_index);

/// Reveals the saved block or a nearby heading after layout is ready. Keeps focus and selection intact.
void RestoreReadingPosition(const huxerui::richeditor::RichEditorController& editor, ReadingPosition position);

} // namespace folio

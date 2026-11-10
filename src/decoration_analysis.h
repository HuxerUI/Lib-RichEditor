#pragma once

#include <unordered_set>

#include "editor_view_internal.h"

namespace huxerui::richeditor::detail {

struct DecorationAnalysis {
  struct Entry {
    Block block;
    std::vector<TextDecorationRange> ranges;
  };

  struct Layer {
    std::shared_ptr<const DecorationProvider> provider;
    std::unordered_map<BlockId, Entry> blocks;
    std::unordered_set<BlockId> pending;
    std::optional<TaskHandle> request;
    bool analyze_all = true;
  };

  RichDocument document;
  std::uint64_t generation;
  std::uint64_t revision;
  std::vector<std::shared_ptr<Layer>> layers;
};

void ScheduleDecorations(const std::shared_ptr<EditorSession>& editor,
                         const std::shared_ptr<EditorViewport>& viewport, const TaskScope& tasks,
                         const std::vector<std::shared_ptr<const DecorationProvider>>& providers);

} // namespace huxerui::richeditor::detail

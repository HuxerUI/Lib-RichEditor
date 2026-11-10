#include "decoration_analysis.h"

#include <stdexcept>

#include <huxerui/lifecycle.h>

#include "editor_session.h"

namespace huxerui::richeditor::detail {
namespace {

bool IsValidDecorationRange(const Block& block, TextRange range) {
  const auto& body = DocumentAccess::Body(block);
  if (!range.IsValid() || range.IsCollapsed() || range.end > body.Length()) {
    return false;
  }
  try {
    (void)body.TextInRange({range.start, range.start});
    (void)body.TextInRange({range.end, range.end});
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

Task<void> AnalyzeDecorations(std::shared_ptr<DecorationAnalysis> analysis,
                              std::weak_ptr<EditorViewport> viewport,
                              std::shared_ptr<DecorationAnalysis::Layer> current_layer) {
  auto& layer = *current_layer;
  const auto& provider = *layer.provider;
  const bool whole_document = provider.scope == DecorationScope::Document;
  do {
    const auto document = analysis->document;
    const auto& storage = DocumentAccess::Get(document);
    DecorationRequest request;
    std::vector<Block> blocks;
    const auto append = [&](const Leaf& leaf) {
      blocks.push_back(leaf.block);
      request.blocks.push_back({leaf.block, whole_document ? leaf.path : BlockPath{}});
    };
    if (whole_document) {
      request.document = document;
    }
    if (layer.analyze_all) {
      for (auto leaf = storage.index.Leaves(); leaf; leaf.Next()) {
        if (IsTextBlock(leaf->block.Data().kind)) {
          append(*leaf);
        }
      }
    } else {
      std::vector<std::size_t> indexes;
      indexes.reserve(layer.pending.size());
      for (const auto id : layer.pending) {
        indexes.push_back(storage.index.IndexOf(id).value());
      }
      std::ranges::sort(indexes);
      for (const auto index : indexes) {
        append(storage.index.At(index));
      }
    }
    layer.analyze_all = false;
    layer.pending.clear();
    if (blocks.empty() && !whole_document) {
      break;
    }

    std::vector<TextDecorationRange> ranges;
    try {
      ranges = co_await provider.analyze(std::move(request));
    } catch (const std::exception&) {
      // A failed request completes with empty results for its current blocks.
    }
    const auto view = viewport.lock();
    if (!view || view->decoration_analysis != analysis ||
        (whole_document && !DocumentAccess::Same(document, analysis->document))) {
      co_return;
    }

    // Unchanged blocks in an incremental batch can still be current after an edit.
    if (!whole_document) {
      const auto& current = DocumentAccess::Get(analysis->document);
      for (const auto& block : blocks) {
        const auto id = DocumentAccess::Id(block);
        if (const auto* leaf = FindTextBlock(current.index, id); leaf && *leaf == block) {
          layer.blocks.try_emplace(id, DecorationAnalysis::Entry{block, {}});
          layer.pending.erase(id);
        }
      }
    }
    bool painted = false;
    for (auto& range : ranges) {
      if (range.block_index >= blocks.size()) {
        continue;
      }
      const auto& block = blocks[range.block_index];
      if (!IsValidDecorationRange(block, range.range)) {
        continue;
      }
      const auto id = DocumentAccess::Id(block);
      const auto entry = whole_document ? layer.blocks.try_emplace(id, DecorationAnalysis::Entry{block, {}}).first
                                        : layer.blocks.find(id);
      if (entry != layer.blocks.end()) {
        entry->second.ranges.push_back(std::move(range));
        painted = true;
      }
    }
    if (painted) {
      view->decoration_revision = view->decoration_revision.Get() + 1;
    }
  } while (!whole_document && !layer.pending.empty());
  layer.request.reset();
}

void CancelRequest(DecorationAnalysis::Layer& layer) {
  if (layer.request) {
    layer.request->Cancel();
    layer.request.reset();
  }
}

bool UpdateBlockCache(const DocumentIndex& index, DecorationAnalysis::Layer& layer,
                      std::span<const BlockId> changed_blocks) {
  bool repaint = false;
  for (const auto id : changed_blocks) {
    const auto* leaf = FindTextBlock(index, id);
    const auto entry = layer.blocks.find(id);
    if (entry != layer.blocks.end()) {
      if (leaf && *leaf == entry->second.block) {
        continue;
      }
      repaint |= !entry->second.ranges.empty();
      layer.blocks.erase(entry);
    }
    if (leaf) {
      layer.pending.insert(id);
    } else {
      layer.pending.erase(id);
    }
  }
  return repaint;
}

bool UpdateProviders(DecorationAnalysis& analysis,
                     const std::vector<std::shared_ptr<const DecorationProvider>>& providers) {
  const auto provider_of = [](const auto& layer) { return layer->provider; };
  if (std::ranges::equal(providers, analysis.layers, std::equal_to{}, std::identity{}, provider_of)) {
    return false;
  }
  auto previous = std::move(analysis.layers);
  analysis.layers.clear();
  analysis.layers.reserve(providers.size());
  for (const auto& provider : providers) {
    const auto found = std::ranges::find_if(previous, [&](const auto& layer) {
      return layer && layer->provider == provider;
    });
    if (found != previous.end()) {
      analysis.layers.push_back(std::move(*found));
    } else {
      analysis.layers.push_back(std::make_shared<DecorationAnalysis::Layer>(
          DecorationAnalysis::Layer{.provider = provider}));
    }
  }
  for (const auto& retired : previous) {
    if (retired) {
      CancelRequest(*retired);
    }
  }
  return true;
}

void ClearAnalysis(EditorViewport& viewport) {
  if (const auto analysis = std::exchange(viewport.decoration_analysis, {})) {
    for (const auto& layer : analysis->layers) {
      CancelRequest(*layer);
    }
  }
}

} // namespace

void ScheduleDecorations(const std::shared_ptr<EditorSession>& editor,
                         const std::shared_ptr<EditorViewport>& viewport, const TaskScope& tasks,
                         const std::vector<std::shared_ptr<const DecorationProvider>>& providers) {
  (void)viewport->decoration_revision.Get();

  Lifecycle([viewport] {
    return [viewport] { ClearAnalysis(*viewport); };
  }, viewport);
  Lifecycle([=] {
    const auto& analysis = viewport->decoration_analysis;
    const auto generation = editor->Generation();
    if (analysis && (providers.empty() || analysis->generation != generation)) {
      ClearAnalysis(*viewport);
      if (providers.empty()) {
        viewport->decoration_revision = viewport->decoration_revision.Get() + 1;
      }
    }
    if (providers.empty()) {
      return;
    }
    const auto document = editor->Document();
    const auto revision = editor->CommittedRevision();
    if (!analysis) {
      viewport->decoration_analysis = std::make_shared<DecorationAnalysis>(
          DecorationAnalysis{document, generation, revision, {}});
    }
    bool repaint = UpdateProviders(*analysis, providers);
    const bool document_changed = analysis->revision != revision;
    std::vector<BlockId> changed_blocks;
    const bool needs_delta = document_changed && std::ranges::any_of(analysis->layers, [](const auto& layer) {
      return layer->provider && layer->provider->analyze && layer->provider->scope == DecorationScope::Block &&
          !layer->analyze_all;
    });
    const bool has_delta = !needs_delta || editor->TextBlockChangesSince(analysis->revision, changed_blocks);
    analysis->document = document;
    analysis->revision = revision;
    for (const auto& layer : analysis->layers) {
      if (!layer->provider || !layer->provider->analyze) {
        continue;
      }
      const bool whole_document = layer->provider->scope == DecorationScope::Document;
      if (document_changed && (whole_document || !has_delta)) {
        CancelRequest(*layer);
        repaint |= !layer->blocks.empty();
        layer->blocks.clear();
        layer->pending.clear();
        layer->analyze_all = true;
      } else if (document_changed && !layer->analyze_all) {
        repaint |= UpdateBlockCache(DocumentAccess::Get(document).index, *layer, changed_blocks);
      }
      if (!layer->request && (layer->analyze_all || !layer->pending.empty())) {
        layer->request = tasks.Launch(AnalyzeDecorations(analysis, viewport, layer));
      }
    }
    if (repaint) {
      viewport->decoration_revision = viewport->decoration_revision.Get() + 1;
    }
  }, editor->Generation(), providers.empty() ? 0 : editor->CommittedRevision(), providers);
}

} // namespace huxerui::richeditor::detail

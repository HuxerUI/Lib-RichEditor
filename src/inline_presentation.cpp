#include "inline_presentation.h"

#include <cmath>
#include <exception>
#include <unordered_set>

#include <huxerui/lifecycle.h>

#include "editor_session.h"

namespace huxerui::richeditor::detail {

struct InlineRendering {
  struct Request {
    InlineObjectData data;
    InlineObjectRenderContext context;
    std::shared_ptr<const InlineObjectRenderer> renderer;
    bool operator==(const Request&) const = default;
  };

  struct Entry {
    std::shared_ptr<const Request> request;
    TaskHandle task;
  };

  std::unordered_map<std::uint64_t, Entry> entries;
};

namespace {

Task<void> ResolveInline(std::weak_ptr<EditorViewport> viewport, std::uint64_t id,
                         std::shared_ptr<const InlineRendering::Request> request) {
  std::optional<InlineObjectGraphic> graphic;
  try {
    graphic = co_await (*request->renderer)(request->data, request->context);
  } catch (const std::exception&) {
    co_return;
  }
  if (!graphic || !std::isfinite(graphic->width) || !std::isfinite(graphic->ascent + graphic->descent) ||
      graphic->width <= 0 || graphic->ascent < 0 || graphic->descent < 0 || graphic->ascent + graphic->descent <= 0 ||
      !std::visit([](const auto& asset) { return asset.HasValue(); }, graphic->asset)) {
    co_return;
  }
  if (const auto view = viewport.lock(); view && view->inline_rendering) {
    const auto found = view->inline_rendering->entries.find(id);
    if (found != view->inline_rendering->entries.end() && found->second.request == request) {
      view->inline_graphics[id] = std::make_shared<const ResolvedInlineGraphic>(
          ResolvedInlineGraphic{request->data, request->context, request->renderer, std::move(*graphic)});
      view->inline_revision = view->inline_revision.Get() + 1;
    }
  }
}

void ClearInlineRendering(EditorViewport& viewport) {
  if (const auto rendering = std::exchange(viewport.inline_rendering, {})) {
    for (auto& [id, entry] : rendering->entries) {
      entry.task.Cancel();
    }
    viewport.inline_graphics.clear();
  }
}

} // namespace

void ScheduleInlineObjects(const std::shared_ptr<EditorSession>& editor,
                           const std::shared_ptr<EditorViewport>& viewport, const TaskScope& tasks,
                           const std::optional<File>& directory) {
  (void)viewport->inline_revision.Get();
  const auto providers = viewport->extensions.inline_objects;
  const auto generation = editor->Generation();
  const auto document = editor->Document();
  const auto theme = viewport->theme;
  const auto style = viewport->style;
  const auto locale = viewport->locale;
  Lifecycle([viewport] {
    return [viewport] { ClearInlineRendering(*viewport); };
  }, viewport);
  Lifecycle([=] {
    if (!viewport->inline_rendering && providers.empty()) {
      return;
    }
    if (!viewport->inline_rendering) {
      viewport->inline_rendering = std::make_shared<InlineRendering>();
    }
    auto& entries = viewport->inline_rendering->entries;
    std::unordered_set<std::uint64_t> retained;
    bool changed = false;
    DocumentAccess::Get(document).index.VisitInlineBlocks([&](const Block& block) {
      for (const auto& object : block.Data().inline_objects) {
        const auto extension = std::ranges::find(providers, object.data.type, &InlineObjectExtension::type);
        if (extension == providers.end() || !extension->render || !*extension->render) {
          continue;
        }
        const auto id = DocumentAccess::Id(object);
        retained.insert(id);
        const InlineRendering::Request request{
            object.data, ResolveInlineContext(block.Data(), object.offset, theme, style, locale, directory),
            extension->render};
        const auto existing = entries.find(id);
        if (existing != entries.end()) {
          if (*existing->second.request == request) {
            continue;
          }
          existing->second.task.Cancel();
          changed |= viewport->inline_graphics.erase(id) != 0;
        }
        auto shared = std::make_shared<const InlineRendering::Request>(request);
        auto task = tasks.Launch(ResolveInline(viewport, id, shared));
        entries.insert_or_assign(id, InlineRendering::Entry{std::move(shared), std::move(task)});
      }
    });
    std::erase_if(entries, [&](const auto& item) {
      if (retained.contains(item.first)) {
        return false;
      }
      item.second.task.Cancel();
      changed |= viewport->inline_graphics.erase(item.first) != 0;
      return true;
    });
    if (changed) {
      viewport->inline_revision = viewport->inline_revision.Get() + 1;
    }
  }, generation, providers.empty() ? nullptr : DocumentAccess::Storage(document), providers, theme, style, locale,
     directory);
}

} // namespace huxerui::richeditor::detail

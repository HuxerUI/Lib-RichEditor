#pragma once

#include <functional>
#include <memory>

#include <huxerui/event.h>
#include <huxerui/presentation.h>
#include <huxerui/richeditor.h>
#include <huxerui/scroll.h>
#include <huxerui/state.h>
#include <huxerui/theme.h>

#include "editor_layout.h"

namespace huxerui::richeditor::detail {

Task<IoResult<ImageAsset>> ResolveImageFile(File directory, std::string source);

struct DecorationAnalysis;
struct InlineRendering;

struct EditorViewport {
  using ScrollInputHandler = std::function<bool(const ScrollInputEvent&)>;
  using PointerInputHandler = std::function<void(const PointerEvent&)>;
  using RevealTextHandler = std::function<bool(const RichDocument&, TextRange, std::optional<ScrollAlignment>)>;
  using ViewportLocationHandler = std::function<std::optional<EditorViewportLocation>(const RichDocument&, float)>;
  using PastePlainTextHandler = std::function<bool()>;

  struct Geometry {
    ScrollMetrics scroll;
    std::optional<std::size_t> revealed_root_block_index;
    std::uint64_t layout_revision = 0;
    bool operator==(const Geometry&) const = default;
  };

  ScrollController scroll;
  /// The mounted surface owns the handler and its node-local coordinate mapping.
  std::weak_ptr<ScrollInputHandler> scroll_input;
  /// Forwards pointer input from the internal scroll view to the mounted surface.
  std::weak_ptr<PointerInputHandler> pointer_input;
  State<float> height{80.0F};
  ThemeSpec theme;
  RichEditorStyle style;
  RichEditorExtensions extensions;
  std::string locale;
  std::optional<PopupHandle> code_language_popup;
  std::optional<ViewHandle> code_language_anchor;
  std::shared_ptr<const ImageResolver> image_resolver;
  std::optional<File> image_directory;
  std::optional<File> document_directory;
  std::uint64_t image_generation = 0;
  State<std::uint64_t> image_revision{0};
  ImageAssets images;
  InlineGraphics inline_graphics;
  State<std::uint64_t> inline_revision{0};
  std::shared_ptr<InlineRendering> inline_rendering;
  std::shared_ptr<DecorationAnalysis> decoration_analysis;
  State<std::uint64_t> decoration_revision{0};
  State<float> content_width{0.0F};
  std::weak_ptr<RevealTextHandler> reveal_text;
  State<Geometry> geometry{Geometry{}};
  std::weak_ptr<ViewportLocationHandler> viewport_location;
  std::weak_ptr<PastePlainTextHandler> paste_plain_text;
  std::shared_ptr<Clipboard> clipboard;
  TaskScope clipboard_tasks;
};

} // namespace huxerui::richeditor::detail

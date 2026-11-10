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

struct EditorViewport {
  using ScrollInputHandler = std::function<bool(const ScrollInputEvent&)>;

  ScrollController scroll;
  /// The mounted surface owns the handler and its node-local coordinate mapping.
  std::weak_ptr<ScrollInputHandler> scroll_input;
  State<float> height{80.0F};
  ThemeSpec theme;
  RichEditorStyle style;
  RichEditorExtensions extensions;
  std::string locale;
  std::optional<PopupHandle> code_language_popup;
  std::optional<ViewHandle> code_language_anchor;
  std::shared_ptr<const ImageResolver> image_resolver;
  std::optional<File> image_directory;
  std::uint64_t image_generation = 0;
  State<std::uint64_t> image_revision{0};
  ImageAssets images;
  std::shared_ptr<DecorationAnalysis> decoration_analysis;
  State<std::uint64_t> decoration_revision{0};
  State<float> content_width{0.0F};
};

} // namespace huxerui::richeditor::detail

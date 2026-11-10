#include "formula.h"

#include <algorithm>

#include <huxerui/richeditor/math.h>

namespace folio {
namespace {

using namespace huxerui;
using namespace huxerui::richeditor;

#if defined(FOLIO_MATH_RENDERER)
struct FormulaPng {
  std::byte* data;
  std::size_t size;
  float ascent;
  float descent;
};

extern "C" FormulaPng folio_render_formula(const char* source, std::size_t size, float font_size,
                                            float red, float green, float blue, float alpha, bool inline_math);
extern "C" void folio_free_formula(FormulaPng png);

std::optional<CustomBlockGraphic> RenderFormula(const CustomBlockData& block, Color color, float font_size) {
  if (block.type != latex_block_type || block.version != 1) {
    return std::nullopt;
  }
  const auto png = folio_render_formula(block.payload.data(), block.payload.size(), font_size + 4.0F,
                                        color.red, color.green, color.blue, color.alpha, false);
  if (!png.data) {
    return std::nullopt;
  }
  try {
    auto image = ImageAsset::CopyEncoded({png.data, png.size}, 2.0F);
    folio_free_formula(png);
    return image;
  } catch (const std::exception&) {
    folio_free_formula(png);
    return std::nullopt;
  }
}

struct InlinePng {
  std::vector<std::byte> bytes;
  float ascent;
  float descent;
};

Task<std::optional<InlineObjectGraphic>> RenderInlineFormula(InlineObjectData data, InlineObjectRenderContext context) {
  if (data.type != latex_block_type || data.version != 1) {
    co_return std::nullopt;
  }
  auto png = co_await RunWorker([](std::string source, float font_size, Color color) -> std::optional<InlinePng> {
    const auto image = folio_render_formula(source.data(), source.size(), font_size,
                                            color.red, color.green, color.blue, color.alpha, true);
    if (!image.data) {
      return std::nullopt;
    }
    const auto free_image = [image](std::byte*) { folio_free_formula(image); };
    const std::unique_ptr<std::byte, decltype(free_image)> memory(image.data, free_image);
    return InlinePng{{image.data, image.data + image.size}, image.ascent, image.descent};
  }, std::move(data.payload), context.font_size, context.foreground);
  if (!png) {
    co_return std::nullopt;
  }
  auto asset = ImageAsset::CopyEncoded(png->bytes, 2.0F);
  const auto size = asset.IntrinsicSize();
  const auto ascent = std::clamp(png->ascent, 0.0F, size.height);
  co_return InlineObjectGraphic{std::move(asset), size.width, ascent, size.height - ascent, {}};
}
#endif

} // namespace

std::shared_ptr<const huxerui::richeditor::CustomBlockRenderer> FormulaRenderer() {
#if defined(FOLIO_MATH_RENDERER)
  static const auto renderer = std::make_shared<const CustomBlockRenderer>(RenderFormula);
  return renderer;
#else
  return {};
#endif
}

huxerui::richeditor::InlineObjectExtension InlineFormulaExtension() {
#if defined(FOLIO_MATH_RENDERER)
  static const auto renderer = std::make_shared<const InlineObjectRenderer>(RenderInlineFormula);
  return {std::string(latex_block_type), renderer};
#else
  return {std::string(latex_block_type), {}};
#endif
}

} // namespace folio

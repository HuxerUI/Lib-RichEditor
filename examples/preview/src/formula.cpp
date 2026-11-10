#include "formula.h"

#include <huxerui/richeditor/math.h>

namespace folio {
namespace {

using namespace huxerui;
using namespace huxerui::richeditor;

#if defined(FOLIO_MATH_RENDERER)
struct FormulaPng {
  std::byte* data;
  std::size_t size;
};

extern "C" FormulaPng folio_render_formula(const char* source, std::size_t size, float font_size,
                                            float red, float green, float blue, float alpha);
extern "C" void folio_free_formula(FormulaPng png);

std::optional<CustomBlockGraphic> RenderFormula(const CustomBlockData& block, Color color, float font_size) {
  if (block.type != latex_block_type || block.version != 1) {
    return std::nullopt;
  }
  const auto png = folio_render_formula(block.payload.data(), block.payload.size(), font_size + 4.0F,
                                        color.red, color.green, color.blue, color.alpha);
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

} // namespace folio

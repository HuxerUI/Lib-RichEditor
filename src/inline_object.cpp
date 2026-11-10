#include "document_internal.h"

#include <nlohmann/json.hpp>

namespace huxerui::richeditor {

InlineObjectData InlineImage(const ImageBlockData& image) {
  nlohmann::json payload{{"title", image.title}};
  auto fallback = detail::NormalizeLineEndings(image.alt_text);
  std::ranges::replace(fallback, '\n', ' ');
  if (fallback != image.alt_text) {
    payload["alt_text"] = image.alt_text;
  }
  if (image.width) {
    payload["width"] = *image.width;
  }
  return {std::string(inline_image_type), 1, payload.dump(), std::move(fallback), {{"image", image.source}}};
}

std::optional<ImageBlockData> ReadInlineImage(const InlineObjectData& object) {
  if (object.type != inline_image_type || object.version != 1 || object.attachments.size() != 1 ||
      object.attachments.front().role != "image") {
    return std::nullopt;
  }
  try {
    const auto payload = nlohmann::json::parse(object.payload);
    ImageBlockData image{object.attachments.front().source, payload.value("alt_text", object.fallback_text),
                         payload.at("title").get<std::string>()};
    if (const auto width = payload.find("width"); width != payload.end()) {
      image.width = width->get<float>();
    }
    if (detail::ValidImageData(image)) {
      return image;
    }
  } catch (const nlohmann::json::exception&) {
  }
  return std::nullopt;
}

} // namespace huxerui::richeditor

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <huxerui/file.h>
#include <huxerui/resource.h>

namespace folio::test {

class PackageResources final : public huxerui::PlatformResources {
public:
  explicit PackageResources(std::string root) : root_(std::move(root)) {}

  huxerui::ResourceConfiguration Configuration() const override { return {}; }
  std::optional<huxerui::InputStream> OpenRead(std::string_view path) override {
    auto result = huxerui::File(root_ + "/" + std::string(path)).OpenRead();
    if (!result.Succeeded()) {
      return std::nullopt;
    }
    return std::move(result).Value();
  }

private:
  std::string root_;
};

} // namespace folio::test

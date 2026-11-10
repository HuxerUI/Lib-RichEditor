#include <huxerui/file.h>
#include <huxerui/resource.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace huxerui::richeditor::detail {
namespace {

int HexDigit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return value >= 'A' && value <= 'F' ? value - 'A' + 10 : -1;
}

std::filesystem::path SystemPath(const File& file) {
  const auto path = file.Path();
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::optional<File> RelativeFile(File directory, std::string_view source) {
  if (source.empty() || source.front() == '/' || source.find_first_of("\\:?#") != source.npos) {
    return std::nullopt;
  }
  std::size_t depth = 0;
  while (!source.empty()) {
    const auto slash = source.find('/');
    const auto part = source.substr(0, slash);
    std::string decoded;
    for (std::size_t i = 0; i < part.size(); ++i) {
      char ch = part[i];
      if (ch == '%') {
        if (part.size() - i < 3) {
          return std::nullopt;
        }
        const auto first = HexDigit(part[i + 1]);
        const auto second = HexDigit(part[i + 2]);
        if (first < 0 || second < 0) {
          return std::nullopt;
        }
        ch = static_cast<char>(first * 16 + second);
        i += 2;
      }
      if (static_cast<unsigned char>(ch) < 32 || ch == '/' || ch == '\\' || ch == ':') {
        return std::nullopt;
      }
      decoded += ch;
    }
    if (decoded == "..") {
      if (depth == 0) {
        return std::nullopt;
      }
      directory = *directory.Parent();
      --depth;
    } else if (!decoded.empty() && decoded != ".") {
      directory = directory.Child(decoded);
      ++depth;
    }
    if (slash == source.npos) {
      break;
    }
    source.remove_prefix(slash + 1);
  }
  return depth != 0 ? std::optional(std::move(directory)) : std::nullopt;
}

IoResult<ImageAsset> ReadImage(File directory, std::string source) {
  try {
    const auto file = RelativeFile(directory, source);
    if (!file) {
      return IoResult<ImageAsset>::Failure({IoErrorCode::Unsupported, "Expected a relative image path"});
    }
    std::error_code error;
    const auto root = std::filesystem::canonical(SystemPath(directory), error);
    if (error) {
      return IoResult<ImageAsset>::Failure({IoErrorCode::NotFound, "Document directory is unavailable"});
    }
    const auto resolved = std::filesystem::canonical(SystemPath(*file), error);
    if (error) {
      return IoResult<ImageAsset>::Failure({IoErrorCode::NotFound, "Image file is unavailable"});
    }
    const auto prefix = std::ranges::mismatch(root, resolved);
    if (prefix.in1 != root.end() || prefix.in2 == resolved.end()) {
      return IoResult<ImageAsset>::Failure({IoErrorCode::Unsupported, "Image is outside the document directory"});
    }
    auto bytes = File(resolved.u8string()).ReadBytes();
    if (!bytes.Succeeded()) {
      return IoResult<ImageAsset>::Failure(bytes.Error());
    }
    return IoResult<ImageAsset>::Success(ImageAsset::FromEncoded(std::move(bytes.Value())));
  } catch (const std::invalid_argument&) {
    return IoResult<ImageAsset>::Failure({IoErrorCode::InvalidEncoding, "Invalid image path or PNG/JPEG asset"});
  }
}

} // namespace

Task<IoResult<ImageAsset>> ResolveImageFile(File directory, std::string source) {
  co_return co_await RunWorker(ReadImage, std::move(directory), std::move(source));
}

} // namespace huxerui::richeditor::detail

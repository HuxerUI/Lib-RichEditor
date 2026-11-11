#include "browser.h"

#include <huxerui/app.h>
#include <huxerui/window.h>

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#elif defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__linux__)
#include <gio/gio.h>
#endif

using namespace huxerui;

namespace folio {
namespace {

#if defined(__ANDROID__)
constexpr char browser_module[] = "folio/Browser";

class BrowserLauncher {
public:
  explicit BrowserLauncher(PlatformChannel channel) : channel_(std::move(channel)) {}

  void Open(const Uri& url, const ToastHandle& toast) const {
    channel_.Invoke<bool>("open", url.ToString(), [toast](PlatformResult<bool> result) {
      const auto* opened = std::get_if<bool>(&result);
      if (!opened || !*opened) {
        toast.Show("Could not open the link");
      }
    });
  }

private:
  PlatformChannel channel_;
};
#elif defined(__EMSCRIPTEN__)
EM_JS(int, OpenBrowserTab, (const char* url), {
  const tab = window.open('about:blank', '_blank');
  if (!tab) return 0;
  tab.opener = null;
  tab.location.replace(UTF8ToString(url));
  return 1;
});
#endif

} // namespace

#if defined(__ANDROID__)
void RegisterBrowser(ApplicationContext& context) {
  context.RegisterPlatformModule<BrowserLauncher>(browser_module,
      android::JavaWindowPlatformModuleFactory<BrowserLauncher>{
          .class_name = "org.huxerui.lib.richeditor.preview.BrowserPlatform",
          .create = [](PlatformChannel channel) { return BrowserLauncher(std::move(channel)); },
      });
}

void InstallBrowser(WindowContext& context) {
  context.Provide(std::make_shared<BrowserLauncher>(OpenPlatformModule<BrowserLauncher>(browser_module)));
}

void OpenBrowser(const Uri& url, const ToastHandle& toast) { UseService<BrowserLauncher>()->Open(url, toast); }
#else
void RegisterBrowser(ApplicationContext&) {}
void InstallBrowser(WindowContext&) {}

#if !defined(__APPLE__)
void OpenBrowser(const Uri& url, const ToastHandle& toast) {
#if defined(_WIN32)
  // Uri contains only ASCII, including percent escapes for non-ASCII addresses.
  const auto& text = url.ToString();
  const std::wstring address(text.begin(), text.end());
  const bool opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__EMSCRIPTEN__)
  const bool opened = OpenBrowserTab(url.ToString().c_str()) != 0;
#elif defined(__linux__)
  const bool opened = g_app_info_launch_default_for_uri(url.ToString().c_str(), nullptr, nullptr);
#else
  const bool opened = false;
#endif
  if (!opened) {
    toast.Show("Could not open the link");
  }
}
#endif
#endif

} // namespace folio

#pragma once

#include <huxerui/data.h>
#include <huxerui/presentation.h>

namespace huxerui {
class ApplicationContext;
class WindowContext;
}

namespace folio {

/// Requests browser navigation from the current window and reports a failed launch there.
void OpenBrowser(const huxerui::Uri& url, const huxerui::ToastHandle& toast);

void RegisterBrowser(huxerui::ApplicationContext& context);
void InstallBrowser(huxerui::WindowContext& context);

} // namespace folio

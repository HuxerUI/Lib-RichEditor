#include "browser.h"

#include <TargetConditionals.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

namespace folio {

void OpenBrowser(const huxerui::Uri& url, const huxerui::ToastHandle& toast) {
  auto* address = [NSURL URLWithString:[NSString stringWithUTF8String:url.ToString().c_str()]];
  if (!address) {
    toast.Show("Could not open the link");
    return;
  }
#if TARGET_OS_IOS
  [UIApplication.sharedApplication openURL:address options:@{} completionHandler:^(BOOL opened) {
    if (!opened) {
      toast.Show("Could not open the link");
    }
  }];
#else
  if (![NSWorkspace.sharedWorkspace openURL:address]) {
    toast.Show("Could not open the link");
  }
#endif
}

} // namespace folio

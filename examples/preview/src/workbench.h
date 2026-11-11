#pragma once

#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

namespace folio {

struct EditorPaneOptions {
  std::optional<huxerui::File> working_directory;
  std::shared_ptr<const huxerui::richeditor::ImageResolver> image_resolver;
  huxerui::State<bool> find_open;
  bool active;
  bool available;
  bool can_pick_images;
  std::function<void(huxerui::ViewHandle, bool replace, bool inline_image)> pick_image;
};

huxerui::View EditorPane(huxerui::richeditor::RichEditorController editor, EditorPaneOptions options);
huxerui::richeditor::ImportResult LoadSample();
huxerui::View Workbench();
huxerui::WindowOptions FolioWindowOptions();
void InstallFolio(huxerui::ApplicationContext& context);
void InstallWorkbench(huxerui::WindowContext& context);

} // namespace folio

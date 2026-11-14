#pragma once

#include <cstdint>

#include <huxerui/huxerui.h>
#include <huxerui/richeditor.h>
#include <huxerui/richeditor/format.h>

#include "document_links.h"

namespace folio {

class WorkspaceStore;

enum class FindField { None, Query, Replacement };

struct FindPanelState {
  bool open = false;
  bool replace_open = false;
  FindField focused = FindField::None;
  std::uint64_t focus_request = 0;
  bool operator==(const FindPanelState&) const = default;
};

struct EditorPaneOptions {
  std::optional<huxerui::File> working_directory;
  std::shared_ptr<const huxerui::richeditor::ImageResolver> image_resolver;
  std::shared_ptr<const huxerui::richeditor::ClipboardAttachmentProvider> clipboard_attachments;
  huxerui::State<FindPanelState> find;
  huxerui::ViewHandle focus;
  bool active;
  bool available;
  bool can_pick_images;
  std::function<void(huxerui::ViewHandle, bool replace, bool inline_image)> pick_image;
  std::function<void(DocumentLink)> open_document_link;
};

huxerui::View EditorPane(huxerui::richeditor::RichEditorController editor, EditorPaneOptions options);
huxerui::richeditor::ImportResult LoadSample();
using WorkbenchKeyHandler = std::function<bool(const huxerui::KeyEvent&)>;
huxerui::View Workbench(huxerui::State<WorkbenchKeyHandler> dispatch);
huxerui::WindowOptions FolioWindowOptions();
void InstallFolio(huxerui::ApplicationContext& context);
void InstallFolioWithWorkspace(huxerui::ApplicationContext& context, std::shared_ptr<WorkspaceStore> store,
                               std::optional<huxerui::File> data_directory = {});
void InstallWorkbench(huxerui::WindowContext& context);

} // namespace folio

#include "gameframe.h"

#include "graphics/renderer.h"
#include "frameprofile.h"
#include "inventorymenu.h"
#include "videowidget.h"

using namespace Tempest;

void drawGameFrame(Renderer& renderer, Attachment& result, Encoder<CommandBuffer>& cmd, uint8_t fId,
                   VectorImage::Mesh& uiLayer, VectorImage::Mesh& numOverlay,
                   InventoryMenu& inventory, VideoWidget& video) {
  renderer.drawScene(result,cmd,fId,!video.isActive());
  FrameProfile::Scope uiTiming(FrameProfile::Zone::EncodeUi);
  cmd.setDebugMarker("UI");
  uiLayer.draw(cmd);

  if(inventory.isOpen()!=InventoryMenu::State::Closed) {
    auto& zb = renderer.uiDepthBuffer();
    cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}},{zb, 1.f, Tempest::Preserve});
    cmd.setDebugMarker("Inventory");
    inventory.draw(cmd);

    cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}});
    cmd.setDebugMarker("Inventory-counters");
    numOverlay.draw(cmd);
    }
  }

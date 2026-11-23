#pragma once

#include <Tempest/CommandBuffer>
#include <Tempest/VectorImage>

class Renderer;
class InventoryMenu;
class VideoWidget;

void drawGameFrame(Renderer& renderer, Tempest::Attachment& result,
                   Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId,
                   Tempest::VectorImage::Mesh& uiLayer, Tempest::VectorImage::Mesh& numOverlay,
                   InventoryMenu& inventory, VideoWidget& video);

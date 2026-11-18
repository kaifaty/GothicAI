#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`.
// The sun's virtual shadow map past its cap of meshlet-page pairs (reports/engine/2026-11-16-modern-trees/
// shadow-flicker.md): the engine's own vsm_visibility_pass.comp and vsm_pack_draws.comp run on six clusters of three
// draws that ask 42 pairs of a page under a cap of 40. The cluster that crosses the cap reserves slots 35..39 and
// leaves them; before the fix those slots kept an older frame's pairs, the packing counted them to their draw past the
// range it had laid out for it, and that draw took the next one's pairs (black pieces of pages in the game).
void vsmPairCapRecords(Tempest::Device& device) {
  Resources resources(device);
  Shaders   shaders;
  shaders.waitCompiler();
  if(!Shaders::isVsmSupported()) {
    std::cout << "vsm pair cap: skipped, no virtual shadow maps on this device\n";
    return;
    }

  constexpr uint32_t Cap = 40, Draws = 3, Clusters = 6, Meshlets = 7, Old = 999;
  struct Cluster { float sphere[4]; uint32_t bucketId_commandId, firstMeshlet; int32_t meshletCount; uint32_t instanceId; };
  struct Cmd     { uint32_t vertexCount, instanceCount, firstVertex, firstInstance, writeOffset; };
  struct Header  { // VsmHeader of vsm_common.glsl
    uint32_t pageCount, meshletCount, counterM, counterV, pagePerMip[16];
    int32_t  pageBbox[16][4];
    uint32_t pageOmniCount, pageListCount, pagePlaced, omniTileOverflow, omniTileLightsMax, pairSpill;
    };
  const size_t headerSize = shaders.vsmClear.sizeofBuffer(0, 0);
  require(headerSize>=sizeof(Header) && headerSize<sizeof(Header)+16, "the test's VsmHeader follows vsm_common.glsl");

  // the light's view is the identity: every sphere stands at the centre of mip 0, on page (16,16) of size 1x1
  std::vector<uint8_t> scene(sizeof(SceneGlobals::UboGlobal), 0);
  const auto identity = Tempest::Matrix4x4::mkIdentity();
  std::memcpy(scene.data(), &identity, sizeof(identity));
  std::vector<Cluster> clusters(Clusters);
  for(uint32_t i=0; i<Clusters; ++i) {
    const uint32_t draw = i%Draws;
    clusters[i] = Cluster{{0.f, 0.f, 0.f, 100.f}, ((10+draw) << 16) | draw, i*100, int32_t(Meshlets), 0xFFFFFFFFu};
    }
  std::vector<uint8_t> pages(shaders.vsmClear.sizeofBuffer(0, 16), 0);
  Header header = {};
  header.pageCount = 1;
  std::memcpy(pages.data(), &header, sizeof(header));
  const uint32_t page = (16u << 1) | (16u << 9) | (1u << 24) | (1u << 28); // packVsmPageInfo(ivec3(16,16,0), ivec2(1))
  std::memcpy(pages.data()+headerSize, &page, sizeof(page));
  // the slots as an older frame left them: draw 0's pairs (bucket 10), meshlet 999, page 0
  std::vector<uint32_t> older(size_t(Cap)*4);
  for(uint32_t i=0; i<Cap; ++i) {
    older[i*4+0] = 0xFFFFFFFFu;
    older[i*4+1] = Old;
    older[i*4+2] = (10u << 16) | 0u;
    older[i*4+3] = 0;
    }
  std::vector<Cmd> cmds(Draws, Cmd{64*3, 0, 1, 1, 0});

  auto sceneUbo    = device.ssbo(scene.data(), scene.size());
  auto clusterBuf  = device.ssbo(clusters.data(), clusters.size()*sizeof(Cluster));
  auto pageBuf     = device.ssbo(pages.data(), pages.size());
  auto payloadSrc  = device.ssbo(older.data(), older.size()*sizeof(uint32_t));
  auto payloadDst  = device.ssbo(Tempest::Uninitialized, size_t(Cap)*4*sizeof(uint32_t));
  auto cmdBuf      = device.ssbo(cmds.data(), cmds.size()*sizeof(Cmd));
  auto indirect    = device.ssbo(Tempest::Uninitialized, 16);
  auto dummy       = device.ssbo(Tempest::Uninitialized, 1024);
  auto clearPages  = device.ssbo(Tempest::Uninitialized, pages.size());
  auto pageHiZ     = device.image3d(Tempest::TextureFormat::R32U, 32, 32, 16);
  auto scratch     = device.image3d(Tempest::TextureFormat::R32U, 32, 32, 16);
  auto dbg         = device.image2d(Tempest::TextureFormat::R32U, 1, 1);

  auto command = device.commandBuffer();
  {
  auto enc = command.startEncoding(device);
  // vsm_clear.comp zeroes its page table: here the receivers' depth of every page, so no cluster is behind them
  enc.setBinding(0, clearPages);
  enc.setBinding(1, pageHiZ);
  enc.setBinding(2, scratch);
  enc.setPipeline(shaders.vsmClear);
  enc.dispatchThreads(32, 32, 16);

  struct Push { uint32_t meshletCount, pairCap; } push = {Clusters, Cap};
  enc.setBinding(0, sceneUbo);
  enc.setBinding(1, payloadSrc);
  enc.setBinding(2, dummy);
  enc.setBinding(3, dummy);
  enc.setBinding(4, cmdBuf);
  enc.setBinding(5, clusterBuf);
  enc.setBinding(6, dummy);
  enc.setBinding(7, pageHiZ);
  enc.setBinding(8, pageBuf);
  enc.setBinding(9, dbg);
  enc.setPushData(&push, sizeof(push));
  enc.setPipeline(shaders.vsmVisibilityPass);
  enc.dispatchThreads(Clusters, 17);

  enc.setBinding(0, sceneUbo);
  enc.setBinding(1, payloadSrc);
  enc.setBinding(2, payloadDst);
  enc.setBinding(3, cmdBuf);
  enc.setBinding(4, pageBuf);
  enc.setBinding(5, indirect);
  enc.setPipeline(shaders.vsmPackDraw0);
  enc.dispatch(1);

  enc.setBinding(0, sceneUbo);
  enc.setBinding(1, payloadSrc);
  enc.setBinding(2, payloadDst);
  enc.setBinding(3, cmdBuf);
  enc.setBinding(4, pageBuf);
  enc.setPushData(&Cap, sizeof(Cap));
  enc.setPipeline(shaders.vsmPackDraw1);
  enc.dispatch((Cap+127)/128);
  }
  device.submit(command).wait();

  Header got = {};
  device.readBytes(pageBuf, &got, sizeof(got));
  std::vector<Cmd> drawn(Draws);
  device.readBytes(cmdBuf, drawn.data(), drawn.size()*sizeof(Cmd));
  std::vector<uint32_t> packed(size_t(Cap)*4);
  device.readBytes(payloadDst, packed.data(), packed.size()*sizeof(uint32_t));

  require(got.meshletCount==Clusters*Meshlets, "the count of pairs runs on past the cap: the frame's demand");
  require(got.pairSpill==0, "no pair is packed past its draw's range");
  uint32_t total = 0;
  for(uint32_t d=0; d<Draws; ++d) {
    require(drawn[d].instanceCount%Meshlets==0, "a draw takes whole clusters");
    require(d+1==Draws || drawn[d].writeOffset+drawn[d].instanceCount==drawn[d+1].writeOffset,
            "a draw ends where the next one's range starts");
    for(uint32_t i=0; i<drawn[d].instanceCount; ++i) {
      const uint32_t* p = &packed[size_t(drawn[d].writeOffset+i)*4];
      const uint32_t  m = p[1]%100;
      require(p[2]==10+d && p[1]!=Old && m<Meshlets && (p[1]/100)%Draws==d && p[3]==0,
              "a draw takes only its own clusters' meshlets, none an older frame left in the reserved slots");
      }
    total += drawn[d].instanceCount;
    }
  require(total==(Cap/Meshlets)*Meshlets, "the clusters under the cap are drawn whole, the one across it not at all");
  }

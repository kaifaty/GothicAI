// Exercise stage 0 of the production expansion, including workgroup slices starting inside a culled branch.
void treeBoughCullCases(Tempest::Device& device) {
  using namespace Tempest;
  using TreeBough::Node;
  struct Box { Vec3 lo, hi; };
  const Box boxes[] = {
    {{1.5f,0.f,.985f}, {1.7f,.1f,.999f}},       // outside the view
    {{-.8f,0.f,.985f}, {-.6f,.1f,.999f}},       // fully behind the left depth tile
    {{-.1f,0.f,.985f}, {.04f,.1f,.999f}},       // also touches the clear right tile
    {{-.8f,0.f,-.01f}, {-.6f,.1f,.999f}},       // crosses the near plane
    {{-.8f,0.f,.97f},  {-.6f,.1f,.999f}},       // touches the occluder
    {{1.004f,0.f,.985f}, {1.02f,.1f,.999f}},    // the wind margin reaches the screen edge
    {{.55f,0.f,.985f}, {.7f,.1f,.999f}},
    };
  constexpr uint32_t group = 23, count = uint32_t(std::size(boxes))*group;
  // Header, crown entry and asset header as in TreeBoughRender::uploadData.
  constexpr uint32_t entry = 5, asset = entry+8, nodes = asset+20, ranges = nodes+16*count;
  std::vector<uint32_t> data(ranges+4*count);
  data[1] = entry;
  data[entry+4] = asset;
  data[entry+5] = 8;
  data[asset+2] = nodes;
  data[asset+3] = count;
  data[asset+4] = ranges;
  data[asset+7] = std::bit_cast<uint32_t>(32.f);
  data[asset+17] = std::bit_cast<uint32_t>(1.f);
  for(uint32_t i=0; i<count; ++i) {
    const bool root = i%group==0;
    const auto& box = boxes[i/group];
    Node node{{},box.lo,1.f,box.hi,0.f,0.f,root ? 32.f : 0.f,
              root ? TreeBough::NoNode : i-i%group,root ? i+group : i+1};
    std::memcpy(&data[nodes+16*i], &node, sizeof(node));
    data[ranges+4*i] = i; // observable ID in the draw payload
    data[ranges+4*i+1] = 6;
    }
  SceneGlobals::UboGlobal globals{};
  globals.viewProject.identity();
  globals.project.identity();
  globals.view.identity();
  globals.screenRes = {32,16};
  globals.hiZTileSize = {16,16};
  const auto scene = device.ssbo(&globals,sizeof(globals));
  const uint32_t unused[64] = {};
  const auto dummy = device.ssbo(unused,sizeof(unused));
  uint32_t status[9] = {};
  status[8] = 2; // visible crown, matching the expansion token
  auto state = device.ssbo(status,sizeof(status));
  auto mainDraw = device.ssbo(unused,sizeof(unused));
  auto mainPayload = device.ssbo(unused,sizeof(unused));
  using Task = std::array<uint32_t,4>;
  using Instance = std::array<float,16>;
  const Instance upright = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
  const auto run = [&](uint32_t mode, bool raw, float pad, const Instance& transform) {
    data[asset+18] = std::bit_cast<uint32_t>(pad);
    const auto records = device.ssbo(data);
    const auto instance = device.ssbo(transform.data(),sizeof(transform));
    uint32_t draw[5] = {1,0,0,0,0};
    auto indirect = device.ssbo(draw,sizeof(draw));
    auto payload = device.ssbo(Uninitialized,count*sizeof(Task));
    Pixmap pixels(2,1,raw ? TextureFormat::R32F : TextureFormat::R16);
    if(raw) {
      const float depth[] = {.97f,1.f};
      std::memcpy(pixels.data(),depth,sizeof(depth));
      } else {
      const uint16_t depth[] = {26215,65535}; // packHiZ(.97) rounded up, clear
      std::memcpy(pixels.data(),depth,sizeof(depth));
      }
    const auto depth = device.texture(pixels,false);
    const auto code = GothicShader::get(mode==0 ? "tree_bough_expand.comp.sprv" : "tree_bough_expand_cull.comp.sprv");
    const auto pipeline = device.pipeline(device.shader(code.data,code.len));
    auto command = device.commandBuffer();
    {
      auto enc = command.startEncoding(device);
      enc.setBinding(0,scene);
      enc.setBinding(1,mainPayload);
      enc.setBinding(2,instance);
      enc.setBinding(3,records);
      enc.setBinding(4,mainDraw);
      enc.setBinding(5,dummy);
      enc.setBinding(6,state);
      enc.setBinding(7,dummy);
      enc.setBinding(8,indirect);
      enc.setBinding(9,payload);
      enc.setBinding(10,dummy);
      if(mode!=0)
        enc.setBinding(11,depth,Sampler::nearest());
      enc.setPipeline(pipeline);
      const uint32_t push[] = {2u,(mode==2 ? 16u : 0u) | (raw ? 8u : 0u),std::bit_cast<uint32_t>(.01f),0u};
      enc.setPushData(push);
      enc.dispatch(1);
    }
    device.submit(command).wait();
    device.readBytes(indirect,draw,sizeof(draw));
    require(draw[1]>0 && draw[1]<=count,"tree bough visibility must emit a valid cut");
    std::vector<Task> tasks(draw[1]);
    device.readBytes(payload,tasks.data(),tasks.size()*sizeof(Task));
    std::ranges::sort(tasks);
    return tasks;
    };
  const auto control = run(0,false,.006f,upright);
  require(control.size()==count-1,"control contains both sides of each band except the near-plane parent");
  for(bool raw : {false,true}) {
    for(uint32_t mode : {1u,2u}) {
      auto expected = control;
      std::erase_if(expected,[&](const Task& t){ return t[1]/group==0 || (mode==2 && t[1]/group==1); });
      require(run(mode,raw,.006f,upright)==expected,"tree bough culling lost a visible node or changed a dissolve weight");
      std::erase_if(expected,[&](const Task& t){ return t[1]/group==5; });
      require(run(mode,raw,0.f,upright)==expected,"tree bough culling ignored the swept extent at the screen edge");
      }
    }
  // Turning and translating the tree puts all seven groups over the left tile.
  // Only the groups crossing the near plane or touching the occluder are still visible.
  const Instance turned = {0,.75f,0, -2,0,0, 0,0,1, -.25f,0,0};
  auto expected = run(0,true,.006f,turned);
  std::erase_if(expected,[&](const Task& t){ return t[1]/group!=3 && t[1]/group!=4; });
  require(run(2,true,.006f,turned)==expected,"rotated and scaled boxes must use the instance's translated screen coverage");
  auto displaced = upright;
  displaced[12] = 1.f;
  require(run(2,true,.006f,displaced)==control,"an instance displaced beyond the boxes must keep its original cut");
  auto sheared = upright;
  sheared[6] = .25f;
  require(run(2,true,.006f,sheared)==run(0,true,.006f,sheared),"a shear must retain the original cut");
}

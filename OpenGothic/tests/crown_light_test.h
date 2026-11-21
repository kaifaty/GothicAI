// Included by material_texture_test.cpp.
void crownLightPixels(Tempest::Device& device, const Fixture& fixture) {
  using namespace Tempest;
  textFile(fixture.root/"materials/materials.json",R"({
    "CROWN.TGA":{"albedo":"solid.dds","foliage":{"anchor":"tree","wind":0,"flutter":0,"translucency":0.25}}
  })");
  Resources resources(device,Resources::Audio::Off);
  Settings settings;
  const auto ini = (fixture.root/"Gothic.ini").u16string();
  textFile(ini,"[INTERNAL]\ncrownLight=1\n");
  settings.load(ini,ini);
  Shaders shaders;
  shaders.waitCompiler();
  const Material material(source("CROWN.TGA"),true);

  constexpr uint32_t side = 4;
  SceneGlobals::UboGlobal globals{};
  globals.view = globals.project = globals.viewProject = Matrix4x4::mkIdentity();
  globals.screenRes = {int(side),int(side)};
  globals.screenResInv = {1.f/side,1.f/side};
  globals.clipInfo = {0.1f,0.f,1.f};
  globals.mipGradScale = 1.f;
  const auto scene = device.ssbo(&globals,sizeof(globals));
  const float identity[16] = {1,0,0, 0,1,0, 0,0,1}; // packed 3x3, translation and instance data
  const auto instance = device.ssbo(identity,sizeof(identity));
  const uint32_t emptySurfels[4] = {};
  const auto surfels = device.ssbo(emptySurfels,sizeof(emptySurfels));
  Pixmap pixel(1,1,TextureFormat::RGBA8);
  std::memset(pixel.data(),255,pixel.dataSize());
  const auto white = device.texture(pixel,false);
  const auto nearest = Sampler::nearest(ClampMode::ClampToEdge);

  auto diffuse = device.attachment(TextureFormat::RGBA8,side,side);
  auto normal  = device.attachment(TextureFormat::R32U,side,side);
  auto orm     = device.attachment(TextureFormat::RGBA8,side,side);
  auto motion  = device.attachment(TextureFormat::RG16F,side,side);
  auto light   = device.attachment(TextureFormat::RGBA32F,side,side);
  RenderState state;
  state.setCullFaceMode(RenderState::CullMode::NoCull);
  state.setZWriteEnabled(false);
  const auto pipeline = [&](const GothicShader& vert, const GothicShader& frag) {
    return device.pipeline(Topology::Triangles,state,device.shader(vert.data,vert.len),device.shader(frag.data,frag.len));
    };
  const auto geometry = pipeline(GothicShader::main_obj_g_orm_slot_vert_sprv,GothicShader::main_obj_g_orm_slot_frag_sprv);
  const auto ambient  = pipeline(GothicShader::triangle_vert_sprv,GothicShader::ambient_light_surf_orm_frag_sprv);

  const auto render = [&](uint8_t alpha, bool lit) {
    const uint32_t color = (uint32_t(alpha)<<24) | 0x00c08040u; // distinguish A from every RGB channel
    StaticMesh mesh(material,{{{-1,-1,0.5f},{0,0,1},{0,0},color},
                              {{ 3,-1,0.5f},{0,0,1},{1,0},color},
                              {{-1, 3,0.5f},{0,0,1},{0,1},color}},{0,1,2});
    DrawBuckets buckets;
    auto bucket = buckets.alloc(material,mesh);
    const uint32_t task[4] = {0,0,bucket.toInt(),0};
    const auto payload = device.ssbo(task,sizeof(task));
    auto command = device.commandBuffer();
    {
      auto enc = command.startEncoding(device);
      buckets.commit(enc,0);
      enc.setFramebuffer({{diffuse,Vec4(),Preserve},{normal,Vec4(),Preserve},
                          {orm,Vec4(),Preserve},{motion,Vec4(),Discard}});
      enc.setBinding(0,scene);
      enc.setBinding(1,payload);
      enc.setBinding(2,instance);
      enc.setBinding(3,buckets.ssbo());
      enc.setBinding(4,bucket->iboBuffer());
      enc.setBinding(5,bucket->vboBuffer());
      enc.setBinding(6,white);
      enc.setBinding(7,nearest);
      enc.setBinding(15,white);
      enc.setBinding(16,white);
      enc.setBinding(21,instance);
      for(auto slot : {23u,24u,25u})
        enc.setBinding(slot,white);
      enc.setPipeline(geometry);
      const uint32_t push[] = {0,1};
      enc.setPushData(push);
      enc.draw(nullptr,0,3);

      enc.setFramebuffer({{light,Vec4(),Preserve}});
      enc.setPipeline(ambient);
      enc.setBinding(0,scene);
      enc.setBinding(1,diffuse);
      enc.setBinding(2,normal);
      enc.setBinding(3,white,nearest);
      enc.setBinding(4,white,nearest);
      enc.setBinding(5,surfels);
      enc.setBinding(12,orm);
      enc.draw(nullptr,0,3);
    }
    device.submit(command).wait();
    constexpr size_t center = 4*((side/2)*side+side/2);
    const auto albedo = device.readPixels(diffuse);
    const auto packed = device.readPixels(orm);
    const auto result = device.readPixels(light);
    const auto hint = static_cast<const uint8_t*>(albedo.data())[center+3];
    require(((hint & 1u)!=0)==lit,"draw bucket wrote the wrong crown bit to the G-buffer");
    const auto expected = lit ? CrownLight::shade(CrownLight::openness(CrownLight::normalLength(alpha))) : 1.f;
    const auto code = static_cast<const uint8_t*>(packed.data())[center+3];
    require(code==CrownLight::shadeCode(expected),"vertex alpha gave the wrong crown shade in the G-buffer");
    return static_cast<const float*>(result.data())[center];
    };

  const uint8_t saved = FoliageWind::enabled();
  const char* env = std::getenv("OPENGOTHIC_FOLIAGE");
  const std::optional<std::string> savedEnv = env ? std::optional<std::string>(env) : std::nullopt;
  setenv("OPENGOTHIC_FOLIAGE","light",1);
  FoliageWind::setSwitch(true);
  const float outside = render(255,true);
  require(std::isfinite(outside) && outside>0.f,"open crown must receive surfel GI");
  for(uint8_t alpha : {uint8_t(0),uint8_t(114)}) {
    const float shade = CrownLight::shade(CrownLight::openness(CrownLight::normalLength(alpha)));
    const float expected = CrownLight::shadeOfCode(CrownLight::shadeCode(shade));
    require(std::abs(render(alpha,true)/outside-expected)<0.0001f,"surfel GI lost the crown shade from the G-buffer");
    }
  setenv("OPENGOTHIC_FOLIAGE","wind",1);
  FoliageWind::setSwitch(true);
  require(std::abs(render(0,false)/outside-1.f)<0.0001f,"wind-only foliage must receive the full surfel GI");
  if(savedEnv)
    setenv("OPENGOTHIC_FOLIAGE",savedEnv->c_str(),1);
  else
    unsetenv("OPENGOTHIC_FOLIAGE");
  FoliageWind::switchBits() = saved;
  }

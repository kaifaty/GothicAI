// Included by material_texture_test.cpp. Two windings of the same card under one point light.
void twoSidedPixels(Tempest::Device& device, Shaders& shaders, std::array<Material,3> materials, const char* lampShot) {
  using namespace Tempest;
  constexpr uint32_t width = 360, height = 200;
  const auto shader = [&](const GothicShader& s) { return device.shader(s.data,s.len); };
  const auto probe = device.pipeline(shader(GothicShader::two_sided_test_comp_sprv));
  auto result = device.ssbo(Uninitialized,3*4*sizeof(uint32_t));
  Pixmap pixel(1,1,TextureFormat::RGBA8);
  const uint8_t grey[] = {184,184,184,255};
  std::memcpy(pixel.data(),grey,sizeof(grey));
  const auto albedo = device.texture(pixel,false);
  for(auto& mat : materials) {
    // Give the three records the same plain surface and isolate the normal from leaf transmission.
    mat.alpha = Material::AlphaTest;
    mat.tex = &albedo;
    mat.normal = mat.orm = nullptr;
    mat.foliage = {};
    }

  std::vector<Resources::Vertex> vertices;
  for(float y : {-25.f,25.f}) {
    vertices.push_back({{-17,y-9,40},{0,0,1},{0,0},0xffffffffu});
    vertices.push_back({{ 17,y-9,40},{0,0,1},{1,0},0xffffffffu});
    vertices.push_back({{ 17,y+9,40},{0,0,1},{1,1},0xffffffffu});
    vertices.push_back({{-17,y+9,40},{0,0,1},{0,1},0xffffffffu});
    }
  StaticMesh mesh(materials[0],std::move(vertices),{0,1,2,0,2,3, 4,6,5,4,7,6});
  DrawBuckets buckets;
  std::array<DrawBuckets::Id,3> ids;
  const auto blas = Resources::blas(mesh.vbo,mesh.ibo,0,12);
  RtScene rays;
  std::array<std::array<float,16>,3> transforms{};
  uint32_t tasks[3][4] = {};
  for(uint32_t i=0; i<3; ++i) {
    ids[i] = buckets.alloc(materials[i],mesh);
    tasks[i][0] = i;
    tasks[i][2] = ids[i].toInt();
    const float x = (float(i)-1.f)*60.f;
    transforms[i] = {1,0,0, 0,1,0, 0,0,1, x,0,0};
    if(!blas.isEmpty()) {
      auto pos = Matrix4x4::mkIdentity();
      pos.translate(x,0,0);
      rays.setSlot(rays.allocSlot(),pos,blas,materials[i],mesh,0,12,true);
      }
    }
  const auto instances = device.ssbo(transforms.data(),sizeof(transforms));
  const auto payload = device.ssbo(tasks,sizeof(tasks));
  SceneGlobals::UboGlobal globals{};
  globals.view = globals.project = Matrix4x4::mkIdentity();
  globals.project[0][0] = 1.f/90;
  globals.project[1][1] = 1.f/50;
  globals.project[2][2] = -1.f/120;
  globals.project[3][2] = 1.f;
  globals.viewProject = globals.viewProjectLwcInv = globals.project;
  globals.viewProjectLwcInv.inverse();
  globals.screenRes = {int(width),int(height)};
  globals.screenResInv = {1.f/width,1.f/height};
  globals.camPos = {0,0,120};
  globals.clipInfo = {0.1f,0.f,1.f};
  globals.exposure = 1.f;
  const auto scene = device.ssbo(&globals,sizeof(globals));
  auto depth   = device.zbuffer(TextureFormat::Depth32F,width,height);
  auto shadow  = device.zbuffer(TextureFormat::Depth32F,width,height);
  auto diffuse = device.attachment(TextureFormat::RGBA8,width,height);
  auto normal  = device.attachment(TextureFormat::R32U,width,height);
  auto orm     = device.attachment(TextureFormat::RGBA8,width,height);
  auto motion  = device.attachment(TextureFormat::RG16F,width,height);
  auto command = device.commandBuffer();
  {
    auto enc = command.startEncoding(device);
    buckets.commit(enc,0);
    if(!blas.isEmpty())
      rays.commit(enc,0);
    for(auto pass : {Shaders::T_Shadow,Shaders::T_Main}) {
      if(pass==Shaders::T_Shadow)
        enc.setFramebuffer({}, {shadow,0.f,Preserve});
      else
        // The grey receiver is at z=0 (far depth), with normal +Z (octahedral code zero).
        enc.setFramebuffer({{diffuse,Vec4(0.64f,0.64f,0.64f,0),Preserve},{normal,Vec4(),Preserve},
                            {orm,Vec4(),Preserve},{motion,Vec4(),Discard}}, {depth,1.f,Preserve});
      enc.setBinding(0,scene);
      enc.setBinding(1,payload);
      enc.setBinding(2,instances);
      enc.setBinding(3,buckets.ssbo());
      enc.setBinding(4,ids[0]->iboBuffer());
      enc.setBinding(5,ids[0]->vboBuffer());
      enc.setBinding(6,albedo);
      enc.setBinding(7,Sampler::nearest());
      enc.setBinding(21,instances);
      for(auto slot : {15u,16u,23u,24u,25u})
        enc.setBinding(slot,albedo);
      for(uint32_t i=0; i<3; ++i) {
        enc.setPipeline(*shaders.materialPipeline(materials[i],DrawCommands::Static,pass,false));
        const uint32_t push[] = {i,1};
        enc.setPushData(push);
        if(Shaders::options().doMeshShading)
          enc.dispatchMesh(1);
        else
          enc.draw(nullptr,0,12);
        }
      }
    enc.setFramebuffer({});
    enc.setPipeline(probe);
    enc.setBinding(0,buckets.ssbo());
    enc.setBinding(1,shadow);
    enc.setBinding(2,result);
    enc.setBinding(3,payload);
    enc.dispatch(3);
  }
  device.submit(command).wait();
  uint32_t rows[3][4] = {};
  device.readBytes(result,rows,sizeof(rows));
  const auto sampled = device.readPixels(diffuse);
  const auto red = [](const Pixmap& image,uint32_t x,uint32_t y) {
    return static_cast<const uint8_t*>(image.data())[(y*width+x)*4];
    };
  for(uint32_t i=0; i<3; ++i) {
    require(rows[i][0]==uint32_t(i!=0) && rows[i][1]==uint32_t(i==2),
            "wrong two-sided or flip flag in the bucket consumed by the shaders");
    uint32_t rasterCoverage = 0;
    for(uint32_t side=0; side<2; ++side) {
      const bool drawn = red(sampled,(2*i+1)*width/6,(2*side+1)*height/4)==grey[0];
      require(drawn==(rows[i][2+side]!=0),"material and shadow passes disagree on the visible winding");
      rasterCoverage += uint32_t(drawn);
      }
    require(rasterCoverage==(i==0 ? 1u : 2u),"wrong number of visible card windings in the material or shadow pipeline");
    }
  if(!blas.isEmpty()) {
    RtScene::RtObjectDesc desc[3] = {};
    device.readBytes(rays.rtDesc,desc,sizeof(desc));
    require(desc[0].bits==0 && desc[1].bits==0x4 && desc[2].bits==0xc,
            "RT hit descriptors lost the back-face or flipped-normal bit of an alpha-tested card");
    }

  // No skylight or sunlight. The lamp lights the cards and the receiver with the game's point-light shaders.
  const float lamp[] = {0,0,110,180, 0.95f,0.6f,0.3f,0, 0,0,0,0}; // scene.glsl LightSource
  const auto lights = device.ssbo(lamp,sizeof(lamp));
  const uint16_t face[] = {0,1,2,0,2,3};
  const auto quad = device.ibo(face,6);
  const bool traceLamp = !blas.isEmpty() && Shaders::options().doBindless;
  auto display = lampShot ? device.attachment(TextureFormat::RGBA8,width,height) : Attachment();
  const auto renderLamp = [&](bool traced) {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setZWriteEnabled(false);
    const auto pipeline = device.pipeline(Topology::Triangles,state,shader(GothicShader::light_vert_sprv),
                                          shader(traced ? GothicShader::light_rq_at_orm_frag_sprv : GothicShader::light_orm_frag_sprv));
    auto light = device.attachment(TextureFormat::RGBA8,width,height);
    auto cmd = device.commandBuffer();
    {
      auto enc = cmd.startEncoding(device);
      enc.setFramebuffer({{light,Vec4(),Preserve}});
      enc.setPipeline(pipeline);
      enc.setBinding(0,scene);
      enc.setBinding(1,diffuse);
      enc.setBinding(2,normal);
      enc.setBinding(3,depth);
      enc.setBinding(4,lights);
      if(traced) {
        enc.setBinding(6,rays.tlas);
        enc.setBinding(7,Sampler::nearest());
        enc.setBinding(8,rays.tex);
        enc.setBinding(9,rays.vbo);
        enc.setBinding(10,rays.ibo);
        enc.setBinding(11,rays.rtDesc);
        }
      enc.setBinding(12,orm);
      for(uint32_t slot=13; slot<=16; ++slot)
        enc.setBinding(slot,albedo);
      enc.setPushData(Vec3());
      enc.draw(nullptr,quad);
      if(lampShot && traced==traceLamp) {
        enc.setFramebuffer({{display,Vec4(),Preserve}});
        enc.setPipeline(shaders.tonemapping);
        enc.setBinding(0,scene);
        enc.setBinding(1,light,Sampler::nearest());
        enc.setPushData(Vec4(0,1,1.f/2.2f,1)); // neutral brightness, contrast, display gamma and exposure
        enc.draw(nullptr,0,3);
        }
    }
    device.submit(cmd).wait();
    return device.readPixels(light);
    };
  auto lit = renderLamp(false);
  for(uint32_t side=0; side<2; ++side) {
    const auto y = (2*side+1)*height/4;
    require(red(lit,width/2,y)>0,"both windings of 'same' must receive the lamp");
    require((red(lit,5*width/6,y)>0)==(rows[0][2+side]!=0),"'flip' must turn the back normal away from this lamp");
    }
  if(traceLamp) {
    auto traced = renderLamp(true);
    // The point light projects the two outer columns towards the edges of the receiver.
    const uint32_t shadowX[] = {width/18,width/2,17*width/18};
    for(uint32_t i=0; i<3; ++i) {
      uint32_t blocked = 0;
      for(auto y : {height/10,9*height/10}) {
        require(red(lit,shadowX[i],y)>0,"the unoccluded receiver must see the lamp at each probe");
        blocked += uint32_t(red(traced,shadowX[i],y)==0);
        }
      require(blocked==(i==0 ? 1u : 2u),"wrong number of card windings that cast a lamp shadow");
      }
    }
  if(lampShot) {
    device.readPixels(display).save(lampShot);
    std::cout << "two-sided lamp: " << (Shaders::options().doMeshShading ? "mesh" : "vertex") << " shader, "
              << (traceLamp ? "ray-traced shadows" : "unshadowed") << '\n';
    }
  }

#pragma once

// A part of mesh_gltf_test.cpp, included inside its unnamed namespace after `require`: the bytes of its GLB
// fixtures, and the cases of COLOR_0. An own GLB carries data for the vertex shader in its vertex colour (the
// tree's wind, reports/engine/2026-11-16-modern-trees): the four bytes must reach Resources::Vertex::color of the
// static mesh as they are, and a mesh without the attribute must keep the ones it had.

void word(std::vector<uint8_t>& bytes, uint32_t value) {
  for(size_t i=0;i<4;++i)
    bytes.push_back(uint8_t(value>>(i*8)));
  }

void real(std::vector<uint8_t>& bytes, float value) {
  uint32_t bits = 0;
  static_assert(sizeof(bits)==sizeof(value));
  std::memcpy(&bits,&value,sizeof(bits));
  word(bytes,bits);
  }

// R | G<<8 | B<<16 | A<<24: the first set of bytes, one colour a vertex; the second set, the same at each vertex;
// and what the float RGB (0.2, 0.4, 1) without alpha must become
constexpr uint32_t ColorFirst[3] = {0x281E140Au, 0x078000FFu, 0x04030201u};
constexpr uint32_t ColorSecond   = 0x06070809u;
constexpr uint32_t ColorReal     = 0xFFFF6633u;

// One triangle without indices. first: the accessor of COLOR_0 (2: bytes RGBA, 3: the second set of them,
// 4: float RGB), or 0 for none; second: the same for a second primitive of the same material, positions and normals
void colorGlbFile(const std::filesystem::path& path, unsigned first, unsigned second = 0, uint16_t cardId = 0) {
  std::vector<uint8_t> binary;
  for(float v:{1.f,2.f,3.f, 4.f,2.f,3.f, 1.f,6.f,3.f})
    real(binary,v);
  for(size_t i=0;i<3;++i)
    for(float v:{0.f,0.f,1.f})
      real(binary,v);
  for(uint32_t c:ColorFirst)
    word(binary,c);
  for(size_t i=0;i<3;++i)
    word(binary,ColorSecond);
  for(size_t i=0;i<3;++i)
    for(float v:{0.2f,0.4f,1.f})
      real(binary,v);
  if(cardId!=0)
    for(size_t i=0;i<3;++i)
      word(binary,cardId);

  auto primitive = [cardId](unsigned color) {
    std::string p = R"({"attributes":{"POSITION":0,"NORMAL":1)";
    if(color!=0)
      p += ",\"COLOR_0\":"+std::to_string(color);
    if(cardId!=0)
      p += R"(,"_CARD_ID":5)";
    return p + R"(},"material":0,"mode":4})";
    };
  std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                     R"("buffers":[{"byteLength":)"+std::to_string(binary.size())+
                     R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
                     R"({"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":12},)"
                     R"({"buffer":0,"byteOffset":84,"byteLength":12},{"buffer":0,"byteOffset":96,"byteLength":36})";
  if(cardId!=0)
    json += R"(,{"buffer":0,"byteOffset":132,"byteLength":12,"byteStride":4})";
  json += R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[1,2,3],"max":[4,6,3]},)"
                     R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},)"
                     R"({"bufferView":2,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
                     R"({"bufferView":3,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
                     R"({"bufferView":4,"componentType":5126,"count":3,"type":"VEC3"})";
  if(cardId!=0)
    json += R"(,{"bufferView":5,"componentType":5123,"count":3,"type":"SCALAR"})";
  json += R"(],"materials":[{"name":"OWN_TEST_BARREL_WOOD","extras":{"group":"WOOD"})";
  if(cardId!=0)
    json += R"(,"alphaMode":"MASK")";
  json += R"(}],"meshes":[{"primitives":[)";
  json += primitive(first);
  if(second!=0)
    json += ","+primitive(second);
  json += "]}]}";
  while(json.size()%4!=0)
    json.push_back(' ');
  std::vector<uint8_t> bytes;
  word(bytes,0x46546c67);
  word(bytes,2);
  word(bytes,uint32_t(12+8+json.size()+8+binary.size()));
  word(bytes,uint32_t(json.size()));
  word(bytes,0x4e4f534a);
  bytes.insert(bytes.end(),json.begin(),json.end());
  word(bytes,uint32_t(binary.size()));
  word(bytes,0x004e4942);
  bytes.insert(bytes.end(),binary.begin(),binary.end());
  std::ofstream out(path,std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!out)
    throw std::runtime_error("unable to write GLB colour fixture");
  }

// the colour of the packed vertex of each wedge: what the vertex shader reads
void packedColors(const asset::StaticMesh& mesh, const GltfMeshColors& colors, std::string_view message) {
  PackedMesh packed(mesh,PackedMesh::PK_Visual,false,&colors);
  for(size_t i=0;i<mesh.sub_meshes[0].wedges.size();++i) {
    const auto& at = mesh.positions[mesh.sub_meshes[0].wedges[i].index];
    size_t found = 0;
    for(const auto& v:packed.vertices)
      if(v.pos[0]==at.x && v.pos[1]==at.y && v.pos[2]==at.z && v.color==colors[0][i])
        ++found;
    require(found>=1,message);
    }
  }

void colorCases(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  colorGlbFile(path,2);
  auto mesh   = loadGltfMesh(path,&bytes);
  auto colors = loadGltfMeshColors(bytes,mesh);
  require(mesh.sub_meshes.size()==1 && mesh.sub_meshes[0].wedges.size()==3 && colors.size()==1 && colors[0].size()==3,
          "a triangle with COLOR_0 must load with a colour for each wedge");
  for(size_t i=0;i<3;++i)
    require(colors[0][i]==ColorFirst[i],"the bytes of COLOR_0 must reach the wedge as R, G, B, A from the low byte");
  packedColors(mesh,colors,"the bytes of COLOR_0 must reach the vertex of the static mesh");

  colorGlbFile(path,4);
  mesh   = loadGltfMesh(path,&bytes);
  colors = loadGltfMeshColors(bytes,mesh);
  require(colors.size()==1 && colors[0].size()==3,"float RGB must load");
  for(const auto c:colors[0])
    require(c==ColorReal,"float RGB must become bytes without a gamma curve, with alpha one");

  colorGlbFile(path,0);
  mesh = loadGltfMesh(path,&bytes);
  require(loadGltfMeshColors(bytes,mesh).empty(),"a mesh without COLOR_0 must have no colours");
  // a list that is not of the sub-mesh's size must not paint it either
  colors = {{ColorSecond}};
  for(const auto* list:{static_cast<const GltfMeshColors*>(nullptr),&std::as_const(colors)}) {
    PackedMesh packed(mesh,PackedMesh::PK_Visual,false,list);
    size_t ones = 0;
    for(const auto& v:packed.vertices)
      ones += (v.color==0xFFFFFFFFu ? 1u : 0u);
    require(ones==3,"the three vertices of a mesh without COLOR_0 must be drawn with ones");
    }

  // primitives with the same positions and normals share their wedges, as before: one colour for both, or none
  colorGlbFile(path,2,2);
  mesh   = loadGltfMesh(path,&bytes);
  colors = loadGltfMeshColors(bytes,mesh);
  require(mesh.sub_meshes[0].wedges.size()==3 && colors[0].size()==3 && colors[0][2]==ColorFirst[2],
          "primitives with the same streams must share wedges and their colour");
  colorGlbFile(path,2,3);
  mesh = loadGltfMesh(path,&bytes);
  require(mesh.sub_meshes[0].wedges.size()==3,"COLOR_0 must not change the wedges of the mesh");
  bool thrown = false;
  try {
    loadGltfMeshColors(bytes,mesh);
    }
  catch(const std::runtime_error&) {
    thrown = true;
    }
  require(thrown,"primitives that share wedges and differ in COLOR_0 must be rejected, not painted by one of them");
  // a walk out of step with the loader's must not paint a wrong vertex: here the mesh has lost a wedge
  colorGlbFile(path,2);
  mesh = loadGltfMesh(path,&bytes);
  mesh.sub_meshes[0].wedges.pop_back();
  thrown = false;
  try {
    loadGltfMeshColors(bytes,mesh);
    }
  catch(const std::runtime_error&) {
    thrown = true;
    }
  require(thrown,"colours that are not of the mesh's wedges must be rejected");
  }

void cardCases(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  colorGlbFile(path,2);
  auto wood = loadGltfMesh(path,&bytes);
  require(loadGltfMeshCardIds(bytes,wood).wedges.empty(),"an unmarked GLB must keep its existing path");
  colorGlbFile(path,2,0,1);
  const auto marked = loadGltfMesh(path,&bytes);
  const auto ids = loadGltfMeshCardIds(bytes,marked);
  require(ids.submesh==0 && ids.wedges==std::vector<uint16_t>(3,1),
          "the strided unsigned-short GLB attribute must map to the crown wedges");

  wood.sub_meshes.resize(2);
  auto& crown = wood.sub_meshes[1];
  crown.mat = marked.sub_meshes[0].mat;
  GltfMeshCardIds cards;
  cards.submesh = 1;
  for(uint16_t id=17; id>0; --id) {
    const auto start = uint16_t(crown.wedges.size());
    const auto count = uint16_t(id==1 ? 5 : 4);
    for(uint16_t v=0; v<count; ++v) {
      const auto pos = uint16_t(wood.positions.size());
      wood.positions.push_back({float(pos),float(v%2),float(id)});
      crown.wedges.push_back({{0.f,0.f,1.f},{float(v),0.f},pos});
      cards.wedges.push_back(id);
      }
    crown.triangles.push_back({{start,uint16_t(start+1),uint16_t(start+2)}});
    crown.triangles.push_back({{start,uint16_t(start+2),uint16_t(start+3)}});
    if(id==1)
      crown.triangles.push_back({{start,uint16_t(start+3),uint16_t(start+4)}});
    }
  PackedMesh legacy(wood,PackedMesh::PK_Visual,true);
  PackedMesh ranked(wood,PackedMesh::PK_Visual,true,nullptr,&cards);
  require(legacy.cardIds.empty() && legacy.meshletFirstCard.empty(),"legacy packing has no card data");
  require(ranked.cardCount==17 && ranked.meshletFirstCard==std::vector<int32_t>{-1,0,15},
          "wood precedes two crown meshlets in rank order and the sixteenth card starts the second");
  require(ranked.vertices.size()==ranked.cardIds.size(),"padded card IDs and vertices must have equal lengths");
  std::array<size_t,18> triangles = {};
  for(size_t m=0; m<ranked.meshletFirstCard.size(); ++m) {
    const size_t tail = (m+1)*PackedMesh::MaxPrim*4-4;
    const size_t count = ranked.indices8[tail+2];
    for(size_t t=0; t<count; ++t) {
      const size_t at = m*PackedMesh::MaxInd+t*3;
      const uint16_t id = ranked.cardIds[ranked.indices[at]];
      require(id==ranked.cardIds[ranked.indices[at+1]] && id==ranked.cardIds[ranked.indices[at+2]],
              "all corners of a packed triangle must agree on its card");
      require(m==(id==0 ? 0u : id<16 ? 1u : 2u),"each card must keep all its triangles in one ranked meshlet");
      ++triangles[id];
      }
    }
  require(triangles[0]==1 && triangles[1]==3 && std::ranges::all_of(triangles.begin()+2,triangles.end(),
                                                                  [](size_t n) { return n==2; }),
          "whole-card packing must preserve every triangle");
  }

void resourceColorCase(Tempest::Device& device, const std::filesystem::path& path) {
  const auto* proto = Resources::loadMesh("OWN_COLOR.3DS");
  require(proto!=nullptr && proto->attach.size()==1,"the own GLB must load as a ProtoMesh");
  const auto& attachment = proto->attach[0];
  std::vector<Resources::Vertex> vertices(attachment.vbo.size());
  std::vector<uint32_t> indices(attachment.ibo.size());
  require(!vertices.empty() && !indices.empty(),"the own GLB must upload indexed vertices");
  device.readBytes(attachment.vbo,vertices.data(),vertices.size()*sizeof(vertices[0]));
  device.readBytes(attachment.ibo,indices.data(),indices.size()*sizeof(indices[0]));

  const auto mesh = loadGltfMesh(path);
  const auto& wedges = mesh.sub_meshes.at(0).wedges;
  require(wedges.size()==std::size(ColorFirst),"the colour fixture must contain three wedges");
  for(size_t i=0;i<wedges.size();++i) {
    const auto& at = mesh.positions.at(wedges[i].index);
    require(std::any_of(indices.begin(),indices.end(),[&](uint32_t index) {
      const auto& v = vertices.at(index);
      return v.pos[0]==at.x && v.pos[1]==at.y && v.pos[2]==at.z && v.color==ColorFirst[i];
      }),"Resources::loadMesh must preserve COLOR_0 for each indexed vertex");
    }
  }

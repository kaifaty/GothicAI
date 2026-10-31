// Converts Gothic resources into our own formats (docs/asset-migration.md).
//
//   content-convert world <gothic dir> <WORLD.ZEN> <out dir>
//       Write chunks/<x>_<z>.glb, world.json and non-mesh world data;
//       read everything back and compare with the source.
//   content-convert verify-world <gothic dir> <WORLD.ZEN> <world dir> [override dir]
//       Compare existing chunks and world data with the source .ZEN.
//   content-convert check-vob-layer <base world dir> <layer.json> [content dir]
//       Validate the layer schema, base ids, world keys and visual resources without a game.
//   content-convert check-particles <base root> <content dir>
//       Validate all particles.d definitions using the game's particle loader.
//   content-convert zen|verify-zen <gothic dir> <dir>
//       Engine-consumed meshless .ZEN files, including loose _work files.
//   content-convert textures <gothic dir> <out dir>
//       Every compiled texture (*-C.TEX) as <NAME>.dds (DXT blocks and mips
//       copied as the engine builds them) or <NAME>.png (other formats, top
//       level, as the engine uploads them); raw .TGA files copied as they are;
//       textures.json lists them with the archives they came from.
//   content-convert verify-textures <gothic dir> <dir>
//   content-convert meshes|verify-meshes <gothic dir> <dir>
//       Every static mesh (*.MRM) as <NAME>.glb (common/basemesh.h);
//       meshes.json lists them with the source archives.
//   content-convert models|verify-models <gothic dir> <dir>
//       Model, hierarchy, script, skeletal and morph animation records as
//       lossless JSON + binary arrays, including loose _work sources.
//   content-convert fonts|verify-fonts <gothic dir> <dir>
//       Every selected FNT as a lossless JSON + binary record (common/fontdata.h),
//       chosen through the engine's runtime VFS; fonts.json indexes them.
//   content-convert programs|verify-programs <gothic dir> <dir>
//       Every Daedalus program the engine VFS, the _compiled directories or the
//       server can select, as field-by-field records (common/programdata.h);
//       programs.json maps the original choices to them.
//   content-convert outputunits|verify-outputunits <gothic dir> <dir>
//       The dialog line library (OU.BIN/OU.DAT) of every engine, server and storygraph choice as
//       WorldData ZenFile records of kind cutscene_library (common/outputunits.h); the import fails
//       when the server's native message view differs from its original scanner.
//   content-convert outputunits-file <OU.BIN|OU.DAT> <record.json>
//       One external library to a native record, e.g. for storygraph --ou. Import only.
//   content-convert audio|verify-audio <gothic dir> <dir>
//       Every .WAV/.OGG request of the runtime VFS copied byte for byte (common/audiodata.h);
//       audio.json maps the request names to payloads, provenance and header facts.
//   content-convert music|verify-music <gothic dir> <dir>
//       Every SGT/STY/DLS GothicKit (runtime VFS basenames) or Dx8 (its six search directories) can
//       select, as each provider's parsed records (common/musicdata.h); music.json maps the selections
//       to records or to the old parser's failure.
//   content-convert sources|verify-sources <gothic dir> <dir>
//       sources.json: selected FNT/audio/BIK/music/DAT/OU inputs of the engine
//       and server readers, their origin and SHA-256; no payload conversion.
//   content-convert resolve-base|resolve <gothic dir>
//       Select or import the base through BaseStore; resolve also prepares videos.
//       Progress goes to stderr and the selected base path goes to stdout.
//   content-convert --self-test
//       Round trips of synthetic meshes, world data and presets; no game files.
//
// The output is Gothic content: it never goes into git.

#include "baseimport.h"
#include "basestoreimport.h"
#include "videodata.h"
#include "sha256.h"
#include "asset/fromzenkit.h"
#include "basemesh.h"
#include "fontdata.h"
#include "programdata.h"
#include "outputunits.h"
#include "audiodata.h"
#include "musicdata.h"
#include "musicnative.h"
#include "worldchunksruntime.h"
#include "worlddata.h"
#include "world/voblayer.h"
#include "game/definitions/particlecontent.h"
#include "gothicglb.h"

#include <zenkit/Archive.hh>
#include <zenkit/CutsceneLibrary.hh>
#include <zenkit/Font.hh>
#include <zenkit/Logger.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/Texture.hh>
#include <zenkit/World.hh>
#include <zenkit/addon/texcvt.hh>

// miniz otherwise #defines crc32 and other zlib names.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void require(bool ok, const std::string& what) {
  if(!ok) throw std::runtime_error(what);
  }

asset::Material material(const std::string& name, const std::string& texture, asset::MaterialGroup group) {
  asset::Material m;
  m.name    = name;
  m.texture = texture;
  m.group   = group;
  m.color   = {200,180,160,255};
  m.texture_scale = {0.25f,0.125f};
  m.alpha_func = asset::AlphaFunction::NONE;
  return m;
  }

int selfTest() {
  // NIST SHA-256 vectors: empty, one block, two padding blocks and a long message.
  const std::pair<std::string,std::string_view> vectors[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {std::string(1000000,'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}
    };
  for(const auto& [input,expected]:vectors) {
    require(BaseImport::sha256(input)==expected,"SHA-256 NIST vector");
    Sha256 streamed;
    // Exercise the small, unaligned updates used by the asset digest.
    for(size_t i=0;i<input.size();i+=7)
      streamed.update(input.data()+i,std::min(size_t(7),input.size()-i));
    require(streamed.finish()==expected,"SHA-256 streamed NIST vector");
    }

  asset::WorldMesh mesh;
  mesh.materials.push_back(material("UNUSED","UNUSED.TGA",asset::MaterialGroup::STONE));
  // Sector name with a byte above 0x7F: must survive byte for byte.
  mesh.materials.push_back(material("C:HAFEN_\xE4","NW_HARBOUR_STAIRS_01.TGA",asset::MaterialGroup::STONE));
  auto water = material("WATER","NW_WATER.TGA",asset::MaterialGroup::WATER);
  water.texture_anim_fps = 0.1f;
  water.wave_mode = asset::WaveMode::GROUND;
  water.wave_max_amplitude = 33.3f;
  water.disable_collision = true;
  water.color.a = 128;
  water.alpha_func = asset::AlphaFunction::BLEND;
  mesh.materials.push_back(water);

  // A strip crossing three 64 m chunks; vertex 2 and 3 are shared across chunks.
  const float xs[] = {-100.f, 6300.f, 6500.f, 12900.1f, 13000.f, -7000.3f};
  for(unsigned i=0; i<6; ++i) {
    mesh.vertices.push_back({xs[i],float(i)*10.f+0.1f,0.f});
    mesh.vertices.push_back({xs[i],float(i)*10.f,700.7f});
    }
  mesh.vertices.push_back({1.f,2.f,3.f}); // referenced by nothing, as is vertex 9
  for(unsigned i=0; i<14; ++i)
    mesh.features.push_back({{float(i)*0.5f,-float(i)},0xFF000000u|(i*0x010203u),{0.f,1.f,-0.0f}});

  auto& p = mesh.polygons;
  auto tri = [&](uint32_t m, std::array<uint32_t,3> v, std::array<uint32_t,3> f, int32_t lm, int16_t sector) {
    p.material_indices.push_back(m);
    p.lightmap_indices.push_back(lm);
    for(unsigned c=0; c<3; ++c) {
      p.vertex_indices.push_back(v[c]);
      p.feature_indices.push_back(f[c]);
      }
    asset::PolygonFlagSet flags {};
    flags.is_sector = 1;
    flags.normal_axis = 2;
    flags.sector_index = sector;
    p.flags.push_back(flags);
    };
  tri(1,{0,1,2},{0,1,2},-1,-1);
  tri(2,{2,1,3},{2,1,3},5,7);
  tri(1,{2,3,4},{4,5,6},-1,-1);   // same vertices, other features
  tri(1,{4,5,6},{6,5,7},12,300);
  tri(2,{6,7,8},{8,9,10},-1,-32768);
  tri(1,{10,11,0},{11,12,13},-1,0);

  const auto dir = fs::temp_directory_path()/("content-convert-self-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  WorldChunkSource source {"TEST.ZEN",1234,0xDEADBEEFu};
  const auto manifest = writeWorldChunks(mesh,source,dir);
  require(manifest.chunks.size()==4,"expected 4 chunks, got "+std::to_string(manifest.chunks.size()));
  const auto back = readWorldChunkManifest(dir);
  require(back.source.world=="TEST.ZEN" && back.source.size==1234 && back.source.crc32==0xDEADBEEFu,"manifest source");
  require(back.triangles==6,"manifest triangles");

  const auto expected = canonicalWorldMesh(mesh);
  require(expected.materials.size()==3 && expected.vertices.size()==13,"canonical form keeps every source entry");
  require(back.vertices==13 && back.features==14 && back.materials.size()==3,"manifest counts");
  auto diff = compareWorldMeshes(expected,readWorldChunks(dir));
  require(diff.empty(),"round trip: "+diff);

  // A chunk in the override directory replaces the base one by name.
  const auto over = dir/"override";
  fs::create_directories(over/"chunks");
  fs::copy_file(dir/manifest.chunks[0],over/manifest.chunks[0]);
  diff = compareWorldMeshes(expected,readWorldChunks(dir,over));
  require(diff.empty(),"override round trip: "+diff);
  fs::remove(over/manifest.chunks[0]);
  fs::copy_file(dir/manifest.chunks[1],over/manifest.chunks[0]);
  bool rejected = false;
  try {
    readWorldChunks(dir,over);
    }
  catch(const std::exception&) {
    rejected = true; // the same source triangles twice
    }
  require(rejected,"duplicate triangle order must be rejected");

  // A static mesh: an unused position, an empty submesh, a sector-like name.
  asset::StaticMesh mrm;
  mrm.positions = {{0,0,0},{100.5f,0,0},{0,-20.25f,3},{7,7,7}};
  mrm.alpha_test = 1;
  mrm.bbox.min = {0,-20.25f,0};
  mrm.bbox.max = {100.5f,0,3};
  mrm.sub_meshes.resize(2);
  mrm.sub_meshes[0].mat = material("EMPTY","EMPTY.TGA",asset::MaterialGroup::WOOD);
  mrm.sub_meshes[1].mat = material("BARREL_\xE4","BARREL.TGA",asset::MaterialGroup::WOOD);
  mrm.sub_meshes[1].wedges = {{{0,1,0},{0,0},0},{{0,1,0},{1,0},1},{{0.6f,0.8f,-0.0f},{0.5f,1},2},{{0,1,0},{0,0},2}};
  mrm.sub_meshes[1].triangles = {{{0,1,2}},{{2,1,3}}};
  writeBaseMesh(mrm,"TEST_MESH",dir/"mesh.glb");
  diff = compareBaseMeshes(mrm,readBaseMesh(dir/"mesh.glb"));
  require(diff.empty(),"mesh round trip: "+diff);

  // A font through ZenKit's own writer and reader: latin-1 name, UV bits outside 0..1,
  // -0.0, infinity and a NaN payload must reach the record and come back unchanged.
  {
  const float nan = std::bit_cast<float>(0x7FC12345u);
  zenkit::Font fnt("FONT_\xE4_TEST",17,{});
  fnt.glyphs.push_back({0,{{0.f,0.f},{0.f,0.f}}});
  fnt.glyphs.push_back({9,{{-0.0f,1.5f},{-0.25f,nan}}});
  fnt.glyphs.push_back({255,{{std::bit_cast<float>(0x7F800000u),0.125f},{1.f,2.f}}});
  std::vector<std::byte> bytes;
  fnt.save(zenkit::Write::to(&bytes).get());
  zenkit::Font loaded;
  loaded.load(zenkit::Read::from(&bytes).get());
  const auto own = asset::fromZenKit(loaded);
  require(own.name=="FONT_\xE4_TEST" && own.height==17 && own.glyphs.size()==3,"font header");
  require(gothicglb::bits(own.glyphs[1].uv[1].y)==0x7FC12345u && gothicglb::bits(own.glyphs[1].uv[0].x)==0x80000000u,
          "font UV bits through ZenKit");
  require(FontData::atlasName("FONT_\xE4_TEST.fnt")=="FONT_\xE4_TEST.TGA","font atlas name");
  const auto file = dir/"FONT_TEST.FNT.json";
  FontData::writeFont(own,"FONT_TEST.TGA",file);
  std::string atlas;
  const auto read = FontData::readFont(file,&atlas);
  diff = FontData::compare(own,read);
  require(diff.empty(),"font round trip: "+diff);
  require(atlas=="FONT_TEST.TGA" && gothicglb::bits(read.glyphs[1].uv[1].y)==0x7FC12345u,"font record bits");
  auto changed = read;
  changed.glyphs[2].uv[0].x = std::bit_cast<float>(0x7F800001u);
  require(!FontData::compare(own,changed).empty(),"font compare must see float bits");
  // A short binary and an unknown version are rejected, not read as a partial font.
  const auto bin = dir/"FONT_TEST.FNT.bin";
  fs::resize_file(bin,fs::file_size(bin)-1);
  rejected = false;
  try { FontData::readFont(file); } catch(const std::exception&) { rejected = true; }
  require(rejected,"truncated font binary must be rejected");
  FontData::writeFont(own,"FONT_TEST.TGA",file);
  auto json = gothicglb::readFile(file);
  const std::string text(json.begin(),json.end());
  const auto at = text.find("\"version\":1");
  require(at!=std::string::npos,"font record version");
  json[at+10] = uint8_t('2');
  gothicglb::writeFile(file,json.data(),json.size());
  rejected = false;
  try { FontData::readFont(file); } catch(const std::exception&) { rejected = true; }
  require(rejected,"unsupported font record version must be rejected");
  }

  // A program record: generated name, raw string bytes with \0 and \xFF, float bits (NaN payload,
  // -0.0, infinity), a member and a FUNCTION without values; through the codec and the native factory.
  {
  using zenkit::DaedalusDataType;
  namespace Flag = zenkit::DaedalusSymbolFlag;
  zenkit::DaedalusProgramRecord program;
  program.version = 50;
  auto& gen = program.symbols.emplace_back();
  gen.name = "$INSTANCE_HELP"; gen.generated = true; gen.type = DaedalusDataType::INSTANCE; gen.address = 0;
  auto& ints = program.symbols.emplace_back();
  ints.name = "INTS"; ints.type = DaedalusDataType::INT; ints.count = 2; ints.flags = Flag::CONST;
  ints.int_values = {-1,0x7FFFFFFF}; ints.file_index = 3; ints.char_start = 0xFFFFFF;
  auto& floats = program.symbols.emplace_back();
  floats.name = "FLOATS"; floats.type = DaedalusDataType::FLOAT; floats.count = 3;
  floats.float_bits = {0x7FC12345u,0x80000000u,0x7F800000u};
  auto& strings = program.symbols.emplace_back();
  strings.name = "STRINGS"; strings.type = DaedalusDataType::STRING; strings.count = 2;
  strings.string_values = {std::string("a\xFF\0b",4),""};
  auto& cls = program.symbols.emplace_back();
  cls.name = "C_TEST"; cls.type = DaedalusDataType::CLASS; cls.count = 1; cls.class_size = 4;
  auto& member = program.symbols.emplace_back();
  member.name = "C_TEST.VALUE"; member.type = DaedalusDataType::INT; member.count = 1; member.flags = Flag::MEMBER;
  member.parent = 4; member.member_offset = 0;
  auto& fn = program.symbols.emplace_back();
  fn.name = "FN"; fn.type = DaedalusDataType::FUNCTION; fn.flags = Flag::RETURN; fn.address = 1;
  fn.return_type = DaedalusDataType::INT;
  program.bytecode = {std::byte{0x4B},std::byte{0x00},std::byte{0xFF}};

  const auto programs = dir/"programs";
  fs::create_directories(programs);
  std::string name;
  const auto sha = ProgramData::writeProgram(program,programs,&name);
  const auto file = programs/(name+".json");
  std::string actual;
  const auto read = ProgramData::readProgram(file,&actual);
  require(actual==sha && ProgramData::recordSha256(file)==sha,"program record identity");
  diff = ProgramData::compare(program,read);
  require(diff.empty(),"program round trip: "+diff);
  require(read.symbols[2].float_bits[0]==0x7FC12345u && read.symbols[3].string_values[0]==std::string("a\xFF\0b",4),
          "program record raw bits");
  diff = ProgramData::compare(program,zenkit::DaedalusScript::from_program(read).export_program());
  require(diff.empty(),"program through the native factory: "+diff);
  auto changed = read;
  changed.symbols[2].float_bits[1] ^= 1u;
  require(!ProgramData::compare(program,changed).empty(),"program compare must see float bits");
  auto shape = read;
  shape.symbols[1].int_values.pop_back();
  rejected = false;
  try { (void)zenkit::DaedalusScript::from_program(shape); } catch(const std::exception&) { rejected = true; }
  require(rejected,"program value shape mismatch must be rejected");
  const auto bin = programs/(name+".bin");
  fs::resize_file(bin,fs::file_size(bin)-1);
  rejected = false;
  try { ProgramData::readProgram(file); } catch(const std::exception&) { rejected = true; }
  require(rejected,"truncated program binary must be rejected");
  }

  // A dialog line library: archive order, duplicate names, a nested block shared by two roots, a null
  // message and raw bytes, through the import, the record and the three views.
  {
  const auto msg = [](std::string text, std::string name) {
    auto m = std::make_shared<zenkit::ConversationMessageEvent>();
    m->text = std::move(text); m->name = std::move(name);
    return m;
    };
  const auto atomic = [](std::shared_ptr<zenkit::ConversationMessageEvent> m) {
    auto a = std::make_shared<zenkit::CutsceneAtomicBlock>();
    a->message = std::move(m);
    return a;
    };
  const auto block = [](std::string name, auto inner) {
    auto b = std::make_shared<zenkit::CutsceneBlock>();
    b->name = std::move(name); b->block = inner;
    return b;
    };
  const std::string nestedText = "\x98 nested\xA0";
  auto shared = block("DIA_B_NESTED",atomic(msg(nestedText,"DIA_B_NESTED_01_01.WAV")));
  auto library = std::make_shared<zenkit::CutsceneLibrary>();
  library->blocks = {block("DIA_C_02_01",atomic(msg(std::string("c\0d",3),"DIA_C_02_01.WAV"))),
                     block("DIA_A_01_01",atomic(msg("first","DIA_A_01_01.WAV"))),
                     block("DIA_A_01_01",atomic(msg("second","DIA_A_01_01.WAV"))),
                     block("DIA_B_OUTER",shared),
                     block("DIA_E",shared),
                     block("DIA_D_NULL",atomic(nullptr))};
  std::vector<std::byte> bytes;
  {
  auto stream = zenkit::Write::to(&bytes);
  auto archive = zenkit::WriteArchive::to(stream.get(),zenkit::ArchiveFormat::BINSAFE);
  archive->write_object(library,zenkit::GameVersion::GOTHIC_2);
  archive->write_header();
  }
  const auto ouFile = dir/"OU.BIN";
  gothicglb::writeFile(ouFile,bytes.data(),bytes.size());
  WorldData::ZenFile own;
  WorldData::loadCutsceneLibrary(bytes,{"OU.BIN",bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())},own);
  const auto names = [](const std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks) {
    std::string out;
    for(const auto& b:blocks) out += b->name+";";
    return out;
    };
  require(own.kind=="cutscene_library" &&
          names(own.blocks)=="DIA_C_02_01;DIA_A_01_01;DIA_A_01_01;DIA_B_OUTER;DIA_E;DIA_D_NULL;",
          "cutscene library roots must keep archive order");
  const auto nestedOf = [](const asset::CutsceneBlock& b) { return std::get<std::shared_ptr<asset::CutsceneBlock>>(b.block); };
  require(nestedOf(*own.blocks[3])==nestedOf(*own.blocks[4]),"a shared nested block must stay one object");
  const auto record = dir/"ou.json";
  const auto identity = OutputUnits::writeLibrary(own,record);
  std::string actual;
  const auto read = OutputUnits::readLibrary(record,&actual);
  require(actual==identity,"cutscene library record identity");
  diff = WorldData::compareZen(own,read);
  require(diff.empty(),"cutscene library round trip: "+diff);
  require(nestedOf(*read.blocks[3])==nestedOf(*read.blocks[4]),"a shared nested block must stay one object in the record");
  const auto sorted = OutputUnits::sortedBlocks(read);
  require(names(sorted)=="DIA_A_01_01;DIA_A_01_01;DIA_B_OUTER;DIA_C_02_01;DIA_D_NULL;DIA_E;","engine view order");
  require(OutputUnits::blockByName(sorted,"A")->name=="DIA_A_01_01" &&
          OutputUnits::blockByName(sorted,"DIA_A_01_02")->name=="DIA_B_OUTER" &&
          OutputUnits::blockByName(sorted,"ZZZ")==nullptr,
          "block lookup must keep lower_bound successor behavior");
  require(OutputUnits::message(*OutputUnits::blockByName(sorted,"DIA_D_NULL"))==nullptr,"null message");
  require(OutputUnits::message(*OutputUnits::blockByName(sorted,"DIA_B_OUTER"))->text==nestedText,"nested message bytes");
  const auto messages = OutputUnits::messagesInArchiveOrder(read);
  require(messages.size()==4 && messages[0]->text==std::string("c\0d",3) && messages[1]->text=="first" &&
          messages[2]->text=="second" && messages[3]->text==nestedText,
          "server view: archive definition order, aliases once, raw bytes");
  require(BaseImport::convertOutputUnitsFile(ouFile,dir/"ou-file.json")==0 &&
          WorldData::compareZen(own,OutputUnits::readLibrary(dir/"ou-file.json")).empty(),
          "one-file conversion must give the same record");
  fs::resize_file(record,fs::file_size(record)-2);
  rejected = false;
  try { OutputUnits::readLibrary(record); } catch(const std::exception&) { rejected = true; }
  require(rejected,"truncated cutscene library record must be rejected");
  }

  // Sounds: a toy installation with one archive and a _work tree. Selection by request name and the
  // engine's Vfs::find winner (archive over its _work shadow), speech class, one payload for equal bytes
  // across extensions, a tiny .WAV copied as is, no non-audio name selected, an empty installation.
  {
  const auto wav = [](uint16_t tag, uint16_t bits, std::vector<uint8_t> data) {
    std::vector<uint8_t> b;
    const auto u32 = [&b](uint32_t v) { for(int i=0;i<4;++i) b.push_back(uint8_t(v>>(8*i))); };
    const auto u16 = [&b](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v>>8)); };
    const auto tagged = [&b](const char* t) { b.insert(b.end(),t,t+4); };
    tagged("RIFF"); u32(uint32_t(36+data.size())); tagged("WAVE");
    tagged("fmt "); u32(16); u16(tag); u16(1); u32(44100); u32(44100u*bits/8); u16(uint16_t(bits/8 ? bits/8 : 1)); u16(bits);
    tagged("data"); u32(uint32_t(data.size())); b.insert(b.end(),data.begin(),data.end());
    return b;
    };
  const auto pcm      = wav(1,16,{0x00,0x00,0xff,0x7f,0x00,0x80,0x01,0x00});
  const auto ima      = wav(17,4,{0x00,0x00,0x10,0x00,0x12,0x34,0x56,0x78});
  const std::vector<uint8_t> tiny    = {'R','I','F'};
  const std::vector<uint8_t> shadow  = {'a','r','c','h','i','v','e',' ','c','o','p','y'};
  const std::vector<uint8_t> riffBin = pcm;
  const auto game = dir/"audio-game";
  fs::create_directories(game/"Data");
  fs::create_directories(game/"_work/Data/Sound/SFX");
  {
  zenkit::Vfs vdf;
  vdf.mkdir("/_WORK/DATA/SOUND/SFX");
  vdf.mkdir("/_WORK/DATA/SOUND/SPEECH");
  const auto add = [&vdf](const char* dirName, const char* name, const std::vector<uint8_t>& b) {
    vdf.mount(zenkit::VfsNode::file(name,zenkit::VfsFileDescriptor{reinterpret_cast<const std::byte*>(b.data()),b.size(),false}),dirName);
    };
  add("/_WORK/DATA/SOUND/SFX","A_PCM.WAV",pcm);
  add("/_WORK/DATA/SOUND/SFX","C_SAME.OGG",pcm);
  add("/_WORK/DATA/SOUND/SFX","TINY.WAV",tiny);
  add("/_WORK/DATA/SOUND/SFX","SHADOW.WAV",shadow);
  add("/_WORK/DATA/SOUND/SFX","RIFF.BIN",riffBin);
  add("/_WORK/DATA/SOUND/SPEECH","B_IMA.WAV",ima);
  auto archive = zenkit::Write::to(game/"Data/SOUND.VDF");
  vdf.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  const std::string hostCopy = "host copy", hostOnly = "host only";
  gothicglb::writeFile(game/"_work/Data/Sound/SFX/SHADOW.WAV",hostCopy.data(),hostCopy.size());
  gothicglb::writeFile(game/"_work/Data/Sound/SFX/HOSTONLY.WAV",hostOnly.data(),hostOnly.size());

  const auto out = dir/"audio-out";
  require(BaseImport::convertAudio(game,out,false)==0 && BaseImport::convertAudio(game,out,true)==0,"audio import and verify");
  const auto index = AudioData::readIndex(out);
  std::string names;
  for(const auto& [name,s]:index.sounds) names += name+";";
  require(names=="A_PCM.WAV;B_IMA.WAV;C_SAME.OGG;HOSTONLY.WAV;SHADOW.WAV;TINY.WAV;","audio selection by request name");
  const auto& a = index.sounds.at("A_PCM.WAV");
  const auto& same = index.sounds.at("C_SAME.OGG");
  require(a.cls=="sound" && index.sounds.at("B_IMA.WAV").cls=="speech" && a.origin=="archive" && a.archive=="SOUND.VDF",
          "audio class and origin");
  require(same.payload==a.payload && same.file==a.file && a.file.size()>4 && a.file.substr(a.file.size()-4)==".wav",
          "equal bytes share the first request's payload file");
  require(index.sounds.at("HOSTONLY.WAV").origin=="host" &&
          index.sounds.at("HOSTONLY.WAV").path=="_work/Data/Sound/SFX/HOSTONLY.WAV","host-only sound");
  require(index.find("a_pcm.wav \t")==&a && index.find(" A_PCM.WAV")==nullptr && index.find("SFX/A_PCM.WAV")==nullptr,
          "audio lookup trims trailing whitespace only, without a basename fallback");
  std::vector<uint8_t> bytes;
  AudioData::readPayload(index,index.sounds.at("SHADOW.WAV"),"SHADOW.WAV",bytes);
  require(bytes==shadow,"the archive copy wins over its _work shadow");
  AudioData::readPayload(index,index.sounds.at("TINY.WAV"),"TINY.WAV",bytes);
  require(bytes==tiny,"tiny sounds are copied as they are");
  AudioData::readPayload(index,a,"A_PCM.WAV",bytes);
  require(bytes==pcm,"payload bytes");

  const auto payload = out/a.file;
  const auto rejectedRead = [&](const char* what) {
    bool failed = false;
    bytes = {1,2,3};
    try { AudioData::readPayload(index,a,"A_PCM.WAV",bytes); }
    catch(const std::exception& e) { failed = std::string(e.what()).find("A_PCM.WAV")!=std::string::npos; }
    require(failed && bytes.empty(),std::string("audio payload must be rejected: ")+what);
    };
  auto altered = pcm;
  altered.back() ^= 1;
  gothicglb::writeFile(payload,altered.data(),altered.size());
  rejectedRead("same size, different bytes");
  gothicglb::writeFile(payload,pcm.data(),pcm.size()-1);
  rejectedRead("resized");
  fs::remove(payload);
  rejectedRead("missing");

  const auto empty = dir/"audio-empty";
  fs::create_directories(empty/"Data");
  fs::create_directories(empty/"_work");
  {
  zenkit::Vfs vdf;
  const std::vector<uint8_t> marker = {1};
  vdf.mount(zenkit::VfsNode::file("FIXTURE.BIN",zenkit::VfsFileDescriptor{reinterpret_cast<const std::byte*>(marker.data()),marker.size(),false}),"/");
  auto archive = zenkit::Write::to(empty/"Data/EMPTY.VDF");
  vdf.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  require(BaseImport::convertAudio(empty,dir/"audio-empty-out",false)==0 &&
          AudioData::readIndex(dir/"audio-empty-out").sounds.empty(),"an installation without sounds gives an empty index");
  }

  {
  // P16 music: synthetic RIFF files both providers parse, an installation around them, both record codecs
  // and the GothicKit string check.
  using Bytes = std::vector<uint8_t>;
  const auto put16 = [](Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v>>8)); };
  const auto put32 = [](Bytes& b, uint32_t v) { for(int i=0; i<4; ++i) b.push_back(uint8_t(v>>(8*i))); };
  const auto chunk = [&](const char* id, const Bytes& body) {
    Bytes b(id,id+4);
    put32(b,uint32_t(body.size()));
    b.insert(b.end(),body.begin(),body.end());
    if(body.size()%2)
      b.push_back(0);
    return b;
    };
  const auto list = [&](const char* id, const char* type, std::initializer_list<Bytes> children) {
    Bytes body(type,type+4);
    for(const auto& c:children)
      body.insert(body.end(),c.begin(),c.end());
    return chunk(id,body);
    };
  const auto utf16 = [&](std::u16string_view s, bool terminated) {
    Bytes b;
    for(const auto c:s)
      put16(b,uint16_t(c));
    if(terminated)
      put16(b,0);
    return b;
    };
  const auto unfo = [&](const Bytes& unam) { return list("LIST","UNFO",{chunk("UNAM",unam)}); };
  Bytes guid;
  for(uint8_t i=1; i<=16; ++i)
    guid.push_back(i);

  Bytes segh;
  for(const uint32_t v:std::initializer_list<uint32_t>{1,768,0,0,768,0})
    put32(segh,v);
  const auto segment = [&](const Bytes& unam) { return list("RIFF","DMSG",{chunk("segh",segh),chunk("guid",guid),unfo(unam)}); };
  const auto seg = segment(utf16(u"Seg",true));
  Bytes styh = {4,4,4,0};
  {
  const double tempo = 120.0;
  uint64_t bits = 0;
  std::memcpy(&bits,&tempo,sizeof bits);
  for(int i=0; i<8; ++i)
    styh.push_back(uint8_t(bits>>(8*i)));
  }
  const auto sty = list("RIFF","DMST",{chunk("guid",guid),chunk("styh",styh),unfo(utf16(u"Sty",true))});
  // One instrument whose region key range ends above 255: GothicKit parses it, Dx8's Hydra finalization throws.
  Bytes colh, insh, rgnh;
  put32(colh,1);
  for(const uint32_t v:std::initializer_list<uint32_t>{1,0,5})
    put32(insh,v);
  for(const uint16_t v:std::initializer_list<uint16_t>{0,300,0,127,0,0})
    put16(rgnh,v);
  const auto dls = list("RIFF","DLS ",{chunk("dlid",guid),chunk("colh",colh),
                        list("LIST","lins",{list("LIST","ins ",{chunk("insh",insh),
                             list("LIST","lrgn",{list("LIST","rgn ",{chunk("rgnh",rgnh)})})})})});

  // The string check runs before the old parser converts in place; the offset names the UNAM chunk
  // (RIFF header 12, segh 32, guid 24, UNFO list header 12).
  const size_t unamAt = 12+32+24+12;
  size_t bad = 99;
  require(MusicNative_checkGkStrings(seg.data(),seg.size(),&bad)==DmResult_SUCCESS && bad==0,"ASCII GothicKit strings pass");
  const auto nonAscii = segment(utf16(u"Seä",true));
  require(MusicNative_checkGkStrings(nonAscii.data(),nonAscii.size(),&bad)==DmResult_FILE_CORRUPT && bad==unamAt,
          "a non-ASCII GothicKit string is rejected before parsing");
  const auto unterminated = segment(utf16(u"Seg!",false));
  require(MusicNative_checkGkStrings(unterminated.data(),unterminated.size(),&bad)==DmResult_FILE_CORRUPT && bad==unamAt,
          "an unterminated GothicKit string is rejected before parsing");

  // GothicKit: old parser -> record -> factory -> the same record; every malformed record is refused whole.
  DmSegment* parsed = nullptr;
  require(MusicNative_parseSegment(seg.data(),seg.size(),&parsed)==DmResult_SUCCESS && parsed!=nullptr,
          "GothicKit parses the synthetic segment");
  uint8_t* enc = nullptr;
  size_t   encLen = 0;
  require(MusicNative_encodeSegment(parsed,&enc,&encLen)==DmResult_SUCCESS,"GothicKit segment encodes");
  MusicNative_releaseSegment(parsed);
  const Bytes gkSeg(enc,enc+encLen);
  std::free(enc);
  DmSegment* made = nullptr;
  require(MusicNative_segment(gkSeg.data(),gkSeg.size(),&made)==DmResult_SUCCESS && made!=nullptr,"GothicKit factory");
  require(MusicNative_encodeSegment(made,&enc,&encLen)==DmResult_SUCCESS && Bytes(enc,enc+encLen)==gkSeg,
          "the factory object encodes as the parsed one");
  std::free(enc);
  MusicNative_releaseSegment(made);
  for(size_t n=0; n<gkSeg.size(); ++n) {
    DmSegment* cut = nullptr;
    require(MusicNative_segment(gkSeg.data(),n,&cut)==DmResult_FILE_CORRUPT && cut==nullptr,
            "a truncated GothicKit record is refused at "+std::to_string(n));
    }
  {
  auto longer = gkSeg;
  longer.push_back(0);
  DmSegment* extra = nullptr;
  require(MusicNative_segment(longer.data(),longer.size(),&extra)==DmResult_FILE_CORRUPT && extra==nullptr,
          "trailing bytes in a GothicKit record are refused");
  }

  // Dx8 codec: every field memberwise, owning lengths kept (embedded NUL), optional tracks.
  const auto rejected = [](const std::function<void()>& fn) {
    try { fn(); }
    catch(const std::exception&) { return true; }
    return false;
    };
  Dx8::Segment dseg;
  dseg.track.emplace_back();
  auto& track = dseg.track.back();
  track.head.dwGroup = 7;
  std::memcpy(track.head.ckid,"sttr",4);
  track.sttr = std::make_shared<Dx8::Track::StyleTrack>();
  track.sttr->styles.emplace_back();
  track.sttr->styles.back().stmp = 3;
  track.sttr->styles.back().reference.file = std::u16string(u"A\0B.STY",7);
  track.cord = std::make_shared<Dx8::Track::Chord>();
  track.cord->ioChord.wszName[0] = u'C';
  track.cord->subchord.resize(2);
  track.cord->subchord[1].bScaleRoot = 9;
  track.cmnd = std::make_shared<Dx8::Track::CommandTrack>();
  track.cmnd->commands.resize(1);
  track.cmnd->commands[0].bCommand = Dx8::DMUS_COMMANDT_FILL;
  dseg.track.emplace_back();
  const auto segBin  = MusicData::encodeDx8(dseg);
  const auto segBack = MusicData::decodeDx8Segment(segBin);
  require(MusicData::encodeDx8(segBack)==segBin && segBack.track.size()==2 && segBack.track[1].sttr==nullptr &&
          segBack.track[0].sttr->styles[0].reference.file==std::u16string(u"A\0B.STY",7) &&
          segBack.track[0].cord->subchord[1].bScaleRoot==9 && segBack.track[0].cmnd->commands[0].bCommand==Dx8::DMUS_COMMANDT_FILL,
          "Dx8 segment codec");
  {
  auto cut = segBin;
  cut.pop_back();
  auto longer = segBin;
  longer.push_back(0);
  require(rejected([&] { (void)MusicData::decodeDx8Segment(cut); }) &&
          rejected([&] { (void)MusicData::decodeDx8Segment(longer); }),"a malformed Dx8 record is refused");
  }

  Dx8::Style dsty;
  dsty.styh.dblTempo = 97.5;
  dsty.band.emplace_back();
  dsty.band.back().intrument.emplace_back();
  dsty.band.back().intrument.back().header.dwPatch = 0x10203;
  dsty.band.back().intrument.back().reference.file = u"BANK.DLS";
  dsty.parts.emplace_back();
  dsty.parts.back().notes.resize(2);
  dsty.parts.back().notes[1].nTimeOffset = -5;
  dsty.parts.back().curves.resize(1);
  dsty.parts.back().curves[0].wMergeIndex = 11;
  dsty.patterns.emplace_back();
  dsty.patterns.back().partref.resize(1);
  dsty.patterns.back().info.unam = u"P";
  const auto styBin  = MusicData::encodeDx8(dsty);
  const auto styBack = MusicData::decodeDx8Style(styBin);
  require(MusicData::encodeDx8(styBack)==styBin && styBack.styh.dblTempo==97.5 && styBack.parts[0].notes[1].nTimeOffset==-5 &&
          styBack.band[0].intrument[0].reference.file==u"BANK.DLS" && styBack.patterns[0].info.unam==u"P","Dx8 style codec");

  Dx8::DlsCollection::Parsed ddls;
  ddls.version = 0x0001000200030004ull;
  ddls.instrument.emplace_back();
  auto& ins = ddls.instrument.back();
  ins.info.inam = "I";
  ins.regions.emplace_back();
  ins.regions.back().head.RangeKey.usHigh = 127;
  ins.regions.back().loop.resize(1);
  ins.articulators.emplace_back();
  ins.articulators.back().connectionBlocks.resize(1);
  ins.articulators.back().connectionBlocks[0].lScale = -65536;
  ddls.wave.emplace_back();
  auto& wave = ddls.wave.back();
  wave.wfmt.wFormatTag     = Dx8::Wave::PCM;
  wave.wfmt.wBitsPerSample = 16;
  wave.extra    = {1,2};
  wave.wavedata = {3,4,5,6};
  wave.loop.resize(1);
  const auto dlsBin  = MusicData::encodeDx8(ddls);
  const auto dlsBack = MusicData::decodeDx8Dls(dlsBin);
  require(MusicData::encodeDx8(dlsBack)==dlsBin && dlsBack.version==ddls.version &&
          dlsBack.instrument[0].articulators[0].connectionBlocks[0].lScale==-65536 &&
          dlsBack.wave[0].wavedata==wave.wavedata && dlsBack.wave[0].extra==wave.extra,"Dx8 DLS codec");

  // An installation: three archive files and one host file in the first Dx8 search directory.
  const auto game = dir/"music-game";
  fs::create_directories(game/"Data");
  fs::create_directories(game/"_work/Data/Music/newworld");
  {
  zenkit::Vfs vdf;
  vdf.mkdir("/_WORK/DATA/MUSIC/NEWWORLD");
  const auto add = [&vdf](const char* name, const Bytes& b) {
    vdf.mount(zenkit::VfsNode::file(name,zenkit::VfsFileDescriptor{reinterpret_cast<const std::byte*>(b.data()),b.size(),false}),
              "/_WORK/DATA/MUSIC/NEWWORLD");
    };
  add("SEG.SGT",seg);
  add("STY.STY",sty);
  add("BANK.DLS",dls);
  auto archive = zenkit::Write::to(game/"Data/MUSIC.VDF");
  vdf.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  gothicglb::writeFile(game/"_work/Data/Music/newworld/HOST.SGT",seg.data(),seg.size());

  const auto out = dir/"music-out";
  require(BaseImport::convertMusic(game,out,false)==0 && BaseImport::convertMusic(game,out,true)==0,"music import and verify");
  const auto index = MusicData::readIndex(out);
  std::string names;
  for(const auto& [name,i]:index.gk)
    names += name+";";
  require(names=="BANK.DLS;HOST.SGT;SEG.SGT;STY.STY;","GothicKit selection by basename");
  require(index.dx8.size()==1 && index.dx8.count("_work/Data/Music/newworld/HOST.SGT")==1,"Dx8 table: files of the search directories");
  const auto* s     = index.findGk("seg.sgt \t");
  const auto* host  = index.findDx8("_WORK/data/Music/NewWorld/host.sgt");
  const auto* bank  = index.findGk("BANK.DLS");
  const auto* style = index.findGk("STY.STY");
  require(s!=nullptr && s==index.findGk("SEG.SGT") && index.findGk(" SEG.SGT")==nullptr && index.findGk("NEWWORLD/SEG.SGT")==nullptr &&
          host!=nullptr && host==index.findGk("HOST.SGT") && bank!=nullptr && style!=nullptr && index.findDx8("SEG.SGT")==nullptr,
          "music lookups");
  require(s->origin=="archive" && s->archive=="MUSIC.VDF" && host->origin=="host" && s->kind=="segment" &&
          bank->kind=="dls" && style->kind=="style","music provenance and kinds");
  require(s->gk.record.rfind("gk/segment/",0)==0 && s->dx8.record.rfind("dx8/segment/",0)==0 && !s->gk.failure && !s->dx8.failure,
          "a segment parsed by both providers");
  require(host->gk.record==s->gk.record && host->dx8.record==s->dx8.record,"equal bytes share their records");
  require(s->gk.identity!=s->sha256 && s->gk.record.substr(s->gk.record.rfind('/')+1)!=s->sha256,
          "record names and identities are not the source SHA-256");
  require(style->gk.record.rfind("gk/style/",0)==0 && style->dx8.record.rfind("dx8/style/",0)==0,"a style parsed by both providers");
  require(bank->gk.record.rfind("gk/dls/",0)==0 && bank->dx8.record.empty() && bank->dx8.failure &&
          *bank->dx8.failure=="hydra.igen range error","a Hydra-rejected DLS is a Dx8 failure; GothicKit still parses it");

  const auto key = s->gk.record;
  const auto gkBin = MusicData::readRecord(index,key);
  require(gkBin==gkSeg,"the GothicKit segment record is the parsed object");
  require(MusicData::decodeDx8Segment(MusicData::readRecord(index,s->dx8.record)).track.empty(),"Dx8 segment without a track list");
  const auto sha      = key.substr(key.rfind('/')+1);
  const auto binFile  = out/"gk"/"segment"/(sha+".bin");
  const auto jsonFile = out/"gk"/"segment"/(sha+".json");
  const auto rejectedRecord = [&](const std::string& k, const char* what) {
    bool failed = false;
    try { (void)MusicData::readRecord(index,k); }
    catch(const std::exception& e) { failed = std::string(e.what()).find(k)!=std::string::npos; }
    require(failed,std::string("music record must be rejected: ")+what);
    };
  rejectedRecord("gk/style/"+sha,"key of another kind");
  auto altered = gkBin;
  altered.back() ^= 1;
  gothicglb::writeFile(binFile,altered.data(),altered.size());
  rejectedRecord(key,"same size, different bytes");
  gothicglb::writeFile(binFile,gkBin.data(),gkBin.size()-1);
  rejectedRecord(key,"resized");
  fs::remove(binFile);
  rejectedRecord(key,"missing");
  gothicglb::writeFile(binFile,gkBin.data(),gkBin.size());
  auto json = gothicglb::readFile(jsonFile);
  const auto jsonBack = json;
  json.push_back(' ');
  gothicglb::writeFile(jsonFile,json.data(),json.size());
  rejectedRecord(key,"altered envelope");
  gothicglb::writeFile(jsonFile,jsonBack.data(),jsonBack.size());
  require(MusicData::readRecord(index,key)==gkBin,"a restored record reads again");

  // A string the old parser cannot convert exactly stops the import with the file named.
  const auto badGame = dir/"music-bad";
  fs::create_directories(badGame/"Data");
  fs::create_directories(badGame/"_work");
  {
  zenkit::Vfs vdf;
  vdf.mount(zenkit::VfsNode::file("BAD.SGT",zenkit::VfsFileDescriptor{reinterpret_cast<const std::byte*>(nonAscii.data()),nonAscii.size(),false}),"/");
  auto archive = zenkit::Write::to(badGame/"Data/BAD.VDF");
  vdf.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  bool stopped = false;
  try { (void)BaseImport::convertMusic(badGame,dir/"music-bad-out",false); }
  catch(const std::exception& e) { stopped = std::string(e.what()).find("BAD.SGT")!=std::string::npos; }
  require(stopped,"a GothicKit string its parser cannot convert stops the music import");

  const auto empty = dir/"music-empty";
  fs::create_directories(empty/"Data");
  fs::create_directories(empty/"_work");
  {
  zenkit::Vfs vdf;
  const std::vector<uint8_t> marker = {1};
  vdf.mount(zenkit::VfsNode::file("FIXTURE.BIN",zenkit::VfsFileDescriptor{reinterpret_cast<const std::byte*>(marker.data()),marker.size(),false}),"/");
  auto archive = zenkit::Write::to(empty/"Data/EMPTY.VDF");
  vdf.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  require(BaseImport::convertMusic(empty,dir/"music-empty-out",false)==0,"music import of an installation without music");
  const auto emptyIndex = MusicData::readIndex(dir/"music-empty-out");
  require(emptyIndex.sources.empty() && emptyIndex.records.empty() && emptyIndex.gk.empty() && emptyIndex.dx8.empty(),
          "an installation without music gives an empty index");
  }

  WorldData::selfTest(dir/"world-data");
  fs::remove_all(dir);
  std::cout << "self-test passed\n";
  return 0;
  }

}

int main(int argc, char** argv) {
  try {
    zenkit::Logger::set(zenkit::LogLevel::ERROR,[](auto level,const char* component,const char* message) {
      std::cerr << "ZenKit " << component << ": " << message << '\n';
      BaseImport::onZenkitLog(level,component,message);
      });
    const std::vector<std::string> args(argv+1,argv+argc);
    if(args.size()==1 && args[0]=="--self-test")
      return selfTest();
    if(args.size()==3 && args[0]=="check-particles") {
      const auto result=ParticleContent::check(args[1],args[2]);
      for(const auto& error:result.errors)
        std::cerr << error << '\n';
      if(!result.errors.empty()) return 1;
      std::cout << args[2] << ": valid (" << result.effects.size() << " particle effects)\n";
      return 0;
      }
    if((args.size()==3 || args.size()==4) && args[0]=="check-vob-layer") {
      const auto result=VobLayer::check(args[1],args[2],args.size()==4 ? fs::path(args[3]) : fs::path());
      for(const auto& error:result.errors)
        std::cerr << error << '\n';
      if(!result.errors.empty()) return 1;
      std::cout << args[2] << ": valid (" << result.added << " added, " << result.removed
                << " removed, " << result.moved << " moved)\n";
      return 0;
      }
    if(args.size()==4 && args[0]=="world")
      return BaseImport::convertWorld(args[1],args[2],args[3]);
    if(args.size()==3 && args[0]=="base") {
      BaseImport::build(args[1],args[2]);
      return 0;
      }
    if(args.size()==3 && (args[0]=="zen" || args[0]=="verify-zen"))
      return BaseImport::convertZen(args[1],args[2],args[0]=="verify-zen");
    if(args.size()==3 && (args[0]=="meshes" || args[0]=="verify-meshes"))
      return BaseImport::convertMeshes(args[1],args[2],args[0]=="verify-meshes");
    if(args.size()==3 && (args[0]=="models" || args[0]=="verify-models"))
      return BaseImport::convertModels(args[1],args[2],args[0]=="verify-models");
    if(args.size()==3 && (args[0]=="fonts" || args[0]=="verify-fonts"))
      return BaseImport::convertFonts(args[1],args[2],args[0]=="verify-fonts");
    if(args.size()==3 && (args[0]=="programs" || args[0]=="verify-programs"))
      return BaseImport::convertPrograms(args[1],args[2],args[0]=="verify-programs");
    if(args.size()==3 && (args[0]=="outputunits" || args[0]=="verify-outputunits"))
      return BaseImport::convertOutputUnits(args[1],args[2],args[0]=="verify-outputunits");
    if(args.size()==3 && args[0]=="outputunits-file")
      return BaseImport::convertOutputUnitsFile(args[1],args[2]);
    if(args.size()==3 && (args[0]=="audio" || args[0]=="verify-audio"))
      return BaseImport::convertAudio(args[1],args[2],args[0]=="verify-audio");
    if(args.size()==3 && (args[0]=="music" || args[0]=="verify-music"))
      return BaseImport::convertMusic(args[1],args[2],args[0]=="verify-music");
    if(args.size()==3 && (args[0]=="videos" || args[0]=="verify-videos"))
      return VideoData::convert(args[1],args[2],args[0]=="verify-videos");
    if(args.size()==2 && (args[0]=="resolve-base" || args[0]=="resolve" || args[0]=="resolve-videos")) {
      // Importers also write detailed reports to cout. Keep stdout exclusively for the result path.
      struct StdoutRedirect {
        std::streambuf* original;
        ~StdoutRedirect() { std::cout.rdbuf(original); }
        } redirect{std::cout.rdbuf(std::cerr.rdbuf())};
      std::ostream output(redirect.original);
      const auto progress=[](std::string_view message) { std::cerr << message << '\n'; };
      fs::path selected;
      BaseStore::Selection base;
      if(args[0]!="resolve-videos") {
        BaseImport::Files archives;
        const auto* explicitBase=std::getenv("OPENGOTHIC_BASE");
        const auto* fingerprint=std::getenv("OPENGOTHIC_BASE_FINGERPRINT");
        if((explicitBase && *explicitBase) || !fingerprint || !*fingerprint)
          for(const auto& archive:BaseImport::runtimeArchives(args[1]))
            if(!archive.mod) archives.emplace_back(archive.name,archive.size);
        base=BaseStore::ensure(args[1],archives,progress);
        if(base.publication.valid()) base.publication.get();
        selected=base.path;
        }
      const auto* videoFingerprint=std::getenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
      const bool videosDisabled=args[0]=="resolve" && videoFingerprint && !*videoFingerprint;
      if(videosDisabled)
        std::cerr << "video base: disabled by OPENGOTHIC_VIDEOS_FINGERPRINT\n";
      else if(args[0]!="resolve-base") {
        try {
          auto videos=BaseStore::ensureVideos(args[1],progress);
          if(videos.publication.valid()) videos.publication.get();
          if(args[0]=="resolve-videos") selected=videos.path;
          else if(VideoData::readIndex(videos.path).empty())
            std::cerr << "video base: warning: installation contains no videos\n";
          }
        catch(const std::exception& error) {
          if(args[0]!="resolve") throw;
          std::cerr << "video base: warning: " << error.what() << '\n';
          }
        }
      output << selected.string() << std::endl;
      return 0;
      }
    if(args.size()==3 && (args[0]=="sources" || args[0]=="verify-sources"))
      return BaseImport::convertSources(args[1],args[2],args[0]=="verify-sources");
    if(args.size()==3 && (args[0]=="textures" || args[0]=="verify-textures"))
      return BaseImport::convertTextures(args[1],args[2],args[0]=="verify-textures");
    if((args.size()==4 || args.size()==5) && args[0]=="verify-world") {
      return BaseImport::verifyWorld(args[1],args[2],args[3],args.size()==5 ? fs::path(args[4]) : fs::path());
      }
    std::cerr << "usage: content-convert world|verify-world <gothic dir> <WORLD.ZEN> <dir> [override]\n"
                 "       content-convert check-vob-layer <base world dir> <layer.json> [content dir]\n"
                 "       content-convert check-particles <base root> <content dir>\n"
                 "       content-convert textures|verify-textures|meshes|verify-meshes|zen|verify-zen <gothic dir> <dir>\n"
                 "       content-convert models|verify-models <gothic dir> <dir>\n"
                 "       content-convert fonts|verify-fonts <gothic dir> <dir>\n"
                 "       content-convert programs|verify-programs <gothic dir> <dir>\n"
                 "       content-convert outputunits|verify-outputunits <gothic dir> <dir>\n"
                 "       content-convert outputunits-file <OU.BIN|OU.DAT> <record.json>\n"
                 "       content-convert audio|verify-audio <gothic dir> <dir>\n"
                 "       content-convert music|verify-music <gothic dir> <dir>\n"
                 "       content-convert videos|verify-videos <gothic dir> <dir>\n"
                 "       content-convert resolve-base|resolve-videos|resolve <gothic dir>\n"
                 "       content-convert sources|verify-sources <gothic dir> <dir>\n"
                 "       content-convert base <gothic dir> <dir>\n"
                 "       content-convert --self-test\n";
    return 2;
    }
  catch(const std::exception& e) {
    std::cerr << "content-convert: " << e.what() << "\n";
    return 1;
    }
  }

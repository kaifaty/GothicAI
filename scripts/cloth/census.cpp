// Extract original mesh-vob and mesh geometry evidence without launching the game.
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/Texture.hh>
#include <zenkit/vobs/VirtualObject.hh>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>

static std::string upper(std::string value) {
  for(auto& c:value) c=char(std::toupper(static_cast<unsigned char>(c)));
  return value;
}

static void csv(std::string_view text) {
  std::cout << '"';
  for(char c:text) { if(c=='"') std::cout << '"'; std::cout << c; }
  std::cout << '"';
}

static size_t visit(const zenkit::VirtualObject& v, std::string_view world) {
  size_t count=1;
  if(v.visual && !v.visual->name.empty()) {
    csv(world); std::cout << ',' << v.id << ','; csv(upper(v.visual->name));
    std::cout << ',' << int(v.anim_mode) << ',' << v.anim_strength
              << ',' << v.position.x << ',' << v.position.y << ',' << v.position.z
              << ',' << v.show_visual << ',' << v.cd_static << ',' << v.cd_dynamic
              << ',' << int(v.visual->type) << '\n';
  }
  for(auto& c:v.children) count+=visit(*c,world);
  return count;
}

int main(int argc,char** argv) {
  try {
    if(argc<2) throw std::runtime_error("usage: cloth-census GAME_ROOT [VISUAL.3DS ...]");
    zenkit::Vfs vfs;
    std::vector<std::filesystem::path> disks;
    for(auto& item:std::filesystem::directory_iterator(std::filesystem::path(argv[1])/"Data"))
      if(upper(item.path().extension().string())==".VDF") disks.push_back(item.path());
    std::sort(disks.begin(),disks.end());
    // Match Resources::loadVdfs: replace older entries with the newer addon
    // resources. NEWER would replace newer entries and select the base worlds.
    for(auto& disk:disks) vfs.mount_disk(disk,zenkit::VfsOverwriteBehavior::OLDER);
    std::cout << std::fixed << std::setprecision(4);
    if(argc==4 && std::string_view(argv[2])=="--texture") {
      auto name=upper(argv[3]);
      auto compiled=std::filesystem::path(name).replace_extension("").string()+"-C.TEX";
      auto* file=vfs.find(compiled);
      if(!file) throw std::runtime_error("missing texture "+compiled);
      zenkit::Texture texture;
      auto input=file->open_read(); texture.load(input.get());
      auto rgba=texture.as_rgba8();
      std::cout << "P6\n" << texture.width() << ' ' << texture.height() << "\n255\n";
      for(size_t p=0;p<rgba.size();p+=4)
        std::cout.write(reinterpret_cast<const char*>(rgba.data()+p),3);
    } else if(argc==2 || std::string_view(argv[2])=="--world-materials") {
      const bool materials=argc>2;
      if(!materials)
        std::cout << "world,zen_id,visual,anim_mode,anim_strength,x_cm,y_cm,z_cm,show_visual,cd_static,cd_dynamic,visual_type\n";
      for(auto name:{"NEWWORLD.ZEN","OLDWORLD.ZEN","ADDONWORLD.ZEN","DRAGONISLAND.ZEN"}) {
        auto* file=vfs.find(name);
        if(!file) throw std::runtime_error(std::string("missing world ")+name);
        zenkit::World world;
        auto input=file->open_read();
        world.load(input.get(),zenkit::GameVersion::GOTHIC_2);
        if(!materials) {
          size_t count=0;
          for(auto& v:world.world_vobs) count+=visit(*v,name);
          std::cerr << name << ": " << count << " vobs\n";
        }
        else {
          auto& mesh=world.world_mesh;
          std::map<unsigned,std::vector<size_t>> faces;
          for(size_t i=0;i<mesh.polygons.material_indices.size();++i)
            faces[mesh.polygons.material_indices[i]].push_back(i);
          for(size_t i=0;i<mesh.materials.size();++i) {
            auto& mat=mesh.materials[i];
            std::cout << "worldmaterial," << name << ',' << i << ','; csv(mat.name);
            std::cout << ','; csv(mat.texture);
            std::cout << ',' << int(mat.wave_mode) << ',' << mat.wave_max_amplitude << ',' << faces[i].size() << '\n';
            if(std::regex_search(upper(mat.name+" "+mat.texture), std::regex("FLAG|BANNER|SAIL|SEGEL|FAHN|CLOTH|CURTAIN|VORHANG|SIGN|SCHILD|STOFF|TEXTIL|CANOPY")))
              for(auto f:faces[i]) {
                std::cout << "worldface," << name << ',' << i << ',' << f;
                for(int c=0;c<3;++c) {
                  auto index=mesh.polygons.vertex_indices[3*f+c];
                  auto& p=mesh.vertices[index];
                  std::cout << ',' << index << ',' << p.x << ',' << p.y << ',' << p.z;
                }
                std::cout << '\n';
              }
          }
        }
      }
    } else {
      for(int i=2;i<argc;++i) {
        auto visual=upper(argv[i]);
        const bool morph=std::filesystem::path(visual).extension()==".MMS";
        auto compiled=std::filesystem::path(visual).replace_extension(morph ? ".MMB" : ".MRM").string();
        auto* file=vfs.find(compiled);
        if(!file) throw std::runtime_error("missing mesh "+compiled);
        zenkit::MultiResolutionMesh mesh;
        auto input=file->open_read();
        if(morph) {
          zenkit::MorphMesh mm; mm.load(input.get());
          mesh=std::move(mm.mesh);
          for(auto& a:mm.animations) {
            std::cout << "animation," << visual << ',' << a.name << ',' << a.vertices.size() << ',' << a.frame_count << '\n';
            for(auto id:a.vertices) std::cout << "animatedvertex," << id << '\n';
          }
        } else mesh.load(input.get());
        std::cout << "mesh," << visual << ',' << mesh.positions.size() << ',' << mesh.sub_meshes.size() << '\n';
        for(size_t n=0;n<mesh.positions.size();++n) {
          auto& p=mesh.positions[n];
          std::cout << "v," << n << ',' << p.x << ',' << p.y << ',' << p.z << '\n';
        }
        for(size_t m=0;m<mesh.sub_meshes.size();++m) {
          auto& sub=mesh.sub_meshes[m];
          std::cout << "material," << m << ','; csv(sub.mat.name); std::cout << ','; csv(sub.mat.texture); std::cout << '\n';
          std::cout << "materialflags," << m << ',' << int(sub.mat.group)
                    << ',' << sub.mat.disable_collision << ',' << sub.mat.disable_lightmap << '\n';
          for(auto& t:sub.triangles)
            std::cout << "f," << m << ',' << sub.wedges[t.wedges[0]].index << ',' << sub.wedges[t.wedges[1]].index << ',' << sub.wedges[t.wedges[2]].index << '\n';
        }
      }
    }
  } catch(const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}

#include "physics/worldcloth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  bool sameFlags(const asset::PolygonFlagSet& a, const asset::PolygonFlagSet& b) {
    return a.is_portal==b.is_portal && a.is_occluder==b.is_occluder &&
           a.is_sector==b.is_sector && a.should_relight==b.should_relight &&
           a.is_outdoor==b.is_outdoor && a.is_ghost_occluder==b.is_ghost_occluder &&
           a.is_dynamically_lit==b.is_dynamically_lit && a.sector_index==b.sector_index &&
           a.is_lod==b.is_lod && a.normal_axis==b.normal_axis;
    }

  bool sameMaterial(const asset::Material& a, const asset::Material& b) {
    return std::tie(a.name,a.group,a.color,a.smooth_angle,a.texture,a.texture_scale,
                    a.texture_anim_fps,a.texture_anim_map_mode,a.texture_anim_map_dir,
                    a.disable_collision,a.disable_lightmap,a.dont_collapse,a.detail_object,
                    a.detail_object_scale,a.force_occluder,a.environment_mapping,
                    a.environment_mapping_strength,a.wave_mode,a.wave_speed,
                    a.wave_max_amplitude,a.wave_grid_size,a.ignore_sun,a.alpha_func,a.default_mapping)==
           std::tie(b.name,b.group,b.color,b.smooth_angle,b.texture,b.texture_scale,
                    b.texture_anim_fps,b.texture_anim_map_mode,b.texture_anim_map_dir,
                    b.disable_collision,b.disable_lightmap,b.dont_collapse,b.detail_object,
                    b.detail_object_scale,b.force_occluder,b.environment_mapping,
                    b.environment_mapping_strength,b.wave_mode,b.wave_speed,
                    b.wave_max_amplitude,b.wave_grid_size,b.ignore_sun,b.alpha_func,b.default_mapping);
    }

  bool sameFeature(const asset::VertexFeature& a, const asset::VertexFeature& b) {
    return a.texture==b.texture && a.light==b.light && a.normal==b.normal;
    }

  // Extraction partitions only triangles; the source arrays remain unchanged.
  bool sameAuthoredData(const asset::WorldMesh& a, const asset::WorldMesh& b) {
    if(a.name!=b.name || a.bbox.min!=b.bbox.min || a.bbox.max!=b.bbox.max ||
       a.vertices!=b.vertices || a.materials.size()!=b.materials.size() || a.features.size()!=b.features.size())
      return false;
    for(size_t at=0;at<a.materials.size();++at)
      if(!sameMaterial(a.materials[at],b.materials[at])) return false;
    for(size_t at=0;at<a.features.size();++at)
      if(!sameFeature(a.features[at],b.features[at])) return false;
    return true;
    }

  bool samePolygonList(const asset::PolygonList& a, const asset::PolygonList& b) {
    if(a.material_indices!=b.material_indices || a.lightmap_indices!=b.lightmap_indices ||
       a.vertex_indices!=b.vertex_indices || a.feature_indices!=b.feature_indices || a.flags.size()!=b.flags.size())
      return false;
    for(size_t at=0;at<a.flags.size();++at)
      if(!sameFlags(a.flags[at],b.flags[at]))
        return false;
    return true;
    }

  bool sameFace(const asset::WorldMesh& a, size_t af, const asset::WorldMesh& b, size_t bf) {
    if(!sameMaterial(a.materials[a.polygons.material_indices[af]],b.materials[b.polygons.material_indices[bf]]) ||
       a.polygons.lightmap_indices[af]!=b.polygons.lightmap_indices[bf] ||
       !sameFlags(a.polygons.flags[af],b.polygons.flags[bf]))
      return false;
    for(size_t corner=0;corner<3;++corner) {
      const auto ai = af*3+corner, bi = bf*3+corner;
      if(a.vertices[a.polygons.vertex_indices[ai]]!=b.vertices[b.polygons.vertex_indices[bi]] ||
         !sameFeature(a.features[a.polygons.feature_indices[ai]],b.features[b.polygons.feature_indices[bi]]))
        return false;
      }
    return true;
    }

  void preservedFaces(const asset::WorldMesh& source, const std::vector<const asset::WorldMesh*>& parts) {
    std::vector<bool> seen(source.polygons.material_indices.size(),false);
    size_t outputFaces = 0;
    for(const auto* mesh:parts) {
      require(mesh->polygons.vertex_indices.size()==mesh->polygons.material_indices.size()*3 &&
              mesh->polygons.feature_indices.size()==mesh->polygons.material_indices.size()*3 &&
              mesh->polygons.flags.size()==mesh->polygons.material_indices.size() &&
              mesh->polygons.lightmap_indices.size()==mesh->polygons.material_indices.size(),
              "each extracted mesh has complete triangulated polygon data");
      for(size_t face=0;face<mesh->polygons.material_indices.size();++face) {
        bool found = false;
        for(size_t original=0;original<seen.size();++original)
          if(!seen[original] && sameFace(source,original,*mesh,face)) {
            seen[original] = found = true;
            break;
            }
        require(found,"every output face preserves exact source positions, UV, features, material and polygon flags");
        ++outputFaces;
        }
      }
    require(outputFaces==seen.size() && std::all_of(seen.begin(),seen.end(),[](bool v) { return v; }),
            "base and cloth together preserve every source face exactly once");
    }

  uint32_t addMaterial(asset::WorldMesh& mesh, const std::string& name, const std::string& texture) {
    asset::Material material;
    material.name = name;
    material.texture = texture;
    material.group = asset::MaterialGroup::WOOD;
    material.color = {12,34,56,178};
    material.smooth_angle = 47.25f;
    material.texture_scale = {128.f,256.f};
    material.texture_anim_fps = 7.5f;
    material.texture_anim_map_mode = asset::AnimationMapping::LINEAR;
    material.texture_anim_map_dir = {.25f,-.125f};
    material.disable_collision = false;
    material.disable_lightmap = true;
    material.dont_collapse = true;
    material.detail_object = "SYNTHETIC_DETAIL";
    material.detail_object_scale = .75f;
    material.force_occluder = true;
    material.environment_mapping = true;
    material.environment_mapping_strength = .125f;
    material.wave_mode = asset::WaveMode::WALL_AMBIENT;
    material.wave_speed = asset::WaveSpeed::SLOW;
    material.wave_max_amplitude = 11.5f;
    material.wave_grid_size = 91.f;
    material.ignore_sun = true;
    material.alpha_func = asset::AlphaFunction::BLEND;
    material.default_mapping = {.5f,.75f};
    const auto id = uint32_t(mesh.materials.size());
    mesh.materials.push_back(material);
    return id;
    }

  void addTriangle(asset::WorldMesh& mesh, uint32_t material, const std::array<uint32_t,3>& indices) {
    const auto face = uint32_t(mesh.polygons.material_indices.size());
    asset::PolygonFlagSet flags = {};
    flags.is_occluder = uint8_t(face%2);
    flags.is_sector = uint8_t((face/2)%2);
    flags.should_relight = uint8_t((face/3)%2);
    flags.is_dynamically_lit = uint8_t((face/5)%2);
    flags.sector_index = int16_t(100+face);
    flags.is_lod = uint8_t((face/7)%2);
    flags.normal_axis = uint8_t(face%3);
    const int32_t lightmap = face%2 ? -1 : 0;
    mesh.polygons.material_indices.push_back(material);
    mesh.polygons.lightmap_indices.push_back(lightmap);
    mesh.polygons.flags.push_back(flags);
    for(size_t corner=0;corner<3;++corner) {
      const auto feature = uint32_t(mesh.features.size());
      mesh.features.push_back({{float(face)*.03125f+float(corner)*.125f,
                                float(face)*.015625f-float(corner)*.25f},
                               0x10203040u+face*3+uint32_t(corner),
                               {.25f,float(corner)*.125f,1.f}});
      mesh.polygons.vertex_indices.push_back(indices[corner]);
      mesh.polygons.feature_indices.push_back(feature);
      }
    }

  uint32_t addBanner(asset::WorldMesh& mesh, uint32_t material, float x) {
    const auto first = uint32_t(mesh.polygons.material_indices.size());
    const auto base = uint32_t(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(),{{x,600,0},{x+200,600,0},{x+200,300,0},{x,300,0}});
    for(const auto& tri:std::array<std::array<uint32_t,3>,4>{{{0,1,2},{0,2,3},{0,2,1},{0,3,2}}})
      addTriangle(mesh,material,{base+tri[0],base+tri[1],base+tri[2]});
    return first;
    }

  void addFinial(asset::WorldMesh& mesh, uint32_t material) {
    const auto base = uint32_t(mesh.vertices.size());
    for(size_t ring=0;ring<2;++ring)
      for(size_t side=0;side<8;++side) {
        const float angle = float(side)*3.14159265358979323846f*.25f;
        mesh.vertices.push_back({100+18.f*std::cos(angle),650.f+float(ring)*90.f,18.f*std::sin(angle)});
        }
    for(uint32_t side=0;side<8;++side) {
      const auto next = (side+1)%8;
      addTriangle(mesh,material,{base+side,base+next,base+8+side});
      addTriangle(mesh,material,{base+next,base+8+next,base+8+side});
      }
    for(uint32_t corner=1;corner<7;++corner) {
      addTriangle(mesh,material,{base,base+corner+1,base+corner});
      addTriangle(mesh,material,{base+8,base+8+corner,base+8+corner+1});
      }
    }

  uint32_t addGrid(asset::WorldMesh& mesh, uint32_t material, bool sail) {
    const uint32_t side = sail ? 5 : 3;
    const auto first = uint32_t(mesh.polygons.material_indices.size());
    const auto base = uint32_t(mesh.vertices.size());
    for(uint32_t row=0;row<side;++row)
      for(uint32_t col=0;col<side;++col) {
        if(sail)
          mesh.vertices.push_back({3000.f+float(col)*200.f,600.f+float(row)*30.f,
                                   20.f*std::sin(float(col)*3.14159265358979323846f*.25f)});
        else
          mesh.vertices.push_back({1200.f,200.f+float(row)*100.f,float(col)*150.f});
        }
    for(uint32_t row=0;row+1<side;++row)
      for(uint32_t col=0;col+1<side;++col) {
        const auto a = base+row*side+col, b = a+1, c = a+side, d = c+1;
        addTriangle(mesh,material,{a,b,c});
        addTriangle(mesh,material,{b,d,c});
        addTriangle(mesh,material,{a,c,b});
        addTriangle(mesh,material,{b,c,d});
        }
    return first;
    }

  void requirePins(const WorldCloth::Surface& surface, const std::string& type) {
    require(!surface.pins.empty() && surface.pins.size()<surface.mesh.vertices.size(),
            "each geometric attachment has both pinned and free source positions");
    std::set<uint32_t> pins(surface.pins.begin(),surface.pins.end());
    require(pins.size()==surface.pins.size(),"geometric attachment indices are unique");
    for(const auto pin:pins)
      require(pin<surface.mesh.vertices.size(),"all geometric attachments address their own surface");
    for(uint32_t at=0;at<surface.mesh.vertices.size();++at) {
      const auto& p = surface.mesh.vertices[at];
      const bool attached = type=="banner" ? p.y==600.f :
                            type=="flag" ? p.z==0.f :
                            p.y==720.f || p.x==3000.f || p.x==3800.f;
      require((pins.count(at)!=0)==attached,"pins follow the authored top edge, mast edge or sail ridge and tied ends");
      }
    }
  }

int main() {
  asset::WorldMesh source;
  source.name = "SYNTHETIC_WORLD_CLOTH";
  source.bbox = {{-100,-200,-300},{4000,1000,500}};
  const auto other = addMaterial(source,"UNSUPPORTED_CLOTH","UNKNOWN_FLAG.TGA");
  const auto sail = addMaterial(source,"MATERIAL_WITH_TEXTURE_RULE","nw_ship_sail_02.tga");
  const auto flag = addMaterial(source,"nw_ship_flag_01","SYNTHETIC_FLAG.TGA");
  const auto city = addMaterial(source,"NW_CITY_FLAG_01","SYNTHETIC_CITY.TGA");
  source.materials[sail].wave_mode = asset::WaveMode::NONE;
  addBanner(source,other,-800);
  const auto sailFirst = addGrid(source,sail,true);
  const auto flagFirst = addGrid(source,flag,false);
  const auto finialFirst = uint32_t(source.polygons.material_indices.size());
  addFinial(source,city);
  const auto cityFirst = addBanner(source,city,0);
  const auto citySecond = addBanner(source,city,500);
  const auto original = source;
  auto extracted = WorldCloth::extract(source);
  require(extracted.surfaces.size()==4,"two city sheets, a ship flag and a sail are extracted");
  require(sameAuthoredData(source,original),"extraction leaves landscape arrays and source indices unchanged");
  preservedFaces(original,{&source,&extracted.original});
  std::vector<const asset::WorldMesh*> surfaces;
  std::set<uint32_t> ids;
  size_t bannerCount = 0, flagCount = 0, sailCount = 0;
  for(const auto& surface:extracted.surfaces) {
    surfaces.push_back(&surface.mesh);
    require(ids.insert(surface.id).second,"synthetic surface IDs are unique");
    if(surface.visual.starts_with("NW_CITY_FLAG_01:")) {
      require(surface.mesh.vertices.size()==4 && surface.mesh.polygons.material_indices.size()==4,
              "the city banner extracts only its four-position double-sided cloth quad");
      requirePins(surface,"banner");
      require(std::abs(surface.stiffness-.86f)<1.e-6f,"city stiffness comes from the authored cloth table");
      ++bannerCount;
      }
    else if(surface.visual.starts_with("NW_SHIP_FLAG_01:")) {
      requirePins(surface,"flag");
      require(surface.mesh.materials.front().wave_mode==asset::WaveMode::WALL_AMBIENT,
              "original ship material wave_mode=3 survives extraction for native OFF fallback");
      ++flagCount;
      }
    else if(surface.visual.starts_with("NW_SHIP_SAIL_02:")) {
      requirePins(surface,"sail");
      ++sailCount;
      }
    else
      require(false,"only table-supported world cloth is extracted");
    }
  require(bannerCount==2 && flagCount==1 && sailCount==1,"supported material names and texture stems select the intended islands");
  require(ids==std::set<uint32_t>{0x80000000u|cityFirst,0x80000000u|citySecond,
                                 0x80000000u|flagFirst,0x80000000u|sailFirst},
          "synthetic IDs derive deterministically from the first authored triangle");
  preservedFaces(extracted.original,surfaces);
  require(source.polygons.material_indices.size()==32,"unsupported quad and the 28-triangle rigid finial remain in the landscape");
  for(size_t face=finialFirst;face<cityFirst;++face) {
    bool retained = false;
    for(size_t at=0;at<source.polygons.material_indices.size();++at)
      retained |= sameFace(original,face,source,at);
    require(retained,"every rigid finial triangle stays static despite sharing the city cloth material");
    }

  auto repeated = original;
  const auto again = WorldCloth::extract(repeated);
  require(samePolygonList(source.polygons,repeated.polygons) && again.surfaces.size()==extracted.surfaces.size() &&
          samePolygonList(extracted.original.polygons,again.original.polygons),"repeated extraction partitions polygons deterministically");
  for(size_t at=0;at<again.surfaces.size();++at) {
    const auto& a = again.surfaces[at];
    const auto& b = extracted.surfaces[at];
    require(a.id==b.id && a.visual==b.visual && a.pins==b.pins && a.stiffness==b.stiffness &&
            sameAuthoredData(a.mesh,b.mesh) && samePolygonList(a.mesh.polygons,b.mesh.polygons),
            "surface order, synthetic identity, compact data and attachments are deterministic");
    }

  asset::WorldMesh unsupported;
  addMaterial(unsupported,"UNSUPPORTED_CLOTH","UNKNOWN_FLAG.TGA");
  addBanner(unsupported,0,-800);
  const auto unsupportedOriginal = unsupported;
  const auto untouched = WorldCloth::extract(unsupported);
  require(untouched.surfaces.empty() && untouched.original.polygons.material_indices.empty() &&
          sameAuthoredData(unsupported,unsupportedOriginal) && samePolygonList(unsupported.polygons,unsupportedOriginal.polygons),
          "unsupported meshes remain completely unchanged");

  std::printf("world cloth contract passed: source_faces=%zu static_faces=%zu cloth_faces=%zu surfaces=%zu banners=%zu flags=%zu sails=%zu finial_faces=28 ids=deterministic\n",
              original.polygons.material_indices.size(),source.polygons.material_indices.size(),
              extracted.original.polygons.material_indices.size(),extracted.surfaces.size(),bannerCount,flagCount,sailCount);
  return EXIT_SUCCESS;
  }

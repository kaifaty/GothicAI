#include "fromzenkit.h"

#include <zenkit/Font.hh>
#include <zenkit/Mesh.hh>
#include <zenkit/world/BspTree.hh>
#include <zenkit/ModelMesh.hh>
#include <algorithm>
#include <set>

namespace asset {

static_assert(int(AlphaFunction::DEFAULT)==int(zenkit::AlphaFunction::DEFAULT));
static_assert(int(AlphaFunction::NONE)==int(zenkit::AlphaFunction::NONE));
static_assert(int(AlphaFunction::BLEND)==int(zenkit::AlphaFunction::BLEND));
static_assert(int(AlphaFunction::ADD)==int(zenkit::AlphaFunction::ADD));
static_assert(int(AlphaFunction::SUBTRACT)==int(zenkit::AlphaFunction::SUBTRACT));
static_assert(int(AlphaFunction::MULTIPLY)==int(zenkit::AlphaFunction::MULTIPLY));
static_assert(int(AlphaFunction::MULTIPLY_ALT)==int(zenkit::AlphaFunction::MULTIPLY_ALT));
static_assert(int(MaterialGroup::UNDEFINED)==int(zenkit::MaterialGroup::UNDEFINED));
static_assert(int(MaterialGroup::METAL)==int(zenkit::MaterialGroup::METAL));
static_assert(int(MaterialGroup::STONE)==int(zenkit::MaterialGroup::STONE));
static_assert(int(MaterialGroup::WOOD)==int(zenkit::MaterialGroup::WOOD));
static_assert(int(MaterialGroup::EARTH)==int(zenkit::MaterialGroup::EARTH));
static_assert(int(MaterialGroup::WATER)==int(zenkit::MaterialGroup::WATER));
static_assert(int(MaterialGroup::SNOW)==int(zenkit::MaterialGroup::SNOW));
static_assert(int(MaterialGroup::NONE)==int(zenkit::MaterialGroup::NONE));
static_assert(int(WaveMode::NONE)==int(zenkit::WaveMode::NONE));
static_assert(int(WaveMode::GROUND_AMBIENT)==int(zenkit::WaveMode::GROUND_AMBIENT));
static_assert(int(WaveMode::GROUND)==int(zenkit::WaveMode::GROUND));
static_assert(int(WaveMode::WALL_AMBIENT)==int(zenkit::WaveMode::WALL_AMBIENT));
static_assert(int(WaveMode::WALL)==int(zenkit::WaveMode::WALL));
static_assert(int(WaveMode::ENVIRONMENT)==int(zenkit::WaveMode::ENVIRONMENT));
static_assert(int(WaveMode::WIND_AMBIENT)==int(zenkit::WaveMode::WIND_AMBIENT));
static_assert(int(WaveMode::WIND)==int(zenkit::WaveMode::WIND));
static_assert(int(WaveSpeed::NONE)==int(zenkit::WaveSpeed::NONE));
static_assert(int(WaveSpeed::SLOW)==int(zenkit::WaveSpeed::SLOW));
static_assert(int(WaveSpeed::NORMAL)==int(zenkit::WaveSpeed::NORMAL));
static_assert(int(WaveSpeed::FAST)==int(zenkit::WaveSpeed::FAST));
static_assert(int(AnimationMapping::NONE)==int(zenkit::AnimationMapping::NONE));
static_assert(int(AnimationMapping::LINEAR)==int(zenkit::AnimationMapping::LINEAR));

Material fromZenKit(const zenkit::Material& s) {
  Material m;
  m.name = s.name;
  m.group = static_cast<MaterialGroup>(s.group);
  m.color = s.color;
  m.smooth_angle = s.smooth_angle;
  m.texture = s.texture;
  m.texture_scale = s.texture_scale;
  m.texture_anim_fps = s.texture_anim_fps;
  m.texture_anim_map_mode = static_cast<AnimationMapping>(s.texture_anim_map_mode);
  m.texture_anim_map_dir = s.texture_anim_map_dir;
  m.disable_collision = s.disable_collision;
  m.disable_lightmap = s.disable_lightmap;
  m.dont_collapse = s.dont_collapse;
  m.detail_object = s.detail_object;
  m.detail_object_scale = s.detail_object_scale;
  m.force_occluder = s.force_occluder;
  m.environment_mapping = s.environment_mapping;
  m.environment_mapping_strength = s.environment_mapping_strength;
  m.wave_mode = static_cast<WaveMode>(s.wave_mode);
  m.wave_speed = static_cast<WaveSpeed>(s.wave_speed);
  m.wave_max_amplitude = s.wave_max_amplitude;
  m.wave_grid_size = s.wave_grid_size;
  m.ignore_sun = s.ignore_sun;
  m.alpha_func = static_cast<AlphaFunction>(s.alpha_func);
  m.default_mapping = s.default_mapping;
  return m;
  }

WorldMesh fromZenKit(const zenkit::Mesh& s) {
  WorldMesh m;
  m.name=s.name; m.bbox=s.bbox; m.vertices=s.vertices;
  m.materials.reserve(s.materials.size());
  for(const auto& material:s.materials) m.materials.push_back(fromZenKit(material));
  m.features.reserve(s.features.size());
  for(const auto& f:s.features) m.features.push_back({f.texture,f.light,f.normal});
  m.polygons.material_indices=s.polygons.material_indices;
  m.polygons.lightmap_indices=s.polygons.lightmap_indices;
  m.polygons.feature_indices=s.polygons.feature_indices;
  m.polygons.vertex_indices=s.polygons.vertex_indices;
  m.polygons.flags.reserve(s.polygons.flags.size());
  for(const auto& f:s.polygons.flags)
    m.polygons.flags.push_back({f.is_portal,f.is_occluder,f.is_sector,f.should_relight,
                               f.is_outdoor,f.is_ghost_occluder,f.is_dynamically_lit,
                               f.sector_index,f.is_lod,f.normal_axis});
  return m;
  }

StaticMesh fromZenKit(const zenkit::MultiResolutionMesh& s) {
  StaticMesh m;
  m.positions=s.positions; m.normals=s.normals; m.alpha_test=s.alpha_test;
  m.bbox=s.bbox; m.obbox=s.obbox;
  m.materials.reserve(s.materials.size());
  for(const auto& material:s.materials) m.materials.push_back(fromZenKit(material));
  m.sub_meshes.resize(s.sub_meshes.size());
  for(size_t i=0;i<s.sub_meshes.size();++i) {
    const auto& src=s.sub_meshes[i];
    auto& sub=m.sub_meshes[i];
    sub.mat=fromZenKit(src.mat);
    sub.triangles.reserve(src.triangles.size());
    for(const auto& t:src.triangles) sub.triangles.push_back({{t.wedges[0],t.wedges[1],t.wedges[2]}});
    sub.wedges.reserve(src.wedges.size());
    for(const auto& w:src.wedges) sub.wedges.push_back({w.normal,w.texture,w.index});
    }
  return m;
  }

SoftSkinMesh fromZenKit(const zenkit::SoftSkinMesh& s) {
  SoftSkinMesh m;
  m.mesh=fromZenKit(s.mesh); m.bboxes=s.bboxes;
  m.weights.resize(s.weights.size());
  for(size_t i=0;i<s.weights.size();++i) {
    m.weights[i].reserve(s.weights[i].size());
    for(const auto& w:s.weights[i]) m.weights[i].push_back({w.weight,w.position,w.node_index});
    }
  return m;
  }

ModelMesh fromZenKit(const zenkit::ModelMesh& s) {
  ModelMesh m;
  m.meshes.reserve(s.meshes.size());
  for(const auto& skin:s.meshes) m.meshes.push_back(fromZenKit(skin));
  m.attachments.reserve(s.attachments.size());
  for(const auto& [name,mesh]:s.attachments) m.attachments.emplace_back(name,fromZenKit(mesh));
  return m;
  }

std::vector<WorldData::Room> roomVolumesFromZenKit(const zenkit::Mesh& mesh, const zenkit::BspTree& bsp) {
  std::vector<WorldData::Room> rooms;
      for(const auto& sector:bsp.sectors) {
        std::set<uint32_t> polygons;
        for(const auto leaf:sector.node_indices) {
          if(leaf>=bsp.leaf_node_indices.size()) continue;
          const auto node=bsp.leaf_node_indices[leaf];
          if(node>=bsp.nodes.size()) continue;
          const auto& bounds=bsp.nodes[node];
          const size_t end=std::min(size_t(bounds.polygon_index)+bounds.polygon_count,bsp.polygon_indices.size());
          for(size_t i=bounds.polygon_index;i<end;++i) {
            const auto polygon=bsp.polygon_indices[i];
            if(polygon<mesh.geometry.size() && mesh.geometry[polygon].flags.is_sector)
              polygons.insert(polygon);
            }
          }
        // Portal polygons are invisible openings in the renderer, but close
        // the indoor volume at doors and windows for this containment test.
        polygons.insert(sector.portal_polygon_indices.begin(),sector.portal_polygon_indices.end());
        WorldData::Room room;
        for(const auto index:polygons) {
          if(index>=mesh.geometry.size()) continue;
          const auto& polygon=mesh.geometry[index];
          if(polygon.index_count<3 || polygon.index_offset+polygon.index_count>mesh.polygon_vertex_indices.size()) continue;
          const auto source=[&](size_t corner) { return mesh.polygon_vertex_indices[polygon.index_offset+corner]; };
          for(size_t corner=2;corner<polygon.index_count;++corner) {
            const auto a=source(0),b=source(corner-1),c=source(corner);
            if(a>=mesh.vertices.size() || b>=mesh.vertices.size() || c>=mesh.vertices.size()) continue;
            WorldData::Triangle triangle;
            const uint32_t indices[3]={a,b,c};
            for(size_t i=0;i<3;++i) {
              const auto point=mesh.vertices[indices[i]];
              triangle.point[i]={point.x,point.y,point.z};
              if(room.triangles.empty() && i==0) room.min=room.max=triangle.point[i];
              room.min.x=std::min(room.min.x,point.x); room.max.x=std::max(room.max.x,point.x);
              room.min.y=std::min(room.min.y,point.y); room.max.y=std::max(room.max.y,point.y);
              room.min.z=std::min(room.min.z,point.z); room.max.z=std::max(room.max.z,point.z);
              }
            room.triangles.push_back(triangle);
            }
          }
        rooms.push_back(std::move(room));
        }
  return rooms;
  }

Font fromZenKit(const zenkit::Font& s) {
  Font f;
  f.name = s.name;
  f.height = s.height;
  f.glyphs.resize(s.glyphs.size());
  for(size_t i=0; i<s.glyphs.size(); ++i) {
    f.glyphs[i].width = s.glyphs[i].width;
    f.glyphs[i].uv[0] = s.glyphs[i].uv[0];
    f.glyphs[i].uv[1] = s.glyphs[i].uv[1];
    }
  return f;
  }

}

#include "fromzenkit.h"

#include <zenkit/Font.hh>
#include <zenkit/Mesh.hh>
#include <zenkit/world/BspTree.hh>
#include <zenkit/ModelMesh.hh>
#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <set>

namespace asset {

static_assert(std::is_standard_layout_v<Vec2> && std::is_standard_layout_v<zenkit::Vec2>);
static_assert(std::is_trivially_copyable_v<Vec2>);
static_assert(sizeof(Vec2)==sizeof(zenkit::Vec2));
static_assert(alignof(Vec2)==alignof(zenkit::Vec2));
static_assert(offsetof(Vec2,x)==offsetof(zenkit::Vec2,x));
static_assert(offsetof(Vec2,y)==offsetof(zenkit::Vec2,y));

static_assert(std::is_standard_layout_v<Vec3> && std::is_standard_layout_v<zenkit::Vec3>);
static_assert(std::is_trivially_copyable_v<Vec3>);
static_assert(sizeof(Vec3)==sizeof(zenkit::Vec3));
static_assert(alignof(Vec3)==alignof(zenkit::Vec3));
static_assert(offsetof(Vec3,x)==offsetof(zenkit::Vec3,x));
static_assert(offsetof(Vec3,y)==offsetof(zenkit::Vec3,y));
static_assert(offsetof(Vec3,z)==offsetof(zenkit::Vec3,z));

static_assert(std::is_standard_layout_v<Vec4> && std::is_standard_layout_v<zenkit::Vec4>);
static_assert(std::is_trivially_copyable_v<Vec4>);
static_assert(sizeof(Vec4)==sizeof(zenkit::Vec4));
static_assert(alignof(Vec4)==alignof(zenkit::Vec4));
static_assert(offsetof(Vec4,x)==offsetof(zenkit::Vec4,x));
static_assert(offsetof(Vec4,y)==offsetof(zenkit::Vec4,y));
static_assert(offsetof(Vec4,z)==offsetof(zenkit::Vec4,z));
static_assert(offsetof(Vec4,w)==offsetof(zenkit::Vec4,w));

static_assert(std::is_standard_layout_v<Quat> && std::is_standard_layout_v<zenkit::Quat>);
static_assert(std::is_trivially_copyable_v<Quat>);
static_assert(sizeof(Quat)==sizeof(zenkit::Quat));
static_assert(alignof(Quat)==alignof(zenkit::Quat));
static_assert(offsetof(Quat,w)==offsetof(zenkit::Quat,w));
static_assert(offsetof(Quat,x)==offsetof(zenkit::Quat,x));
static_assert(offsetof(Quat,y)==offsetof(zenkit::Quat,y));
static_assert(offsetof(Quat,z)==offsetof(zenkit::Quat,z));

static_assert(std::is_standard_layout_v<Mat3> && std::is_standard_layout_v<zenkit::Mat3>);
static_assert(std::is_trivially_copyable_v<Mat3>);
static_assert(sizeof(Mat3)==sizeof(zenkit::Mat3));
static_assert(alignof(Mat3)==alignof(zenkit::Mat3));
static_assert(offsetof(Mat3,columns)==offsetof(zenkit::Mat3,columns));

static_assert(std::is_standard_layout_v<Mat4> && std::is_standard_layout_v<zenkit::Mat4>);
static_assert(std::is_trivially_copyable_v<Mat4>);
static_assert(sizeof(Mat4)==sizeof(zenkit::Mat4));
static_assert(alignof(Mat4)==alignof(zenkit::Mat4));
static_assert(offsetof(Mat4,columns)==offsetof(zenkit::Mat4,columns));

static_assert(std::is_standard_layout_v<Color> && std::is_standard_layout_v<zenkit::Color>);
static_assert(std::is_trivially_copyable_v<Color>);
static_assert(sizeof(Color)==sizeof(zenkit::Color));
static_assert(alignof(Color)==alignof(zenkit::Color));
static_assert(offsetof(Color,r)==offsetof(zenkit::Color,r));
static_assert(offsetof(Color,g)==offsetof(zenkit::Color,g));
static_assert(offsetof(Color,b)==offsetof(zenkit::Color,b));
static_assert(offsetof(Color,a)==offsetof(zenkit::Color,a));

static_assert(std::is_standard_layout_v<Aabb> && std::is_standard_layout_v<zenkit::AxisAlignedBoundingBox>);
static_assert(std::is_trivially_copyable_v<Aabb>);
static_assert(sizeof(Aabb)==sizeof(zenkit::AxisAlignedBoundingBox));
static_assert(alignof(Aabb)==alignof(zenkit::AxisAlignedBoundingBox));
static_assert(offsetof(Aabb,min)==offsetof(zenkit::AxisAlignedBoundingBox,min));
static_assert(offsetof(Aabb,max)==offsetof(zenkit::AxisAlignedBoundingBox,max));

static_assert(std::is_standard_layout_v<Obb> && std::is_standard_layout_v<zenkit::OrientedBoundingBox>);
static_assert(sizeof(Obb)==sizeof(zenkit::OrientedBoundingBox));
static_assert(alignof(Obb)==alignof(zenkit::OrientedBoundingBox));
static_assert(offsetof(Obb,center)==offsetof(zenkit::OrientedBoundingBox,center));
static_assert(offsetof(Obb,axes)==offsetof(zenkit::OrientedBoundingBox,axes));
static_assert(offsetof(Obb,half_width)==offsetof(zenkit::OrientedBoundingBox,half_width));
static_assert(offsetof(Obb,children)==offsetof(zenkit::OrientedBoundingBox,children));

Vec2 fromZenKit(const zenkit::Vec2& s) {
  return {s.x,s.y};
  }

Vec3 fromZenKit(const zenkit::Vec3& s) {
  return {s.x,s.y,s.z};
  }

Vec4 fromZenKit(const zenkit::Vec4& s) {
  return {s.x,s.y,s.z,s.w};
  }

Quat fromZenKit(const zenkit::Quat& s) {
  return {s.w,s.x,s.y,s.z};
  }

Mat3 fromZenKit(const zenkit::Mat3& s) {
  Mat3 m;
  for(unsigned i=0;i<3;++i) m.columns[i]=fromZenKit(s.columns[i]);
  return m;
  }

Mat4 fromZenKit(const zenkit::Mat4& s) {
  Mat4 m;
  for(unsigned i=0;i<4;++i) m.columns[i]=fromZenKit(s.columns[i]);
  return m;
  }

Color fromZenKit(const zenkit::Color& s) {
  return {s.r,s.g,s.b,s.a};
  }

Aabb fromZenKit(const zenkit::AxisAlignedBoundingBox& s) {
  return {fromZenKit(s.min),fromZenKit(s.max)};
  }

Obb fromZenKit(const zenkit::OrientedBoundingBox& s) {
  Obb b;
  b.center=fromZenKit(s.center);
  for(unsigned i=0;i<3;++i) b.axes[i]=fromZenKit(s.axes[i]);
  b.half_width=fromZenKit(s.half_width);
  b.children.reserve(s.children.size());
  for(const auto& child:s.children) b.children.push_back(fromZenKit(child));
  return b;
  }

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
  m.color = fromZenKit(s.color);
  m.smooth_angle = s.smooth_angle;
  m.texture = s.texture;
  m.texture_scale = fromZenKit(s.texture_scale);
  m.texture_anim_fps = s.texture_anim_fps;
  m.texture_anim_map_mode = static_cast<AnimationMapping>(s.texture_anim_map_mode);
  m.texture_anim_map_dir = fromZenKit(s.texture_anim_map_dir);
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
  m.default_mapping = fromZenKit(s.default_mapping);
  return m;
  }

WorldMesh fromZenKit(const zenkit::Mesh& s) {
  WorldMesh m;
  m.name=s.name; m.bbox=fromZenKit(s.bbox);
  m.vertices.reserve(s.vertices.size());
  for(const auto& v:s.vertices) m.vertices.push_back(fromZenKit(v));
  m.materials.reserve(s.materials.size());
  for(const auto& material:s.materials) m.materials.push_back(fromZenKit(material));
  m.features.reserve(s.features.size());
  for(const auto& f:s.features) m.features.push_back({fromZenKit(f.texture),f.light,fromZenKit(f.normal)});
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
  m.positions.reserve(s.positions.size());
  for(const auto& v:s.positions) m.positions.push_back(fromZenKit(v));
  m.normals.reserve(s.normals.size());
  for(const auto& v:s.normals) m.normals.push_back(fromZenKit(v));
  m.alpha_test=s.alpha_test;
  m.bbox=fromZenKit(s.bbox); m.obbox=fromZenKit(s.obbox);
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
    for(const auto& w:src.wedges) sub.wedges.push_back({fromZenKit(w.normal),fromZenKit(w.texture),w.index});
    }
  return m;
  }

SoftSkinMesh fromZenKit(const zenkit::SoftSkinMesh& s) {
  SoftSkinMesh m;
  m.mesh=fromZenKit(s.mesh);
  m.bboxes.reserve(s.bboxes.size());
  for(const auto& b:s.bboxes) m.bboxes.push_back(fromZenKit(b));
  m.weights.resize(s.weights.size());
  for(size_t i=0;i<s.weights.size();++i) {
    m.weights[i].reserve(s.weights[i].size());
    for(const auto& w:s.weights[i]) m.weights[i].push_back({w.weight,fromZenKit(w.position),w.node_index});
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
    f.glyphs[i].uv[0] = fromZenKit(s.glyphs[i].uv[0]);
    f.glyphs[i].uv[1] = fromZenKit(s.glyphs[i].uv[1]);
    }
  return f;
  }

}

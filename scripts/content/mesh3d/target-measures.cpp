// Original mesh data stays in memory; output contains names and scalar measurements only.
#include <zenkit/Model.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/VirtualObject.hh>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using Vec=std::array<double,3>;
using Mat=std::array<std::array<double,4>,4>; // [column][row], matching ZenKit
std::string upper(std::string s) {
  for(auto& c:s) c=char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
Mat identity() { Mat m{}; for(int i=0;i<4;++i) m[i][i]=1; return m; }
Mat matrix(const zenkit::Mat4& m) {
  Mat out{}; for(int c=0;c<4;++c) for(int r=0;r<4;++r) out[c][r]=m[c][r]; return out;
}
Mat multiply(const Mat& a,const Mat& b) {
  Mat out{};
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) for(int k=0;k<4;++k) out[c][r]+=a[k][r]*b[c][k];
  return out;
}
Vec point(const Mat& m,const zenkit::Vec3& p) {
  Vec out{};
  for(int r=0;r<3;++r) out[r]=m[0][r]*p.x+m[1][r]*p.y+m[2][r]*p.z+m[3][r];
  return out;
}
zenkit::Mat4 sampleTransform(const zenkit::AnimationSample& sample) {
  const double x=sample.rotation.x,y=sample.rotation.y,z=sample.rotation.z,w=sample.rotation.w;
  // Same conjugate quaternion convention as animmath.cpp::mkMatrix.
  auto m=zenkit::Mat4::identity();
  m[0][0]=w*w+x*x-y*y-z*z; m[0][1]=2*(x*y-w*z); m[0][2]=2*(x*z+w*y);
  m[1][0]=2*(x*y+w*z); m[1][1]=w*w-x*x+y*y-z*z; m[1][2]=2*(y*z-w*x);
  m[2][0]=2*(x*z-w*y); m[2][1]=2*(y*z+w*x); m[2][2]=w*w-x*x-y*y+z*z;
  m[3][0]=sample.position.x; m[3][1]=sample.position.y; m[3][2]=sample.position.z;
  return m;
}
struct Measure {
  Vec min{INFINITY,INFINITY,INFINITY}, max{-INFINITY,-INFINITY,-INFINITY};
  size_t tris=0, points=0;
  void add(const Vec& p) {
    for(int i=0;i<3;++i) {
      if(!std::isfinite(p[i])) throw std::runtime_error("non-finite mesh position");
      min[i]=std::min(min[i],p[i]); max[i]=std::max(max[i],p[i]);
    }
    ++points;
  }
  template<class Mesh> void mesh(const Mesh& mesh,const Mat& transform) {
    // Count every highest-detail triangle, independently of UV wedges and materials.
    // Only referenced vertices define the visual bbox; helper/unreferenced vertices do not.
    std::set<size_t> used;
    for(const auto& sub:mesh.sub_meshes) for(const auto& tri:sub.triangles) {
      ++tris;
      for(auto index:tri.wedges) {
        if(index>=sub.wedges.size()) throw std::runtime_error("triangle wedge index out of range");
        used.insert(sub.wedges[index].index);
      }
    }
    for(auto index:used) {
      if(index>=mesh.positions.size()) throw std::runtime_error("wedge vertex index out of range");
      add(point(transform,mesh.positions[index]));
    }
  }
};
std::vector<Mat> transforms(const zenkit::ModelHierarchy& h) {
  std::vector<Mat> out(h.nodes.size());
  std::vector<int> state(h.nodes.size());
  std::function<void(size_t)> visit=[&](size_t i) {
    if(state[i]==2) return;
    if(state[i]==1) throw std::runtime_error("hierarchy cycle");
    state[i]=1;
    const auto& n=h.nodes[i];
    auto local=matrix(n.transform);
    if(n.parent_index==-1) {
      // Same root-local translation as Skeleton::Skeleton / Matrix4x4::translate.
      auto translation=identity();
      translation[3][0]=h.root_translation.x;
      translation[3][1]=h.root_translation.y;
      translation[3][2]=h.root_translation.z;
      out[i]=multiply(local,translation);
    } else {
      if(n.parent_index<0 || size_t(n.parent_index)>=h.nodes.size()) throw std::runtime_error("invalid hierarchy parent");
      visit(size_t(n.parent_index));
      out[i]=multiply(out[n.parent_index],local);
    }
    state[i]=2;
  };
  for(size_t i=0;i<h.nodes.size();++i) visit(i);
  return out;
}
template<class ModelMesh> Measure measureModel(const ModelMesh& mesh,const zenkit::ModelHierarchy& hierarchy) {
  if(!mesh.meshes.empty()) throw std::runtime_error("softskin MOBs are outside this rigid furniture pilot");
  const auto global=transforms(hierarchy);
  Measure out;
  for(const auto& [name,attachment]:mesh.attachments) {
    const auto node=std::find_if(hierarchy.nodes.begin(),hierarchy.nodes.end(),[&](const auto& n) { return n.name==name; });
    if(node==hierarchy.nodes.end()) throw std::runtime_error("attachment has no hierarchy node: "+name);
    out.mesh(attachment,global[size_t(node-hierarchy.nodes.begin())]);
  }
  return out;
}
#ifndef TARGET_MEASURES_SELF_TEST
zenkit::Vfs mount(const std::filesystem::path& game) {
  std::map<std::string,std::filesystem::path> archives;
  for(const auto& entry:std::filesystem::directory_iterator(game/"Data")) {
    const auto name=upper(entry.path().filename().string());
    if(entry.is_regular_file() && name.ends_with(".VDF") &&
       (name.starts_with("MESHES") || name.starts_with("ANIMS") || name.starts_with("WORLDS"))) archives[name]=entry.path();
  }
  if(archives.empty()) throw std::runtime_error("no Meshes/Anims/Worlds VDF archives");
  zenkit::Vfs vfs;
  for(const auto& [name,path]:archives) vfs.mount_disk(path,zenkit::VfsOverwriteBehavior::OLDER);
  return vfs;
}
std::unique_ptr<zenkit::Read> open(const zenkit::Vfs& vfs,const std::string& name) {
  const auto* file=vfs.find(name);
  if(!file) throw std::runtime_error("missing original visual: "+name);
  return file->open_read();
}
std::string stem(std::string n) { const auto dot=n.rfind('.'); return dot==n.npos?n:n.substr(0,dot); }
void counts(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs,std::map<std::string,size_t>& out) {
  for(const auto& v:vobs) {
    if(v->visual) ++out[stem(upper(v->visual->name))];
    counts(v->children,out);
  }
}
#endif
void vec(const Vec& v) { std::cout<<'['<<v[0]<<','<<v[1]<<','<<v[2]<<']'; }
void selfTest() {
  auto require=[](bool ok) { if(!ok) throw std::runtime_error("target-measures self-test failed"); };
  // Synthetic equivalents avoid Material virtual methods, so this test links without ZenKit.
  struct Wedge { size_t index; };
  struct Triangle { size_t wedges[3]; };
  struct Sub { std::vector<Wedge> wedges; std::vector<Triangle> triangles; };
  struct Mesh { std::vector<zenkit::Vec3> positions; std::vector<Sub> sub_meshes; };
  struct ModelMesh { std::vector<int> meshes; std::map<std::string,Mesh> attachments; };
  Mesh mesh;
  mesh.positions={{0,0,0},{10,0,0},{0,20,0},{999,999,999}};
  mesh.sub_meshes.resize(2);
  for(auto& sub:mesh.sub_meshes) { sub.wedges.resize(3); for(int i=0;i<3;++i)sub.wedges[i].index=i; sub.triangles.push_back({{0,1,2}}); }
  zenkit::AnimationSample sample;
  sample.position={1,2,3}; sample.rotation=zenkit::Quat(std::sqrt(0.5f),0,std::sqrt(0.5f),0);
  const auto rotated=point(matrix(sampleTransform(sample)),{10,0,0});
  require(std::abs(rotated[0]-1)<1e-5 && std::abs(rotated[1]-2)<1e-5 && std::abs(rotated[2]-13)<1e-5);
  zenkit::ModelHierarchy h;
  // Child deliberately precedes parent to test recursive hierarchy composition.
  h.nodes.resize(2);
  h.nodes[0].name="LID"; h.nodes[0].parent_index=1; h.nodes[0].transform=zenkit::Mat4::identity();
  h.nodes[0].transform[3][0]=100;
  h.nodes[1].name="BODY"; h.nodes[1].parent_index=-1; h.nodes[1].transform=zenkit::Mat4::identity();
  h.nodes[1].transform[3][1]=30;
  h.root_translation={0,5,0};
  ModelMesh model;
  model.attachments["LID"]=mesh; model.attachments["BODY"]=mesh;
  auto result=measureModel(model,h);
  require(result.tris==4 && result.min==Vec{0,35,0} && result.max==Vec{110,55,0});
  auto rejected=[&](auto fn) { try { fn(); } catch(const std::runtime_error&) { return true; } return false; };
  h.nodes[1].parent_index=0; require(rejected([&]{transforms(h);}));
  h.nodes[1].parent_index=5; require(rejected([&]{transforms(h);}));
  h.nodes[1].parent_index=-1;
  model.attachments["MISSING"]=mesh; require(rejected([&]{measureModel(model,h);}));
  model.attachments.erase("MISSING"); model.meshes.resize(1); require(rejected([&]{measureModel(model,h);}));
  mesh.sub_meshes[0].triangles[0].wedges[0]=99; require(rejected([&]{Measure m;m.mesh(mesh,identity());}));
  std::cout<<"target-measures: 7 synthetic checks passed\n";
}
}
int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") { selfTest(); return 0; }
#ifndef TARGET_MEASURES_SELF_TEST
    if(argc<3) throw std::runtime_error("usage: target-measures <game dir> <VISUAL.MRM|VISUAL.MDS|VISUAL.ASC>...");
    const auto vfs=mount(argv[1]);
    auto worldStream=open(vfs,"NEWWORLD.ZEN");
    zenkit::World world; world.load(worldStream.get(),zenkit::GameVersion::GOTHIC_2);
    std::map<std::string,size_t> frequency; counts(world.world_vobs,frequency);
    std::cout<<std::setprecision(12)<<'[';
    for(int i=2;i<argc;++i) {
      const auto name=upper(argv[i]);
      const auto resolved=name.ends_with(".MDS") || name.ends_with(".ASC") ? stem(name)+".MDL" : name;
      if(name.find_first_of("/\\\"\r\n")!=name.npos) throw std::runtime_error("expected a visual basename");
      Measure result;
      std::string pose="static";
      if(resolved.ends_with(".MRM")) {
        zenkit::MultiResolutionMesh mesh; auto stream=open(vfs,resolved); mesh.load(stream.get()); result.mesh(mesh,identity());
      } else if(resolved.ends_with(".MDL")) {
        zenkit::Model model; auto stream=open(vfs,resolved); model.load(stream.get());
        pose="bind";
        if(name.ends_with(".MDS")) {
          // This pilot's locked chest is measured closed, using its one-frame rest animation.
          auto stream=open(vfs,stem(name)+"-S_S0.MAN");
          zenkit::ModelAnimation animation; animation.load(stream.get());
          if(animation.frame_count!=1 || animation.samples.size()!=animation.node_indices.size())
            throw std::runtime_error("expected one-frame S_S0 rest pose");
          if(animation.checksum!=model.hierarchy.checksum) throw std::runtime_error("animation hierarchy checksum mismatch");
          for(size_t n=0;n<animation.node_indices.size();++n) {
            const auto index=animation.node_indices[n];
            if(index>=model.hierarchy.nodes.size()) throw std::runtime_error("animation node out of range");
            const auto& sample=animation.samples[n];
            model.hierarchy.nodes[index].transform=sampleTransform(sample);
          }
          pose="S_S0_frame_0_closed";
        }
        result=measureModel(model.mesh,model.hierarchy);
      } else throw std::runtime_error("unsupported visual extension");
      if(!result.points || !result.tris) throw std::runtime_error("empty original visual");
      if(i>2) std::cout<<',';
      std::cout<<"{\"visual\":\""<<name<<"\",\"resolved_visual\":\""<<resolved<<"\",\"source_bbox_min_cm\":"; vec(result.min);
      std::cout<<",\"source_bbox_max_cm\":"; vec(result.max);
      std::cout<<",\"source_tris\":"<<result.tris<<",\"newworld_instances\":"<<frequency[stem(name)]
               <<",\"source_pose\":\""<<pose<<"\"}";
    }
    std::cout<<"]\n";
#else
    throw std::runtime_error("this test build accepts --self-test only");
#endif
  } catch(const std::exception& error) { std::cerr<<"target-measures: "<<error.what()<<'\n'; return 1; }
}

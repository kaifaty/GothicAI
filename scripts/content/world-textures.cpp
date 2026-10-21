// Offline surface inventory and geometry export. No texture pixels leave the game archives.
// scripts/content-textures.sh builds this against the engine's ZenKit ABI.
#include <zenkit/Logger.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/VirtualObject.hh>
#include <zenkit/world/WayNet.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::array worlds = {"NEWWORLD.ZEN", "OLDWORLD.ZEN", "ADDONWORLD.ZEN", "DRAGONISLAND.ZEN"};
constexpr std::array citySamples = {"NW_CITY_WALL_03.TGA", "NW_CITY_DUNGEONFLOOR_02.TGA", "NW_CITY_RICH_WALL_HALF_01.TGA"};
constexpr double uvTolerance = 1e-6;

std::string upper(std::string s) {
  for(auto& c:s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string textureName(std::string s) {
  std::replace(s.begin(),s.end(),'\\','/');
  s = upper(fs::path(s).filename().string());
  return s.empty() ? "<NO_TEXTURE>" : s;
}

struct Point {
  double x=0, y=0, z=0;
  Point() = default;
  Point(double x,double y,double z):x(x),y(y),z(z) {}
  explicit Point(zenkit::Vec3 p):x(p.x),y(p.y),z(p.z) {}
  Point operator-(Point b) const { return {x-b.x,y-b.y,z-b.z}; }
  Point operator+(Point b) const { return {x+b.x,y+b.y,z+b.z}; }
  Point operator*(double a) const { return {x*a,y*a,z*a}; }
};

Point transform(Point p,const zenkit::VirtualObject* v) {
  if(!v) return p;
  const auto& r = v->rotation.columns;
  // ZEN stores object-to-world transforms, including for nested vobs. Do not compose parents.
  return {r[0].x*p.x+r[1].x*p.y+r[2].x*p.z+v->position.x,
          r[0].y*p.x+r[1].y*p.y+r[2].y*p.z+v->position.y,
          r[0].z*p.x+r[1].z*p.y+r[2].z*p.z+v->position.z};
}

double area(Point a,Point b,Point c) {
  const auto u=b-a, v=c-a;
  const Point cross {u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
  return 0.5*std::hypot(cross.x,cross.y,cross.z)/10000.0; // cm² -> m²
}

double uvArea(const std::array<zenkit::Vec2,3>& uv) {
  const double ux=double(uv[1].x)-uv[0].x, uy=double(uv[1].y)-uv[0].y;
  const double vx=double(uv[2].x)-uv[0].x, vy=double(uv[2].y)-uv[0].y;
  return 0.5*std::abs(ux*vy-uy*vx);
}

bool tiled(const std::array<zenkit::Vec2,3>& uv) {
  for(unsigned axis=0;axis<2;++axis) {
    double lo=std::numeric_limits<double>::infinity(), hi=-lo;
    for(auto p:uv) {
      lo=std::min(lo,double(p[axis])); hi=std::max(hi,double(p[axis]));
    }
    if(lo < -uvTolerance || hi > 1+uvTolerance || hi-lo > 1+uvTolerance) return true;
  }
  return false;
}

struct Triangle {
  std::array<Point,3> p;
  std::array<zenkit::Vec2,3> uv;
  std::string texture;
  Point shadingNormal; // archive normals orient the Gothic clockwise geometry for camera placement
};
using Geometry = std::vector<Triangle>;

Triangle meshTriangle(const zenkit::Mesh& m,size_t a,size_t b,size_t c,
                      const std::vector<uint32_t>& vertices,const std::vector<uint32_t>& features,
                      uint32_t material) {
  Triangle t;
  const std::array indices={a,b,c};
  for(size_t k=0;k<3;++k) {
    t.p[k]=Point(m.vertices.at(vertices.at(indices[k])));
    const auto& feature=m.features.at(features.at(indices[k]));
    t.uv[k]=feature.texture; t.shadingNormal=t.shadingNormal+Point(feature.normal);
  }
  t.texture=textureName(m.materials.at(material).texture);
  return t;
}

Geometry worldGeometry(const zenkit::Mesh& m) {
  Geometry out;
  const auto& p=m.polygons;
  if(p.vertex_indices.size()!=3*p.material_indices.size() || p.feature_indices.size()!=p.vertex_indices.size())
    throw std::runtime_error("inconsistent world triangle indices");
  out.reserve(p.material_indices.size());
  // ZenKit has selected BSP leaf polygons, removed portals/ghost occluders/outdoor polygons,
  // and triangulated fans exactly as the engine's PackedMesh does.
  for(size_t i=0;i<p.material_indices.size();++i)
    out.push_back(meshTriangle(m,3*i,3*i+1,3*i+2,p.vertex_indices,p.feature_indices,p.material_indices[i]));
  return out;
}

Geometry basicGeometry(const zenkit::Mesh& m) {
  Geometry out;
  // Mesh::load without a BSP list does NOT populate m.polygons. Triangulate its raw fans.
  for(const auto& p:m.geometry) {
    if(p.flags.is_portal || p.flags.is_ghost_occluder || p.flags.is_outdoor) continue;
    for(size_t k=2;k<p.index_count;++k)
      out.push_back(meshTriangle(m,p.index_offset,p.index_offset+k-1,p.index_offset+k,
                                m.polygon_vertex_indices,m.polygon_feature_indices,p.material));
  }
  return out;
}

Geometry staticGeometry(const zenkit::VfsNode& file,bool msh) {
  auto input=file.open_read();
  Geometry out;
  if(msh) {
    zenkit::Mesh m;
    m.load(input.get(),false);
    return basicGeometry(m);
  } else {
    zenkit::MultiResolutionMesh m;
    m.load(input.get());
    for(const auto& sub:m.sub_meshes) {
      const auto name=textureName(sub.mat.texture);
      for(const auto& face:sub.triangles) {
        Triangle t;
        t.texture=name;
        for(size_t k=0;k<3;++k) {
          const auto& w=sub.wedges.at(face.wedges[k]);
          t.p[k]=Point(m.positions.at(w.index)); t.uv[k]=w.texture;
          t.shadingNormal=t.shadingNormal+Point(w.normal);
        }
        out.push_back(std::move(t));
      }
    }
  }
  return out;
}

struct Sample { double density, weight; };
struct Stats {
  uint64_t triangles=0, zeroArea=0;
  double area=0, meshArea=0, vobArea=0, tiledArea=0, validUvArea=0;
  std::vector<Sample> samples;
  std::set<size_t> vobs; // IDs are unique across all four worlds, not ZEN archive object IDs.

  void add(const Triangle& t,const zenkit::VirtualObject* v,size_t id) {
    const double a=::area(transform(t.p[0],v),transform(t.p[1],v),transform(t.p[2],v));
    if(!std::isfinite(a)) throw std::runtime_error("non-finite surface area");
    for(auto uv:t.uv)
      if(!std::isfinite(uv.x) || !std::isfinite(uv.y)) throw std::runtime_error("non-finite UV");
    ++triangles; area+=a;
    if(a==0) ++zeroArea;
    if(v) { vobArea+=a; vobs.insert(id); } else meshArea+=a;
    if(tiled(t.uv)) tiledArea+=a;
    const double u=uvArea(t.uv);
    if(a>0 && u>0) {
      validUvArea+=a;
      samples.push_back({std::sqrt(a/u),a});
    }
  }

  void merge(const Stats& s) {
    triangles+=s.triangles; zeroArea+=s.zeroArea;
    area+=s.area; meshArea+=s.meshArea; vobArea+=s.vobArea;
    tiledArea+=s.tiledArea; validUvArea+=s.validUvArea;
    samples.insert(samples.end(),s.samples.begin(),s.samples.end());
    vobs.insert(s.vobs.begin(),s.vobs.end());
  }

  void sort() {
    std::sort(samples.begin(),samples.end(),[](auto a,auto b) {return a.density<b.density;});
  }

  double quantile(double q) const {
    if(samples.empty()) return std::numeric_limits<double>::quiet_NaN();
    double sum=0;
    for(const auto& s:samples) {
      sum+=s.weight;
      if(sum>=q*validUvArea) return s.density;
    }
    return samples.back().density;
  }
};

using Textures = std::map<std::string,Stats>;
struct Inventory {
  std::string world;
  Textures textures;
  uint64_t triangles=0;
  double triangleArea=0, meshArea=0, vobArea=0;
  size_t includedVobs=0, hiddenStaticVobs=0, excludedVisuals=0;

  void add(const Geometry& g,const zenkit::VirtualObject* v=nullptr,size_t id=0) {
    // Independent reference sum before assigning any triangle to a texture bucket.
    for(const auto& t:g) {
      const double a=::area(transform(t.p[0],v),transform(t.p[1],v),transform(t.p[2],v));
      ++triangles; triangleArea+=a;
      if(v) vobArea+=a; else meshArea+=a;
    }
    for(const auto& t:g) textures[t.texture].add(t,v,id);
  }

  double textureArea() const {
    double a=0;
    for(const auto& [name,s]:textures) a+=s.area;
    return a;
  }

  double errorPct() const {
    return triangleArea==0 ? 0 : 100*std::abs(textureArea()-triangleArea)/triangleArea;
  }

  void validate() {
    uint64_t count=0;
    for(auto& [name,s]:textures) { s.sort(); count+=s.triangles; }
    if(triangleArea<=0 || count!=triangles || errorPct()>0.1)
      throw std::runtime_error(world+": texture totals do not match geometry");
  }
};

std::vector<std::pair<std::string,const Stats*>> ranked(const Textures& textures) {
  std::vector<std::pair<std::string,const Stats*>> rows;
  for(const auto& [name,s]:textures) rows.push_back({name,&s});
  std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b) {
    if(a.second->area!=b.second->area) return a.second->area>b.second->area;
    return a.first<b.first;
  });
  return rows;
}

std::ofstream output(const fs::path& p) {
  std::ofstream f(p);
  f.exceptions(std::ios::failbit|std::ios::badbit);
  f.imbue(std::locale::classic());
  f << std::fixed << std::setprecision(9);
  return f;
}

void csvString(std::ostream& f,const std::string& s) {
  f << '"';
  for(char c:s) { if(c=='"') f << '"'; f << c; }
  f << '"';
}

double fraction(double a,double total) { return total==0 ? 0 : a/total; }

void writeCsv(const fs::path& path,const Inventory& inv) {
  auto f=output(path);
  f << "world,texture,triangles,area_m2,world_area_fraction,uv_m_per_unit_p10,uv_m_per_unit_median,"
       "uv_m_per_unit_p90,uv_valid_area_fraction,world_mesh_area_m2,vob_area_m2,world_mesh_area_fraction,"
       "vob_area_fraction,vob_count,tiled_area_fraction,is_tiled,zero_area_triangles\n";
  for(const auto& [name,ptr]:ranked(inv.textures)) {
    const auto& s=*ptr;
    csvString(f,inv.world); f << ','; csvString(f,name);
    f << ',' << s.triangles << ',' << s.area << ',' << fraction(s.area,inv.triangleArea);
    for(double q:{0.1,0.5,0.9}) {
      f << ',';
      if(!s.samples.empty()) f << s.quantile(q);
    }
    f << ',' << fraction(s.validUvArea,s.area) << ',' << s.meshArea << ',' << s.vobArea
      << ',' << fraction(s.meshArea,s.area) << ',' << fraction(s.vobArea,s.area)
      << ',' << s.vobs.size() << ',' << fraction(s.tiledArea,s.area) << ',' << (s.tiledArea>0 ? 1 : 0)
      << ',' << s.zeroArea << '\n';
  }
}

void writeReadme(const fs::path& path,const std::vector<Inventory>& inventories,const Inventory& all) {
  auto f=output(path);
  f << std::setprecision(3);
  f << "# Текстуры поверхностей четырёх миров Gothic II + Ночь Ворона\n\n"
       "Офлайн-перепись для этапа 1 [плана своего контента](../../../docs/own-content.md), 2026-10-18. "
       "В репозитории только имена ресурсов и агрегированные числа; пиксели и геометрия не сохраняются.\n\n"
       "Повторить на Mac из корня репозитория:\n\n```sh\nscripts/content-textures.sh\n"
       "scripts/content-textures.sh --self-test\n```\n\n"
       "`GOTHIC_DIR` задаёт установленную игру (по умолчанию `/Volumes/DevSSD/Dev/games/Gothic II`). "
       "Скрипт собирает C++-инструмент на ZenKit; при отсутствии библиотеки собирает только цель `zenkit`. "
       "Игра и модели ИИ не запускаются.\n\n"
       "## Состав и метод\n\n"
       "- Архивы `Meshes.vdf`, `Meshes_Addon.vdf`, `Worlds.vdf`, `Worlds_Addon.vdf`; аддон явно перекрывает базу "
       "независимо от порядка файлов и временных меток. Loose-файлы и моды не подключаются.\n"
       "- Меш мира: триангулированные BSP leaf-полигоны ZenKit, как при загрузке в движке; порталы, ghost occluder "
       "и outdoor-полигоны исключены ZenKit. Статичные визуалы видимых vob: `.3DS` → `.MRM`, также `.MRM` и `.MSH`; "
       "полная детализация, каждый экземпляр со своим object-to-world трансформом. Трансформы вложенных vob уже мировые. "
       "Флаг `vob_static` не служит фильтром: считаются все визуалы этих форматов, включая предметы с таким визуалом.\n"
       "- NPC, `.MDS`/`.MDL` и другие анимированные/морф-модели, частицы, декали и скрытые визуалы не входят. "
       "Дочерние vob обходятся независимо от визуала родителя. Отсутствующий статичный меш или ошибка ZenKit прерывает прогон.\n"
       "- Площадь — сумма площадей 3D-треугольников, см² / 10 000; это площадь всех поверхностей, включая воду, "
       "растительность, перекрытия и обратные стороны, если они отдельные треугольники. Это не площадь земли на карте "
       "и не видимая площадь на экране; прозрачные пиксели не вычитаются.\n"
       "- На треугольнике масштаб UV = `sqrt(area_m2 / abs(signed_uv_area))`, м на единицу UV. P10/P50/P90 — "
       "взвешенные площадью квантили (первое значение с накопленным весом ≥ 10/50/90 %), без интерполяции. "
       "Сводка пересчитывается по всем треугольникам, а не по медианам миров. Нулевая площадь/вырожденные UV не участвуют "
       "в квантилях; их геометрическая площадь остаётся в итогах. Анизотропию это скалярное значение не описывает.\n"
       "- Плиточность: доля площади треугольников с UV за `[0,1]` или размахом U либо V > 1; допуск границы `1e-6`. "
       "`is_tiled=1`, если такая площадь > 0. Это геометрический признак повторения UV, а не проверка бесшовности картинки; "
       "даже смещённый в соседнюю ячейку атлас может его получить.\n"
       "- Имена текстур приведены к верхнему регистру и basename. `<NO_TEXTURE>` сохраняет площадь материалов без имени "
       "текстуры, если такие встретились. Vob считается один раз на текстуру; в сводке экземпляры разных миров различны.\n\n"
       "## Файлы и столбцы\n\n"
       "[Сводный CSV](all-worlds.csv), [Хоринис](newworld.csv), [Долина рудников](oldworld.csv), "
       "[Яркендар](addonworld.csv), [Остров драконов](dragonisland.csv). Строки по убыванию площади.\n\n"
       "`triangles`, `area_m2` — число треугольников и площадь текстуры; `world_area_fraction` — доля всей учтённой "
       "площади мира (в сводке — четырёх миров). `uv_m_per_unit_{p10,median,p90}` — масштаб UV, пусто при отсутствии "
       "валидных UV; `uv_valid_area_fraction` — доля площади текстуры, на которой он определён. "
       "`world_mesh_area_m2`, `vob_area_m2` и соответствующие `_fraction` делят площадь **этой текстуры** по источнику. "
       "`vob_count` — число её экземпляров vob; `tiled_area_fraction` — плиточная доля её площади; "
       "`zero_area_triangles` — число треугольников нулевой площади. Все доли 0–1.\n\n"
       "## Сверка площадей\n\n"
       "Контрольная сумма считается по треугольникам отдельно, до разнесения по текстурам. "
       "Прогон завершается ошибкой при несовпадении числа треугольников или площади более чем на 0,1 %.\n\n"
       "| Мир | Треугольников | Строк CSV | Меш мира, м² | Vob, м² | Контрольная сумма, м² | Сумма CSV, м² | Ошибка, % |\n"
       "|---|---:|---:|---:|---:|---:|---:|---:|\n";
  for(const auto& inv:inventories)
    f << "| " << inv.world << " | " << inv.triangles << " | " << inv.textures.size() << " | " << inv.meshArea
      << " | " << inv.vobArea << " | " << inv.triangleArea << " | " << inv.textureArea()
      << " | " << std::setprecision(9) << inv.errorPct() << std::setprecision(3) << " |\n";
  f << "| Все миры | " << all.triangles << " | " << all.textures.size() << " | " << all.meshArea
    << " | " << all.vobArea << " | " << all.triangleArea << " | " << all.textureArea()
    << " | " << std::setprecision(9) << all.errorPct() << std::setprecision(3) << " |\n\n"
       "| Мир | Учтено vob | Скрытых статичных визуалов | Прочих исключённых непустых визуалов |\n"
       "|---|---:|---:|---:|\n";
  for(const auto& inv:inventories)
    f << "| " << inv.world << " | " << inv.includedVobs << " | " << inv.hiddenStaticVobs << " | " << inv.excludedVisuals << " |\n";
  double validArea=0; uint64_t zeroArea=0;
  for(const auto& [name,s]:all.textures) { validArea+=s.validUvArea; zeroArea+=s.zeroArea; }
  f << "\nМасштаб UV определён на " << 100*fraction(validArea,all.triangleArea)
    << " % площади; треугольников нулевой площади: " << zeroArea << ".\n\n";
  const auto noTexture=all.textures.find("<NO_TEXTURE>");
  f << "Именованных текстур: " << all.textures.size()-(noTexture!=all.textures.end()) << '.';
  if(noTexture!=all.textures.end())
    f << " Отдельная строка `<NO_TEXTURE>`: " << noTexture->second.triangles << " треугольников, "
      << noTexture->second.area << " м² (" << 100*noTexture->second.area/all.triangleArea
      << " %); она входит в общую площадь, но не в число текстур для замены.";
  f << "\n\n"
       "## Приоритет замены по площади\n\n"
       "Сортировка именованных текстур сводки по площади; их число до первого достижения порога "
       "от **всей** учтённой площади (включая `<NO_TEXTURE>` в знаменателе). "
       "Плиточные здесь — `is_tiled=1` (есть ненулевая площадь повторяющихся UV).\n\n"
       "| Покрытие | Текстур | Из них плиточных | Достигнутая доля, % |\n|---|---:|---:|---:|\n";
  const auto rows=ranked(all.textures);
  for(double target:{0.5,0.8,0.95}) {
    double a=0; size_t count=0, tiles=0;
    for(const auto& [name,s]:rows) {
      if(name=="<NO_TEXTURE>") continue;
      a+=s->area; ++count; tiles+=s->tiledArea>0;
      if(a>=target*all.triangleArea) break;
    }
    f << "| " << int(100*target) << " % | " << count << " | " << tiles << " | " << 100*a/all.triangleArea << " |\n";
  }
  f << "\n## Топ-30 во всех мирах\n\n"
       "| Текстура | Площадь, м² | Доля, % | UV P10 / медиана / P90, м | Плиточная площадь, % | Vob |\n"
       "|---|---:|---:|---:|---:|---:|\n";
  for(size_t i=0;i<std::min(size_t(30),rows.size());++i) {
    const auto& [name,s]=rows[i];
    f << "| `" << name << "` | " << s->area << " | " << 100*s->area/all.triangleArea << " | "
      << s->quantile(0.1) << " / " << s->quantile(0.5) << " / " << s->quantile(0.9)
      << " | " << 100*fraction(s->tiledArea,s->area) << " | " << s->vobs.size() << " |\n";
  }
  f << "\n## Выборочная проверка Хориниса\n\n"
       "Три фиксированные поверхности `NW_CITY_*` в `NEWWORLD.ZEN`: городская стена, пол подземелья "
       "и стена верхнего квартала. Ожидаемый порядок повтора — 1–4 м на единицу UV. "
       "Это офлайн-проверка масштаба по геометрии и UV.\n\n"
       "| Текстура | Площадь, м² | UV P10, м | Медиана, м | UV P90, м |\n"
       "|---|---:|---:|---:|---:|\n";
  for(const auto* name:citySamples) {
    const auto& s=inventories.front().textures.at(name);
    f << "| `" << name << "` | " << s.area << " | " << s.quantile(0.1) << " | " << s.quantile(0.5)
      << " | " << s.quantile(0.9) << " |\n";
  }
  f << "\nВсе три медианы и диапазоны P10–P90 находятся в 1–4 м: масштаб правдоподобен. "
       "При повторном прогоне инструмент проверяет этот диапазон для тех же трёх имён.\n";
}

#include "world-intent.inc"

struct TriangleBounds {
  double xmin=-INFINITY,zmin=-INFINITY,xmax=INFINITY,zmax=INFINITY;
  bool intersects(const Triangle& t) const {
    double xlo=INFINITY,zlo=INFINITY,xhi=-INFINITY,zhi=-INFINITY;
    for(auto p:t.p) {
      xlo=std::min(xlo,p.x); xhi=std::max(xhi,p.x);
      zlo=std::min(zlo,p.z); zhi=std::max(zhi,p.z);
    }
    return xhi>=xmin && xlo<=xmax && zhi>=zmin && zlo<=zmax;
  }
};

void jsonTriangle(std::ostream& f,const Triangle& t,size_t source,const std::string& visual) {
  f << "{\"texture\":"; jsonString(f,t.texture);
  f << ",\"visual\":"; jsonString(f,visual);
  f << ",\"source\":" << source << ",\"area_m2\":" << area(t.p[0],t.p[1],t.p[2])
    << ",\"center\":"; jsonPoint(f,triangleCenter(t));
  f << ",\"normal\":"; jsonPoint(f,triangleNormal(t));
  f << ",\"points\":[";
  for(size_t k=0;k<3;++k) { if(k) f << ','; jsonPoint(f,t.p[k]); }
  f << "],\"uv\":[";
  for(size_t k=0;k<3;++k) { if(k) f << ','; f << '[' << t.uv[k].x << ',' << t.uv[k].y << ']'; }
  f << "]}";
}

int trianglesMain(const fs::path& game,const fs::path& destination,const fs::path& textureFile,
                  const TriangleBounds& bounds,bool bounded) {
  std::ifstream selected(textureFile);
  if(!selected) throw std::runtime_error("cannot read texture selection "+textureFile.string());
  std::set<std::string> textures;
  for(std::string line;std::getline(selected,line);) {
    const auto first=line.find_first_not_of(" \t\r"),last=line.find_last_not_of(" \t\r");
    if(first!=std::string::npos && line[first]!='#') textures.insert(textureName(line.substr(first,last-first+1)));
  }
  if(textures.empty()) throw std::runtime_error("texture selection is empty");
  size_t zenkitErrors=0;
  zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
    ++zenkitErrors; std::cerr << "ZenKit " << component << ": " << message << '\n';
  });
  std::map<std::string,fs::path> archives;
  for(const auto& e:fs::directory_iterator(game/"Data")) if(e.is_regular_file())
    archives[upper(e.path().filename().string())]=e.path();
  zenkit::Vfs vfs;
  // Deliberately mount only geometry archives; texture archives are never opened.
  for(const auto* name:{"MESHES.VDF","MESHES_ADDON.VDF","WORLDS.VDF","WORLDS_ADDON.VDF"}) {
    const auto it=archives.find(name); if(it==archives.end()) throw std::runtime_error(std::string("missing archive ")+name);
    vfs.mount_disk(it->second,zenkit::VfsOverwriteBehavior::ALL);
  }
  const auto* worldFile=vfs.find("NEWWORLD.ZEN");
  if(!worldFile) throw std::runtime_error("missing NEWWORLD.ZEN");
  zenkit::World world; auto input=worldFile->open_read(); world.load(input.get(),zenkit::GameVersion::GOTHIC_2);
  const auto worldTriangles=worldGeometry(world.world_mesh);
  fs::create_directories(destination.parent_path());
  auto f=output(destination);
  f << "{\"world\":\"NEWWORLD.ZEN\",\"coordinates\":\"Gothic world centimetres (Y up)\",\"bounds_xz_cm\":";
  if(bounded) f << '[' << bounds.xmin << ',' << bounds.zmin << ',' << bounds.xmax << ',' << bounds.zmax << ']';
  else f << "null";
  f << ",\"bounds_rule\":\"uncut triangles with intersecting XZ bounding rectangles\",\"candidate_house_triangles\":[";
  size_t triangleCount=0,nextVob=0;
  std::vector<std::array<Point,3>> occluders;
  auto add=[&](const Geometry& geometry,const zenkit::VirtualObject* v,size_t source,const std::string& visual) {
    for(const auto& original:geometry) {
      auto t=original;
      for(auto& p:t.p) p=transform(p,v);
      if(!bounds.intersects(t)) continue;
      if(!std::isfinite(area(t.p[0],t.p[1],t.p[2]))) throw std::runtime_error("non-finite triangle area");
      occluders.push_back(t.p);
      if(!textures.contains(t.texture)) continue;
      t.shadingNormal=transformNormal(t.shadingNormal,v);
      for(auto uv:t.uv) if(!std::isfinite(uv.x) || !std::isfinite(uv.y)) throw std::runtime_error("non-finite UV");
      if(triangleCount++) f << ',';
      jsonTriangle(f,t,source,visual);
    }
  };
  add(worldTriangles,nullptr,0,"");
  std::map<std::string,Geometry> cache;
  std::vector<const zenkit::VirtualObject*> freepoints;
  std::function<void(const zenkit::VirtualObject&)> visit=[&](const auto& v) {
    if(v.type==zenkit::VirtualObjectType::zCVobSpot) freepoints.push_back(&v);
    if(v.visual && !v.visual->name.empty()) {
      auto original=upper(v.visual->name); std::replace(original.begin(),original.end(),'\\','/');
      original=fs::path(original).filename().string(); const auto ext=fs::path(original).extension().string();
      if((ext==".3DS" || ext==".MRM" || ext==".MSH") && v.show_visual &&
         v.type!=zenkit::VirtualObjectType::oCNpc && v.visual->type!=zenkit::VisualType::DECAL &&
         v.visual->type!=zenkit::VisualType::PARTICLE_EFFECT) {
        auto visual=ext==".3DS" ? fs::path(original).replace_extension(".MRM").string() : original;
        auto found=cache.find(visual);
        if(found==cache.end()) {
          const auto* file=vfs.find(visual); if(!file) throw std::runtime_error("missing static mesh "+visual);
          found=cache.emplace(visual,staticGeometry(*file,ext==".MSH")).first;
        }
        add(found->second,&v,++nextVob,original);
      }
    }
    for(const auto& child:v.children) if(child) visit(*child);
  };
  for(const auto& v:world.world_vobs) if(v) visit(*v);
  if(zenkitErrors) throw std::runtime_error("ZenKit reported "+std::to_string(zenkitErrors)+" errors");
  f << "],\"occluders\":[";
  for(size_t i=0;i<occluders.size();++i) {
    if(i) f << ','; f << '[';
    for(size_t k=0;k<3;++k) { if(k) f << ','; jsonPoint(f,occluders[i][k]); }
    f << ']';
  }
  f << "],\"waypoints\":[";
  size_t pointCount=0;
  if(world.way_net) for(const auto& wp:world.way_net->points) if(wp) {
    if(pointCount++) f << ',';
    f << "{\"name\":"; jsonString(f,wp->name);
    f << ",\"position\":"; jsonPoint(f,Point(wp->position));
    f << ",\"direction\":"; jsonPoint(f,Point(wp->direction)); f << '}';
  }
  f << "],\"freepoints\":[";
  for(size_t k=0;k<freepoints.size();++k) {
    if(k) f << ','; const auto& v=*freepoints[k];
    f << "{\"name\":"; jsonString(f,v.vob_name);
    f << ",\"position\":"; jsonPoint(f,Point(v.position));
    f << ",\"direction\":"; jsonPoint(f,Point(v.rotation.columns[2])); f << '}';
  }
  f << "],\"ground_triangles\":[";
  size_t groundCount=0;
  for(const auto& t:worldTriangles) if(bounds.intersects(t) && triangleNormal(t).y>0.5) {
    if(groundCount++) f << ','; f << '[';
    for(size_t k=0;k<3;++k) { if(k) f << ','; jsonPoint(f,t.p[k]); }
    f << ']';
  }
  f << "],\"ground_textures\":[";
  size_t groundTextureCount=0;
  for(const auto& t:worldTriangles) if(bounds.intersects(t) && triangleNormal(t).y>0.5) {
    if(groundTextureCount++) f << ',';
    jsonString(f,t.texture);
  }
  f << "]}\n";
  std::cerr << "Triangles: " << triangleCount << " selected, " << groundCount << " upward ground, "
            << occluders.size() << " occluders, "
            << pointCount << " waypoints, " << freepoints.size() << " freepoints; " << destination << '\n';
  return 0;
}

void require(bool condition,const char* message) {
  if(!condition) throw std::runtime_error(message);
}

void selfTest() {
  const Triangle t {{{Point(0,0,0),Point(200,0,0),Point(0,200,0)}},
                    {{{0,0},{1,0},{0,1}}},"FLOOR.TGA"};
  require(area(t.p[0],t.p[1],t.p[2])==2,"cm to m conversion");
  require(uvArea(t.uv)==0.5 && !tiled(t.uv),"UV area and unit tile");
  auto repeated=t; repeated.uv[1].x=2;
  require(tiled(repeated.uv),"UV span");
  auto offset=t; for(auto& p:offset.uv) p.x+=2;
  require(tiled(offset.uv),"offset UV outside unit square");
  zenkit::VirtualObject v;
  v.rotation=zenkit::Mat3({0,2,0},{-3,0,0},{0,0,1}); v.position={10000,-123,47};
  require(std::abs(area(transform(t.p[0],&v),transform(t.p[1],&v),transform(t.p[2],&v))-12)<1e-9,
          "rotation, translation and nonuniform scale");
  Stats s; s.add(t,nullptr,0); s.add(t,&v,1); s.add(t,&v,1); s.sort();
  require(s.vobs.size()==1 && s.triangles==3 && s.area==26,"per-texture vob deduplication");
  Stats q; q.samples={{1,1},{2,8},{10,1}}; q.validUvArea=10; q.sort();
  require(q.quantile(0.1)==1 && q.quantile(0.5)==2 && q.quantile(0.9)==2,"area-weighted quantiles");
  auto degenerate=t; degenerate.uv={{{0,0},{0,0},{0,0}}};
  Stats d; d.add(degenerate,nullptr,0);
  require(d.area==2 && d.samples.empty(),"degenerate UV retains geometry area");
  Inventory inv; inv.world="TEST"; inv.add({t,repeated,degenerate}); inv.validate();
  require(inv.triangles==3 && inv.textureArea()==inv.triangleArea,"texture conservation");
  Stats merged; merged.merge(q); merged.merge(d); merged.sort();
  require(merged.quantile(0.5)==2 && merged.area==2,"merge keeps samples and UV weights");
  zenkit::Mesh quad;
  quad.materials.resize(1); quad.materials[0].texture="floor.tga";
  quad.vertices={{0,0,0},{200,0,0},{200,200,0},{0,200,0}};
  for(auto uv:std::array<zenkit::Vec2,4>{{{0,0},{1,0},{1,1},{0,1}}}) {
    zenkit::VertexFeature feature{}; feature.texture=uv; quad.features.push_back(feature);
  }
  quad.polygon_vertex_indices={0,1,2,3}; quad.polygon_feature_indices={0,1,2,3};
  zenkit::Polygon poly{}; poly.index_count=4; quad.geometry.push_back(poly);
  Inventory msh; msh.world="MSH"; msh.add(basicGeometry(quad)); msh.validate();
  require(msh.triangles==2 && msh.triangleArea==4 && msh.textures.at("FLOOR.TGA").quantile(0.5)==2,
          "MSH fan triangulation and density");
  quad.geometry[0].flags.is_portal=1;
  require(basicGeometry(quad).empty(),"MSH portal exclusion");
  const TriangleBounds bounds{50,50,150,150};
  auto flat=t; flat.p={Point(0,0,0),Point(200,0,0),Point(0,0,200)};
  require(bounds.intersects(flat),"triangle export XZ overlap");
  for(auto& p:flat.p) p.x+=500;
  require(!bounds.intersects(flat),"triangle export XZ rejection");
  std::ostringstream exported;
  jsonTriangle(exported,t,7,"HOUSE.3DS");
  require(exported.str().find("\"source\":7")!=std::string::npos &&
          exported.str().find("\"uv\":[[0,0],[1,0],[0,1]]")!=std::string::npos,
          "triangle export source and per-vertex UV");
  std::cout << "self-test: 15 checks passed\n";
}
}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") { selfTest(); intentSelfTest(); return 0; }
    if(argc==4 && std::string(argv[1])=="--intent") return intentMain(argv[2],argv[3]);
    if(argc>1 && std::string(argv[1])=="--triangles") {
      if((argc!=6 && argc!=11) || std::string(argv[4])!="--textures" ||
         (argc==11 && std::string(argv[6])!="--bounds"))
        throw std::runtime_error("usage: --triangles <game dir> <outside json> --textures <names file> [--bounds xmin zmin xmax zmax]");
      TriangleBounds bounds;
      if(argc==11) {
        std::array<double*,4> values={&bounds.xmin,&bounds.zmin,&bounds.xmax,&bounds.zmax};
        for(size_t k=0;k<4;++k) {
          size_t used=0; *values[k]=std::stod(argv[7+k],&used);
          if(used!=std::string(argv[7+k]).size() || !std::isfinite(*values[k])) throw std::runtime_error("invalid XZ bound");
        }
        if(bounds.xmin>bounds.xmax || bounds.zmin>bounds.zmax) throw std::runtime_error("inverted XZ bounds");
      }
      return trianglesMain(argv[2],argv[3],argv[5],bounds,argc==11);
    }
    if(argc!=3) throw std::runtime_error("usage: world-textures <game dir> <out dir> | --intent <game dir> <outside json> | --triangles <game dir> <outside json> --textures <names file> [--bounds xmin zmin xmax zmax] | --self-test");
    size_t zenkitErrors=0;
    zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
      ++zenkitErrors; std::cerr << "ZenKit " << component << ": " << message << '\n';
    });
    std::map<std::string,fs::path> archives;
    for(const auto& e:fs::directory_iterator(fs::path(argv[1])/"Data"))
      if(e.is_regular_file()) archives[upper(e.path().filename().string())]=e.path();
    zenkit::Vfs vfs;
    for(const auto* name:{"MESHES.VDF","MESHES_ADDON.VDF","WORLDS.VDF","WORLDS_ADDON.VDF"}) {
      const auto found=archives.find(name);
      if(found==archives.end()) throw std::runtime_error(std::string("missing archive ")+name);
      vfs.mount_disk(found->second,zenkit::VfsOverwriteBehavior::ALL);
    }
    std::map<std::string,Geometry> cache;
    std::vector<Inventory> inventories;
    size_t nextVob=0;
    for(const auto* name:worlds) {
      const auto* file=vfs.find(name);
      if(!file) throw std::runtime_error(std::string("missing world ")+name);
      std::cerr << "Reading " << name << "...\n";
      zenkit::World world;
      auto input=file->open_read(); world.load(input.get(),zenkit::GameVersion::GOTHIC_2);
      Inventory inv; inv.world=name;
      inv.add(worldGeometry(world.world_mesh));
      std::function<void(const zenkit::VirtualObject&)> visit = [&](const auto& v) {
        if(v.visual && !v.visual->name.empty()) {
          auto visual=upper(v.visual->name);
          std::replace(visual.begin(),visual.end(),'\\','/');
          visual=fs::path(visual).filename().string();
          const auto ext=fs::path(visual).extension().string();
          const bool eligible=(ext==".3DS" || ext==".MRM" || ext==".MSH") &&
                              v.type!=zenkit::VirtualObjectType::oCNpc &&
                              v.visual->type!=zenkit::VisualType::DECAL &&
                              v.visual->type!=zenkit::VisualType::PARTICLE_EFFECT;
          if(eligible && v.show_visual) {
            if(ext==".3DS") visual=fs::path(visual).replace_extension(".MRM").string();
            auto found=cache.find(visual);
            if(found==cache.end()) {
              const auto* mesh=vfs.find(visual);
              if(!mesh) throw std::runtime_error(inv.world+": missing static mesh "+visual);
              found=cache.emplace(visual,staticGeometry(*mesh,ext==".MSH")).first;
            }
            inv.add(found->second,&v,++nextVob); ++inv.includedVobs;
          } else if(eligible) ++inv.hiddenStaticVobs;
          else ++inv.excludedVisuals;
        }
        for(const auto& child:v.children) if(child) visit(*child);
      };
      for(const auto& v:world.world_vobs) if(v) visit(*v);
      inv.validate();
      if(zenkitErrors) throw std::runtime_error("ZenKit reported "+std::to_string(zenkitErrors)+" errors");
      std::cerr << std::fixed << std::setprecision(6) << name << ": " << inv.triangles << " triangles, "
                << inv.textures.size() << " textures, " << inv.includedVobs << " vobs, " << inv.triangleArea
                << " m2; area mismatch " << inv.errorPct() << " %\n";
      inventories.push_back(std::move(inv));
    }
    Inventory all; all.world="ALL";
    for(const auto& inv:inventories) {
      all.triangleArea+=inv.triangleArea; all.triangles+=inv.triangles;
      all.meshArea+=inv.meshArea; all.vobArea+=inv.vobArea;
      for(const auto& [name,s]:inv.textures) all.textures[name].merge(s);
    }
    all.validate();
    for(const auto* name:citySamples) {
      const auto& s=inventories.front().textures.at(name);
      require(s.quantile(0.1)>=1 && s.quantile(0.9)<=4,"city sample UV repeat outside expected 1-4 m");
    }
    const fs::path out=argv[2]; fs::create_directories(out);
    for(size_t i=0;i<inventories.size();++i) {
      auto filename=fs::path(worlds[i]).stem().string();
      for(auto& c:filename) c=char(std::tolower(static_cast<unsigned char>(c)));
      writeCsv(out/(filename+".csv"),inventories[i]);
    }
    writeCsv(out/"all-worlds.csv",all);
    writeReadme(out/"README.md",inventories,all);
    std::cerr << "Wrote 5 CSV files and README.md; ZenKit errors: " << zenkitErrors << '\n';
  } catch(const std::exception& e) {
    std::cerr << "world-textures: " << e.what() << '\n'; return 1;
  }
}

#include "worldchunkindex.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

#include "gothicglb.h"

namespace {

static_assert(sizeof(wci::Cell)==80 && offsetof(wci::Cell,glbSize)==8 && offsetof(wci::Cell,glbSha256)==16 &&
              offsetof(wci::Cell,addedStart)==48 && offsetof(wci::Cell,vertices)==52 &&
              offsetof(wci::Cell,features)==60 && offsetof(wci::Cell,materials)==68 && offsetof(wci::Cell,zero)==76);
static_assert(sizeof(wci::Exception)==8);

constexpr char magic[4] = {'G','W','C','I'};
constexpr char what[]   = "chunk index";

enum Section : uint32_t {
  Cells=1, Missing=2, VertexExceptions=3, FeatureExceptions=4, MaterialExceptions=5, Materials=6, Strings=7, SectionEnd=8
  };

constexpr wcb::SectionSpec specs[] = {
  {"cells",sizeof(wci::Cell)},
  {"missing orders",4},
  {"vertex exceptions",sizeof(wci::Exception)},
  {"feature exceptions",sizeof(wci::Exception)},
  {"material exceptions",sizeof(wci::Exception)},
  {"materials",sizeof(wcb::Material)},
  {"strings",1},
  };
static_assert(std::size(specs)==SectionEnd-1);

std::string cellName(const wci::Cell& c) {
  return std::to_string(c.x)+","+std::to_string(c.z);
  }

}

std::vector<uint8_t> encodeCookedIndex(const CookedIndexData& data) {
  using wcb::count32;
  const auto& h = data.header;
  wcb::Writer w;
  for(const auto c:magic)
    w.u8(uint8_t(c));
  w.u32(wci::Version);
  w.u32(wcb::ByteOrder);
  w.u32(wci::HeaderSize);
  const size_t fileSizeAt = w.out.size();
  w.u64(0);
  w.u64(h.worldSize);
  w.u32(h.worldCrc32);
  w.u32(count32(data.cells.size(),"cells"));
  w.u32(h.vertices);
  w.u32(h.features);
  w.u32(h.materials);
  w.u32(h.triangles);
  w.u32(h.sourceTriangles);
  w.u32(SectionEnd-1);
  w.u64(wci::HeaderSize);
  w.u64(0);
  if(w.out.size()!=wci::HeaderSize)
    throw std::runtime_error("chunk index: header layout");

  w.directory(SectionEnd-1);
  w.section(Cells,sizeof(wci::Cell),data.cells.size(),"cells",[&] {
    for(const auto& c:data.cells) {
      w.i32(c.x); w.i32(c.z); w.u64(c.glbSize);
      for(const auto b:c.glbSha256)
        w.u8(b);
      w.u32(c.addedStart);
      for(const auto* r:{&c.vertices,&c.features,&c.materials}) {
        w.u32(r->first); w.u32(r->count);
        }
      w.u32(0);
      }
    });
  w.section(Missing,4,data.missing.size(),"missing orders",[&] {
    for(const auto o:data.missing)
      w.u32(o);
    });
  uint32_t type = VertexExceptions;
  for(const auto* list:{&data.vertices,&data.features,&data.materials}) {
    w.section(type,sizeof(wci::Exception),list->size(),specs[type-1].name,[&] {
      for(const auto& e:*list) {
        w.u32(e.local); w.u32(e.id);
        }
      });
    ++type;
    }
  std::string strings;
  w.section(Materials,sizeof(wcb::Material),data.appended.size(),"materials",[&] {
    for(const auto& m:data.appended)
      w.material(wcb::None,m,strings);
    });
  w.strings(Strings,strings);
  w.put64(fileSizeAt,w.out.size());
  return std::move(w.out);
  }

CookedIndex CookedIndex::open(std::span<const uint8_t> bytes, const std::string& name, std::shared_ptr<const void> owner) {
  using wcb::le32;
  using wcb::le64;
  // The cell records hold a u64: they are viewed in place, so 8-byte alignment.
  wcb::checkPrologue(bytes,magic,wci::Version,wci::HeaderSize,alignof(uint64_t),what,name);
  const auto* b = bytes.data();
  CookedIndex x;
  x.owner = std::move(owner);
  x.bytes = bytes;
  x.label = name;
  auto& h = x.head;
  h.worldSize       = le64(b+24);
  h.worldCrc32      = le32(b+32);
  h.cells           = le32(b+36);
  h.vertices        = le32(b+40);
  h.features        = le32(b+44);
  h.materials       = le32(b+48);
  h.triangles       = le32(b+52);
  h.sourceTriangles = le32(b+56);
  const auto sections = wcb::readDirectory(bytes,wci::HeaderSize,le32(b+60),le64(b+64),specs,what,name);
  auto reject = [&](const char* part, const std::string& reason) {
    wcb::reject(what,name,part,reason);
    };
  if(sections[Cells-1].count!=h.cells)
    reject("cells","the header counts "+std::to_string(h.cells)+", the section holds "+
           std::to_string(sections[Cells-1].count));
  x.cellSpan     = wcb::view<wci::Cell>(bytes,sections[Cells-1]);
  x.missingSpan  = wcb::view<uint32_t>(bytes,sections[Missing-1]);
  for(uint32_t k=0;k<3;++k)
    x.exceptionSpan[k] = wcb::view<wci::Exception>(bytes,sections[VertexExceptions-1+k]);
  x.materialSpan = wcb::view<wcb::Material>(bytes,sections[Materials-1]);
  x.stringSpan   = bytes.subspan(size_t(sections[Strings-1].offset),sections[Strings-1].count);

  if(h.sourceTriangles>h.triangles)
    reject("header",std::to_string(h.sourceTriangles)+" source triangles of "+std::to_string(h.triangles));
  for(size_t i=1;i<x.missingSpan.size();++i)
    if(x.missingSpan[i]<=x.missingSpan[i-1])
      reject("missing orders","not ascending at entry "+std::to_string(i));

  // Ranges: the cells' exceptions in cell order, back to back; ids below the totals.
  const uint32_t totals[3] = {h.vertices,h.features,h.materials};
  const char* kinds[3] = {"vertex exceptions","feature exceptions","material exceptions"};
  uint64_t next[3] = {0,0,0};
  for(size_t i=0;i<x.cellSpan.size();++i) {
    const auto& c = x.cellSpan[i];
    const auto where = "cell "+std::to_string(i)+" ("+cellName(c)+")";
    if(c.zero!=0)
      reject("cells",where+": the reserved field is not zero");
    if(c.addedStart<h.sourceTriangles || c.addedStart>h.triangles ||
       (i==0 && c.addedStart!=h.sourceTriangles) || (i>0 && c.addedStart<x.cellSpan[i-1].addedStart))
      reject("cells",where+": its Added triangles start at rank "+std::to_string(c.addedStart)+
             ", out of cell order or outside "+std::to_string(h.sourceTriangles)+".."+std::to_string(h.triangles));
    const wci::Range* ranges[3] = {&c.vertices,&c.features,&c.materials};
    for(unsigned k=0;k<3;++k) {
      const auto& r = *ranges[k];
      if(r.first!=next[k] || uint64_t(r.first)+r.count>x.exceptionSpan[k].size())
        reject(kinds[k],where+": entries "+std::to_string(r.first)+"+"+std::to_string(r.count)+
               " are not the next ones of "+std::to_string(x.exceptionSpan[k].size()));
      next[k] = uint64_t(r.first)+r.count;
      for(uint32_t e=r.first;e<r.first+r.count;++e) {
        const auto& ex = x.exceptionSpan[k][e];
        if(e>r.first && ex.local<=x.exceptionSpan[k][e-1].local)
          reject(kinds[k],where+": local indices are not ascending at entry "+std::to_string(e));
        if(ex.id>=totals[k])
          reject(kinds[k],where+": local "+std::to_string(ex.local)+" has the stitched id "+std::to_string(ex.id)+
                 ", past the totals ("+std::to_string(totals[k])+")");
        }
      }
    }
  for(unsigned k=0;k<3;++k)
    if(next[k]!=x.exceptionSpan[k].size())
      reject(kinds[k],std::to_string(x.exceptionSpan[k].size()-next[k])+" entries belong to no cell");
  if(x.materialSpan.size()>h.materials)
    reject("materials",std::to_string(x.materialSpan.size())+" appended materials, the stitched mesh has "+
           std::to_string(h.materials));
  for(size_t i=0;i<x.materialSpan.size();++i) {
    if(x.materialSpan[i].source!=wcb::None)
      reject("materials","appended material "+std::to_string(i)+" carries a source index");
    if(!wcb::materialStringsFit(x.materialSpan[i],x.stringSpan.size()))
      reject("materials","appended material "+std::to_string(i)+" has a string past the string section");
    }
  return x;
  }

CookedIndex CookedIndex::read(const std::filesystem::path& path) {
  auto bytes = std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(path));
  const std::span<const uint8_t> view(bytes->data(),bytes->size());
  return open(view,path.string(),std::move(bytes));
  }

std::span<const wci::Exception> CookedIndex::vertexExceptions(size_t cell) const {
  const auto& r = cellSpan[cell].vertices;
  return exceptionSpan[0].subspan(r.first,r.count);
  }

std::span<const wci::Exception> CookedIndex::featureExceptions(size_t cell) const {
  const auto& r = cellSpan[cell].features;
  return exceptionSpan[1].subspan(r.first,r.count);
  }

std::span<const wci::Exception> CookedIndex::materialExceptions(size_t cell) const {
  const auto& r = cellSpan[cell].materials;
  return exceptionSpan[2].subspan(r.first,r.count);
  }

asset::Material CookedIndex::appended(size_t i) const {
  return wcb::readMaterial(materialSpan[i],stringSpan);
  }

uint32_t CookedIndex::sourceRank(uint32_t order) const {
  if(order==wcb::Added)
    return wcb::None;
  const auto at = std::lower_bound(missingSpan.begin(),missingSpan.end(),order);
  if(at!=missingSpan.end() && *at==order)
    return wcb::None;
  const auto rank = order-uint32_t(at-missingSpan.begin());
  return rank<head.sourceTriangles ? rank : wcb::None;
  }

uint32_t CookedIndex::addedEnd(size_t cell) const {
  return cell+1<cellSpan.size() ? cellSpan[cell+1].addedStart : head.triangles;
  }

uint32_t CookedIndex::resolve(std::span<const wci::Exception> exceptions, uint32_t local, uint32_t source) {
  if(exceptions.empty())
    return source;
  const auto at = std::lower_bound(exceptions.begin(),exceptions.end(),local,
                                   [](const wci::Exception& e, uint32_t l) { return e.local<l; });
  return at!=exceptions.end() && at->local==local ? at->id : source;
  }

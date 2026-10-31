#include "musicdata.h"
#include "gothicglb.h"
#include "sha256.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>

namespace MusicData {
namespace {
namespace fs = std::filesystem;
using Json = rapidjson::Value;

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

std::string upperAscii(std::string_view s) {
  std::string out(s);
  for(auto& c:out)
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
  return out;
  }

// zenkit::VfsNode::child trims the requested name with std::isspace.
std::string_view trimTrailing(std::string_view s) {
  while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
  }

const Json& member(const Json& obj, const char* name) {
  require(obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }

uint64_t unsignedValue(const Json& value, const char* name) {
  require(value.IsUint64(),std::string(name)+" must be an unsigned integer");
  return value.GetUint64();
  }

void exactMembers(const Json& obj, const char* what, std::initializer_list<const char*> names) {
  require(obj.IsObject(),std::string(what)+" must be an object");
  for(const char* name:names)
    member(obj,name);
  require(obj.MemberCount()==names.size(),std::string(what)+" has unknown members");
  }

std::string optionalString(const Json& v, const char* name) {
  return v.IsNull() ? std::string() : gothicglb::jsonToString(v,name);
  }

bool isHex64(std::string_view s) {
  return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
  }

bool isKind(std::string_view k) {
  return k=="segment" || k=="style" || k=="dls";
  }

// Host paths stay inside the installation, as Resources::BaseIndex::load requires of loose files.
std::string relativePath(const Json& v, const char* name) {
  auto path = gothicglb::jsonToString(v,name);
  const fs::path p(path);
  require(!path.empty() && !p.is_absolute() && path.find('\0')==std::string::npos &&
          std::none_of(p.begin(),p.end(),[](const fs::path& part) { return part==".."; }),
          std::string("invalid relative ")+name);
  return path;
  }

struct Key {
  std::string projection, kind, sha;
  };

// "<projection>/<kind>/<BIN_SHA>"; false when malformed.
bool splitKey(std::string_view key, Key& out) {
  const auto a = key.find('/');
  if(a==std::string_view::npos)
    return false;
  const auto b = key.find('/',a+1);
  if(b==std::string_view::npos)
    return false;
  out.projection = std::string(key.substr(0,a));
  out.kind       = std::string(key.substr(a+1,b-a-1));
  out.sha        = std::string(key.substr(b+1));
  return (out.projection=="gk" || out.projection=="dx8") && isKind(out.kind) && isHex64(out.sha);
  }

Outcome readOutcome(const Json& v, const char* projection, const std::string& kind,
                    const std::map<std::string,std::string>& records) {
  require(v.IsObject() && v.MemberCount()==1,std::string(projection)+" outcome must have exactly one member");
  Outcome o;
  if(v.HasMember("record")) {
    o.record = gothicglb::jsonToString(v["record"],"record");
    Key k;
    require(splitKey(o.record,k) && k.projection==projection && k.kind==kind,
            "record "+o.record+" does not match "+projection+"/"+kind);
    const auto r = records.find(o.record);
    require(r!=records.end(),"unknown record "+o.record);
    o.identity = r->second;
    return o;
    }
  require(v.HasMember("failure"),std::string(projection)+" outcome needs record or failure");
  const auto& f = v["failure"];
  if(std::string_view(projection)=="gk") {
    exactMembers(f,"gk failure",{"result"});
    o.failure = gothicglb::jsonToString(f["result"],"result");
    // DmResult names without the prefix; only results a parse of a file can end with.
    require(*o.failure=="INVALID_ARGUMENT" || *o.failure=="INVALID_STATE" || *o.failure=="NOT_FOUND" ||
            *o.failure=="FILE_CORRUPT","gk failure must name a failing parse result");
    } else {
    exactMembers(f,"dx8 failure",{"error"});
    o.failure = gothicglb::jsonToString(f["error"],"error");
    }
  return o;
  }

bool equalsAsciiCase(std::string_view a, std::string_view b) {
  if(a.size()!=b.size())
    return false;
  for(size_t i=0; i<a.size(); ++i) {
    char x = a[i], y = b[i];
    if(x>='a' && x<='z')
      x = char(x-'a'+'A');
    if(y>='a' && y<='z')
      y = char(y-'a'+'A');
    if(x!=y)
      return false;
    }
  return true;
  }

std::vector<uint8_t> readWhole(const fs::path& file, const std::function<void(const std::string&)>& fail) {
  std::ifstream f(file,std::ios::binary|std::ios::ate);
  if(!f)
    fail("missing "+file.filename().string());
  const auto end = f.tellg();
  if(end<0)
    fail("read error "+file.filename().string());
  std::vector<uint8_t> out(static_cast<size_t>(end));
  f.seekg(0,std::ios::beg);
  f.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()));
  out.resize(size_t(f.gcount()));
  return out;
  }

// ---------------------------------------------------------------------------------------------------------
// Dx8 codec: little-endian, declaration order, every DMUS_IO_* member by its declared (underlying) type,
// strings as u32 length + owned units, arrays as u32 count + elements, optional tracks as u8 present.

struct Writer {
  std::vector<uint8_t> out;

  void bytes(const void* p, size_t n) {
    auto b = reinterpret_cast<const uint8_t*>(p);
    out.insert(out.end(),b,b+n);
    }
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) { for(int i=0; i<2; ++i) out.push_back(uint8_t(v>>(8*i))); }
  void u32(uint32_t v) { for(int i=0; i<4; ++i) out.push_back(uint8_t(v>>(8*i))); }
  void u64(uint64_t v) { for(int i=0; i<8; ++i) out.push_back(uint8_t(v>>(8*i))); }
  void i16(int16_t v)  { u16(uint16_t(v)); }
  void i32(int32_t v)  { u32(uint32_t(v)); }
  void f64(double v)   { uint64_t u; std::memcpy(&u,&v,sizeof u); u64(u); }
  void count(size_t n) {
    require(n<=UINT32_MAX,"music data: array too large");
    u32(uint32_t(n));
    }
  void str(const std::string& s) { count(s.size()); bytes(s.data(),s.size()); }
  void str(const std::u16string& s) { count(s.size()); for(auto c:s) u16(uint16_t(c)); }
  void blob(const std::vector<uint8_t>& b) { count(b.size()); bytes(b.data(),b.size()); }
  };

struct Reader {
  const std::vector<uint8_t>& in;
  size_t                      at = 0;

  void need(size_t n) {
    require(in.size()-at>=n,"music data: record truncated");
    }
  void bytes(void* p, size_t n) {
    need(n);
    std::memcpy(p,in.data()+at,n);
    at += n;
    }
  uint8_t  u8() { need(1); return in[at++]; }
  uint16_t u16() { uint16_t v=0; need(2); for(int i=0; i<2; ++i) v = uint16_t(v|(uint16_t(in[at++])<<(8*i))); return v; }
  uint32_t u32() { uint32_t v=0; need(4); for(int i=0; i<4; ++i) v |= uint32_t(in[at++])<<(8*i); return v; }
  uint64_t u64() { uint64_t v=0; need(8); for(int i=0; i<8; ++i) v |= uint64_t(in[at++])<<(8*i); return v; }
  int16_t  i16() { return int16_t(u16()); }
  int32_t  i32() { return int32_t(u32()); }
  double   f64() { uint64_t u = u64(); double v; std::memcpy(&v,&u,sizeof v); return v; }
  bool     present() {
    const uint8_t v = u8();
    require(v<=1,"music data: invalid present flag");
    return v==1;
    }
  // An element count whose elements take at least `minSize` bytes each, checked before allocation.
  size_t count(size_t minSize) {
    const uint32_t n = u32();
    require(uint64_t(n)*minSize<=in.size()-at,"music data: count exceeds record");
    return n;
    }
  void str(std::string& s) {
    s.resize(count(1));
    bytes(s.data(),s.size());
    }
  void str(std::u16string& s) {
    s.resize(count(2));
    for(auto& c:s)
      c = char16_t(u16());
    }
  void blob(std::vector<uint8_t>& b) {
    b.resize(count(1));
    bytes(b.data(),b.size());
    }
  void finish() const {
    require(at==in.size(),"music data: trailing bytes in record");
    }
  };

void put(Writer& w, const Dx8::GUID& g) {
  w.u32(g.Data1);
  w.u16(g.Data2);
  w.u16(g.Data3);
  w.u64(g.Data4);
  }
void get(Reader& r, Dx8::GUID& g) {
  g.Data1 = r.u32();
  g.Data2 = r.u16();
  g.Data3 = r.u16();
  g.Data4 = r.u64();
  }

void put(Writer& w, const Dx8::DMUS_IO_VERSION& v) { w.u32(v.dwVersionMS); w.u32(v.dwVersionLS); }
void get(Reader& r, Dx8::DMUS_IO_VERSION& v) { v.dwVersionMS = r.u32(); v.dwVersionLS = r.u32(); }

void put(Writer& w, const Dx8::DMUS_IO_TIMESIG& t) {
  w.u8(t.bBeatsPerMeasure);
  w.u8(t.bBeat);
  w.u16(t.wGridsPerBeat);
  }
void get(Reader& r, Dx8::DMUS_IO_TIMESIG& t) {
  t.bBeatsPerMeasure = r.u8();
  t.bBeat            = r.u8();
  t.wGridsPerBeat    = r.u16();
  }

void put(Writer& w, const Dx8::Unfo& u) { w.str(u.unam); }
void get(Reader& r, Dx8::Unfo& u) { r.str(u.unam); }
void put(Writer& w, const Dx8::Info& i) { w.str(i.inam); }
void get(Reader& r, Dx8::Info& i) { r.str(i.inam); }

void put(Writer& w, const Dx8::Reference& ref) {
  put(w,ref.header.guidClassID);
  put(w,ref.guid);
  w.str(ref.name);
  w.str(ref.file);
  w.str(ref.category);
  put(w,ref.version);
  }
void get(Reader& r, Dx8::Reference& ref) {
  get(r,ref.header.guidClassID);
  get(r,ref.guid);
  r.str(ref.name);
  r.str(ref.file);
  r.str(ref.category);
  get(r,ref.version);
  }

// Segment

void put(Writer& w, const Dx8::Track& t) {
  put(w,t.head.guidClassID);
  w.u32(t.head.dwPosition);
  w.u32(t.head.dwGroup);
  w.bytes(t.head.ckid,sizeof t.head.ckid);
  w.bytes(t.head.fccType,sizeof t.head.fccType);

  w.u8(t.sttr ? 1 : 0);
  if(t.sttr) {
    w.count(t.sttr->styles.size());
    for(auto& s:t.sttr->styles) {
      w.u16(s.stmp);
      put(w,s.reference);
      }
    }
  w.u8(t.cord ? 1 : 0);
  if(t.cord) {
    auto& c = *t.cord;
    w.u32(c.header);
    // Packed struct arrays are indexed: a reference to their elements must not be formed.
    for(size_t k=0; k<16; ++k)
      w.u16(uint16_t(c.ioChord.wszName[k]));
    w.u32(c.ioChord.mtTime);
    w.u16(c.ioChord.wMeasure);
    w.u8(c.ioChord.bBeat);
    w.u8(uint8_t(c.ioChord.bFlags));
    w.count(c.subchord.size());
    for(auto& s:c.subchord) {
      w.u32(s.dwChordPattern);
      w.u32(s.dwScalePattern);
      w.u32(s.dwInversionPoints);
      w.u32(s.dwLevels);
      w.u8(s.bChordRoot);
      w.u8(s.bScaleRoot);
      }
    }
  w.u8(t.cmnd ? 1 : 0);
  if(t.cmnd) {
    w.count(t.cmnd->commands.size());
    for(auto& c:t.cmnd->commands) {
      w.u32(c.mtTime);
      w.u16(c.wMeasure);
      w.u8(c.bBeat);
      w.u8(uint8_t(c.bCommand));
      w.u8(c.bGrooveLevel);
      w.u8(c.bGrooveRange);
      w.u8(c.bRepeatMode);
      }
    }
  }

void get(Reader& r, Dx8::Track& t) {
  get(r,t.head.guidClassID);
  t.head.dwPosition = r.u32();
  t.head.dwGroup    = r.u32();
  r.bytes(t.head.ckid,sizeof t.head.ckid);
  r.bytes(t.head.fccType,sizeof t.head.fccType);

  if(r.present()) {
    t.sttr = std::make_shared<Dx8::Track::StyleTrack>();
    t.sttr->styles.resize(r.count(2+52));
    for(auto& s:t.sttr->styles) {
      s.stmp = r.u16();
      get(r,s.reference);
      }
    }
  if(r.present()) {
    t.cord = std::make_shared<Dx8::Track::Chord>();
    auto& c = *t.cord;
    c.header = r.u32();
    for(size_t k=0; k<16; ++k)
      c.ioChord.wszName[k] = char16_t(r.u16());
    c.ioChord.mtTime   = r.u32();
    c.ioChord.wMeasure = r.u16();
    c.ioChord.bBeat    = r.u8();
    c.ioChord.bFlags   = Dx8::DMUS_CHORDKEYF_FLAGS(r.u8());
    c.subchord.resize(r.count(18));
    for(auto& s:c.subchord) {
      s.dwChordPattern    = r.u32();
      s.dwScalePattern    = r.u32();
      s.dwInversionPoints = r.u32();
      s.dwLevels          = r.u32();
      s.bChordRoot        = r.u8();
      s.bScaleRoot        = r.u8();
      }
    }
  if(r.present()) {
    t.cmnd = std::make_shared<Dx8::Track::CommandTrack>();
    t.cmnd->commands.resize(r.count(11));
    for(auto& c:t.cmnd->commands) {
      c.mtTime       = r.u32();
      c.wMeasure     = r.u16();
      c.bBeat        = r.u8();
      c.bCommand     = Dx8::DMUS_COMMANDT_TYPES(r.u8());
      c.bGrooveLevel = r.u8();
      c.bGrooveRange = r.u8();
      c.bRepeatMode  = r.u8();
      }
    }
  }

// Style

void put(Writer& w, const Dx8::Band& b) {
  put(w,b.guid);
  put(w,b.vers);
  put(w,b.info);
  w.count(b.intrument.size());
  for(auto& i:b.intrument) {
    w.u32(i.header.dwPatch);
    w.u32(i.header.dwAssignPatch);
    for(size_t k=0; k<4; ++k)
      w.u32(i.header.dwNoteRanges[k]);
    w.u32(i.header.dwPChannel);
    w.u32(uint32_t(i.header.dwFlags));
    w.u8(i.header.bPan);
    w.u8(i.header.bVolume);
    put(w,i.reference);
    }
  }

void get(Reader& r, Dx8::Band& b) {
  get(r,b.guid);
  get(r,b.vers);
  get(r,b.info);
  b.intrument.resize(r.count(34+52));
  for(auto& i:b.intrument) {
    i.header.dwPatch       = r.u32();
    i.header.dwAssignPatch = r.u32();
    for(size_t k=0; k<4; ++k)
      i.header.dwNoteRanges[k] = r.u32();
    i.header.dwPChannel = r.u32();
    i.header.dwFlags    = Dx8::DMUS_IO_INST_FLSGS(r.u32());
    i.header.bPan       = r.u8();
    i.header.bVolume    = r.u8();
    get(r,i.reference);
    }
  }

void put(Writer& w, const Dx8::Style::Part& p) {
  put(w,p.header.timeSig);
  for(size_t k=0; k<32; ++k)
    w.u32(p.header.dwVariationChoices[k]);
  put(w,p.header.guidPartID);
  w.u16(p.header.wNbrMeasures);
  w.u8(uint8_t(p.header.bPlayModeFlags));

  w.count(p.notes.size());
  for(auto& n:p.notes) {
    w.u32(n.mtGridStart);
    w.u32(n.dwVariation);
    w.u32(n.mtDuration);
    w.i16(n.nTimeOffset);
    w.u16(n.wMusicValue);
    w.u8(n.bVelocity);
    w.u8(n.bTimeRange);
    w.u8(n.bDurRange);
    w.u8(n.bVelRange);
    w.u8(n.bInversionID);
    w.u8(uint8_t(n.bPlayModeFlags));
    w.u8(uint8_t(n.bNoteFlags));
    }
  w.count(p.curves.size());
  for(auto& c:p.curves) {
    w.u32(c.mtGridStart);
    w.u32(c.dwVariation);
    w.u32(c.mtDuration);
    w.u32(c.mtResetDuration);
    w.i16(c.nTimeOffset);
    w.u16(c.nStartValue);
    w.u16(c.nEndValue);
    w.u16(c.nResetValue);
    w.u8(uint8_t(c.bEventType));
    w.u8(uint8_t(c.bCurveShape));
    w.u8(uint8_t(c.bCCData));
    w.u8(c.bFlags);
    w.u16(c.wParamType);
    w.u16(c.wMergeIndex);
    }
  }

void get(Reader& r, Dx8::Style::Part& p) {
  get(r,p.header.timeSig);
  for(size_t k=0; k<32; ++k)
    p.header.dwVariationChoices[k] = r.u32();
  get(r,p.header.guidPartID);
  p.header.wNbrMeasures   = r.u16();
  p.header.bPlayModeFlags = Dx8::DMUS_PLAYMODE_FLAGS(r.u8());

  p.notes.resize(r.count(23));
  for(auto& n:p.notes) {
    n.mtGridStart    = r.u32();
    n.dwVariation    = r.u32();
    n.mtDuration     = r.u32();
    n.nTimeOffset    = r.i16();
    n.wMusicValue    = r.u16();
    n.bVelocity      = r.u8();
    n.bTimeRange     = r.u8();
    n.bDurRange      = r.u8();
    n.bVelRange      = r.u8();
    n.bInversionID   = r.u8();
    n.bPlayModeFlags = Dx8::DMUS_PLAYMODE_FLAGS(r.u8());
    n.bNoteFlags     = Dx8::DMUS_NOTEF_FLAGS(r.u8());
    }
  p.curves.resize(r.count(32));
  for(auto& c:p.curves) {
    c.mtGridStart     = r.u32();
    c.dwVariation     = r.u32();
    c.mtDuration      = r.u32();
    c.mtResetDuration = r.u32();
    c.nTimeOffset     = r.i16();
    c.nStartValue     = r.u16();
    c.nEndValue       = r.u16();
    c.nResetValue     = r.u16();
    c.bEventType      = Dx8::DMUS_CURVE_TYPE(r.u8());
    c.bCurveShape     = Dx8::Shape(r.u8());
    c.bCCData         = Dx8::Control(r.u8());
    c.bFlags          = r.u8();
    c.wParamType      = r.u16();
    c.wMergeIndex     = r.u16();
    }
  }

void put(Writer& w, const Dx8::Pattern& p) {
  put(w,p.header.timeSig);
  w.u8(p.header.bGrooveBottom);
  w.u8(p.header.bGrooveTop);
  w.u16(uint16_t(p.header.wEmbellishment));
  w.u16(p.header.wNbrMeasures);
  put(w,p.info);
  w.count(p.partref.size());
  for(auto& ref:p.partref) {
    put(w,ref.io.guidPartID);
    w.u16(ref.io.wLogicalPartID);
    put(w,ref.unfo);
    }
  }

void get(Reader& r, Dx8::Pattern& p) {
  get(r,p.header.timeSig);
  p.header.bGrooveBottom  = r.u8();
  p.header.bGrooveTop     = r.u8();
  p.header.wEmbellishment = Dx8::DMUS_EMBELLISHT_TYPES(r.u16());
  p.header.wNbrMeasures   = r.u16();
  get(r,p.info);
  p.partref.resize(r.count(22));
  for(auto& ref:p.partref) {
    get(r,ref.io.guidPartID);
    ref.io.wLogicalPartID = r.u16();
    get(r,ref.unfo);
    }
  }

// DLS

void put(Writer& w, const Dx8::DlsCollection::WaveSample& s) {
  w.u32(s.cbSize);
  w.u16(s.usUnityNote);
  w.i16(s.sFineTune);
  w.i32(s.lAttenuation);
  w.u32(s.fulOptions);
  w.u32(s.cSampleLoops);
  }
void get(Reader& r, Dx8::DlsCollection::WaveSample& s) {
  s.cbSize       = r.u32();
  s.usUnityNote  = r.u16();
  s.sFineTune    = r.i16();
  s.lAttenuation = r.i32();
  s.fulOptions   = r.u32();
  s.cSampleLoops = r.u32();
  }

void put(Writer& w, const Dx8::Wave::WaveSample& s) {
  w.u32(s.cbSize);
  w.u16(s.usUnityNote);
  w.i16(s.sFineTune);
  w.i32(s.lAttenuation);
  w.u32(s.fulOptions);
  w.u32(s.cSampleLoops);
  }
void get(Reader& r, Dx8::Wave::WaveSample& s) {
  s.cbSize       = r.u32();
  s.usUnityNote  = r.u16();
  s.sFineTune    = r.i16();
  s.lAttenuation = r.i32();
  s.fulOptions   = r.u32();
  s.cSampleLoops = r.u32();
  }

template<class Loop>
void putLoops(Writer& w, const std::vector<Loop>& loops) {
  w.count(loops.size());
  for(auto& l:loops) {
    w.u32(l.cbSize);
    w.u32(l.ulLoopType);
    w.u32(l.ulLoopStart);
    w.u32(l.ulLoopLength);
    }
  }
template<class Loop>
void getLoops(Reader& r, std::vector<Loop>& loops) {
  loops.resize(r.count(16));
  for(auto& l:loops) {
    l.cbSize       = r.u32();
    l.ulLoopType   = r.u32();
    l.ulLoopStart  = r.u32();
    l.ulLoopLength = r.u32();
    }
  }

void put(Writer& w, const Dx8::Wave& wv) {
  w.u16(uint16_t(wv.wfmt.wFormatTag));
  w.u16(wv.wfmt.wChannels);
  w.u32(wv.wfmt.dwSamplesPerSec);
  w.u32(wv.wfmt.dwAvgBytesPerSec);
  w.u16(wv.wfmt.wBlockAlign);
  w.u16(wv.wfmt.wBitsPerSample);
  w.blob(wv.extra);
  w.blob(wv.wavedata);
  put(w,wv.waveSample);
  putLoops(w,wv.loop);
  put(w,wv.info);
  }
void get(Reader& r, Dx8::Wave& wv) {
  wv.wfmt.wFormatTag       = Dx8::Wave::WaveFormatTag(r.u16());
  wv.wfmt.wChannels        = r.u16();
  wv.wfmt.dwSamplesPerSec  = r.u32();
  wv.wfmt.dwAvgBytesPerSec = r.u32();
  wv.wfmt.wBlockAlign      = r.u16();
  wv.wfmt.wBitsPerSample   = r.u16();
  r.blob(wv.extra);
  r.blob(wv.wavedata);
  get(r,wv.waveSample);
  getLoops(r,wv.loop);
  get(r,wv.info);
  }

void put(Writer& w, const Dx8::DlsCollection::Instrument& ins) {
  put(w,ins.info);
  w.u32(ins.header.cRegions);
  w.u32(ins.header.Locale.ulBank);
  w.u32(ins.header.Locale.ulInstrument);
  w.count(ins.regions.size());
  for(auto& rg:ins.regions) {
    w.u16(rg.head.RangeKey.usLow);
    w.u16(rg.head.RangeKey.usHigh);
    w.u16(rg.head.RangeVelocity.usLow);
    w.u16(rg.head.RangeVelocity.usHigh);
    w.u16(rg.head.fusOptions);
    w.u16(rg.head.usKeyGroup);
    w.u16(rg.wlink.fusOptions);
    w.u16(rg.wlink.usPhaseGroup);
    w.u32(rg.wlink.ulChannel);
    w.u32(rg.wlink.ulTableIndex);
    put(w,rg.waveSample);
    putLoops(w,rg.loop);
    }
  w.count(ins.articulators.size());
  for(auto& a:ins.articulators) {
    w.count(a.connectionBlocks.size());
    for(auto& c:a.connectionBlocks) {
      w.u16(c.usSource);
      w.u16(c.usControl);
      w.u16(c.usDestination);
      w.u16(c.usTransform);
      w.i32(c.lScale);
      }
    }
  }

void get(Reader& r, Dx8::DlsCollection::Instrument& ins) {
  get(r,ins.info);
  ins.header.cRegions            = r.u32();
  ins.header.Locale.ulBank       = r.u32();
  ins.header.Locale.ulInstrument = r.u32();
  ins.regions.resize(r.count(48));
  for(auto& rg:ins.regions) {
    rg.head.RangeKey.usLow       = r.u16();
    rg.head.RangeKey.usHigh      = r.u16();
    rg.head.RangeVelocity.usLow  = r.u16();
    rg.head.RangeVelocity.usHigh = r.u16();
    rg.head.fusOptions           = r.u16();
    rg.head.usKeyGroup           = r.u16();
    rg.wlink.fusOptions          = r.u16();
    rg.wlink.usPhaseGroup        = r.u16();
    rg.wlink.ulChannel           = r.u32();
    rg.wlink.ulTableIndex        = r.u32();
    get(r,rg.waveSample);
    getLoops(r,rg.loop);
    }
  ins.articulators.resize(r.count(4));
  for(auto& a:ins.articulators) {
    a.connectionBlocks.resize(r.count(12));
    for(auto& c:a.connectionBlocks) {
      c.usSource      = r.u16();
      c.usControl     = r.u16();
      c.usDestination = r.u16();
      c.usTransform   = r.u16();
      c.lScale        = r.i32();
      }
    }
  }

template<class T>
T decodeWith(const std::vector<uint8_t>& bin, const std::function<void(Reader&,T&)>& fn) {
  Reader r{bin};
  T out;
  fn(r,out);
  r.finish();
  return out;
  }
}

const Source* Index::findGk(std::string_view name) const {
  const auto it = gk.find(upperAscii(trimTrailing(name)));
  return it==gk.end() ? nullptr : &sources[it->second];
  }

const Source* Index::findDx8(std::string_view relativePath) const {
  const auto exact = dx8.find(std::string(relativePath));
  if(exact!=dx8.end())
    return &sources[exact->second];
  const Source* found = nullptr;
  for(auto& [path,i]:dx8) {
    if(!equalsAsciiCase(path,relativePath))
      continue;
    if(found!=nullptr)
      throw std::runtime_error("music data: ambiguous host path "+std::string(relativePath));
    found = &sources[i];
    }
  return found;
  }

Index readIndex(const fs::path& dir) {
  const auto file = dir/"music.json";
  try {
    const auto bytes = gothicglb::readFile(file);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
    exactMembers(doc,"index",{"format","version","archives","loose_files","records","sources","gk","dx8"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-music","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");

    Index index;
    index.dir = dir;
    require(doc["archives"].IsArray(),"archives must be an array");
    for(const auto& a:doc["archives"].GetArray()) {
      exactMembers(a,"archive",{"name","size"});
      index.archives.emplace_back(gothicglb::jsonToString(a["name"],"name"),unsignedValue(a["size"],"size"));
      }
    require(doc["loose_files"].IsArray(),"loose_files must be an array");
    for(const auto& l:doc["loose_files"].GetArray()) {
      exactMembers(l,"loose file",{"path","size"});
      index.looseFiles.emplace_back(relativePath(l["path"],"path"),unsignedValue(l["size"],"size"));
      }

    require(doc["records"].IsObject(),"records must be an object");
    for(const auto& e:doc["records"].GetObject()) {
      const auto key = gothicglb::jsonToString(e.name,"record key");
      Key k;
      require(splitKey(key,k),"invalid record key "+key);
      const auto identity = gothicglb::jsonToString(e.value,"record identity");
      require(isHex64(identity),"invalid identity of "+key);
      require(index.records.emplace(key,identity).second,"duplicate record "+key);
      }

    require(doc["sources"].IsArray(),"sources must be an array");
    for(const auto& v:doc["sources"].GetArray()) {
      exactMembers(v,"source",{"origin","path","archive","size","sha256","kind","gk","dx8"});
      Source s;
      s.origin  = gothicglb::jsonToString(v["origin"],"origin");
      s.archive = optionalString(v["archive"],"archive");
      s.size    = unsignedValue(v["size"],"size");
      s.sha256  = gothicglb::jsonToString(v["sha256"],"sha256");
      s.kind    = gothicglb::jsonToString(v["kind"],"kind");
      require(s.origin=="host" || s.origin=="archive","invalid origin");
      require((s.origin=="archive")==!s.archive.empty(),"archive name must match the origin");
      s.path = s.origin=="host" ? relativePath(v["path"],"path") : gothicglb::jsonToString(v["path"],"path");
      require(isHex64(s.sha256),"invalid sha256 of "+s.path);
      require(isKind(s.kind),"invalid kind of "+s.path);
      s.gk  = readOutcome(v["gk"],"gk",s.kind,index.records);
      s.dx8 = readOutcome(v["dx8"],"dx8",s.kind,index.records);
      index.sources.push_back(std::move(s));
      }

    require(doc["gk"].IsObject(),"gk must be an object");
    for(const auto& e:doc["gk"].GetObject()) {
      const auto name = gothicglb::jsonToString(e.name,"gk name");
      require(!name.empty() && name==upperAscii(name),"gk name must be ASCII upper case: "+name);
      const auto i = unsignedValue(e.value,"gk source");
      require(i<index.sources.size(),"gk source out of range: "+name);
      index.gk[name] = size_t(i);
      }
    require(doc["dx8"].IsObject(),"dx8 must be an object");
    for(const auto& e:doc["dx8"].GetObject()) {
      const auto path = relativePath(e.name,"dx8 path");
      const auto i = unsignedValue(e.value,"dx8 source");
      require(i<index.sources.size(),"dx8 source out of range: "+path);
      require(index.sources[size_t(i)].origin=="host" && index.sources[size_t(i)].path==path,
              "dx8 path must name its host source: "+path);
      require(index.dx8.emplace(path,size_t(i)).second,"duplicate dx8 path "+path);
      }
    return index;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("music data: "+file.string()+": "+e.what());
    }
  }

std::vector<uint8_t> readRecord(const Index& index, std::string_view key) {
  const auto fail = [&](const std::string& reason) {
    throw std::runtime_error("music data: "+std::string(key)+": "+reason);
    };
  Key k;
  if(!splitKey(key,k))
    fail("invalid record key");
  const auto id = index.records.find(std::string(key));
  if(id==index.records.end())
    fail("not in the index");

  const auto base = index.dir/k.projection/k.kind;
  const auto json = readWhole(base/(k.sha+".json"),fail);
  auto       bin  = readWhole(base/(k.sha+".bin"),fail);

  // The writer's exact single-line JSON: any other bytes (member order, spacing, values) are rejected.
  const std::string expected = std::string("{\"format\":\"gothic-music-record\",\"version\":1,\"projection\":\"")+
                               k.projection+"\",\"kind\":\""+k.kind+"\",\"buffer\":\""+k.sha+".bin\",\"byte_length\":"+
                               std::to_string(bin.size())+"}";
  if(std::string_view(reinterpret_cast<const char*>(json.data()),json.size())!=expected)
    fail("record JSON does not match its key and buffer");
  if(Sha256::hash(std::string_view(reinterpret_cast<const char*>(bin.data()),bin.size()))!=k.sha)
    fail("buffer sha256 differs");
  std::string whole(reinterpret_cast<const char*>(json.data()),json.size());
  whole.append(reinterpret_cast<const char*>(bin.data()),bin.size());
  if(Sha256::hash(whole)!=id->second)
    fail("record identity differs");
  return bin;
  }

std::vector<uint8_t> encodeDx8(const Dx8::Segment& segment) {
  Writer w;
  w.count(segment.track.size());
  for(auto& t:segment.track)
    put(w,t);
  return std::move(w.out);
  }

std::vector<uint8_t> encodeDx8(const Dx8::Style& style) {
  Writer w;
  put(w,style.guid);
  put(w,style.styh.timeSig);
  w.f64(style.styh.dblTempo);
  w.count(style.band.size());
  for(auto& b:style.band)
    put(w,b);
  w.count(style.parts.size());
  for(auto& p:style.parts)
    put(w,p);
  w.count(style.patterns.size());
  for(auto& p:style.patterns)
    put(w,p);
  return std::move(w.out);
  }

std::vector<uint8_t> encodeDx8(const Dx8::DlsCollection::Parsed& dls) {
  Writer w;
  w.u64(dls.version);
  put(w,dls.dlid);
  w.count(dls.instrument.size());
  for(auto& i:dls.instrument)
    put(w,i);
  w.count(dls.wave.size());
  for(auto& wv:dls.wave)
    put(w,wv);
  return std::move(w.out);
  }

Dx8::Segment decodeDx8Segment(const std::vector<uint8_t>& bin) {
  return decodeWith<Dx8::Segment>(bin,[](Reader& r, Dx8::Segment& s) {
    s.track.resize(r.count(33));
    for(auto& t:s.track)
      get(r,t);
    });
  }

Dx8::Style decodeDx8Style(const std::vector<uint8_t>& bin) {
  return decodeWith<Dx8::Style>(bin,[](Reader& r, Dx8::Style& s) {
    get(r,s.guid);
    get(r,s.styh.timeSig);
    s.styh.dblTempo = r.f64();
    s.band.resize(r.count(32));
    for(auto& b:s.band)
      get(r,b);
    s.parts.resize(r.count(159));
    for(auto& p:s.parts)
      get(r,p);
    s.patterns.resize(r.count(18));
    for(auto& p:s.patterns)
      get(r,p);
    });
  }

Dx8::DlsCollection::Parsed decodeDx8Dls(const std::vector<uint8_t>& bin) {
  return decodeWith<Dx8::DlsCollection::Parsed>(bin,[](Reader& r, Dx8::DlsCollection::Parsed& d) {
    d.version = r.u64();
    get(r,d.dlid);
    d.instrument.resize(r.count(24));
    for(auto& i:d.instrument)
      get(r,i);
    d.wave.resize(r.count(52));
    for(auto& wv:d.wave)
      get(r,wv);
    });
  }

}

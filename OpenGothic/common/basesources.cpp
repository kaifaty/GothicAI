// P10 source manifest: which original inputs of the not yet migrated families the
// engine and the production tools select, from where, and their content identity.
// Import evidence only: nothing here converts a payload or changes a loader.
#include "baseimport.h"
#include "basemusic.h"
#include "gothicglb.h"
#include "audiodata.h"
#include "musicdata.h"
#include "musicnative.h"
#include "outputunits.h"
#include "programdata.h"
#include "asset/fromzenkit.h"
#include "sha256.h"
#include "dmusic/segment.h"
#include "dmusic/sourcefile.h"
#include "dmusic/style.h"
#include "utils/fileutil.h"

#include <Tempest/Dir>
#include <Tempest/File>

#include <zenkit/Archive.hh>
#include <zenkit/CutsceneLibrary.hh>
#include <zenkit/DaedalusScript.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace fs = std::filesystem;

namespace BaseImport {
namespace {

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

std::string upper(std::string s) {
  for(auto& c:s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
  }

std::string extension(const std::string& name) {
  const auto dot = name.find_last_of('.');
  return dot==std::string::npos ? std::string() : upper(name.substr(dot));
  }

std::string str(const std::string& s) {
  return gothicglb::jsonString(s);
  }

std::string hex(const uint8_t (&bytes)[16]) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  for(const auto b:bytes) {
    s += digits[b>>4];
    s += digits[b&15];
    }
  return s;
  }

using Tempest::Dir;

// CommandLine's form of the installation path: forward slashes and a trailing '/'.
std::u16string installation(const fs::path& gothic) {
  auto path = gothic.u16string();
  for(auto& c:path)
    if(c==u'\\')
      c = u'/';
  if(!path.empty() && path.back()!=u'/')
    path.push_back(u'/');
  return path;
  }

// Gothic::loadScript and GameScript::loadDialogOU widen each request byte to char16_t.
std::u16string widen(const std::string& s) {
  std::u16string out;
  for(const char c:s)
    out.push_back(char16_t(c));
  return out;
  }

// UTF-16 code units as JSON: exact, whatever the spelling.
std::string str16(const std::u16string& s) {
  static const char digits[] = "0123456789abcdef";
  std::string out = "\"";
  for(const char16_t c:s) {
    if(c>=0x20 && c<0x7F && c!=u'"' && c!=u'\\') {
      out.push_back(char(c));
      continue;
      }
    out += "\\u";
    for(int shift=12; shift>=0; shift-=4)
      out.push_back(digits[(c>>shift)&15]);
    }
  return out+"\"";
  }

const std::array<const char16_t*,8> scriptLanguages = {u"Scripts_EN",u"Scripts_DE",u"Scripts_PL",u"Scripts_RU",
                                                       u"Scripts_FR",u"Scripts_ES",u"Scripts_IT",u"Scripts_CZ"};
const std::array<const char*,8> languageNames = {"EN","DE","PL","RU","FR","ES","IT","CZ"};

// Full virtual paths without a leading slash, as the game census writes them.
void collectPaths(const zenkit::VfsNode& node, const std::string& prefix,
                  std::map<const zenkit::VfsNode*,std::string>& out) {
  for(const auto& child:node.children()) {
    const auto path = prefix.empty() ? child.name() : prefix+"/"+child.name();
    if(child.type()==zenkit::VfsNodeType::DIRECTORY)
      collectPaths(child,path,out);
    else
      out[&child] = path;
    }
  }

struct Digest {
  uint64_t    size = 0;
  std::string sha256;
  std::vector<uint8_t> head; // first bytes, for header facts
  };

Digest digest(zenkit::Read& in, std::vector<uint8_t>* bytes = nullptr) {
  Digest d;
  Sha256 sha;
  uint8_t buf[1<<16];
  for(;;) {
    const auto n = in.read(buf,sizeof(buf));
    if(n==0)
      break;
    if(d.head.empty())
      d.head.assign(buf,buf+n);
    if(bytes!=nullptr)
      bytes->insert(bytes->end(),buf,buf+n);
    sha.update(buf,n);
    d.size += n;
    }
  d.sha256 = sha.finish();
  return d;
  }

uint32_t le32(const std::vector<uint8_t>& b, size_t o) {
  return uint32_t(b[o]) | uint32_t(b[o+1])<<8 | uint32_t(b[o+2])<<16 | uint32_t(b[o+3])<<24;
  }

uint16_t le16(const std::vector<uint8_t>& b, size_t o) {
  return uint16_t(b[o] | b[o+1]<<8);
  }

// Header facts only: the extension does not determine the codec.
std::string audioHeader(const std::vector<uint8_t>& b) {
  if(b.size()>=4 && std::memcmp(b.data(),"OggS",4)==0)
    return "{\"container\":\"ogg\"}";
  if(b.size()<12 || std::memcmp(b.data(),"RIFF",4)!=0 || std::memcmp(b.data()+8,"WAVE",4)!=0)
    return "{\"container\":\"unknown\"}";
  for(size_t o=12; o+8<=b.size();) {
    const auto size = le32(b,o+4);
    if(std::memcmp(b.data()+o,"fmt ",4)==0) {
      if(o+8+16>b.size())
        break;
      return "{\"container\":\"wav\",\"format_tag\":"+std::to_string(le16(b,o+8))+
             ",\"channels\":"+std::to_string(le16(b,o+10))+
             ",\"sample_rate\":"+std::to_string(le32(b,o+12))+
             ",\"bits_per_sample\":"+std::to_string(le16(b,o+22))+"}";
      }
    o += 8+size+(size&1);
    }
  return "{\"container\":\"wav\",\"format_tag\":null}";
  }

class Manifest {
  public:
    explicit Manifest(const fs::path& root)
      : root16(installation(fs::absolute(root))), gothic(fs::path(root16).parent_path()) {
      vfs.set_open_observer([this](std::string_view, std::string_view source) { opened = source; });
      archives = mountRuntime(vfs,gothic,work);
      collectPaths(vfs.root(),"",paths);
      }

    std::string json();

  private:
    struct Choice {
      std::string step, path, source;
      fs::path    host; // absolute, when a loose file was selected
      Digest      digest;
      };

    struct Attempt {
      std::string request;
      std::optional<Choice> found;
      };

    struct Trace {
      Manifest*                       self = nullptr;
      std::vector<Attempt>            attempts;
      std::vector<BaseMusicReference> references;
      std::vector<std::string>        strings; // owned copies of file and name per reference
      std::vector<bool>               hasFile;
      };

    // Gothic::nestedPath: the game's case-insensitive path below the installation.
    std::u16string nested(const std::initializer_list<const char16_t*>& segments, Dir::FileType type) const {
      return FileUtil::nestedPath(root16,segments,type);
      }

    // Every path here starts with the installation exactly as spelled (it may contain
    // ".." or symlinks the I/O keeps): strip that prefix first, then normalize only the rest.
    std::string relative(const fs::path& p) const {
      return p.lexically_relative(gothic).lexically_normal().generic_string();
      }

    std::string sourceName(std::string_view source) const {
      const fs::path p(source);
      return p.is_absolute() ? relative(p) : upper(std::string(source));
      }

    // Opens through the VFS so the open observer reports the winning archive or host file.
    Choice openNode(const zenkit::VfsNode* node, const std::string& step, std::vector<uint8_t>* bytes = nullptr) {
      Choice c;
      c.step = step;
      c.path = paths.at(node);
      opened.clear();
      auto in = node->open_read();
      c.source = sourceName(opened);
      if(fs::path(opened).is_absolute())
        c.host = fs::path(opened);
      c.digest = digest(*in,bytes);
      return c;
      }

    Choice openHost(const fs::path& path, const std::string& step) {
      Choice c;
      c.step = step;
      c.path = relative(path);
      c.source = c.path;
      c.host = path;
      const auto key = path.lexically_normal().string();
      if(auto it=hostDigests.find(key); it!=hostDigests.end()) {
        c.digest = it->second;
        return c;
        }
      auto in = zenkit::Read::from(path);
      require(in!=nullptr,"cannot open "+path.string());
      c.digest = digest(*in);
      hostDigests[key] = c.digest;
      return c;
      }

    static std::string choiceJson(const Choice& c) {
      return "{\"step\":"+str(c.step)+",\"path\":"+str(c.path)+",\"source\":"+str(c.source)+
             ",\"size\":"+std::to_string(c.digest.size)+",\"sha256\":\""+c.digest.sha256+"\"}";
      }

    static void* resolveMusic(void* ctx, char const* file, size_t* len);
    static void  visitMusic(void* ctx, BaseMusicReference const* reference);

    std::string archivesJson() const;
    std::string filesJson();
    std::string musicJson();
    // `closed`: the root's traversal reached every reference it declares.
    std::string gothicKitJson(const std::string& root, bool& closed);
    std::string dx8Json(const std::vector<std::u16string>& dirs, const std::u16string& root, bool& closed);
    std::string scriptsJson();
    std::string outputUnitsJson();
    std::string serverJson();

    std::u16string                               root16;
    fs::path                                     gothic, work;
    zenkit::Vfs                                  vfs;
    std::string                                  opened;
    std::vector<RuntimeArchive>                  archives;
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::map<std::string,Digest>                 hostDigests;
    // Host files of the default profile's GOTHIC.DAT and OU choices, for comparison with the
    // server readers; empty when the winner came from an archive.
    fs::path                                     engineGameDat, engineOu;
  };

std::string Manifest::archivesJson() const {
  std::string s = "[";
  for(size_t i=0; i<archives.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\":"+str(archives[i].name)+",\"size\":"+std::to_string(archives[i].size)+
         ",\"timestamp\":"+std::to_string(archives[i].timestamp)+"}";
  s += "\n  ],\n  \"order_ties\":[";
  bool first = true;
  for(size_t i=0; i<archives.size();) {
    size_t j = i+1;
    while(j<archives.size() && archives[j].timestamp==archives[i].timestamp)
      ++j;
    if(j-i>1) {
      s += std::string(first ? "" : ",")+"\n    [";
      for(size_t k=i; k<j; ++k)
        s += std::string(k==i ? "" : ",")+str(archives[k].name);
      s += "]";
      first = false;
      }
    i = j;
    }
  return s+"\n  ]";
  }

// FNT, audio and BIK: the engine asks Vfs::find for a basename.
std::string Manifest::filesJson() {
  static const std::set<std::string> kinds = {".FNT",".WAV",".OGG",".BIK"};
  struct Copy {
    std::string source;
    int64_t     time = -1; // archives only
    };
  // Every copy of each virtual path, from each source mounted alone.
  std::map<std::string,std::vector<Copy>> copies;
  for(const auto& a:archives) {
    zenkit::Vfs single;
    single.mount_disk(a.path,zenkit::VfsOverwriteBehavior::OLDER);
    std::map<const zenkit::VfsNode*,std::string> own;
    collectPaths(single.root(),"",own);
    for(const auto& [node,path]:own)
      if(kinds.count(extension(node->name())))
        copies[upper(path)].push_back({a.name,a.timestamp});
    }
  if(fs::is_directory(work))
    for(const auto& e:fs::recursive_directory_iterator(work))
      if(e.is_regular_file() && kinds.count(extension(e.path().filename().string())))
        copies[upper("_work/"+e.path().lexically_relative(work).generic_string())].push_back({relative(e.path()),-1});

  std::map<std::string,std::vector<const zenkit::VfsNode*>> requests;
  for(const auto& [node,path]:paths)
    if(kinds.count(extension(node->name())))
      requests[upper(node->name())].push_back(node);

  std::string s = "[";
  size_t count = 0, collisions = 0, shadowed = 0, dependent = 0;
  for(auto& [request,nodes]:requests) {
    const auto* winner = vfs.find(request);
    require(winner!=nullptr,"Vfs::find does not resolve "+request);
    const auto c = openNode(winner,"vfs");
    const auto ext = extension(request);
    std::string e = "\n    {\"request\":"+str(request)+",\"path\":"+str(c.path)+",\"source\":"+str(c.source)+
                    ",\"size\":"+std::to_string(c.digest.size)+",\"sha256\":\""+c.digest.sha256+"\"";
    if(ext==".WAV" || ext==".OGG")
      e += ",\"audio\":"+audioHeader(c.digest.head);
    std::sort(nodes.begin(),nodes.end(),[&](auto* a, auto* b) { return upper(paths.at(a))<upper(paths.at(b)); });
    const auto& winnerCopies = copies[upper(c.path)];
    if(nodes.size()>1 || winnerCopies.size()>1) {
      if(nodes.size()>1) ++collisions;
      if(winnerCopies.size()>1) ++shadowed;
      e += ",\"candidates\":[";
      for(size_t i=0; i<nodes.size(); ++i) {
        const auto& path = paths.at(nodes[i]);
        e += std::string(i ? "," : "")+"{\"path\":"+str(path)+",\"selected\":"+(nodes[i]==winner ? "true" : "false")+",\"copies\":[";
        const auto& list = copies[upper(path)];
        for(size_t k=0; k<list.size(); ++k)
          e += std::string(k ? "," : "")+str(list[k].source);
        e += "]}";
        }
      e += "]";
      int64_t winnerTime = -1;
      size_t  atTime = 0;
      for(const auto& copy:winnerCopies)
        if(copy.source==c.source) winnerTime = copy.time;
      for(const auto& copy:winnerCopies)
        if(copy.time>=0 && copy.time==winnerTime) ++atTime;
      if(atTime>1) {
        e += ",\"order_dependent\":true";
        ++dependent;
        }
      }
    s += std::string(count++ ? "," : "")+e+"}";
    }
  std::cout << "sources: " << count << " FNT/audio/BIK requests, " << collisions << " basename collisions, "
            << shadowed << " shadowed paths, " << dependent << " mount-order dependent\n";
  return s+"\n  ]";
  }

// The game's GothicKit resolver: Vfs::find of the exact requested name, whole file in
// malloc'd memory that dmusic frees.
void* Manifest::resolveMusic(void* ctx, char const* file, size_t* len) {
  auto& trace = *static_cast<Trace*>(ctx);
  Attempt attempt;
  attempt.request = file;
  const auto* node = trace.self->vfs.find(file);
  if(node==nullptr) {
    trace.attempts.push_back(std::move(attempt));
    return nullptr;
    }
  std::vector<uint8_t> bytes;
  attempt.found = trace.self->openNode(node,"vfs",&bytes);
  trace.attempts.push_back(std::move(attempt));
  void* out = std::malloc(bytes.empty() ? 1 : bytes.size());
  if(out==nullptr)
    return nullptr;
  std::memcpy(out,bytes.data(),bytes.size());
  *len = bytes.size();
  return out;
  }

void Manifest::visitMusic(void* ctx, BaseMusicReference const* reference) {
  auto& trace = *static_cast<Trace*>(ctx);
  auto r = *reference;
  // The segment is released after the trace: keep the strings.
  trace.strings.emplace_back(r.file ? r.file : "");
  trace.strings.emplace_back(r.name ? r.name : "");
  trace.hasFile.push_back(r.file!=nullptr);
  r.file = nullptr;
  r.name = nullptr;
  trace.references.push_back(r);
  }

std::string Manifest::gothicKitJson(const std::string& root, bool& closed) {
  Trace trace;
  trace.self = this;
  int loaded = 0;
  const int result = BaseMusic_trace(root.c_str(),&Manifest::resolveMusic,&trace,&Manifest::visitMusic,&trace,&loaded);
  // DmSegment_download stops at the first failed style or band, so the references after it
  // were never opened: only a successful download is a complete traversal.
  closed = loaded && result==0;
  std::string s = "\n      {\"root\":"+str(root)+",\"result\":"+std::to_string(result)+",\"loaded\":"+(loaded ? "true" : "false")+
                  ",\"dependency_closure\":"+(closed ? "true" : "false")+",\"attempts\":[";
  for(size_t i=0; i<trace.attempts.size(); ++i) {
    const auto& a = trace.attempts[i];
    s += std::string(i ? "," : "")+"{\"request\":"+str(a.request)+",\"found\":"+(a.found ? choiceJson(*a.found) : std::string("null"))+"}";
    }
  s += "],\"references\":[";
  // Objects come from the loader's GUID caches: later references reuse the first load.
  std::map<const void*,std::pair<size_t,std::string>> objects; // id, file of the first reference
  for(size_t i=0; i<trace.references.size(); ++i) {
    const auto& r = trace.references[i];
    const auto& file = trace.strings[2*i];
    const bool hasFile = trace.hasFile[i];
    bool requested = false;
    for(const auto& a:trace.attempts)
      requested = requested || (hasFile && a.request==file);
    std::string e = std::string("{\"kind\":")+(r.style ? "\"style\"" : "\"instrument\"")+",\"message\":"+std::to_string(r.message);
    if(!r.style)
      e += std::string(",\"band\":")+(r.band==SIZE_MAX ? std::string("\"segment\"") : std::to_string(r.band))+
           ",\"instrument\":"+std::to_string(r.instrument)+",\"options\":"+std::to_string(r.options)+
           ",\"downloadable\":"+(r.downloadable ? "true" : "false");
    e += ",\"file\":"+(hasFile ? str(file) : std::string("null"))+",\"name\":"+str(trace.strings[2*i+1])+
         ",\"guid\":\""+hex(r.guid)+"\",\"name_requested\":"+(requested ? "true" : "false");
    if(r.object!=nullptr) {
      auto [it,inserted] = objects.emplace(r.object,std::make_pair(objects.size(),file));
      const bool alias = !inserted && it->second.second!=file;
      e += ",\"object\":{\"id\":"+std::to_string(it->second.first)+",\"guid\":\""+hex(r.object_guid)+
           "\",\"first_file\":"+str(it->second.second)+"}";
      if(alias || std::memcmp(r.guid,r.object_guid,16)!=0)
        e += ",\"cache_alias\":true";
      }
    else {
      e += ",\"object\":null";
      }
    s += std::string(i ? "," : "")+e+"}";
    }
  return s+"]}";
  }

std::string Manifest::musicJson() {
  static const std::set<std::string> kinds = {".SGT",".STY",".DLS"};
  std::string s = "{\n    \"corpus\":[";
  std::map<std::string,const zenkit::VfsNode*> roots;
  // Serialize by full virtual path, not by node address.
  std::map<std::string,const zenkit::VfsNode*> music;
  for(const auto& [node,path]:paths)
    if(kinds.count(extension(node->name())))
      music.emplace(path,node);
  size_t count = 0;
  for(const auto& [path,node]:music) {
    s += std::string(count++ ? "," : "")+"\n      "+choiceJson(openNode(node,"corpus"));
    if(extension(node->name())==".SGT")
      roots.emplace(upper(node->name()),node);
    }
  // Every selected segment is a root: full file coverage, not reachability from a theme.
  // Each root gets a cold loader, so its edges do not depend on other roots' caches.
  s += "\n    ],\n    \"gothickit\":{\"roots\":[";
  size_t traced = 0;
  bool gothicKitClosed = true;
  for(const auto& [key,any]:roots) {
    const auto* winner = vfs.find(any->name());
    require(winner!=nullptr,"Vfs::find does not resolve "+key);
    bool closed = false;
    s += std::string(traced++ ? "," : "")+gothicKitJson(winner->name(),closed);
    gothicKitClosed = gothicKitClosed && closed;
    }
  s += std::string("\n    ],\"dependency_closure\":")+(gothicKitClosed ? "true" : "false")+"},\n";

  // Resources: the Dx8::DirectMusic search paths, in order.
  const std::vector<std::u16string> dirs = {
    nested({u"_work",u"Data",u"Music",u"newworld"},  Dir::FT_Dir),
    nested({u"_work",u"Data",u"Music",u"AddonWorld"},Dir::FT_Dir),
    nested({u"_work",u"Data",u"Music",u"dungeon"},   Dir::FT_Dir),
    nested({u"_work",u"Data",u"Music",u"menu_men"},  Dir::FT_Dir),
    nested({u"_work",u"Data",u"Music",u"orchestra"}, Dir::FT_Dir),
    nested({u"_work",u"Data",u"Music"},              Dir::FT_Dir),
    };
  // Roots: every segment file in a search directory, by its own spelling.
  std::set<std::u16string> dx8Roots;
  s += "    \"dx8\":{\"search\":[";
  for(size_t i=0; i<dirs.size(); ++i) {
    s += std::string(i ? "," : "")+"{\"path\":"+str(relative(fs::path(dirs[i])))+",\"exists\":"+
         (FileUtil::exists(dirs[i]) ? "true" : "false")+"}";
    Dir::scan(dirs[i],[&dx8Roots](const std::u16string& name, Dir::FileType t) {
      if(t!=Dir::FT_File || name.size()<4)
        return;
      auto ext = name.substr(name.size()-4);
      for(auto& c:ext)
        if(u'A'<=c && c<=u'Z')
          c = char16_t(c-u'A'+u'a');
      if(ext==u".sgt")
        dx8Roots.insert(name);
      });
    }
  s += "],\"roots\":[";
  size_t dx8Count = 0;
  bool dx8Closed = true;
  for(const auto& root:dx8Roots) {
    bool closed = false;
    s += std::string(dx8Count++ ? "," : "")+dx8Json(dirs,root,closed);
    dx8Closed = dx8Closed && closed;
    }
  return s+"\n    ],\"dependency_closure\":"+(dx8Closed ? "true" : "false")+"}\n  }";
  }

// Dx8::DirectMusic::load and the PatternList constructor for one root with cold caches:
// the segment, then for every style reference the style (cached by its exact file string)
// and every non-empty instrument file of its bands (DLS, cached the same way). Files are
// found by the game's own search; DLS files are opened and hashed, not parsed.
std::string Manifest::dx8Json(const std::vector<std::u16string>& dirs, const std::u16string& root, bool& closed) {
  const auto open = [&](const std::u16string& file, std::vector<uint8_t>* bytes) {
    std::u16string opened;
    auto fin = Dx8::openSourceFile(dirs,file.c_str(),opened);
    std::vector<uint8_t> data(fin.size());
    (void)fin.read(data.data(),data.size());
    Choice c;
    c.step = "dx8";
    c.host = fs::path(opened);
    c.path = relative(c.host);
    c.source = c.path;
    Sha256 sha;
    sha.update(data.data(),data.size());
    c.digest.size = data.size();
    c.digest.sha256 = sha.finish();
    if(bytes!=nullptr)
      *bytes = std::move(data);
    return c;
    };
  std::vector<std::string> events;
  size_t failure = SIZE_MAX; // first event at which the game's load throws
  const auto event = [&](const std::string& where, const std::u16string& request, const std::string& result, bool failed) {
    if(failed && failure==SIZE_MAX)
      failure = events.size();
    events.push_back("{"+where+",\"request\":"+str16(request)+","+result+"}");
    };
  const auto error = [](const std::exception& e) { return "\"error\":"+str(e.what()); };

  std::map<std::u16string,std::shared_ptr<Dx8::Style>> styles;
  std::map<std::u16string,Choice> dls;
  bool unopenedStyle = false; // its bands, and so their DLS references, stay unknown
  size_t emptyFiles = 0;
  std::vector<uint8_t> data;
  std::optional<Dx8::Segment> segment;
  try {
    const auto c = open(root,&data);
    try {
      Dx8::Riff riff(data.data(),data.size());
      segment.emplace(riff);
      event("\"kind\":\"segment\"",root,"\"found\":"+choiceJson(c),false);
      }
    catch(const std::exception& e) { event("\"kind\":\"segment\"",root,"\"found\":"+choiceJson(c)+","+error(e),true); }
    }
  catch(const std::exception& e) { event("\"kind\":\"segment\"",root,error(e),true); }

  if(segment) {
    for(size_t t=0; t<segment->track.size(); ++t) {
      const auto& track = segment->track[t];
      if(track.sttr==nullptr)
        continue;
      for(size_t k=0; k<track.sttr->styles.size(); ++k) {
        const auto& ref = track.sttr->styles[k].reference;
        const auto where = "\"kind\":\"style\",\"track\":"+std::to_string(t)+",\"style\":"+std::to_string(k);
        std::shared_ptr<Dx8::Style> style;
        if(auto it=styles.find(ref.file); it!=styles.end()) {
          style = it->second;
          event(where,ref.file,"\"cache\":true",false);
          }
        else {
          try {
            std::vector<uint8_t> bytes;
            const auto c = open(ref.file,&bytes);
            try {
              Dx8::Riff riff{bytes.data(),bytes.size()};
              style = std::make_shared<Dx8::Style>(riff);
              styles[ref.file] = style;
              event(where,ref.file,"\"found\":"+choiceJson(c),false);
              }
            catch(const std::exception& e) {
              unopenedStyle = true;
              event(where,ref.file,"\"found\":"+choiceJson(c)+","+error(e),true);
              }
            }
          catch(const std::exception& e) {
            unopenedStyle = true;
            event(where,ref.file,error(e),true);
            }
          }
        if(style==nullptr)
          continue;
        for(size_t b=0; b<style->band.size(); ++b)
          for(size_t i=0; i<style->band[b].intrument.size(); ++i) {
            const auto& file = style->band[b].intrument[i].reference.file;
            if(file.empty()) {
              ++emptyFiles;
              continue;
              }
            const auto at = where+",\"band\":"+std::to_string(b)+",\"instrument\":"+std::to_string(i);
            const auto dlsWhere = "\"kind\":\"dls\""+at.substr(std::string("\"kind\":\"style\"").size());
            if(dls.count(file)) {
              event(dlsWhere,file,"\"cache\":true",false);
              continue;
              }
            try {
              const auto c = open(file,nullptr);
              dls.emplace(file,c);
              event(dlsWhere,file,"\"found\":"+choiceJson(c),false);
              }
            catch(const std::exception& e) { event(dlsWhere,file,error(e),true); }
            }
        }
      }
    }
  // A missing DLS is a known leaf; a segment or style that did not parse hides its children.
  closed = segment.has_value() && !unopenedStyle;
  std::string s = "\n      {\"root\":"+str16(root)+",\"dependency_closure\":"+(closed ? "true" : "false")+",\"engine_failure\":"+
                  (failure==SIZE_MAX ? std::string("null") : std::to_string(failure))+
                  ",\"empty_instrument_files\":"+std::to_string(emptyFiles)+",\"events\":[";
  for(size_t i=0; i<events.size(); ++i)
    s += std::string(i ? "," : "")+events[i];
  return s+"]}";
  }

std::string Manifest::scriptsJson() {
  struct Request {
    const char* name;
    bool        localized;
    bool        game;
    };
  // Gothic::createPhoenixVm callers, spelled as they are; GOTHIC.DAT is the default game DAT.
  const std::array<Request,8> requests = {{
    {"Camera.dat",false,false},{"Fight.dat",false,false},{"Sfx.dat",false,false},{"Music.dat",false,false},
    {"ParticleFx.dat",false,false},{"VisualFx.dat",false,false},{"MENU.DAT",true,false},{"GOTHIC.DAT",true,true},
    }};
  // CommandLine::scriptPath() and scriptPath(lang).
  const auto scripts = nested({u"_work",u"Data",u"Scripts",u"_compiled"},Dir::FT_Dir);
  std::string s = "[";
  size_t count = 0;
  for(const auto& r:requests) {
    const std::string name = r.name;
    const auto segment = name.find_last_of("\\/");
    const auto base = segment==std::string::npos ? name : name.substr(segment+1);
    std::unique_ptr<zenkit::Read> program;
    std::string chosen = "null", localized;
    // Gothic::loadScript resolution order.
    if(const auto* node = vfs.find(name)) {
      const auto c = openNode(node,"vfs");
      chosen = choiceJson(c);
      program = node->open_read();
      if(r.game) engineGameDat = c.host;
      }
    else if(const auto* node = segment==std::string::npos ? nullptr : vfs.find(base)) {
      const auto c = openNode(node,"vfs-basename");
      chosen = choiceJson(c);
      program = node->open_read();
      if(r.game) engineGameDat = c.host;
      }
    else if(const auto file = FileUtil::caseInsensitiveSegment(scripts,widen(name).c_str(),Dir::FT_File);
            FileUtil::exists(file)) {
      chosen = choiceJson(openHost(fs::path(file),"scripts"));
      program = zenkit::Read::from(fs::path(file));
      if(r.game) engineGameDat = fs::path(file);
      }
    else if(r.localized) {
      localized = ",\"localized\":{";
      for(size_t i=0; i<scriptLanguages.size(); ++i) {
        const auto dir = nested({u"_work",u"Data",scriptLanguages[i],u"_compiled"},Dir::FT_Dir);
        auto file = FileUtil::caseInsensitiveSegment(dir,widen(name).c_str(),Dir::FT_File);
        if(!FileUtil::exists(file))
          file = FileUtil::caseInsensitiveSegment(dir,widen(base).c_str(),Dir::FT_File);
        localized += std::string(i ? "," : "")+"\""+languageNames[i]+"\":"+
                     (FileUtil::exists(file) ? choiceJson(openHost(fs::path(file),"localized")) : std::string("null"));
        }
      localized += "}";
      }
    require(chosen!="null" || localized.find("\"step\"")!=std::string::npos,"script not found: "+name);
    std::string declared;
    if(program!=nullptr) {
      // Parsing only: no VM is created and no instance initializer runs.
      zenkit::DaedalusScript script;
      script.load(program.get());
      declared = ",\"symbols\":"+std::to_string(script.symbols().size())+(r.game ? ",\"declared_counts\":{" : ",\"declared\":{");
      std::map<std::string,std::vector<std::string>> classes;
      for(const auto& sym:script.symbols())
        if(sym.type()==zenkit::DaedalusDataType::CLASS)
          classes[sym.name()];
      for(auto& [cls,instances]:classes)
        script.enumerate_instances_by_class_name(cls,[&instances](zenkit::DaedalusSymbol& sym) { instances.push_back(sym.name()); });
      bool first = true;
      for(const auto& [cls,instances]:classes) {
        if(instances.empty())
          continue;
        declared += std::string(first ? "" : ",")+str(cls)+":";
        first = false;
        if(r.game) {
          declared += std::to_string(instances.size());
          continue;
          }
        declared += "[";
        for(size_t i=0; i<instances.size(); ++i)
          declared += std::string(i ? "," : "")+str(instances[i]);
        declared += "]";
        }
      declared += "}";
      }
    s += std::string(count++ ? "," : "")+"\n    {\"request\":"+str(name)+",\"language\":"+(r.localized ? "\"setting\"" : "\"none\"")+
         ",\"selected\":"+chosen+localized+declared+"}";
    }
  return s+"\n  ]";
  }

// GameScript::loadDialogOU with the default OUTPUTUNITS prefix, for no language and each
// language setting. Candidates are looked at only when the game reaches them: it returns
// after the first CutsceneLibrary that parses completely.
std::string Manifest::outputUnitsJson() {
  // CommandLine::cutscenePath() and cutscenePath(lang).
  const auto cutscene = nested({u"_work",u"Data",u"Scripts",u"content",u"CUTSCENE"},Dir::FT_Dir);
  // Host branch: any failure lets the game try the next candidate. A null object is
  // dereferenced by the game; it is not treated as a successful parse here.
  std::map<std::string,std::string> hostParse; // path -> "" when parsed, otherwise the reason
  const auto parseHost = [&](const fs::path& file) {
    const auto key = file.lexically_normal().string();
    if(auto it=hostParse.find(key); it!=hostParse.end())
      return it->second;
    std::string reason;
    try {
      auto in = zenkit::Read::from(file);
      auto zen = in==nullptr ? nullptr : zenkit::ReadArchive::from(in.get());
      if(zen==nullptr)
        reason = "not a ZenGin archive";
      else if(zen->read_object<zenkit::CutsceneLibrary>(zenkit::GameVersion::GOTHIC_2)==nullptr)
        reason = "no object";
      }
    catch(const std::exception& e) { reason = e.what(); }
    catch(...) { reason = "unknown error"; }
    return hostParse[key] = reason;
    };
  // VFS branch: a parse error ends the game's load; once reached, the import fails the same way.
  std::map<std::string,Choice> vfsParsed;
  const auto parseVfs = [&](const std::string& name, const zenkit::VfsNode* node) {
    if(auto it=vfsParsed.find(name); it!=vfsParsed.end())
      return it->second;
    const auto choice = openNode(node,"vfs");
    try {
      auto in = node->open_read();
      auto zen = zenkit::ReadArchive::from(in.get());
      require(zen!=nullptr,"not a ZenGin archive");
      require(zen->read_object<zenkit::CutsceneLibrary>(zenkit::GameVersion::GOTHIC_2)!=nullptr,"no object");
      }
    catch(const std::exception& e) { throw std::runtime_error(name+" in the VFS does not load: "+e.what()); }
    return vfsParsed[name] = choice;
    };

  std::string reached, selected;
  bool any = false;
  for(int lang=-1; lang<int(scriptLanguages.size()); ++lang) {
    const std::string language = lang<0 ? "NONE" : languageNames[size_t(lang)];
    std::optional<Choice> chosen;
    for(const char* candidate:{"OU.BIN","OU.DAT"}) {
      const std::string name = candidate;
      std::string result;
      if(const auto* node = vfs.find(name)) {
        chosen = parseVfs(name,node);
        result = choiceJson(*chosen);
        }
      else {
        auto file = fs::path(FileUtil::caseInsensitiveSegment(cutscene,widen(name).c_str(),Dir::FT_File));
        std::string step = "cutscene";
        if(!FileUtil::exists(file.u16string()) && lang>=0) {
          const auto dir = nested({u"_work",u"Data",scriptLanguages[size_t(lang)]},Dir::FT_Dir);
          file = fs::path(FileUtil::caseInsensitiveSegment(dir,widen(name).c_str(),Dir::FT_File));
          step = "localized";
          }
        if(!FileUtil::exists(file.u16string()))
          result = "{\"step\":"+str(step)+",\"path\":"+str(relative(file))+",\"missing\":true}";
        else if(const auto reason = parseHost(file); !reason.empty())
          result = "{\"step\":"+str(step)+",\"path\":"+str(relative(file))+",\"rejected\":"+str(reason)+"}";
        else {
          chosen = openHost(file,step);
          result = choiceJson(*chosen);
          }
        }
      reached += std::string(reached.empty() ? "" : ",")+"{\"language\":\""+language+"\",\"name\":"+str(name)+
                 ",\"result\":"+result+"}";
      if(chosen)
        break;
      }
    selected += std::string(selected.empty() ? "" : ",")+"\""+language+"\":"+(chosen ? choiceJson(*chosen) : std::string("null"));
    any = any || chosen.has_value();
    // The server readers are compared with the default profile's file; an empty host
    // means the winner came from an archive.
    if(lang<0 && chosen)
      engineOu = chosen->host;
    }
  require(any,"output units not found: no loadable OU.BIN/OU.DAT");
  return "{\"prefix\":\"OU\",\"reached\":["+reached+"],\"selected\":{"+selected+"}}";
  }

// The first entry of `dir` with this name in any case, as the server and storygraph look up files.
fs::path firstChild(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  for(const auto& e:fs::directory_iterator(dir,ec))
    if(upper(e.path().filename().string())==upper(name))
      return e.path();
  return fs::path();
  }

// server/src/gothic_ai/gamedata/storygraph.py find_dat; the result may not exist.
fs::path findDat(const fs::path& gothic) {
  fs::path dat = gothic;
  for(const char* part:{"_work","Data","Scripts","_compiled","GOTHIC.DAT"}) {
    if(!fs::is_directory(dat)) { dat.clear(); break; }
    const auto next = firstChild(dat,part);
    dat = next.empty() ? dat/part : next;
    }
  return dat;
  }

std::string Manifest::serverJson() {
  const auto entry = [&](const fs::path& file, const fs::path& engine) {
    if(file.empty() || !fs::is_regular_file(file))
      return std::string("null");
    std::error_code ec;
    const bool same = !engine.empty() && fs::equivalent(file,engine,ec);
    auto json = choiceJson(openHost(file,"filesystem"));
    json.insert(json.size()-1,std::string(",\"same_as_engine\":")+(same ? "true" : "false"));
    return json;
    };
  // server/src/gothic_ai/gamedata/ou.py find_ou_file
  fs::path ou;
  if(const auto dir = gothic/"_work"/"Data"/"Scripts"/"CONTENT"/"CUTSCENE"; fs::is_directory(dir))
    ou = firstChild(dir,"OU.BIN");
  // server/src/gothic_ai/gamedata/storygraph.py find_dat
  const fs::path dat = findDat(gothic);
  // OpenGothic/ai_bridge/storygraph findOu(dat)
  fs::path graphOu;
  if(!dat.empty() && fs::is_regular_file(dat)) {
    const auto content = firstChild(dat.parent_path().parent_path(),"CONTENT");
    const auto cut = content.empty() ? fs::path() : firstChild(content,"CUTSCENE");
    graphOu = cut.empty() ? fs::path() : firstChild(cut,"OU.BIN");
    }
  return "{\n    \"ou_py\":"+entry(ou,engineOu)+",\n    \"storygraph_dat\":"+entry(dat,engineGameDat)+
         ",\n    \"storygraph_ou\":"+entry(graphOu,engineOu)+"\n  }";
  }

std::string Manifest::json() {
  const auto files   = filesJson();
  const auto music   = musicJson();
  const auto scripts = scriptsJson();
  const auto ou      = outputUnitsJson();
  const auto server  = serverJson();
  return "{\n  \"format\":\"gothic-sources\",\n  \"version\":1,\n  \"evidence\":\"import\",\n"
         "  \"profile\":{\"game\":\"GOTHIC.DAT\",\"output_units\":\"OU\",\"mods\":[],\"languages\":[\"NONE\",\"EN\",\"DE\",\"PL\",\"RU\",\"FR\",\"ES\",\"IT\",\"CZ\"]},\n"
         "  \"archives\":"+archivesJson()+",\n  \"files\":"+files+",\n  \"music\":"+music+
         ",\n  \"scripts\":"+scripts+",\n  \"output_units\":"+ou+",\n  \"server\":"+server+",\n"
         "  \"not_covered\":[\"runtime reachability of listed files\",\"request name variants built at run time\","
         "\"names produced by instance initializers\",\"music caches shared across themes at run time\","
         "\"Dx8 DLS parsing (DLS files are opened and hashed only)\","
         "\"payload decoding\"]\n}\n";
  }

rapidjson::Document parse(const std::string& text, const std::string& what) {
  rapidjson::Document doc;
  doc.Parse(text.c_str(),text.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid JSON: "+what);
  return doc;
  }

std::string readText(const fs::path& file) {
  std::ifstream f(file,std::ios::binary);
  require(bool(f),"cannot open "+file.string());
  return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
  }

// zenkit::Vfs compares names ignoring ASCII case only; ProgramData::Index::findVfs folds the same way.
std::string upperAscii(std::string s) {
  for(auto& c:s)
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
  return s;
  }

// P12: every origin Gothic::loadScript or the server can take a program from, converted to our
// own records (common/programdata.h) and indexed by the original choices.
class ProgramImport {
  public:
    explicit ProgramImport(const fs::path& root)
      : root16(installation(fs::absolute(root))), gothic(fs::path(root16).parent_path()) {
      vfs.set_open_observer([this](std::string_view, std::string_view source) { opened = source; });
      archives = mountRuntime(vfs,gothic,work);
      collectPaths(vfs.root(),"",paths);
      }

    // Converts, or with verifyOnly compares, every program; returns a summary line.
    std::string run(const fs::path& out, bool verifyOnly);

  private:
    using Key = std::pair<std::string,std::string>; // origin, path

    struct Entry {
      std::string archive, sha256, program, recordSha256, skipped;
      uint64_t    size = 0, trailing = 0;
      };

    struct Row {
      std::string        request;
      int                language = -1;
      std::string        step, path;
      std::optional<Key> source;
      };

    std::u16string nested(const std::initializer_list<const char16_t*>& segments, Dir::FileType type) const {
      return FileUtil::nestedPath(root16,segments,type);
      }

    std::string relative(const fs::path& p) const {
      return p.lexically_relative(gothic).lexically_normal().generic_string();
      }

    Key  fromNode(const zenkit::VfsNode* node);
    Key  fromHost(const fs::path& file);
    void add(const Key& key, const std::string& archive, const std::vector<uint8_t>& bytes, const Digest& d);
    void convert(Entry& e, const std::vector<uint8_t>& bytes);
    std::string json() const;

    std::u16string                               root16;
    fs::path                                     gothic, work, out;
    bool                                         verifyOnly = false;
    zenkit::Vfs                                  vfs;
    std::string                                  opened;
    std::vector<RuntimeArchive>                  archives;
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::map<const zenkit::VfsNode*,Key>         nodeKeys;
    std::map<Key,Entry>                          sources;
    std::map<std::string,std::string>            programs; // record name -> identity
    std::map<std::string,Key>                    vfsTable;
    std::vector<Row>                             rows;
    std::optional<Key>                           server;
  };

ProgramImport::Key ProgramImport::fromNode(const zenkit::VfsNode* node) {
  if(auto it=nodeKeys.find(node); it!=nodeKeys.end())
    return it->second;
  opened.clear();
  auto in = node->open_read();
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  if(fs::path(opened).is_absolute()) {
    const auto key = fromHost(fs::path(opened));
    require(sources.at(key).sha256==d.sha256,"host file changed while reading: "+opened);
    return nodeKeys[node] = key;
    }
  const Key key{"archive",paths.at(node)};
  add(key,upper(opened),bytes,d);
  return nodeKeys[node] = key;
  }

ProgramImport::Key ProgramImport::fromHost(const fs::path& file) {
  Key key{"host",relative(file)};
  // One physical file, however a lookup spelled it (case-insensitive file systems).
  for(const auto& [k,e]:sources) {
    std::error_code ec;
    if(k.first=="host" && upperAscii(k.second)==upperAscii(key.second) && fs::equivalent(gothic/k.second,file,ec))
      return k;
    }
  auto in = zenkit::Read::from(file);
  require(in!=nullptr,"cannot open "+file.string());
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  add(key,std::string(),bytes,d);
  return key;
  }

void ProgramImport::add(const Key& key, const std::string& archive, const std::vector<uint8_t>& bytes, const Digest& d) {
  if(sources.count(key))
    return;
  Entry e;
  e.archive = archive;
  e.size    = d.size;
  e.sha256  = d.sha256;
  convert(e,bytes);
  sources.emplace(key,std::move(e));
  }

void ProgramImport::convert(Entry& e, const std::vector<uint8_t>& bytes) {
  // OU.DAT and other ZenGin archives are not scripts: never parsed as Daedalus.
  static const char zengin[] = "ZenGin Archive";
  if(bytes.size()>=sizeof(zengin)-1 && std::memcmp(bytes.data(),zengin,sizeof(zengin)-1)==0) {
    e.skipped = "zengin archive";
    return;
    }
  // version u8, symbol count, sort table, text size: a corrupt count would allocate billions of symbols.
  if(bytes.size()<9 || uint64_t(le32(bytes,1))*4+9>bytes.size()) {
    e.skipped = "not a daedalus program";
    return;
    }
  zenkit::DaedalusProgramRecord program;
  size_t end = 0;
  try {
    auto read = zenkit::Read::from(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size());
    zenkit::DaedalusScript script;
    script.load(read.get());
    end = read->tell();
    // Exported right after parsing: no class registration, no VM, no runtime symbol.
    program = script.export_program();
    }
  catch(const std::exception& ex) {
    e.skipped = std::string("parse failed: ")+ex.what();
    return;
    }
  const auto length = program.bytecode.size();
  if(end>bytes.size() || length>end ||
     (length>0 && std::memcmp(bytes.data()+end-length,program.bytecode.data(),length)!=0)) {
    e.skipped = "truncated program";
    return;
    }
  require(program.symbols.size()==le32(bytes,1),"program symbol count differs from its header");
  e.trailing = bytes.size()-end;

  const auto enc = ProgramData::encodeProgram(program);
  const auto file = out/(enc.name+".json");
  if(!programs.count(enc.name)) {
    if(verifyOnly) {
      require(gothicglb::readFile(file)==std::vector<uint8_t>(enc.json.begin(),enc.json.end()),
              "program record differs: "+file.string());
      require(gothicglb::readFile(out/(enc.name+".bin"))==enc.bin,"program record differs: "+enc.name+".bin");
      }
    else {
      require(ProgramData::writeProgram(program,out)==enc.sha256,"program record identity changed while writing");
      }
    std::string sha;
    const auto back = ProgramData::readProgram(file,&sha);
    require(sha==enc.sha256,"program record identity differs after reading: "+file.string());
    auto diff = ProgramData::compare(program,back);
    require(diff.empty(),"program record differs after reading: "+diff);
    diff = ProgramData::compare(program,zenkit::DaedalusScript::from_program(back).export_program());
    require(diff.empty(),"program differs after the native factory: "+diff);
    programs[enc.name] = enc.sha256;
    }
  e.program      = enc.name;
  e.recordSha256 = enc.sha256;
  }

std::string ProgramImport::run(const fs::path& outDir, bool verify) {
  out        = outDir;
  verifyOnly = verify;
  if(!verifyOnly)
    fs::create_directories(out);

  // The runtime VFS: every .DAT basename, chosen by the engine's own Vfs::find.
  std::set<std::string> names;
  for(const auto& [node,path]:paths)
    if(extension(node->name())==".DAT")
      names.insert(upperAscii(node->name()));
  for(const auto& name:names) {
    const auto* node = vfs.find(name);
    require(node!=nullptr,"VFS lost "+name);
    vfsTable[name] = fromNode(node);
    }

  // Host programs below every _compiled directory CommandLine::scriptPath() and scriptPath(lang) name.
  std::vector<const char16_t*> dirs = {u"Scripts"};
  dirs.insert(dirs.end(),scriptLanguages.begin(),scriptLanguages.end());
  for(const auto* dir:dirs) {
    const fs::path root(nested({u"_work",u"Data",dir,u"_compiled"},Dir::FT_Dir));
    if(!fs::is_directory(root))
      continue;
    std::vector<fs::path> files;
    for(const auto& e:fs::recursive_directory_iterator(root))
      if(e.is_regular_file() && extension(e.path().filename().string())==".DAT")
        files.push_back(e.path());
    std::sort(files.begin(),files.end());
    for(const auto& f:files)
      fromHost(f);
    }

  if(const auto dat = findDat(gothic); !dat.empty() && fs::is_regular_file(dat))
    server = fromHost(dat);

  // Gothic::loadScript for the canonical requests (Gothic::createPhoenixVm callers and the default
  // game DAT) and every language setting; evidence, whatever the outcome.
  const std::array<std::string,8> requests = {"Camera.dat","Fight.dat","Sfx.dat","Music.dat","ParticleFx.dat",
                                               "VisualFx.dat","MENU.DAT","GOTHIC.DAT"};
  const auto widened = [](const std::string& s) { return widen(s.substr(0,std::min<size_t>(s.size(),255))); };
  const auto scripts = nested({u"_work",u"Data",u"Scripts",u"_compiled"},Dir::FT_Dir);
  for(const auto& request:requests)
    for(int lang=-1; lang<int(scriptLanguages.size()); ++lang) {
      Row r;
      r.request  = request;
      r.language = lang;
      const auto segment = request.find_last_of("\\/");
      if(const auto* node = vfs.find(request)) {
        r.step   = "vfs_find_request";
        r.path   = paths.at(node);
        r.source = fromNode(node);
        }
      else if(const auto* node = segment==std::string::npos ? nullptr : vfs.find(request.substr(segment+1))) {
        r.step   = "vfs_find_basename";
        r.path   = paths.at(node);
        r.source = fromNode(node);
        }
      else {
        auto path = FileUtil::caseInsensitiveSegment(scripts,widened(request).c_str(),Dir::FT_File);
        r.step = "host_ordinary";
        if(!FileUtil::exists(path) && lang>=0) {
          const auto localized = nested({u"_work",u"Data",scriptLanguages[size_t(lang)],u"_compiled"},Dir::FT_Dir);
          path   = FileUtil::caseInsensitiveSegment(localized,widened(request).c_str(),Dir::FT_File);
          r.step = "host_localized_full";
          if(!FileUtil::exists(path)) {
            path   = FileUtil::caseInsensitiveSegment(localized,widened(request.substr(segment+1)).c_str(),Dir::FT_File);
            r.step = "host_localized_basename";
            }
          }
        if(FileUtil::exists(path)) {
          r.source = fromHost(fs::path(path));
          r.path   = r.source->second;
          }
        }
      rows.push_back(std::move(r));
      }

  const auto text = json();
  if(!verifyOnly) {
    const auto tmp = out/"programs.json.tmp";
    gothicglb::writeFile(tmp,text.data(),text.size());
    fs::rename(tmp,out/"programs.json");
    }
  require(parse(readText(out/"programs.json"),"programs.json")==parse(text,"expected programs"),
          "programs.json differs from the installation");
  const auto index = ProgramData::readIndex(out);
  require(index.sources.size()==sources.size() && index.programs.size()==programs.size(),"programs.json reads back differently");

  size_t skipped = 0;
  for(const auto& [k,e]:sources)
    skipped += e.skipped.empty() ? 0 : 1;
  return std::to_string(programs.size())+" program records from "+std::to_string(sources.size())+" sources, "+
         std::to_string(skipped)+" skipped";
  }

std::string ProgramImport::json() const {
  std::map<Key,size_t> index;
  for(const auto& [k,e]:sources)
    index.emplace(k,index.size());
  const auto ref = [&](const std::optional<Key>& k) { return k ? std::to_string(index.at(*k)) : std::string("null"); };
  const auto opt = [](const std::string& s) { return s.empty() ? std::string("null") : str(s); };

  Files mounted;
  for(const auto& a:archives)
    mounted.emplace_back(a.name,a.size);
  std::sort(mounted.begin(),mounted.end());

  std::string s = "{\n  \"format\":\"gothic-programs\",\n  \"version\":1,\n  \"archives\":[";
  for(size_t i=0; i<mounted.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\":"+str(mounted[i].first)+",\"size\":"+std::to_string(mounted[i].second)+"}";
  s += "\n  ],\n  \"loose_files\":[";
  size_t n = 0;
  for(const auto& [k,e]:sources)
    if(k.first=="host")
      s += std::string(n++ ? "," : "")+"\n    {\"path\":"+str(k.second)+",\"size\":"+std::to_string(e.size)+"}";
  s += "\n  ],\n  \"programs\":{";
  n = 0;
  for(const auto& [name,sha]:programs)
    s += std::string(n++ ? "," : "")+"\n    \""+name+"\":\""+sha+"\"";
  s += "\n  },\n  \"sources\":[";
  n = 0;
  for(const auto& [k,e]:sources)
    s += std::string(n++ ? "," : "")+"\n    {\"origin\":\""+k.first+"\",\"path\":"+str(k.second)+
         ",\"archive\":"+opt(e.archive)+",\"size\":"+std::to_string(e.size)+",\"sha256\":\""+e.sha256+
         "\",\"program\":"+opt(e.program)+",\"trailing_bytes\":"+std::to_string(e.trailing)+
         ",\"skipped\":"+opt(e.skipped)+"}";
  s += "\n  ],\n  \"vfs\":{";
  n = 0;
  for(const auto& [name,k]:vfsTable)
    s += std::string(n++ ? "," : "")+"\n    "+str(name)+":"+ref(k);
  s += "\n  },\n  \"host\":{";
  n = 0;
  for(const auto& [k,e]:sources)
    if(k.first=="host")
      s += std::string(n++ ? "," : "")+"\n    "+str(k.second)+":"+ref(k);
  s += "\n  },\n  \"selections\":[";
  for(size_t i=0; i<rows.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"request\":"+str(rows[i].request)+",\"language\":"+
         std::to_string(rows[i].language)+",\"step\":\""+rows[i].step+"\",\"path\":"+opt(rows[i].path)+
         ",\"source\":"+ref(rows[i].source)+"}";
  s += "\n  ],\n  \"server\":{\"storygraph_dat\":"+ref(server)+"}\n}\n";
  return s;
  }

// P14, import only: server/src/gothic_ai/gamedata/ou.py parse_ou_bin over the original bytes, before
// decoding: upper stem -> raw text bytes, last wins. Kept to prove the native message view equal.
std::map<std::string,std::string> originalServerLines(const std::vector<uint8_t>& data) {
  const auto stem = [](uint8_t c) {
    return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
    };
  const auto u16 = [&data](size_t at) { return size_t(data[at]) | size_t(data[at+1])<<8; };
  std::map<std::string,std::string> lines;
  const size_t n = data.size();
  for(size_t i=0; i<n;) {
    // _WAV_ENTRY = \x01(..)([A-Za-z0-9_]+)\.WAV, DOTALL; finditer resumes after each match.
    if(data[i]!=0x01 || i+3>=n) { ++i; continue; }
    size_t end = i+3;
    while(end<n && stem(data[end]))
      ++end;
    if(end==i+3 || end+4>n || std::memcmp(data.data()+end,".WAV",4)!=0) { ++i; continue; }
    const size_t matchEnd = end+4;
    const std::string name(reinterpret_cast<const char*>(data.data()+i+3),end-i-3);
    const size_t start = i;
    i = matchEnd;
    if(u16(start+1)!=name.size()+4 || start<5 || data[start-5]!=0x12)
      continue;
    // _string_ending_at(data, hash_pos): the nearest string ending there, within 4096 starts.
    const size_t hashPos = start-5;
    if(hashPos<3)
      continue;
    const ptrdiff_t stop = std::max<ptrdiff_t>(ptrdiff_t(hashPos)-3-4096,-1);
    for(ptrdiff_t s=ptrdiff_t(hashPos)-3; s>stop; --s) {
      const auto at = size_t(s);
      if(data[at]==0x01 && u16(at+1)==hashPos-at-3) {
        lines[upperAscii(name)] = std::string(reinterpret_cast<const char*>(data.data()+at+3),hashPos-at-3);
        break;
        }
      }
    }
  return lines;
  }

// The same map from the native record: messages in archive order, the server's ASCII WAV-name rule
// (whole name [A-Za-z0-9_]+\.WAV), raw text, last wins. The server applies exactly this to the
// native messages, then its own cp1251 decode and strip.
std::map<std::string,std::string> nativeServerLines(const WorldData::ZenFile& library) {
  std::map<std::string,std::string> lines;
  for(const auto& msg:OutputUnits::messagesInArchiveOrder(library)) {
    const auto& name = msg->name;
    if(name.size()<=4 || name.compare(name.size()-4,4,".WAV")!=0)
      continue;
    const auto stem = std::string_view(name).substr(0,name.size()-4);
    if(!std::all_of(stem.begin(),stem.end(),[](char c) {
         return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_'; }))
      continue;
    lines[upperAscii(std::string(stem))] = msg->text;
    }
  return lines;
  }

std::string printable(const std::string& s) {
  std::string out;
  for(const char c:s.substr(0,120)) {
    const auto b = uint8_t(c);
    if(b>=0x20 && b<0x7F && c!='\\') { out.push_back(c); continue; }
    static const char digits[] = "0123456789abcdef";
    out += "\\x"; out.push_back(digits[b>>4]); out.push_back(digits[b&15]);
    }
  return out+(s.size()>120 ? "..." : "");
  }

// Fails unless the old server scanner and the native message view give the same complete map.
void requireServerView(const std::vector<uint8_t>& original, const WorldData::ZenFile& library, const std::string& what) {
  const auto expected = originalServerLines(original);
  const auto actual   = nativeServerLines(library);
  if(expected==actual)
    return;
  size_t onlyOld = 0, onlyNative = 0, different = 0;
  std::string first;
  for(const auto& [key,text]:expected) {
    const auto it = actual.find(key);
    if(it==actual.end()) {
      ++onlyOld;
      if(first.empty()) first = key+": only the original scanner has '"+printable(text)+"'";
      }
    else if(it->second!=text) {
      ++different;
      if(first.empty()) first = key+": scanner '"+printable(text)+"', native '"+printable(it->second)+"'";
      }
    }
  for(const auto& [key,text]:actual)
    if(!expected.count(key)) {
      ++onlyNative;
      if(first.empty()) first = key+": only the native view has '"+printable(text)+"'";
      }
  throw std::runtime_error(what+": server OU view differs from the original scanner (unsupported source structure): "+
                           std::to_string(expected.size())+" scanner lines, "+std::to_string(actual.size())+
                           " native, "+std::to_string(onlyOld)+" only scanner, "+std::to_string(onlyNative)+
                           " only native, "+std::to_string(different)+" different; first "+first);
  }

// P14: every origin GameScript::loadDialogOU, the server or storygraph can take the dialog line library
// from, converted to our own records (common/outputunits.h) and indexed by the original choices.
class OutputUnitsImport {
  public:
    explicit OutputUnitsImport(const fs::path& root)
      : root16(installation(fs::absolute(root))), gothic(fs::path(root16).parent_path()) {
      vfs.set_open_observer([this](std::string_view, std::string_view source) { opened = source; });
      archives = mountRuntime(vfs,gothic,work);
      collectPaths(vfs.root(),"",paths);
      }

    // Converts, or with verifyOnly compares, every library; returns a summary line.
    std::string run(const fs::path& out, bool verifyOnly);

  private:
    using Key = std::pair<std::string,std::string>; // origin, path

    struct Entry {
      std::string archive, sha256, record, skipped;
      uint64_t    size = 0;
      std::optional<WorldData::ZenFile> library;
      std::vector<uint8_t> bytes;                    // kept only for the server-view proof
      };

    struct Row {
      int                language = -1;
      std::string        candidate, step, path;
      std::optional<Key> source;
      };

    std::u16string nested(const std::initializer_list<const char16_t*>& segments, Dir::FileType type) const {
      return FileUtil::nestedPath(root16,segments,type);
      }

    std::string relative(const fs::path& p) const {
      return p.lexically_relative(gothic).lexically_normal().generic_string();
      }

    Key  fromNode(const zenkit::VfsNode* node);
    Key  fromHost(const fs::path& file);
    void add(const Key& key, const std::string& archive, std::vector<uint8_t>&& bytes, const Digest& d);
    void writeRecord(Entry& e);
    std::string json() const;

    std::u16string                               root16;
    fs::path                                     gothic, work, out;
    bool                                         verifyOnly = false;
    zenkit::Vfs                                  vfs;
    std::string                                  opened;
    std::vector<RuntimeArchive>                  archives;
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::map<const zenkit::VfsNode*,Key>         nodeKeys;
    std::map<Key,Entry>                          sources;
    std::set<std::string>                        records;  // written or verified record names
    std::map<std::string,Key>                    vfsTable;
    std::vector<Row>                             rows;
    std::optional<Key>                           serverOuPy;
  };

OutputUnitsImport::Key OutputUnitsImport::fromNode(const zenkit::VfsNode* node) {
  if(auto it=nodeKeys.find(node); it!=nodeKeys.end())
    return it->second;
  opened.clear();
  auto in = node->open_read();
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  if(fs::path(opened).is_absolute()) {
    const auto key = fromHost(fs::path(opened));
    require(sources.at(key).sha256==d.sha256,"host file changed while reading: "+opened);
    return nodeKeys[node] = key;
    }
  const Key key{"archive",paths.at(node)};
  add(key,upper(opened),std::move(bytes),d);
  return nodeKeys[node] = key;
  }

OutputUnitsImport::Key OutputUnitsImport::fromHost(const fs::path& file) {
  Key key{"host",relative(file)};
  // One physical file, however a lookup spelled it (case-insensitive file systems).
  for(const auto& [k,e]:sources) {
    std::error_code ec;
    if(k.first=="host" && upperAscii(k.second)==upperAscii(key.second) && fs::equivalent(gothic/k.second,file,ec))
      return k;
    }
  auto in = zenkit::Read::from(file);
  require(in!=nullptr,"cannot open "+file.string());
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  add(key,std::string(),std::move(bytes),d);
  return key;
  }

void OutputUnitsImport::add(const Key& key, const std::string& archive, std::vector<uint8_t>&& bytes, const Digest& d) {
  if(sources.count(key))
    return;
  Entry e;
  e.archive = archive;
  e.size    = d.size;
  e.sha256  = d.sha256;
  static const char zengin[] = "ZenGin Archive";
  if(bytes.size()<sizeof(zengin)-1 || std::memcmp(bytes.data(),zengin,sizeof(zengin)-1)!=0) {
    e.skipped = "not a zengin archive";
    sources.emplace(key,std::move(e));
    return;
    }
  // The game's host branch skips any failing candidate; a failing VFS one ends its load, and the
  // engine reports that from the skipped entry. Neither makes the import fail here.
  try {
    const std::vector<std::byte> raw(reinterpret_cast<const std::byte*>(bytes.data()),
                                     reinterpret_cast<const std::byte*>(bytes.data())+bytes.size());
    const auto name = upper(fs::path(key.second).filename().string());
    const WorldChunkSource source{name,bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())};
    WorldData::ZenFile library;
    WorldData::loadCutsceneLibrary(raw,source,library);
    e.library = std::move(library);
    e.bytes = std::move(bytes);
    }
  catch(const std::exception& ex) {
    e.skipped = std::string("parse failed: ")+ex.what();
    }
  sources.emplace(key,std::move(e));
  }

void OutputUnitsImport::writeRecord(Entry& e) {
  // The record name is the SHA-256 of its bytes; write beside the index first.
  const auto staging = (verifyOnly ? fs::temp_directory_path() : out)/
                       ("outputunits-"+e.sha256+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json.tmp");
  const auto identity = OutputUnits::writeLibrary(*e.library,staging);
  const auto file = out/(identity+".json");
  if(verifyOnly) {
    const bool same = fs::is_regular_file(file) && gothicglb::readFile(file)==gothicglb::readFile(staging);
    std::error_code ec; fs::remove(staging,ec);
    require(same,"output units record differs: "+file.string());
    }
  else if(records.count(identity)) {
    fs::remove(staging);
    }
  else {
    fs::rename(staging,file);
    }
  std::string actual;
  const auto back = OutputUnits::readLibrary(file,&actual);
  require(actual==identity,"output units record identity differs after reading: "+file.string());
  const auto diff = WorldData::compareZen(*e.library,back);
  require(diff.empty(),"output units record differs after reading: "+diff);
  records.insert(identity);
  e.record = identity;
  }

std::string OutputUnitsImport::run(const fs::path& outDir, bool verify) {
  out        = outDir;
  verifyOnly = verify;
  if(!verifyOnly)
    fs::create_directories(out);

  // The runtime VFS: every .BIN/.DAT basename, chosen by the engine's own Vfs::find, whatever it is.
  std::set<std::string> names;
  for(const auto& [node,path]:paths) {
    const auto ext = extension(node->name());
    if(ext==".BIN" || ext==".DAT")
      names.insert(upperAscii(node->name()));
    }
  for(const auto& name:names) {
    const auto* node = vfs.find(name);
    require(node!=nullptr,"VFS lost "+name);
    vfsTable[name] = fromNode(node);
    }

  const auto scan = [this](const fs::path& root) {
    if(!fs::is_directory(root))
      return;
    std::vector<fs::path> files;
    for(const auto& e:fs::recursive_directory_iterator(root))
      if(e.is_regular_file()) {
        const auto ext = extension(e.path().filename().string());
        if(ext==".BIN" || ext==".DAT")
          files.push_back(e.path());
        }
    std::sort(files.begin(),files.end());
    for(const auto& f:files)
      fromHost(f);
    };
  // CommandLine::cutscenePath() and cutscenePath(lang).
  const auto cutscene = nested({u"_work",u"Data",u"Scripts",u"content",u"CUTSCENE"},Dir::FT_Dir);
  scan(fs::path(cutscene));
  for(const auto* dir:scriptLanguages)
    scan(fs::path(nested({u"_work",u"Data",dir},Dir::FT_Dir)));

  // server/src/gothic_ai/gamedata/ou.py find_ou_file: exact-case directories, first OU.BIN entry.
  if(const auto dir = gothic/"_work"/"Data"/"Scripts"/"CONTENT"/"CUTSCENE"; fs::is_directory(dir))
    if(const auto file = firstChild(dir,"OU.BIN"); !file.empty() && fs::is_regular_file(file))
      serverOuPy = fromHost(file);
  // ai_bridge/storygraph findOu(dat) for every host DAT a storygraph run can name.
  std::vector<const char16_t*> compiled = {u"Scripts"};
  compiled.insert(compiled.end(),scriptLanguages.begin(),scriptLanguages.end());
  for(const auto* dir:compiled) {
    const fs::path root(nested({u"_work",u"Data",dir,u"_compiled"},Dir::FT_Dir));
    if(!fs::is_directory(root))
      continue;
    std::vector<fs::path> dats;
    for(const auto& e:fs::recursive_directory_iterator(root))
      if(e.is_regular_file() && extension(e.path().filename().string())==".DAT")
        dats.push_back(e.path());
    std::sort(dats.begin(),dats.end());
    for(const auto& dat:dats) {
      const auto content = firstChild(dat.parent_path().parent_path(),"CONTENT");
      const auto cut     = content.empty() ? fs::path() : firstChild(content,"CUTSCENE");
      const auto ou      = cut.empty() ? fs::path() : firstChild(cut,"OU.BIN");
      if(!ou.empty() && fs::is_regular_file(ou))
        fromHost(ou);
      }
    }

  // GameScript::loadDialogOU for the default prefix and every language setting; evidence only.
  const auto widened = [](const std::string& s) { return widen(s.substr(0,std::min<size_t>(s.size(),255))); };
  for(int lang=-1; lang<int(scriptLanguages.size()); ++lang)
    for(const std::string candidate:{"OU.BIN","OU.DAT"}) {
      Row r;
      r.language  = lang;
      r.candidate = candidate;
      if(const auto* node = vfs.find(candidate)) {
        r.step   = "vfs_find_request";
        r.path   = paths.at(node);
        r.source = fromNode(node);
        rows.push_back(std::move(r));
        break; // loaded, or the VFS failure ends the game's load
        }
      auto file = FileUtil::caseInsensitiveSegment(cutscene,widened(candidate).c_str(),Dir::FT_File);
      r.step = "host_ordinary";
      if(!FileUtil::exists(file) && lang>=0) {
        const auto localized = nested({u"_work",u"Data",scriptLanguages[size_t(lang)]},Dir::FT_Dir);
        file   = FileUtil::caseInsensitiveSegment(localized,widened(candidate).c_str(),Dir::FT_File);
        r.step = "host_localized";
        }
      bool loaded = false;
      if(FileUtil::exists(file)) {
        r.source = fromHost(fs::path(file));
        r.path   = r.source->second;
        loaded   = sources.at(*r.source).library.has_value();
        }
      rows.push_back(std::move(r));
      if(loaded)
        break;
      }

  // The server reads its view from the native messages: prove it equals the original scanner first.
  if(serverOuPy) {
    auto& e = sources.at(*serverOuPy);
    if(e.library)
      requireServerView(e.bytes,*e.library,serverOuPy->second);
    }

  size_t libraries = 0;
  for(auto& [key,e]:sources)
    if(e.library) {
      writeRecord(e);
      e.bytes.clear();
      ++libraries;
      }

  const auto text = json();
  if(!verifyOnly) {
    const auto tmp = out/"outputunits.json.tmp";
    gothicglb::writeFile(tmp,text.data(),text.size());
    fs::rename(tmp,out/"outputunits.json");
    }
  require(parse(readText(out/"outputunits.json"),"outputunits.json")==parse(text,"expected output units"),
          "outputunits.json differs from the installation");
  const auto index = OutputUnits::readIndex(out);
  require(index.sources.size()==sources.size() && index.records.size()==records.size(),
          "outputunits.json reads back differently");
  return std::to_string(records.size())+" library records from "+std::to_string(sources.size())+" sources, "+
         std::to_string(sources.size()-libraries)+" skipped";
  }

std::string OutputUnitsImport::json() const {
  std::map<Key,size_t> index;
  for(const auto& [k,e]:sources)
    index.emplace(k,index.size());
  const auto ref = [&](const std::optional<Key>& k) { return k ? std::to_string(index.at(*k)) : std::string("null"); };
  const auto opt = [](const std::string& s) { return s.empty() ? std::string("null") : str(s); };

  Files mounted;
  for(const auto& a:archives)
    mounted.emplace_back(a.name,a.size);
  std::sort(mounted.begin(),mounted.end());

  std::string s = "{\n  \"format\":\"gothic-outputunits\",\n  \"version\":1,\n  \"archives\":[";
  for(size_t i=0; i<mounted.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\":"+str(mounted[i].first)+",\"size\":"+std::to_string(mounted[i].second)+"}";
  s += "\n  ],\n  \"loose_files\":[";
  size_t n = 0;
  for(const auto& [k,e]:sources)
    if(k.first=="host")
      s += std::string(n++ ? "," : "")+"\n    {\"path\":"+str(k.second)+",\"size\":"+std::to_string(e.size)+"}";
  s += "\n  ],\n  \"records\":{";
  n = 0;
  for(const auto& name:records)
    s += std::string(n++ ? "," : "")+"\n    \""+name+"\":\""+name+"\"";
  s += "\n  },\n  \"sources\":[";
  n = 0;
  for(const auto& [k,e]:sources)
    s += std::string(n++ ? "," : "")+"\n    {\"origin\":\""+k.first+"\",\"path\":"+str(k.second)+
         ",\"archive\":"+opt(e.archive)+",\"size\":"+std::to_string(e.size)+",\"sha256\":\""+e.sha256+
         "\",\"record\":"+opt(e.record)+",\"skipped\":"+opt(e.skipped)+"}";
  s += "\n  ],\n  \"vfs\":{";
  n = 0;
  for(const auto& [name,k]:vfsTable)
    s += std::string(n++ ? "," : "")+"\n    "+str(name)+":"+ref(k);
  s += "\n  },\n  \"host\":{";
  n = 0;
  for(const auto& [k,e]:sources)
    if(k.first=="host")
      s += std::string(n++ ? "," : "")+"\n    "+str(k.second)+":"+ref(k);
  s += "\n  },\n  \"selections\":[";
  for(size_t i=0; i<rows.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"language\":"+std::to_string(rows[i].language)+",\"candidate\":"+
         str(rows[i].candidate)+",\"step\":\""+rows[i].step+"\",\"path\":"+opt(rows[i].path)+
         ",\"source\":"+ref(rows[i].source)+"}";
  s += "\n  ],\n  \"server\":{\"ou_py\":"+ref(serverOuPy)+"}\n}\n";
  return s;
  }

// P15: every sound request of the runtime VFS (P10's selection: each distinct .WAV/.OGG basename and its
// Vfs::find winner), copied byte for byte into the base (common/audiodata.h).
class AudioImport {
  public:
    explicit AudioImport(const fs::path& root)
      : root16(installation(fs::absolute(root))), gothic(fs::path(root16).parent_path()) {
      vfs.set_open_observer([this](std::string_view, std::string_view source) { opened = source; });
      archives = mountRuntime(vfs,gothic,work);
      collectPaths(vfs.root(),"",paths);
      }

    // Writes, or with verifyOnly compares, every payload and the index; returns a summary line.
    std::string run(const fs::path& out, bool verifyOnly);

  private:
    struct Entry {
      std::string payload, cls, origin, path, archive, header;
      uint64_t    size = 0;
      };

    std::string relative(const fs::path& p) const {
      return p.lexically_relative(gothic).lexically_normal().generic_string();
      }

    std::string json() const;

    std::u16string                                        root16;
    fs::path                                              gothic, work;
    zenkit::Vfs                                           vfs;
    std::string                                           opened;
    std::vector<RuntimeArchive>                           archives;
    std::map<const zenkit::VfsNode*,std::string>          paths;
    std::map<std::string,Entry>                           sounds;   // ASCII-upper request -> entry
    std::map<std::string,std::pair<std::string,uint64_t>> payloads; // sha -> (file, size)
    std::map<std::string,uint64_t>                        loose;    // host relative path -> size
  };

std::string AudioImport::run(const fs::path& out, bool verifyOnly) {
  if(!verifyOnly)
    fs::create_directories(out);

  std::set<std::string> requests;
  for(const auto& [node,path]:paths) {
    const auto ext = extension(node->name());
    if(ext==".WAV" || ext==".OGG")
      requests.insert(upperAscii(node->name()));
    }

  uint64_t totalBytes = 0, uniqueBytes = 0;
  size_t speech = 0;
  for(const auto& request:requests) {
    const auto* winner = vfs.find(request);
    require(winner!=nullptr,"Vfs::find does not resolve "+request);
    opened.clear();
    auto in = winner->open_read();
    std::vector<uint8_t> bytes;
    const auto d = digest(*in,&bytes);

    Entry e;
    e.payload = d.sha256;
    e.size    = d.size;
    e.header  = audioHeader(d.head);
    if(fs::path(opened).is_absolute()) {
      e.origin = "host";
      e.path   = relative(fs::path(opened));
      loose[e.path] = d.size;
      }
    else {
      e.origin  = "archive";
      e.path    = paths.at(winner);
      e.archive = upper(opened);
      }
    e.cls = "sound";
    for(const auto& part:fs::path(e.path).lexically_normal())
      if(upperAscii(part.generic_string())=="SPEECH")
        e.cls = "speech";
    speech += e.cls=="speech" ? 1 : 0;
    totalBytes += d.size;

    // The first request (in name order) with these bytes names their payload file; later ones reuse it.
    if(!payloads.count(e.payload)) {
      const auto ext = extension(request)==".OGG" ? std::string(".ogg") : std::string(".wav");
      const auto file = e.payload.substr(0,2)+"/"+e.payload+ext;
      const auto target = out/file;
      if(verifyOnly) {
        require(fs::is_regular_file(target) && gothicglb::readFile(target)==bytes,"audio payload differs: "+target.string());
        }
      else {
        fs::create_directories(target.parent_path());
        gothicglb::writeFile(target,bytes.data(),bytes.size());
        require(gothicglb::readFile(target)==bytes,"audio payload differs after writing: "+target.string());
        }
      payloads[e.payload] = {file,d.size};
      uniqueBytes += d.size;
      }
    sounds.emplace(request,std::move(e));
    }

  const auto text = json();
  if(!verifyOnly) {
    const auto tmp = out/"audio.json.tmp";
    gothicglb::writeFile(tmp,text.data(),text.size());
    fs::rename(tmp,out/"audio.json");
    }
  require(parse(readText(out/"audio.json"),"audio.json")==parse(text,"expected audio index"),
          "audio.json differs from the installation");
  const auto index = AudioData::readIndex(out);
  require(index.sounds.size()==sounds.size(),"audio.json reads back differently");
  return std::to_string(sounds.size())+" sounds ("+std::to_string(speech)+" speech, "+
         std::to_string(sounds.size()-speech)+" sound), "+std::to_string(payloads.size())+" payloads, "+
         std::to_string(uniqueBytes)+" unique of "+std::to_string(totalBytes)+" selected bytes";
  }

std::string AudioImport::json() const {
  Files mounted;
  for(const auto& a:archives)
    mounted.emplace_back(a.name,a.size);
  std::sort(mounted.begin(),mounted.end());

  std::string s = "{\n  \"format\":\"gothic-audio\",\n  \"version\":1,\n  \"archives\":[";
  for(size_t i=0; i<mounted.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\":"+str(mounted[i].first)+",\"size\":"+std::to_string(mounted[i].second)+"}";
  s += "\n  ],\n  \"loose_files\":[";
  size_t n = 0;
  for(const auto& [path,size]:loose)
    s += std::string(n++ ? "," : "")+"\n    {\"path\":"+str(path)+",\"size\":"+std::to_string(size)+"}";
  s += "\n  ],\n  \"payloads\":{";
  n = 0;
  for(const auto& [sha,payload]:payloads)
    s += std::string(n++ ? "," : "")+"\n    \""+sha+"\":{\"file\":"+str(payload.first)+",\"size\":"+
         std::to_string(payload.second)+"}";
  s += "\n  },\n  \"sounds\":{";
  n = 0;
  for(const auto& [name,e]:sounds)
    s += std::string(n++ ? "," : "")+"\n    "+str(name)+":{\"payload\":\""+e.payload+"\",\"class\":\""+e.cls+
         "\",\"origin\":\""+e.origin+"\",\"path\":"+str(e.path)+",\"archive\":"+
         (e.archive.empty() ? std::string("null") : str(e.archive))+",\"source_sha256\":\""+e.payload+
         "\",\"header\":"+e.header+"}";
  s += "\n  }\n}\n";
  return s;
  }

// "segment" | "style" | "dls" by extension, empty for other files.
std::string musicKind(const std::string& name) {
  const auto ext = extension(name);
  return ext==".SGT" ? "segment" : ext==".STY" ? "style" : ext==".DLS" ? "dls" : "";
  }

const char* resultName(DmResult rv) {
  switch(rv) {
    case DmResult_SUCCESS:          return "SUCCESS";
    case DmResult_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case DmResult_INVALID_STATE:    return "INVALID_STATE";
    case DmResult_MEMORY_EXHAUSTED: return "MEMORY_EXHAUSTED";
    case DmResult_NOT_FOUND:        return "NOT_FOUND";
    case DmResult_FILE_CORRUPT:     return "FILE_CORRUPT";
    case DmResult_MUTEX_ERROR:      return "MUTEX_ERROR";
    }
  return "UNKNOWN";
  }

// A GothicKit object built by the old parser (factory=false) or by the record factory, then encoded.
// Returns the build result; `out` holds the encoding on success.
DmResult gkBuildAndEncode(const std::string& kind, bool factory, const std::vector<uint8_t>& in,
                          std::vector<uint8_t>& out) {
  out.clear();
  uint8_t* enc = nullptr;
  size_t   len = 0;
  DmResult rv, er = DmResult_SUCCESS;
  if(kind=="segment") {
    DmSegment* o = nullptr;
    rv = factory ? MusicNative_segment(in.data(),in.size(),&o) : MusicNative_parseSegment(in.data(),in.size(),&o);
    if(rv==DmResult_SUCCESS) {
      er = MusicNative_encodeSegment(o,&enc,&len);
      MusicNative_releaseSegment(o);
      }
    }
  else if(kind=="style") {
    DmStyle* o = nullptr;
    rv = factory ? MusicNative_style(in.data(),in.size(),&o) : MusicNative_parseStyle(in.data(),in.size(),&o);
    if(rv==DmResult_SUCCESS) {
      er = MusicNative_encodeStyle(o,&enc,&len);
      MusicNative_releaseStyle(o);
      }
    }
  else {
    DmDls* o = nullptr;
    rv = factory ? MusicNative_dls(in.data(),in.size(),&o) : MusicNative_parseDls(in.data(),in.size(),&o);
    if(rv==DmResult_SUCCESS) {
      er = MusicNative_encodeDls(o,&enc,&len);
      MusicNative_releaseDls(o);
      }
    }
  if(rv!=DmResult_SUCCESS)
    return rv;
  require(er==DmResult_SUCCESS,std::string("GothicKit object cannot be encoded: ")+resultName(er));
  out.assign(enc,enc+len);
  std::free(enc);
  return rv;
  }

// P16: every SGT/STY/DLS either music provider can select, parsed by that provider's old parser and stored
// as its own records (common/musicdata.h). GothicKit selects each distinct basename of the runtime VFS by
// Vfs::find; Dx8 opens host files directly in its six search directories.
class MusicImport {
  public:
    explicit MusicImport(const fs::path& root)
      : root16(installation(fs::absolute(root))), gothic(fs::path(root16).parent_path()) {
      vfs.set_open_observer([this](std::string_view, std::string_view source) { opened = source; });
      archives = mountRuntime(vfs,gothic,work);
      collectPaths(vfs.root(),"",paths);
      // Resources: the Dx8::DirectMusic search paths, in order.
      dirs = {
        nested({u"_work",u"Data",u"Music",u"newworld"},  Dir::FT_Dir),
        nested({u"_work",u"Data",u"Music",u"AddonWorld"},Dir::FT_Dir),
        nested({u"_work",u"Data",u"Music",u"dungeon"},   Dir::FT_Dir),
        nested({u"_work",u"Data",u"Music",u"menu_men"},  Dir::FT_Dir),
        nested({u"_work",u"Data",u"Music",u"orchestra"}, Dir::FT_Dir),
        nested({u"_work",u"Data",u"Music"},              Dir::FT_Dir),
        };
      }

    // Writes, or with verifyOnly compares, every record and the index; returns a summary line.
    std::string run(const fs::path& out, bool verifyOnly);

  private:
    using Key = std::pair<std::string,std::string>; // origin, path

    struct Entry {
      std::string        archive, sha256, kind;
      uint64_t           size = 0;
      MusicData::Outcome gk, dx8;
      };

    // BaseMusic_trace callbacks: references are collected, never thrown through the C loader. The first
    // exception is kept, later callbacks do nothing, and it is rethrown once BaseMusic_trace has returned
    // (its loader lock released and its objects freed).
    struct Trace {
      MusicImport*                                 self = nullptr;
      std::vector<std::pair<bool,std::string>>     references; // (style, file)
      std::exception_ptr                           error;
      };

    std::u16string nested(const std::initializer_list<const char16_t*>& segments, Dir::FileType type) const {
      return FileUtil::nestedPath(root16,segments,type);
      }

    std::string relative(const fs::path& p) const {
      return p.lexically_relative(gothic).lexically_normal().generic_string();
      }

    Key  fromNode(const zenkit::VfsNode* node);
    Key  fromHost(const fs::path& file);
    void add(const Key& key, const std::string& archive, const std::vector<uint8_t>& bytes, const Digest& d);
    void convertGk(const Key& key, Entry& e, const std::vector<uint8_t>& bytes);
    void convertDx8(Entry& e, const std::vector<uint8_t>& bytes);
    MusicData::Outcome   writeRecord(const std::string& projection, const std::string& kind, const std::vector<uint8_t>& bin);
    std::vector<uint8_t> readBack(const MusicData::Outcome& o) const;
    const Entry*         findDx8File(const std::u16string& file) const;
    void                 checkGkReferences();
    void                 checkDx8References();
    std::string          json() const;

    static void* resolveGk(void* ctx, char const* file, size_t* len);
    static void  visitGk(void* ctx, BaseMusicReference const* reference);

    std::u16string                               root16;
    fs::path                                     gothic, work, out;
    bool                                         verifyOnly = false;
    zenkit::Vfs                                  vfs;
    std::string                                  opened;
    std::vector<RuntimeArchive>                  archives;
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::vector<std::u16string>                  dirs;
    std::map<const zenkit::VfsNode*,Key>         nodeKeys;
    std::map<Key,Entry>                          sources;
    std::map<std::string,std::string>            records;  // record key -> identity
    std::map<std::string,Key>                    gkTable;  // ASCII-upper basename -> Vfs::find winner
    std::map<std::string,Key>                    dx8Table; // installation-relative host path -> source
  };

MusicImport::Key MusicImport::fromNode(const zenkit::VfsNode* node) {
  if(auto it=nodeKeys.find(node); it!=nodeKeys.end())
    return it->second;
  opened.clear();
  auto in = node->open_read();
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  if(fs::path(opened).is_absolute()) {
    const auto key = fromHost(fs::path(opened));
    require(sources.at(key).sha256==d.sha256,"host file changed while reading: "+opened);
    return nodeKeys[node] = key;
    }
  const Key key{"archive",paths.at(node)};
  add(key,upper(opened),bytes,d);
  return nodeKeys[node] = key;
  }

MusicImport::Key MusicImport::fromHost(const fs::path& file) {
  // Keyed by the spelling that reached it, so the Dx8 table names its own source exactly.
  const Key key{"host",relative(file)};
  if(sources.count(key))
    return key;
  auto in = zenkit::Read::from(file);
  require(in!=nullptr,"cannot open "+file.string());
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  add(key,std::string(),bytes,d);
  return key;
  }

void MusicImport::add(const Key& key, const std::string& archive, const std::vector<uint8_t>& bytes, const Digest& d) {
  if(sources.count(key))
    return;
  Entry e;
  e.archive = archive;
  e.size    = d.size;
  e.sha256  = d.sha256;
  e.kind    = musicKind(fs::path(key.second).filename().string());
  require(!e.kind.empty(),"not a music file: "+key.second);
  convertGk(key,e,bytes);
  convertDx8(e,bytes);
  sources.emplace(key,std::move(e));
  }

void MusicImport::convertGk(const Key& key, Entry& e, const std::vector<uint8_t>& bytes) {
  // The old parser converts these strings in place with an unchecked wcrtomb: refuse what it cannot
  // convert exactly, before it runs.
  size_t bad = 0;
  require(MusicNative_checkGkStrings(bytes.data(),bytes.size(),&bad)==DmResult_SUCCESS,
          "music: "+key.second+": GothicKit string chunk at byte "+std::to_string(bad)+
          " is not terminated ASCII UTF-16 inside its parent");

  std::vector<uint8_t> bin;
  const auto rv = gkBuildAndEncode(e.kind,false,bytes,bin);
  require(rv!=DmResult_MEMORY_EXHAUSTED && rv!=DmResult_MUTEX_ERROR,
          "music: "+key.second+": GothicKit parser failed transiently: "+resultName(rv));
  if(rv!=DmResult_SUCCESS) {
    e.gk.failure = resultName(rv);
    return;
    }
  e.gk = writeRecord("gk",e.kind,bin);

  // The runtime factory rebuilds the same object from the record as read back.
  std::vector<uint8_t> again;
  const auto back = readBack(e.gk);
  const auto fr = gkBuildAndEncode(e.kind,true,back,again);
  require(fr==DmResult_SUCCESS,"music: "+e.gk.record+": factory failed: "+resultName(fr));
  require(again==bin,"music: "+e.gk.record+": factory object differs from the parsed one");
  }

void MusicImport::convertDx8(Entry& e, const std::vector<uint8_t>& bytes) {
  // The Riff constructors exactly as DirectMusic runs them; a DLS also runs its SoundFont/Hydra
  // finalization, whose exception is a failure of that file too. Waves are captured before it clears them.
  std::optional<Dx8::Segment>               segment;
  std::optional<Dx8::Style>                 style;
  std::optional<Dx8::DlsCollection::Parsed> dls;
  try {
    Dx8::Riff riff(bytes.data(),bytes.size());
    if(e.kind=="segment") {
      segment.emplace(riff);
      }
    else if(e.kind=="style") {
      style.emplace(riff);
      }
    else {
      auto parsed = Dx8::DlsCollection::parse(riff);
      Dx8::DlsCollection::Parsed copy = parsed;
      Dx8::DlsCollection finalized(std::move(parsed));
      dls.emplace(std::move(copy));
      }
    }
  catch(const std::exception& ex) {
    e.dx8.failure = ex.what();
    return;
    }

  const auto bin = segment ? MusicData::encodeDx8(*segment) :
                   style   ? MusicData::encodeDx8(*style)   : MusicData::encodeDx8(*dls);
  e.dx8 = writeRecord("dx8",e.kind,bin);

  const auto back  = readBack(e.dx8);
  const auto again = segment ? MusicData::encodeDx8(MusicData::decodeDx8Segment(back)) :
                     style   ? MusicData::encodeDx8(MusicData::decodeDx8Style(back))   :
                               MusicData::encodeDx8(MusicData::decodeDx8Dls(back));
  require(again==bin,"music: "+e.dx8.record+": decoded object differs from the parsed one");
  }

MusicData::Outcome MusicImport::writeRecord(const std::string& projection, const std::string& kind,
                                            const std::vector<uint8_t>& bin) {
  const std::string_view view(reinterpret_cast<const char*>(bin.data()),bin.size());
  const auto sha  = sha256(view);
  const auto json = "{\"format\":\"gothic-music-record\",\"version\":1,\"projection\":\""+projection+
                    "\",\"kind\":\""+kind+"\",\"buffer\":\""+sha+".bin\",\"byte_length\":"+
                    std::to_string(bin.size())+"}";
  MusicData::Outcome o;
  o.record   = projection+"/"+kind+"/"+sha;
  o.identity = sha256(json+std::string(view));

  const auto dir      = out/projection/kind;
  const auto jsonFile = dir/(sha+".json");
  const auto binFile  = dir/(sha+".bin");
  if(verifyOnly) {
    require(fs::is_regular_file(binFile) && fs::is_regular_file(jsonFile) &&
            gothicglb::readFile(binFile)==bin && readText(jsonFile)==json,"music record differs: "+o.record);
    }
  else if(!records.count(o.record)) {
    // Staged under the identity and renamed into place: a torn write never carries a record name.
    fs::create_directories(dir);
    const auto tmpBin  = dir/(o.identity+".bin.tmp");
    const auto tmpJson = dir/(o.identity+".json.tmp");
    gothicglb::writeFile(tmpBin,bin.data(),bin.size());
    gothicglb::writeFile(tmpJson,json.data(),json.size());
    fs::rename(tmpBin,binFile);
    fs::rename(tmpJson,jsonFile);
    }
  records[o.record] = o.identity;
  return o;
  }

std::vector<uint8_t> MusicImport::readBack(const MusicData::Outcome& o) const {
  MusicData::Index index;
  index.dir = out;
  index.records[o.record] = o.identity;
  return MusicData::readRecord(index,o.record);
  }

void* MusicImport::resolveGk(void* ctx, char const* file, size_t* len) {
  auto& trace = *static_cast<Trace*>(ctx);
  if(trace.error)
    return nullptr;
  try {
    auto& self = *trace.self;
    // Only music files are offered: every one of them passed the string check before any parser saw it.
    std::string_view name(file);
    while(!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
      name.remove_suffix(1);
    if(!self.gkTable.count(upperAscii(std::string(name))))
      return nullptr;
    const auto* node = self.vfs.find(file);
    if(node==nullptr)
      return nullptr;
    auto in = node->open_read();
    std::vector<uint8_t> bytes;
    (void)digest(*in,&bytes);
    void* buf = std::malloc(bytes.empty() ? 1 : bytes.size());
    if(buf==nullptr)
      return nullptr;
    std::memcpy(buf,bytes.data(),bytes.size());
    *len = bytes.size();
    return buf;
    }
  catch(...) {
    trace.error = std::current_exception();
    return nullptr;
    }
  }

void MusicImport::visitGk(void* ctx, BaseMusicReference const* reference) {
  auto& trace = *static_cast<Trace*>(ctx);
  if(trace.error || reference->file==nullptr)
    return;
  try {
    trace.references.emplace_back(reference->style!=0,std::string(reference->file));
    }
  catch(...) {
    trace.error = std::current_exception();
    }
  }

// A reference that names a file of another kind fails the import (both providers would misparse it).
void MusicImport::checkGkReferences() {
  for(const auto& [name,key]:gkTable) {
    if(sources.at(key).kind!="segment" || sources.at(key).gk.record.empty())
      continue;
    Trace trace;
    trace.self = this;
    int loaded = 0;
    (void)BaseMusic_trace(name.c_str(),&MusicImport::resolveGk,&trace,&MusicImport::visitGk,&trace,&loaded);
    if(trace.error)
      std::rethrow_exception(trace.error);
    for(const auto& [isStyle,file]:trace.references) {
      std::string_view f(file);
      while(!f.empty() && std::isspace(static_cast<unsigned char>(f.back())))
        f.remove_suffix(1);
      const auto it = gkTable.find(upperAscii(std::string(f)));
      if(it==gkTable.end())
        continue;
      const auto& kind = sources.at(it->second).kind;
      require(kind==(isStyle ? "style" : "dls"),
              "music: "+name+": GothicKit "+(isStyle ? "style" : "instrument")+" reference "+file+" names a "+kind);
      }
    }
  }

const MusicImport::Entry* MusicImport::findDx8File(const std::u16string& file) const {
  std::u16string openedPath;
  try {
    (void)Dx8::openSourceFile(dirs,file.c_str(),openedPath);
    }
  catch(const std::exception&) {
    return nullptr;
    }
  const auto rel = relative(fs::path(openedPath));
  if(auto it=dx8Table.find(rel); it!=dx8Table.end())
    return &sources.at(it->second);
  for(const auto& [path,key]:dx8Table)
    if(upperAscii(path)==upperAscii(rel))
      return &sources.at(key);
  return nullptr;
  }

void MusicImport::checkDx8References() {
  for(const auto& [path,key]:dx8Table) {
    const auto& e = sources.at(key);
    if(e.dx8.record.empty())
      continue;
    if(e.kind=="segment") {
      const auto segment = MusicData::decodeDx8Segment(readBack(e.dx8));
      for(const auto& track:segment.track) {
        if(track.sttr==nullptr)
          continue;
        for(const auto& s:track.sttr->styles)
          if(const auto* target = findDx8File(s.reference.file))
            require(target->kind=="style","music: "+path+": Dx8 style reference "+str16(s.reference.file)+
                                          " names a "+target->kind);
        }
      }
    else if(e.kind=="style") {
      const auto style = MusicData::decodeDx8Style(readBack(e.dx8));
      for(const auto& band:style.band)
        for(const auto& ins:band.intrument) {
          if(ins.reference.file.empty())
            continue;
          if(const auto* target = findDx8File(ins.reference.file))
            require(target->kind=="dls","music: "+path+": Dx8 instrument reference "+str16(ins.reference.file)+
                                        " names a "+target->kind);
          }
      }
    }
  }

std::string MusicImport::run(const fs::path& outDir, bool verify) {
  out        = outDir;
  verifyOnly = verify;
  if(!verifyOnly)
    fs::create_directories(out);

  // GothicKit: every music basename of the runtime VFS and its Vfs::find winner.
  std::set<std::string> names;
  for(const auto& [node,path]:paths)
    if(!musicKind(node->name()).empty())
      names.insert(upperAscii(node->name()));
  for(const auto& name:names) {
    const auto* winner = vfs.find(name);
    require(winner!=nullptr,"Vfs::find does not resolve "+name);
    gkTable[name] = fromNode(winner);
    }

  // Dx8: every regular music file directly in each search directory, as nestedPath spells it.
  for(const auto& dir:dirs) {
    std::vector<std::u16string> files;
    Dir::scan(dir,[&files](const std::u16string& name, Dir::FileType t) {
      if(t==Dir::FT_File)
        files.push_back(name);
      });
    std::sort(files.begin(),files.end());
    for(const auto& name:files) {
      const fs::path file = fs::path(dir)/fs::path(name);
      if(musicKind(file.filename().string()).empty() || !fs::is_regular_file(file))
        continue;
      dx8Table[relative(file)] = fromHost(file);
      }
    }

  checkGkReferences();
  checkDx8References();

  const auto text = json();
  if(!verifyOnly) {
    const auto tmp = out/"music.json.tmp";
    gothicglb::writeFile(tmp,text.data(),text.size());
    fs::rename(tmp,out/"music.json");
    }
  require(parse(readText(out/"music.json"),"music.json")==parse(text,"expected music index"),
          "music.json differs from the installation");
  const auto index = MusicData::readIndex(out);
  require(index.sources.size()==sources.size() && index.records.size()==records.size() &&
          index.gk.size()==gkTable.size() && index.dx8.size()==dx8Table.size(),"music.json reads back differently");

  size_t gkRecords = 0, dx8Records = 0;
  for(const auto& [key,e]:sources) {
    gkRecords  += e.gk.record.empty()  ? 0 : 1;
    dx8Records += e.dx8.record.empty() ? 0 : 1;
    }
  return std::to_string(sources.size())+" music sources ("+std::to_string(gkTable.size())+" GothicKit names, "+
         std::to_string(dx8Table.size())+" Dx8 files), GothicKit "+std::to_string(gkRecords)+" parsed / "+
         std::to_string(sources.size()-gkRecords)+" failed, Dx8 "+std::to_string(dx8Records)+" parsed / "+
         std::to_string(sources.size()-dx8Records)+" failed, "+std::to_string(records.size())+" records";
  }

std::string MusicImport::json() const {
  std::map<Key,size_t> index;
  for(const auto& [k,e]:sources)
    index.emplace(k,index.size());
  const auto outcome = [](const MusicData::Outcome& o, bool gk) {
    if(!o.failure)
      return "{\"record\":\""+o.record+"\"}";
    return std::string("{\"failure\":{")+(gk ? "\"result\":" : "\"error\":")+str(*o.failure)+"}}";
    };

  Files mounted;
  for(const auto& a:archives)
    mounted.emplace_back(a.name,a.size);
  std::sort(mounted.begin(),mounted.end());

  std::string s = "{\n  \"format\":\"gothic-music\",\n  \"version\":1,\n  \"archives\":[";
  for(size_t i=0; i<mounted.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\":"+str(mounted[i].first)+",\"size\":"+std::to_string(mounted[i].second)+"}";
  s += "\n  ],\n  \"loose_files\":[";
  size_t n = 0;
  for(const auto& [k,e]:sources)
    if(k.first=="host")
      s += std::string(n++ ? "," : "")+"\n    {\"path\":"+str(k.second)+",\"size\":"+std::to_string(e.size)+"}";
  s += "\n  ],\n  \"records\":{";
  n = 0;
  for(const auto& [key,identity]:records)
    s += std::string(n++ ? "," : "")+"\n    \""+key+"\":\""+identity+"\"";
  s += "\n  },\n  \"sources\":[";
  n = 0;
  for(const auto& [k,e]:sources)
    s += std::string(n++ ? "," : "")+"\n    {\"origin\":\""+k.first+"\",\"path\":"+str(k.second)+
         ",\"archive\":"+(e.archive.empty() ? std::string("null") : str(e.archive))+",\"size\":"+std::to_string(e.size)+
         ",\"sha256\":\""+e.sha256+"\",\"kind\":\""+e.kind+"\",\"gk\":"+outcome(e.gk,true)+
         ",\"dx8\":"+outcome(e.dx8,false)+"}";
  s += "\n  ],\n  \"gk\":{";
  n = 0;
  for(const auto& [name,k]:gkTable)
    s += std::string(n++ ? "," : "")+"\n    "+str(name)+":"+std::to_string(index.at(k));
  s += "\n  },\n  \"dx8\":{";
  n = 0;
  for(const auto& [path,k]:dx8Table)
    s += std::string(n++ ? "," : "")+"\n    "+str(path)+":"+std::to_string(index.at(k));
  s += "\n  }\n}\n";
  return s;
  }
}

std::vector<RuntimeArchive> mountRuntime(zenkit::Vfs& vfs, const fs::path& gothic, fs::path& work) {
  // Resources::loadVdfs without a mod ini: every .mod is filtered out.
  std::vector<RuntimeArchive> archives;
  for(auto& a:runtimeArchives(gothic)) {
    if(a.mod)
      continue;
    try {
      vfs.mount_disk(a.path,zenkit::VfsOverwriteBehavior::OLDER);
      }
    catch(const std::exception& e) {
      std::cerr << "unable to load archive " << a.path.filename().string() << ": " << e.what() << "\n";
      continue;
      }
    archives.push_back(std::move(a));
    }

  Files runtime, supported;
  for(const auto& a:archives)
    runtime.emplace_back(a.name,a.size);
  for(const auto& e:fs::directory_iterator(gothic/"Data"))
    if(e.is_regular_file() && upper(e.path().extension().string())==".VDF")
      supported.emplace_back(upper(e.path().filename().string()),e.file_size());
  std::sort(runtime.begin(),runtime.end());
  std::sort(supported.begin(),supported.end());
  require(runtime==supported,"runtime archive set differs from the supported Gothic II import");

  // Gothic::nestedPath({u"_work"}) ends with '/'; drop it so relative paths stay exact.
  work = fs::path(FileUtil::nestedPath(installation(gothic),{u"_work"},Dir::FT_Dir)).parent_path();
  if(fs::is_directory(work)) {
    // Resources::mountWork: after the archives, never replacing them.
    vfs.mkdir("/_work");
    vfs.mount_host(work,"/_work",zenkit::VfsOverwriteBehavior::NONE);
    }
  return archives;
  }

int convertSources(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto text = Manifest(gothic).json();
  if(!verifyOnly) {
    fs::create_directories(out);
    const auto tmp = out/"sources.json.tmp";
    {
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
    f.write(text.data(),std::streamsize(text.size()));
    require(bool(f),"cannot write "+tmp.string());
    }
    fs::rename(tmp,out/"sources.json");
    }
  require(parse(readText(out/"sources.json"),"sources.json")==parse(text,"expected sources"),
          "sources.json differs from the installation");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified" : "written and verified") << " source manifest (" << ms << " ms)\n";
  return 0;
  }

int convertAudio(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  AudioImport audio(gothic);
  const auto summary = audio.run(out,verifyOnly);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << summary << " (" << ms << " ms)\n";
  return 0;
  }

int convertMusic(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  MusicImport music(gothic);
  const auto summary = music.run(out,verifyOnly);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << summary << " (" << ms << " ms)\n";
  return 0;
  }

int convertOutputUnits(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  OutputUnitsImport outputUnits(gothic);
  const auto summary = outputUnits.run(out,verifyOnly);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << summary << " (" << ms << " ms)\n";
  return 0;
  }

int convertOutputUnitsFile(const fs::path& source, const fs::path& record) {
  auto in = zenkit::Read::from(source);
  require(in!=nullptr,"cannot open "+source.string());
  std::vector<uint8_t> bytes;
  const auto d = digest(*in,&bytes);
  const std::vector<std::byte> raw(reinterpret_cast<const std::byte*>(bytes.data()),
                                   reinterpret_cast<const std::byte*>(bytes.data())+bytes.size());
  const WorldChunkSource origin{upper(source.filename().string()),bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())};
  WorldData::ZenFile library;
  WorldData::loadCutsceneLibrary(raw,origin,library);
  const auto identity = OutputUnits::writeLibrary(library,record);
  std::string actual;
  const auto back = OutputUnits::readLibrary(record,&actual);
  require(actual==identity,"output units record identity differs after reading: "+record.string());
  const auto diff = WorldData::compareZen(library,back);
  require(diff.empty(),"output units record differs after reading: "+diff);
  std::cout << "record sha256 " << identity << ", source sha256 " << d.sha256 << ", "
            << library.blocks.size() << " roots, " << OutputUnits::messagesInArchiveOrder(library).size()
            << " messages\n";
  return 0;
  }

int convertPrograms(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  ProgramImport programs(gothic);
  const auto summary = programs.run(out,verifyOnly);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << summary << " (" << ms << " ms)\n";
  return 0;
  }

}

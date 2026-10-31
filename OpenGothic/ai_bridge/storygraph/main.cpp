// storygraph: lifts the imported game program into a graph (docs/story-graph.md).
// The positional input is a game directory or a legacy DAT path used only as a source selector.
//
//   storygraph <GOTHIC.DAT>                         statistics of the lifted model
//   storygraph <GOTHIC.DAT> --json story.json       the model for the AI server (--pretty: indented)
//   storygraph <GOTHIC.DAT> --node DIA_LOBART_WORKNOW
//   storygraph <GOTHIC.DAT> --quest MIS_LOBART_RUEBEN
//   storygraph <GOTHIC.DAT> --scan [--tsv dialogs.tsv] [--top N]   first measurement, per dialog bytecode summary
//   storygraph <GOTHIC.DAT> --disasm DIA_LOBART_RUEBENRUNNING_INFO
//   storygraph <GOTHIC.DAT> --check <save dir> [--check <save dir>...] [--node ID] [--verbose]
//                                  every lifted condition against the VM on original saves (stage 2);
//                                  with --node, both computations of that dialog are traced
//   storygraph <GOTHIC.DAT> --fuzz N [--node ID] [--verbose]
//                                  the same in N random states per dialog
//   ... --check/--fuzz with --effects: what dialogs and choices do; with --exec: their programs run by the
//                                  Interpreter against the VM (docs/story-graph.md, «Исполнение без VM»); with
//                                  --via-json too, each program written as JSON and read back first
//   storygraph <GOTHIC.DAT> --program DIA_LOBART_WORKNOW   the programs of a dialog (condition, information)
//                                  or of a script function
//   storygraph <game directory> --ou-messages    owned raw message pairs for the Python server
//
// Subtitles use the indexed program-relative OU, or --ou <native record.json>.

#include "bytecode.h"
#include "baseimport.h"
#include "basestoreimport.h"
#include "check.h"
#include "execcheck.h"
#include "gothicglb.h"
#include "lifter.h"
#include "model.h"
#include "outputunits.h"
#include "program.h"
#include "programdata.h"
#include "vocab.h"

#include <zenkit/DaedalusVm.hh>
#include <zenkit/Logger.hh>
#include <zenkit/addon/daedalus.hh>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace storygraph;
namespace fs = std::filesystem;

namespace storygraph {
  int scanDialogs(zenkit::DaedalusVm& vm, const Bytecode& code, const char* tsvPath, size_t top);
  }

static std::string upper(std::string s) {
  for(auto& c : s)
    c = char(std::toupper(uint8_t(c)));
  return s;
  }

static fs::path installationRoot(const fs::path& input, bool directory) {
  const auto absolute = fs::absolute(input);
  if(directory)
    return absolute;
  for(auto parent = absolute.parent_path(); !parent.empty() && parent!=parent.parent_path();
      parent = parent.parent_path()) {
    if(upper(parent.filename().string())=="_WORK")
      return parent.parent_path();
    }
  throw std::runtime_error("input must be a game directory or a DAT source path under its _work directory");
  }

static std::string sourceMetadata(const std::string& programSha, const ProgramData::Source& source,
                                  const std::string& ouNativeSha, const std::string& ouSourceSha,
                                  const char* executable) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
  json.StartObject();
  json.Key("program_native_sha256"); json.String(programSha.c_str());
  json.Key("program_source_sha256"); json.String(source.sha256.c_str());
  json.Key("ou_native_sha256");
  if(ouNativeSha.empty())
    json.Null();
  else
    json.String(ouNativeSha.c_str());
  json.Key("ou_source_sha256");
  if(ouSourceSha.empty())
    json.Null();
  else
    json.String(ouSourceSha.c_str());
  json.Key("converter_sha256");
  std::error_code error;
  if(fs::is_regular_file(fs::path(executable),error))
    json.String(BaseImport::sha256File(fs::path(executable)).c_str());
  else
    json.Null();
  json.EndObject();
  return std::string(buffer.GetString(),buffer.GetSize());
  }

static WorldData::ZenFile loadLibrary(const OutputUnits::Index& index, const OutputUnits::Source& source,
                                     std::string& actualSha) {
  auto library = OutputUnits::readLibrary(index.path(source),&actualSha);
  if(actualSha!=source.recordSha256)
    throw std::runtime_error("selected OU record identity differs from its index");
  return library;
  }

static LineTexts loadLines(const WorldData::ZenFile& library) {
  LineTexts lines;
  for(const auto& b:OutputUnits::sortedBlocks(library))
    if(auto msg = OutputUnits::message(*b))
      lines[upper(b->name)] = msg->text;
  return lines;
  }

static int printOuMessages(const OutputUnits::Index& index) {
  std::string text = "{\"format\":\"gothic-ou-messages\",\"version\":1,\"source\":";
  const auto* source = index.serverOuPy();
  if(source==nullptr) {
    text += "null,\"native_sha256\":null,\"messages\":null}";
    } else {
    std::string actualSha;
    const auto library = loadLibrary(index,*source,actualSha);
    text += "{\"path\":"+gothicglb::jsonString(source->path)+",\"sha256\":"+
            gothicglb::jsonString(source->sha256)+"},\"native_sha256\":"+
            gothicglb::jsonString(actualSha)+",\"messages\":[";
    bool first = true;
    for(const auto& message:OutputUnits::messagesInArchiveOrder(library)) {
      if(!first)
        text += ',';
      first = false;
      text += '['+gothicglb::jsonString(message->name)+','+gothicglb::jsonString(message->text)+']';
      }
    text += "]}";
    }
  text += '\n';
  if(std::fwrite(text.data(),1,text.size(),stdout)!=text.size() || std::fflush(stdout)!=0)
    throw std::runtime_error("cannot write OU messages to stdout");
  return 0;
  }

static double pct(size_t a, size_t b) { return b==0 ? 0.0 : 100.0*double(a)/double(b); }

static void printStats(const Model& m, size_t lineTexts) {
  size_t dialogs=0, choices=0, lifted=0, errors=0, effects=0, complexGuards=0, sideEffects=0;
  std::map<std::string,size_t> why, kinds;
  for(auto& n : m.nodes) {
    (n.choice ? choices : dialogs)++;
    errors += !n.error.empty();
    for(auto& e : n.effects) {
      kinds[e.kind]++;
      effects++;
      complexGuards += e.when && ex::opaque(e.when);
      }
    if(n.choice)
      continue;
    sideEffects += !n.conditionEffects.empty();
    if(n.notLifted.empty())
      lifted++;
    else
      why[n.notLifted]++;
    }
  size_t helpersLifted = size_t(std::count_if(m.helpers.begin(), m.helpers.end(),
                                              [](auto& h){ return h.second.notLifted.empty(); }));
  size_t withTopic = size_t(std::count_if(m.quests.begin(), m.quests.end(), [](auto& q){ return !q.second.topics.empty(); }));

  std::printf("nodes: %zu dialogs, %zu choices; subtitles loaded: %zu\n", dialogs, choices, lineTexts);
  std::printf("conditions lifted: %zu (%.1f%%); checking them changes the world in %zu\n", lifted, pct(lifted,dialogs),
              sideEffects);
  std::printf("helpers used by conditions: %zu, lifted %zu\n", m.helpers.size(), helpersLifted);
  std::printf("nodes whose effects were not lifted: %zu\n", errors);
  std::printf("effects: %zu, with a guard that is not fully known: %zu\n", effects, complexGuards);
  std::printf("quests (MIS_*): %zu, with a journal topic %zu; story variables %zu; instance globals %zu\n",
              m.quests.size(), withTopic, m.vars.size(), m.instances.size());

  std::vector<std::pair<size_t,std::string>> v;
  for(auto& [k,c] : why)
    v.emplace_back(c,k);
  std::sort(v.rbegin(), v.rend());
  std::printf("\nnot lifted, by reason:\n");
  for(auto& [c,k] : v)
    std::printf("  %5zu  %s\n", c, k.c_str());

  v.clear();
  for(auto& [k,c] : kinds)
    v.emplace_back(c,k);
  std::sort(v.rbegin(), v.rend());
  std::printf("\neffects by kind:\n");
  for(auto& [c,k] : v)
    std::printf("  %6zu  %s\n", c, k.c_str());
  }

static int run(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // what was printed survives a crash
  const char* datPath = nullptr;
  const char* ouPath  = nullptr;
  const char* json    = nullptr;
  const char* node    = nullptr;
  const char* quest   = nullptr;
  const char* tsvPath = nullptr;
  const char* disasm  = nullptr;
  const char* program = nullptr;
  bool        pretty  = false;
  bool        scan    = false;
  bool        verbose = false;
  bool        effects = false;
  bool        exec    = false;
  bool        ouMessages = false;
  size_t      top     = 0;
  uint32_t    fuzz    = 0;
  std::vector<std::string> saves;
  for(int i=1; i<argc; ++i) {
    auto opt = [&](const char* name) { return std::strcmp(argv[i],name)==0 && i+1<argc; };
    if(opt("--ou"))
      ouPath = argv[++i];
    else if(opt("--json"))
      json = argv[++i];
    else if(opt("--node"))
      node = argv[++i];
    else if(opt("--quest"))
      quest = argv[++i];
    else if(opt("--tsv"))
      tsvPath = argv[++i];
    else if(opt("--disasm"))
      disasm = argv[++i];
    else if(opt("--program"))
      program = argv[++i];
    else if(opt("--top"))
      top = size_t(std::strtoul(argv[++i],nullptr,10));
    else if(std::strcmp(argv[i],"--pretty")==0)
      pretty = true;
    else if(opt("--check"))
      saves.push_back(argv[++i]);
    else if(opt("--fuzz"))
      fuzz = uint32_t(std::strtoul(argv[++i],nullptr,10));
    else if(std::strcmp(argv[i],"--scan")==0)
      scan = true;
    else if(std::strcmp(argv[i],"--verbose")==0)
      verbose = true;
    else if(std::strcmp(argv[i],"--effects")==0)
      effects = true;
    else if(std::strcmp(argv[i],"--exec")==0)
      exec = true;
    else if(std::strcmp(argv[i],"--via-json")==0)
      storygraph::ExecCheck::viaJson = true;
    else if(std::strcmp(argv[i],"--ou-messages")==0)
      ouMessages = true;
    else
      datPath = argv[i];
    }
  if(datPath==nullptr) {
    std::fprintf(stderr, "usage: storygraph <game directory|DAT source path> [--ou native.json] [--ou-messages] "
                         "[--json out.json [--pretty]] [--node ID] "
                         "[--quest MIS_*] [--scan [--tsv out.tsv] [--top N]] [--disasm FN] "
                         "[--check <save dir>... [--verbose]] [--fuzz N] [--effects|--exec [--via-json]] [--program FN]\n");
    return 2;
    }

  const fs::path input(datPath);
  const bool directory = fs::is_directory(input);
  const auto gothic = installationRoot(input,directory);
  // A cold base import writes converter summaries to cout; the export's stdout is only JSON.
  auto* importOutput = ouMessages ? std::cout.rdbuf(std::cerr.rdbuf()) : nullptr;
  zenkit::Logger::set(zenkit::LogLevel::ERROR,[](auto level, const char* component, const char* message) {
    BaseImport::onZenkitLog(level,component,message);
    });
  BaseImport::Files mounted;
  for(const auto& archive:BaseImport::runtimeArchives(gothic))
    if(!archive.mod) mounted.emplace_back(archive.name,archive.size);
  auto base = BaseStore::ensure(gothic,mounted,[](std::string_view message) {
    std::fprintf(stderr,"%.*s\n",int(message.size()),message.data());
    });
  if(importOutput!=nullptr)
    std::cout.rdbuf(importOutput);
  zenkit::Logger::set(zenkit::LogLevel::INFO,{});
  const auto identity = BaseImport::readIdentity(base.path);
  rapidjson::Document metadata;
  metadata.Parse(identity.json.data(),identity.json.size());
  BaseImport::Files archives;
  for(const auto& archive:metadata["archives"].GetArray())
    archives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
  std::sort(archives.begin(),archives.end());
  const auto ouIndex = OutputUnits::readIndex(base.path/"outputunits");
  if(ouIndex.archives!=archives)
    throw std::runtime_error("output units index archives differ from the selected base");
  if(ouMessages)
    return printOuMessages(ouIndex);
  const auto index = ProgramData::readIndex(base.path/"programs");
  if(index.archives!=archives)
    throw std::runtime_error("program index archives differ from the selected base");
  const auto* source = directory ? index.server() : index.findHost(fs::absolute(input).lexically_relative(gothic).generic_string());
  if(source==nullptr || source->origin!="host")
    throw std::runtime_error("selected host program is missing from the base");
  const auto record = index.path(*source);
  const auto recordPath = record.string();
  std::string programSha;
  auto programData = ProgramData::readProgram(record,&programSha);
  if(programSha!=source->recordSha256)
    throw std::runtime_error("selected program record identity differs from its index");
  auto script = zenkit::DaedalusScript::from_program(std::move(programData));
  zenkit::register_all_script_classes(script);
  zenkit::DaedalusVm vm(std::move(script), zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.register_default_external([](const zenkit::DaedalusSymbol&){});
  const zenkit::DaedalusScript& dat = vm;
  Bytecode code(dat);

  if(disasm!=nullptr)
    return code.print(upper(disasm)) ? 0 : 1;
  if(program!=nullptr) {
    // a dialog: its condition and information; otherwise the function itself
    auto* sym = vm.find_symbol_by_name(upper(program));
    if(sym==nullptr) {
      std::fprintf(stderr, "no symbol %s\n", program);
      return 1;
      }
    std::vector<uint32_t> fns = {sym->index()};
    if(code.isInfo(*sym)) {
      auto info = vm.init_instance<zenkit::IInfo>(sym);
      fns.clear();
      for(auto f : {info->condition, info->information})
        if(f!=0)
          fns.push_back(uint32_t(f));
      }
    for(auto f : fns) {
      auto* s = dat.find_symbol_by_index(f);
      std::printf("%s:\n%s\n", s ? ident(s->name()).c_str() : "?", describe(liftProgram(dat, code, f)).c_str());
      }
    return 0;
    }
  if(scan)
    return scanDialogs(vm, code, tsvPath, top);

  LineTexts lines;
  std::string ouNativeSha, ouSourceSha;
  if(ouPath!=nullptr) {
    const fs::path ou(ouPath);
    if(ou.extension()!=".json")
      throw std::runtime_error("--ou requires a native cutscene-library JSON record; invoke "
                               "content-convert outputunits-file <file> <record.json>");
    lines = loadLines(OutputUnits::readLibrary(ou,&ouNativeSha));
    } else {
    const auto relative = fs::path(source->path).parent_path().parent_path()/"CONTENT"/"CUTSCENE"/"OU.BIN";
    if(const auto* ouSource = ouIndex.findHost(relative.generic_string())) {
      lines = loadLines(loadLibrary(ouIndex,*ouSource,ouNativeSha));
      ouSourceSha = ouSource->sha256;
      }
    }
  if(lines.empty())
    throw std::runtime_error("selected base has no storygraph OU subtitles; select a base with the program-relative OU");

  Lifter lifter(dat, code);
  Model  m = buildModel(vm, code, lifter, lines);

  if(!saves.empty() || fuzz>0) {
    auto id = node ? upper(node) : std::string();
    int  r  = 0;
    if(!saves.empty())
      r |= checkSaves(recordPath.c_str(), m, saves, node ? id.c_str() : nullptr, verbose, effects, exec);
    if(fuzz>0 && exec)
      r |= fuzzExec(recordPath.c_str(), m, fuzz, node ? id.c_str() : nullptr, verbose);
    else if(fuzz>0 && effects)
      r |= fuzzEffects(recordPath.c_str(), m, fuzz, node ? id.c_str() : nullptr, verbose);
    else if(fuzz>0)
      r |= fuzzConditions(recordPath.c_str(), m, fuzz, node ? id.c_str() : nullptr, verbose);
    return r;
    }
  if(node!=nullptr) {
    std::fputs(describeNode(m, upper(node)).c_str(), stdout);
    return 0;
    }
  if(quest!=nullptr) {
    std::fputs(describeQuest(m, upper(quest)).c_str(), stdout);
    return 0;
    }
  if(json!=nullptr) {
    auto text = toJson(m, pretty);
    text.insert(1,"\"sources\":"+sourceMetadata(programSha,*source,ouNativeSha,ouSourceSha,argv[0])+",");
    FILE* f = std::fopen(json, "wb");
    if(f==nullptr) {
      std::fprintf(stderr, "can't write %s\n", json);
      return 1;
      }
    const auto written = std::fwrite(text.data(), 1, text.size(), f);
    const auto closed = std::fclose(f);
    if(written!=text.size() || closed!=0) {
      std::fprintf(stderr,"can't write %s\n",json);
      return 1;
      }
    }
  printStats(m, lines.size());
  return 0;
  }

int main(int argc, char** argv) {
  try {
    return run(argc,argv);
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"storygraph: %s\n",error.what());
    return 1;
    }
  }

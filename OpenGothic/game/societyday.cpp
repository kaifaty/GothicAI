// gothic-society-day: plays N days of a `society-world` export (v1 or v2) by the routine on the society stand's core
// (docs/society-sim.md) and writes its journal and 5-minute marks.
//
//   gothic-society-day --world <export.json> --out <dir> [--days N] [--seed S] [--config <config.json>]
//
// <dir>/society-day.journal — the binary journal (common/society/journal.h);
// <dir>/society-marks.csv   — a row per NPC every 5 game minutes from the start.
// With --config (data/society/config.json) people have needs: journal v3 with need_threshold, marks with a level
// column per need. Without it, or with `"needs": []`, the run is the routine-only one byte for byte.
// Prints the load, each day's events and simulation time (advance only, without writing) and the totals.
#include "society/journal.h"
#include "society/society.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point from) {
  return std::chrono::duration<double>(Clock::now()-from).count();
  }

int usage() {
  std::fprintf(stderr,"usage: gothic-society-day --world <export.json> --out <dir> [--days N] [--seed S] [--config <config.json>]\n");
  return 2;
  }

void write(std::ofstream& out, std::string& bytes) {
  out.write(bytes.data(),std::streamsize(bytes.size()));
  if(!out)
    throw std::runtime_error("cannot write the output");
  bytes.clear();
  }

}

int main(int argc, char** argv) {
  std::filesystem::path world, outDir, configPath;
  unsigned long days = 1;
  unsigned long long seed = 0;
  for(int i=1; i<argc; ++i) {
    const bool hasValue = i+1<argc;
    if(std::strcmp(argv[i],"--world")==0 && hasValue)
      world = argv[++i];
    else if(std::strcmp(argv[i],"--out")==0 && hasValue)
      outDir = argv[++i];
    else if(std::strcmp(argv[i],"--days")==0 && hasValue)
      days = std::strtoul(argv[++i],nullptr,10);
    else if(std::strcmp(argv[i],"--seed")==0 && hasValue)
      seed = std::strtoull(argv[++i],nullptr,10);
    else if(std::strcmp(argv[i],"--config")==0 && hasValue)
      configPath = argv[++i];
    else
      return usage();
    }
  if(world.empty() || outDir.empty() || days==0)
    return usage();

  try {
    auto from = Clock::now();
    const auto w = Society::loadWorld(world,configPath);
    std::printf("world %s: %zu bytes, hash %016llx; npcs %zu (people %zu), routine entries %zu, points %zu, "
                "edges %zu, objects %zu; loaded in %.1f ms\n",world.string().c_str(),w.exportBytes,
                (unsigned long long)w.exportHash,w.npcs.size(),w.people,w.routines.size(),w.points.size(),
                w.edgeTo.size(),w.objects.size(),since(from)*1000.0);
    if(w.config.loaded) {
      size_t carded = 0, kinds = 0, freepoints = 0;
      for(uint32_t i=0; i<w.npcs.size(); ++i)
        carded += w.npcs.person[i] && w.npcCard[i] ? 1 : 0;
      for(auto k:w.objectKind)
        kinds += k!=Society::NoKind ? 1 : 0;
      for(auto k:w.pointKind)
        freepoints += k!=Society::NoKind ? 1 : 0;
      std::printf("config %s: %zu bytes, hash %016llx; needs %zu, states %zu, object kinds %zu (objects %zu, freepoints "
                  "%zu), cards %zu (people with one %zu)\n",configPath.string().c_str(),w.config.bytes,
                  (unsigned long long)w.config.hash,w.config.needs.size(),w.config.states.size(),
                  w.config.objectKinds.size(),kinds,freepoints,w.config.cards,carded);
      }

    std::filesystem::create_directories(outDir);
    const auto journalPath = outDir/"society-day.journal";
    const auto marksPath   = outDir/"society-marks.csv";
    std::ofstream journal(journalPath,std::ios::binary), marks(marksPath,std::ios::binary);
    if(!journal || !marks)
      throw std::runtime_error("cannot create the output in "+outDir.string());

    Society::SimConfig config;
    config.seed = seed;
    auto sim = Society::makeSim(w,config);
    Society::DecisionBatch batch;
    std::string jbytes = Society::Journal::header(w,seed);
    std::string mbytes = Society::Marks::header(w);
    Society::Journal::append(jbytes,Society::drainEvents(sim));
    Society::Marks::append(mbytes,sim);

    const Society::Time start = w.startTime();
    const Society::Time marksPerDay = Society::DaySeconds/Society::Marks::Period;
    double   simTotal = 0, writeTotal = 0;
    uint64_t events = sim.s.emitted, journalBytes = 0, rows = 1;
    for(unsigned long day=0; day<days; ++day) {
      const uint64_t eventsBefore = sim.s.emitted;
      double simDay = 0;
      for(Society::Time k=1; k<=marksPerDay; ++k) {
        const auto t = start+(Society::Time(day)*marksPerDay+k)*Society::Marks::Period;
        from = Clock::now();
        Society::advance(sim,batch,t);
        simDay += since(from);
        Society::Journal::append(jbytes,Society::drainEvents(sim));
        Society::Marks::append(mbytes,sim);
        ++rows;
        }
      from = Clock::now();
      journalBytes += jbytes.size();
      write(journal,jbytes);
      write(marks,mbytes);
      writeTotal += since(from);
      simTotal   += simDay;
      events      = sim.s.emitted;
      std::printf("day %lu: %llu events, %.2f ms\n",day+1,(unsigned long long)(sim.s.emitted-eventsBefore),simDay*1000.0);
      std::fflush(stdout);
      }
    journal.close();
    marks.close();
    std::printf("%lu days, seed %llu: %llu events, %llu queue pops, path searches %zu (cached pairs %zu); "
                "simulation %.3f s (%.2f ms per day, %.0f events/s); journal %s %llu bytes, marks %s %llu marks, "
                "write %.2f s\n",
                days,seed,(unsigned long long)events,(unsigned long long)sim.s.processed,sim.paths.searches(),
                sim.paths.cached(),simTotal,simTotal*1000.0/double(days),simTotal>0 ? double(events)/simTotal : 0.0,
                journalPath.string().c_str(),(unsigned long long)journalBytes,marksPath.string().c_str(),
                (unsigned long long)rows,writeTotal);
    return EXIT_SUCCESS;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr,"gothic-society-day: %s\n",e.what());
    return EXIT_FAILURE;
    }
  }

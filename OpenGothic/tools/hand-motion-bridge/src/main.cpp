// hand-motion-bridge: Joy-Con (SDL3) to the game's "hand motion" frames over UDP. README.md tells the rest.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "motion.h"
#include "record.h"
#include "run.h"
#include "sinks.h"

namespace Bridge {
int runSelfTest();
}

namespace {

std::atomic<bool> stopFlag{false};

void onSignal(int) {
  stopFlag.store(true);
  }

void usage(std::FILE* f) {
  std::fprintf(f,
    "usage: hand-motion-bridge [--host 127.0.0.1] [--port 47821] [--record FILE] [--print]\n"
    "       hand-motion-bridge --replay FILE [--loop] [--host H] [--port P] [--print]\n"
    "       hand-motion-bridge --self-test\n"
    "\n"
    "  live (default)  Joy-Con and other gamepads with sensors over SDL3, a frame per sample to the game\n"
    "  --record FILE   also write the raw samples to FILE (CSV)\n"
    "  --replay FILE   send a record at its pace through the same processing\n"
    "  --loop          play the record over and over\n"
    "  --print         print the frames instead of sending them\n"
    "  --self-test     checks without devices, over the loopback only\n");
  }

uint16_t randomSession() {
  std::random_device rd;
  return uint16_t(std::uniform_int_distribution<unsigned>(0,0xFFFF)(rd));
  }

}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", record, replayFile;
  unsigned    port = HandMotion::DefaultPort;
  bool        loop = false, print = false, selfTest = false;
  for(int i=1; i<argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char* what) -> const char* {
      if(i+1>=argc) {
        std::fprintf(stderr,"%s needs %s\n",a.c_str(),what);
        std::exit(2);
        }
      return argv[++i];
      };
    if(a=="--host")
      host = value("an address"); else
    if(a=="--port") {
      const char* v = value("a port");
      char* end = nullptr;
      port = unsigned(std::strtoul(v,&end,10));
      if(end==v || *end!='\0' || port==0 || port>65535) {
        std::fprintf(stderr,"--port: not a port: %s\n",v);
        return 2;
        }
      } else
    if(a=="--record")
      record = value("a file"); else
    if(a=="--replay")
      replayFile = value("a file"); else
    if(a=="--loop")
      loop = true; else
    if(a=="--print")
      print = true; else
    if(a=="--self-test")
      selfTest = true; else
    if(a=="--help" || a=="-h") {
      usage(stdout);
      return 0;
      } else {
      std::fprintf(stderr,"unknown argument: %s\n",a.c_str());
      usage(stderr);
      return 2;
      }
    }

  if(selfTest)
    return Bridge::runSelfTest();
  if(loop && replayFile.empty()) {
    std::fprintf(stderr,"--loop needs --replay\n");
    return 2;
    }
  if(!record.empty() && !replayFile.empty()) {
    std::fprintf(stderr,"--record writes live devices only, not a replay\n");
    return 2;
    }

  std::signal(SIGINT,onSignal);
  std::signal(SIGTERM,onSignal);

  Bridge::UdpSender udp;
  Bridge::PrintSink printer;
  if(!print) {
    std::string err;
    if(!udp.open(host,uint16_t(port),err)) {
      std::fprintf(stderr,"%s\n",err.c_str());
      return 1;
      }
    }
  Bridge::FrameSink& sink = print ? static_cast<Bridge::FrameSink&>(printer) : udp;
  Bridge::Bridge     bridge(randomSession(),sink);
  if(!print)
    std::printf("sending to %s:%u\n",host.c_str(),port);

  int rc = 0;
  if(!replayFile.empty()) {
    std::vector<Bridge::RecordEvent> events;
    std::string err;
    if(!Bridge::readRecord(replayFile,events,err)) {
      std::fprintf(stderr,"%s\n",err.c_str());
      return 1;
      }
    Bridge::StatusLine    line(bridge);
    Bridge::ReplayOptions o;
    o.passes = loop ? 0 : 1;
    o.stop   = &stopFlag;
    if(!print)
      o.tick = [&line]() { line.tick(); };
    Bridge::replay(events,bridge,o);
    } else {
    Bridge::RecordWriter rec;
    if(!record.empty()) {
      std::string err;
      if(!rec.open(record,err)) {
        std::fprintf(stderr,"%s\n",err.c_str());
        return 1;
        }
      }
    rc = Bridge::runLive(bridge,record.empty() ? nullptr : &rec,stopFlag,!print);
    }
  if(udp.errors()>0)
    std::fprintf(stderr,"%llu frames not sent, last error: %s\n",(unsigned long long)udp.errors(),
                 udp.lastError().c_str());
  return rc;
  }

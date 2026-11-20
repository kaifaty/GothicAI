#include "record.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace Bridge {

namespace {

constexpr const char* Header = "hand,arrival_us,sensor_us,gx,gy,gz,ax,ay,az,buttons";

char handChar(HandMotion::Hand h) {
  return h==HandMotion::Left ? 'L' : 'R';
  }

bool parseHand(const std::string& s, HandMotion::Hand& h) {
  if(s=="L") {
    h = HandMotion::Left;
    return true;
    }
  if(s=="R") {
    h = HandMotion::Right;
    return true;
    }
  return false;
  }

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  for(;;) {
    const size_t c = line.find(',',start);
    out.push_back(line.substr(start,c==std::string::npos ? std::string::npos : c-start));
    if(c==std::string::npos)
      return out;
    start = c+1;
    }
  }

bool toInt(const std::string& s, int64_t& v) {
  if(s.empty())
    return false;
  char* end = nullptr;
  errno = 0;
  v = std::strtoll(s.c_str(),&end,10);
  return errno==0 && end!=nullptr && *end=='\0';
  }

bool toFloat(const std::string& s, float& v) {
  if(s.empty())
    return false;
  char* end = nullptr;
  v = std::strtof(s.c_str(),&end);
  return end!=nullptr && *end=='\0';
  }

}

bool readRecord(const std::string& path, std::vector<RecordEvent>& out, std::string& err) {
  std::ifstream in(path);
  if(!in) {
    err = path+": "+std::strerror(errno);
    return false;
    }
  out.clear();
  std::string line;
  size_t      lineNo = 0;
  while(std::getline(in,line)) {
    ++lineNo;
    if(!line.empty() && line.back()=='\r')
      line.pop_back();
    if(line.empty() || line==Header)
      continue;
    if(line[0]=='#') {
      constexpr const char* tag = "# device ";
      const size_t n = std::strlen(tag);
      if(line.compare(0,n,tag)==0 && line.size()>n+2 && line[n+1]==' ') {
        RecordEvent e;
        e.isDevice = true;
        e.device   = line.substr(n+2);
        if(!parseHand(line.substr(n,1),e.hand)) {
          err = path+":"+std::to_string(lineNo)+": bad hand in a device line";
          return false;
          }
        out.push_back(std::move(e));
        }
      continue;
      }
    const auto f = split(line);
    HandMotion::Hand hand = HandMotion::Left;
    int64_t   arrival = 0, sensor = 0, buttons = 0;
    RawSample s;
    bool ok = f.size()==10 && parseHand(f[0],hand) && toInt(f[1],arrival) && toInt(f[2],sensor) &&
              toInt(f[9],buttons) && buttons>=0 && buttons<=0xFFFFFFFFll;
    for(int i=0; ok && i<3; ++i)
      ok = toFloat(f[3+i],s.gyro[i]) && toFloat(f[6+i],s.accel[i]);
    if(!ok) {
      err = path+":"+std::to_string(lineNo)+": not a sample row: "+line;
      return false;
      }
    s.sensorUs = sensor;
    s.buttons  = uint32_t(buttons);
    // rows of one packet follow each other with the same hand and arrival
    if(out.empty() || out.back().isDevice || out.back().packet.hand!=hand || out.back().packet.arrivalUs!=arrival) {
      RecordEvent e;
      e.hand             = hand;
      e.packet.hand      = hand;
      e.packet.arrivalUs = arrival;
      out.push_back(std::move(e));
      }
    out.back().packet.samples.push_back(s);
    }
  return true;
  }

RecordWriter::~RecordWriter() {
  if(f!=nullptr)
    std::fclose(f);
  }

bool RecordWriter::open(const std::string& path, std::string& err) {
  f = std::fopen(path.c_str(),"w");
  if(f==nullptr) {
    err = path+": "+std::strerror(errno);
    return false;
    }
  std::fprintf(f,"# hand-motion-bridge record v1\n%s\n",Header);
  return true;
  }

void RecordWriter::device(HandMotion::Hand hand, const std::string& name) {
  if(f!=nullptr)
    std::fprintf(f,"# device %c %s\n",handChar(hand),name.c_str());
  }

// gyro to 0.0001 rad/s and accel to 0.001 m/s²: below the Joy-Con's step of 0.0012 rad/s and 0.0024 m/s²
void RecordWriter::packet(const Packet& p) {
  if(f==nullptr)
    return;
  for(auto& s:p.samples)
    std::fprintf(f,"%c,%lld,%lld,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%u\n",handChar(p.hand),
                 (long long)p.arrivalUs,(long long)s.sensorUs,
                 s.gyro[0],s.gyro[1],s.gyro[2],s.accel[0],s.accel[1],s.accel[2],unsigned(s.buttons));
  }

void RecordWriter::flush() {
  if(f!=nullptr)
    std::fflush(f);
  }

}

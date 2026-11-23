#include "baseimport.h"
#include "basestoreimport.h"
#include "bink/video.h"
#include "videodata.h"
#include "sha256.h"

#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool condition, const std::string& message) {
  if(!condition) throw std::runtime_error(message);
  }

struct Temporary {
  fs::path path;
  explicit Temporary(const fs::path& parent) {
    fs::create_directories(parent);
    constexpr std::string_view prefix="video-contract-";
    for(const auto& entry:fs::directory_iterator(parent)) {
      const auto name=entry.path().filename().string();
      if(!entry.is_directory() || !name.starts_with(prefix)) continue;
      const auto suffix=std::string_view(name).substr(prefix.size());
      if(suffix.empty() || !std::all_of(suffix.begin(),suffix.end(),[](char c) { return c>='0' && c<='9'; })) continue;
      uint64_t pid=0;
      for(const auto c:suffix) {
        pid=pid*10+uint64_t(c-'0');
        if(pid>uint64_t(std::numeric_limits<pid_t>::max())) break;
        }
      if(pid>0 && pid<=uint64_t(std::numeric_limits<pid_t>::max()) && kill(pid_t(pid),0)<0 && errno==ESRCH)
        fs::remove_all(entry.path());
      }
    path=parent/("video-contract-"+std::to_string(getpid()));
    require(fs::create_directory(path),"temporary directory already exists: "+path.string());
    }
  ~Temporary() { std::error_code error; fs::remove_all(path,error); }
  };

struct Environment {
  std::string key, previous;
  bool existed=false;
  Environment(const char* name, const fs::path& value):key(name) {
    if(const auto* old=std::getenv(name)) { previous=old; existed=true; }
    require(setenv(name,value.c_str(),1)==0,"cannot set test environment");
    }
  ~Environment() {
    if(existed) setenv(key.c_str(),previous.c_str(),1);
    else unsetenv(key.c_str());
    }
  };

class Input final : public Bink::Video::Input {
  public:
    explicit Input(const std::vector<std::byte>& bytes):bytes(bytes) {}
    void read(void* out, size_t size) override {
      require(at<=bytes.size() && size<=bytes.size()-at,"truncated oracle BIK");
      std::memcpy(out,bytes.data()+at,size);
      at+=size;
      }
    void seek(size_t position) override { require(position<=bytes.size(),"oracle BIK seek"); at=position; }
    void skip(size_t size) override { require(at<=bytes.size() && size<=bytes.size()-at,"oracle BIK skip"); at+=size; }
  private:
    const std::vector<std::byte>& bytes;
    size_t at=0;
  };

std::vector<std::byte> readSource(const zenkit::VfsNode& node) {
  const auto input=node.open_read();
  std::vector<std::byte> bytes;
  std::array<std::byte,65536> buffer;
  for(;;) {
    const auto count=input->read(buffer.data(),buffer.size());
    if(count==0) return bytes;
    bytes.insert(bytes.end(),buffer.data(),buffer.data()+count);
    }
  }

// Both pipes carry only the compressed source or one decoded reference frame at a time.
class Ffmpeg {
  public:
    explicit Ffmpeg(const fs::path& executable,const std::vector<std::byte>& bytes,int track=-1,uint32_t sampleRate=0,uint32_t channels=0) {
      const auto selection="0:a:"+std::to_string(track), rate=std::to_string(sampleRate), channelCount=std::to_string(channels);
      int source[2], decoded[2];
      require(pipe(source)==0,"cannot create ffmpeg source pipe");
      if(pipe(decoded)!=0) { close(source[0]); close(source[1]); throw std::runtime_error("cannot create ffmpeg output pipe"); }
      decoder=fork();
      if(decoder<0) {
        close(source[0]); close(source[1]); close(decoded[0]); close(decoded[1]);
        throw std::runtime_error("cannot fork ffmpeg");
        }
      if(decoder==0) {
        if(dup2(source[0],STDIN_FILENO)<0 || dup2(decoded[1],STDOUT_FILENO)<0) _exit(125);
        close(source[0]); close(source[1]); close(decoded[0]); close(decoded[1]);
        if(track>=0) {
          execl(executable.c_str(),"ffmpeg","-hide_banner","-v","error","-nostdin","-threads","1","-filter_threads","1","-i","pipe:0",
                "-map",selection.c_str(),"-vn","-f","s16le","-acodec","pcm_s16le","-ar",rate.c_str(),
                "-ac",channelCount.c_str(),"-threads","1","pipe:1",static_cast<char*>(nullptr));
          _exit(127);
          }
        execl(executable.c_str(),"ffmpeg","-hide_banner","-v","error","-nostdin",
              "-threads","1","-filter_threads","1","-i","pipe:0","-map","0:v:0","-an",
              "-fps_mode","passthrough","-f","rawvideo","-pix_fmt","yuv420p","-threads","1","pipe:1",
              static_cast<char*>(nullptr));
        _exit(127);
        }
      close(source[0]); close(decoded[1]); output=decoded[0];
      writer=fork();
      if(writer<0) {
        close(source[1]); close(output); output=-1; kill(decoder,SIGTERM); waitpid(decoder,nullptr,0); decoder=-1;
        throw std::runtime_error("cannot fork ffmpeg source writer");
        }
      if(writer==0) {
        close(output);
        size_t at=0;
        while(at<bytes.size()) {
          const auto count=::write(source[1],bytes.data()+at,bytes.size()-at);
          if(count<0 && errno==EINTR) continue;
          if(count<=0) _exit(1);
          at+=size_t(count);
          }
        close(source[1]); _exit(0);
        }
      close(source[1]);
      }
    ~Ffmpeg() {
      if(output>=0) close(output);
      if(decoder>=0) { kill(decoder,SIGTERM); while(waitpid(decoder,nullptr,0)<0 && errno==EINTR) {} }
      if(writer>=0) { kill(writer,SIGTERM); while(waitpid(writer,nullptr,0)<0 && errno==EINTR) {} }
      }
    size_t read(void* data,size_t size) {
      auto* bytes=static_cast<uint8_t*>(data); size_t at=0;
      while(at<size) {
        const auto count=::read(output,bytes+at,size-at);
        if(count<0 && errno==EINTR) continue;
        require(count>=0,"cannot read ffmpeg reference");
        if(count==0) break;
        at+=size_t(count);
        }
      return at;
      }
    bool finish() {
      close(output); output=-1;
      const auto wait=[](pid_t& child) {
        int status=0; pid_t result;
        do { result=waitpid(child,&status,0); } while(result<0 && errno==EINTR);
        child=-1;
        return result>=0 && WIFEXITED(status) && WEXITSTATUS(status)==0;
        };
      const bool decoded=wait(decoder), written=wait(writer);
      return decoded && written;
      }
  private:
    pid_t decoder=-1, writer=-1;
    int output=-1;
  };

int64_t heldFrameOf(std::string_view name) {
  return name=="CREDITS_EXTRO.BIK" ? 147 : name=="SHIP.BIK" ? 1048 : -1;
  }

struct PlaneDifference {
  uint64_t frames=0, bytes=0, absoluteDelta=0;
  unsigned maxDelta=0;
  };

void preflightReference(const fs::path& gothic,const fs::path& executable) {
  require(access(executable.c_str(),X_OK)==0,"optional video reference is not executable: "+executable.string());
  zenkit::Vfs vfs; fs::path work;
  BaseImport::mountRuntime(vfs,gothic,work);
  const auto sources=VideoData::sources(gothic);
  require(sources.size()==21,"ffmpeg reference requires all 21 selected BIK files");
  uint64_t totalMatched=0, totalMismatched=0, totalUncompared=0, totalReference=0, totalHeld=0, totalExpected=0;
  unsigned decoderFailures=0, referenceFailures=0, audioFailures=0;
  std::string failureContext;
  std::cout << "video ffmpeg command: " << executable.string() << " -hide_banner -v error -nostdin -threads 1 -filter_threads 1"
               " -i pipe:0 -map 0:v:0 -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p -threads 1 pipe:1\n";
  for(const auto& source:sources) {
    const auto* selected=vfs.find(source.name);
    require(selected!=nullptr,source.name+": missing ffmpeg reference source");
    const auto bytes=readSource(*selected);
    Input input(bytes); Bink::Video oracle(&input,source.name); Ffmpeg reference(executable,bytes);
    const auto width=oracle.frame(0).width(), height=oracle.frame(0).height();
    require(width%2==0 && height%2==0,source.name+": reference expects even YUV420 dimensions");
    const std::array<size_t,3> planeSizes={size_t(width)*height,size_t(width/2)*(height/2),size_t(width/2)*(height/2)};
    const auto frameSize=planeSizes[0]+planeSizes[1]+planeSizes[2];
    totalExpected+=oracle.frameCount();
    std::vector<uint8_t> expected(frameSize), actual(frameSize);
    std::array<std::vector<uint8_t>,4> previousPlanes;
    std::array<PlaneDifference,3> differences;
    std::vector<uint64_t> audioSamples(oracle.audioCount());
    const bool credits=source.name=="CREDITS_EXTRO.BIK", ship=source.name=="SHIP.BIK";
    const auto heldFrame=heldFrameOf(source.name);
    uint64_t matched=0, mismatched=0, decodedFrames=0, referenceFrames=0, referencePartialBytes=0, held=0;
    int64_t firstMismatch=-1, decoderErrorFrame=-1;
    bool heldPlanesEqual=true;
    std::string firstActualHash, firstExpectedHash, decoderError;
    for(uint32_t frame=0;frame<oracle.frameCount();++frame) {
      const auto recoveries=oracle.recoveryCount();
      const Bink::Frame* decoded=nullptr;
      try { decoded=&oracle.nextFrame(); }
      catch(const std::exception& error) { decoderError=error.what(); decoderErrorFrame=frame; break; }
      if(oracle.recoveryCount()!=recoveries+(int64_t(frame)==heldFrame ? 1u : 0u)) {
        decoderError="unexpected recovery location/count"; decoderErrorFrame=frame; break;
        }
      ++decodedFrames;
      for(size_t track=0;track<audioSamples.size();++track) audioSamples[track]+=decoded->audio(uint8_t(track)).samples.size();
      bool heldEqual=true;
      if(int64_t(frame)+1==heldFrame || int64_t(frame)==heldFrame) {
        for(uint8_t plane=0;plane<4;++plane) {
          const auto rows=(((plane==1 || plane==2 ? height/2 : height)+15)/16)*16;
          const auto size=size_t(decoded->plane(plane).stride())*rows;
          const auto* data=decoded->plane(plane).data();
          if(int64_t(frame)==heldFrame)
            heldEqual&=previousPlanes[plane].size()==size && std::memcmp(data,previousPlanes[plane].data(),size)==0;
          else previousPlanes[plane].assign(data,data+size);
          }
        }
      size_t offset=0;
      for(uint8_t plane=0;plane<3;++plane) {
        const auto rows=plane==0 ? height : height/2, columns=plane==0 ? width : width/2;
        for(uint32_t row=0;row<rows;++row)
          std::memcpy(actual.data()+offset+size_t(row)*columns,decoded->plane(plane).data()+size_t(row)*decoded->plane(plane).stride(),columns);
        offset+=planeSizes[plane];
        }
      const bool isHeld=int64_t(frame)==heldFrame;
      if(isHeld) heldPlanesEqual=heldEqual;
      const auto count=isHeld ? frameSize : reference.read(expected.data(),expected.size());
      const bool available=count==frameSize;
      if(!available) referencePartialBytes+=count;
      if(available && !isHeld) ++referenceFrames;
      if(isHeld) ++held;
      const auto actualHash=Sha256::hash({reinterpret_cast<const char*>(actual.data()),actual.size()});
      const auto expectedHash=available ? Sha256::hash({reinterpret_cast<const char*>(expected.data()),expected.size()}) : "missing";
      const bool equal=heldEqual && actualHash==expectedHash;
      if(available && equal) ++matched;
      else if(available) {
        ++mismatched;
        if(firstMismatch<0) { firstMismatch=frame; firstActualHash=actualHash; firstExpectedHash=expectedHash; }
        offset=0;
        for(size_t plane=0;plane<3;++plane) {
          auto& difference=differences[plane]; uint64_t differing=0;
          for(size_t sample=0;sample<planeSizes[plane];++sample) {
            const auto a=actual[offset+sample], b=expected[offset+sample];
            const auto delta=unsigned(a>b ? a-b : b-a);
            if(delta) { ++differing; difference.absoluteDelta+=delta; difference.maxDelta=std::max(difference.maxDelta,delta); }
            }
          if(differing) ++difference.frames;
          difference.bytes+=differing; offset+=planeSizes[plane];
          }
        }
      }
    uint64_t extraBytes=0;
    for(;;) { const auto count=reference.read(expected.data(),expected.size()); if(!count) break; extraBytes+=count; }
    const bool referenceSucceeded=reference.finish();
    referenceFrames+=extraBytes/frameSize; referencePartialBytes+=extraBytes%frameSize;
    const auto uncompared=oracle.frameCount()-matched-mismatched;
    if(!decoderError.empty()) {
      ++decoderFailures;
      failureContext+=" "+source.name+" frame "+std::to_string(decoderErrorFrame)+": "+decoderError+";";
      }
    const bool referenceComplete=referenceSucceeded && referenceFrames+held==oracle.frameCount() && referencePartialBytes==0;
    if(!referenceComplete) { ++referenceFailures; failureContext+=" "+source.name+" ffmpeg frames="+std::to_string(referenceFrames)+";"; }
    const bool complete=decoderError.empty() && referenceComplete;
    const bool luma=differences[0].bytes>0, chroma=differences[1].bytes>0 || differences[2].bytes>0;
    const auto kind=mismatched==0 && complete ? "exact" : luma && chroma ? "luma+chroma" : luma ? "luma" : chroma ? "chroma" : "incomplete-or-hold";
    std::cout << "video ffmpeg: " << source.name << " frames=" << oracle.frameCount() << " decoded_frames=" << decodedFrames
              << " reference_frames=" << referenceFrames << " held_frames=" << held << " held_frame_zero_based=" << heldFrame
              << " held_previous_planes_equal=" << heldPlanesEqual << " recovery_count=" << oracle.recoveryCount()
              << " reference_exit_ok=" << referenceSucceeded << " reference_partial_bytes=" << referencePartialBytes
              << " matched=" << matched << " mismatched=" << mismatched << " uncompared=" << uncompared
              << " comparison=frame_sha256 kind=" << kind << " complete=" << complete
              << " first_mismatch_zero_based=" << firstMismatch << " decoder_error_frame_zero_based=" << decoderErrorFrame;
    if(!decoderError.empty()) std::cout << " decoder_error=\"" << decoderError << '\"';
    for(size_t plane=0;plane<3;++plane) {
      const auto& difference=differences[plane];
      std::cout << ' ' << "YUV"[plane] << "_diff_frames=" << difference.frames << ' ' << "YUV"[plane] << "_diff_bytes=" << difference.bytes
                << ' ' << "YUV"[plane] << "_max_delta=" << difference.maxDelta << ' ' << "YUV"[plane] << "_mean_abs_delta="
                << (difference.bytes ? double(difference.absoluteDelta)/double(difference.bytes) : 0.0);
      }
    if(firstMismatch>=0) std::cout << " first_our_sha256=" << firstActualHash << " first_ffmpeg_sha256=" << firstExpectedHash;
    std::cout << std::endl;
    if(credits || ship) {
      for(size_t track=0;track<audioSamples.size();++track) {
        const auto& audio=oracle.audio(uint8_t(track));
        const uint32_t channels=audio.isMono ? 1u : 2u;
        Ffmpeg referenceAudio(executable,bytes,int(track),audio.sampleRate,channels);
        std::array<uint8_t,65536> buffer; uint64_t audioBytes=0;
        for(;;) { const auto count=referenceAudio.read(buffer.data(),buffer.size()); if(!count) break; audioBytes+=count; }
        const bool audioSucceeded=referenceAudio.finish();
        const auto referenceSamples=audioBytes/2;
        const bool equal=audioSucceeded && audioBytes%2==0 && referenceSamples==audioSamples[track];
        std::cout << "video ffmpeg audio: " << source.name << " track=" << track << " sample_rate=" << audio.sampleRate
                  << " channels=" << channels << " our_samples=" << audioSamples[track] << " reference_samples=" << referenceSamples
                  << " reference_exit_ok=" << audioSucceeded << " sample_count_equal=" << equal << std::endl;
        if(!equal) { ++audioFailures; failureContext+=" "+source.name+" audio track "+std::to_string(track)+" sample count differs;"; }
        }
      }
    totalMatched+=matched; totalMismatched+=mismatched; totalUncompared+=uncompared;
    totalReference+=referenceFrames; totalHeld+=held;
    }
  std::cout << "video ffmpeg: 21/21 checked_frames=" << totalMatched+totalMismatched << " matched_frames=" << totalMatched
            << " mismatched_frames=" << totalMismatched << " uncompared_frames=" << totalUncompared << " reference_frames=" << totalReference
            << " held_frames=" << totalHeld << " decoder_failures=" << decoderFailures << " reference_failures=" << referenceFailures
            << " audio_count_failures=" << audioFailures << std::endl;
  require(decoderFailures==0 && referenceFailures==0 && audioFailures==0,
          "needs_decision: decoder failures="+std::to_string(decoderFailures)+", ffmpeg incomplete="+
          std::to_string(referenceFailures)+", audio count failures="+std::to_string(audioFailures)+";"+failureContext);
  require(totalMatched==totalExpected && totalMismatched==0 && totalUncompared==0 && totalReference+totalHeld==totalExpected,
          "optional ffmpeg reference differs: expected "+std::to_string(totalExpected)+" checked source frames");
  }

std::map<std::string,std::string> hashes(const fs::path& root) {
  std::map<std::string,std::string> result;
  for(const auto& entry:fs::recursive_directory_iterator(root))
    if(entry.is_regular_file())
      result.emplace(entry.path().lexically_relative(root).generic_string(),BaseImport::sha256File(entry.path()));
  return result;
  }

uint64_t directorySize(const fs::path& root) {
  uint64_t bytes=0;
  for(const auto& entry:fs::recursive_directory_iterator(root))
    if(entry.is_regular_file()) bytes+=entry.file_size();
  return bytes;
  }

int16_t pcm(float value) {
  return value < -1.00004566f ? int16_t(-32768) :
         value > 1.00001514f ? int16_t(32767) : int16_t(value*32767.5f);
  }

uint32_t alignedRows(uint32_t height) { return ((height+15)/16)*16; }

void compareCorpus(const fs::path& gothic, const fs::path& part) {
  zenkit::Vfs vfs;
  fs::path work;
  BaseImport::mountRuntime(vfs,gothic,work);
  const auto entries=VideoData::readIndex(part);
  require(entries.size()==21,"expected 21 selected videos, got "+std::to_string(entries.size()));
  uint64_t totalFrames=0, totalSamples=0;
  double totalSeconds=0;
  for(const auto& entry:entries) {
    const auto* selected=vfs.find(entry.source.name);
    require(selected!=nullptr,entry.source.name+": missing oracle source");
    const auto bytes=readSource(*selected);
    require(bytes.size()==entry.source.size,entry.source.name+": source size");
    require(BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()))==entry.source.sha256,
            entry.source.name+": selected source SHA-256");
    Input input(bytes), repeatedInput(bytes);
    Bink::Video oracle(&input,entry.source.name), repeated(&repeatedInput,entry.source.name);
    VideoData::Reader native(part/entry.file);
    native.verifyPayload();
    const auto& info=native.info();
    require(info.frames==oracle.frameCount() && info.frames==entry.info.frames,entry.source.name+": frame count");
    require(info.fpsNum==oracle.fps().num && info.fpsDen==oracle.fps().den,entry.source.name+": frame rate");
    require(info.tracks.size()==oracle.audioCount(),entry.source.name+": track count");
    std::vector<uint64_t> samples(info.tracks.size());
    std::vector<std::pair<uint32_t,VideoData::Frame>> seekFrames;
    const auto heldFrame=heldFrameOf(entry.source.name);
    std::array<std::vector<uint8_t>,4> previousPlanes;
    for(size_t track=0;track<samples.size();++track) {
      const auto& expected=oracle.audio(uint8_t(track));
      require(info.tracks[track].sampleRate==expected.sampleRate &&
              info.tracks[track].channels==(expected.isMono ? 1u : 2u),entry.source.name+": audio format");
      }
    for(uint32_t frame=0;frame<info.frames;++frame) {
      const auto recoveries=oracle.recoveryCount(), repeatedRecoveries=repeated.recoveryCount();
      const auto& expected=oracle.nextFrame();
      const auto& again=repeated.nextFrame();
      const auto actual=native.nextFrame();
      if(frame%std::max(1u,info.frames/7)==0 || frame+1==info.frames) seekFrames.emplace_back(frame,actual);
      const auto location=entry.source.name+" frame "+std::to_string(frame);
      const auto recovered=int64_t(frame)==heldFrame ? 1u : 0u;
      require(oracle.recoveryCount()==recoveries+recovered && repeated.recoveryCount()==repeatedRecoveries+recovered,
              location+": unexpected Bink recovery location/count");
      require(info.width==expected.width() && info.height==expected.height(),location+": dimensions");
      require(actual.audio.size()==info.tracks.size(),location+": audio tracks");
      for(uint8_t plane=0;plane<4;++plane) {
        const auto& source=expected.plane(plane);
        const auto rows=alignedRows(plane==1 || plane==2 ? info.height/2 : info.height);
        const auto size=size_t(source.stride())*rows;
        require(info.planes[plane].stride==source.stride() && info.planes[plane].rows==rows,
                location+": plane layout "+std::to_string(plane));
        if(std::memcmp(source.data(),again.plane(plane).data(),size)!=0)
          throw std::runtime_error("needs_decision: nondeterministic common/bink: "+location+" plane "+std::to_string(plane));
        require(actual.planes[plane].size()==size && std::memcmp(actual.planes[plane].data(),source.data(),size)==0,
                location+": plane bytes "+std::to_string(plane));
        if(int64_t(frame)+1==heldFrame) previousPlanes[plane]=actual.planes[plane];
        if(int64_t(frame)==heldFrame)
          require(actual.planes[plane]==previousPlanes[plane],location+": recovered frame must repeat previous plane "+std::to_string(plane));
        }
      for(size_t track=0;track<samples.size();++track) {
        const auto& expectedSamples=expected.audio(uint8_t(track)).samples;
        const auto& againSamples=again.audio(uint8_t(track)).samples;
        require(expectedSamples.size()==againSamples.size(),location+": nondeterministic source audio size");
        require(actual.audio[track].size()==expectedSamples.size(),location+": PCM sample count");
        for(size_t sample=0;sample<expectedSamples.size();++sample) {
          const auto value=pcm(expectedSamples[sample]);
          if(value!=pcm(againSamples[sample]))
            throw std::runtime_error("needs_decision: nondeterministic common/bink PCM: "+location+" track "+
                                     std::to_string(track)+" sample "+std::to_string(sample));
          if(actual.audio[track][sample]!=value)
            throw std::runtime_error(location+": PCM track "+std::to_string(track)+" sample "+std::to_string(sample));
          }
        samples[track]+=expectedSamples.size();
        }
      }
    require(oracle.recoveryCount()==(heldFrame>=0 ? 1u : 0u),entry.source.name+": recovery total");
    for(auto saved=seekFrames.rbegin();saved!=seekFrames.rend();++saved) {
      const auto sought=native.readFrame(saved->first);
      require(sought.planes==saved->second.planes && sought.audio==saved->second.audio,
              entry.source.name+": indexed frame differs from sequential frame "+std::to_string(saved->first));
      }
    uint64_t fileSamples=0;
    for(size_t track=0;track<samples.size();++track) {
      require(samples[track]==info.tracks[track].samples,entry.source.name+": track sample total");
      fileSamples+=samples[track];
      }
    const auto seconds=double(info.frames)*info.fpsDen/info.fpsNum;
    totalFrames+=info.frames; totalSamples+=fileSamples; totalSeconds+=seconds;
    std::cout << "video corpus: " << entry.source.name << " frames=" << info.frames
              << " fps=" << info.fpsNum << '/' << info.fpsDen << " size=" << info.width << 'x' << info.height
              << " duration_s=" << seconds << " pcm_samples=" << fileSamples << " bytes=" << entry.size;
    for(size_t track=0;track<samples.size();++track)
      std::cout << " track" << track << '=' << info.tracks[track].sampleRate << "Hz/"
                << info.tracks[track].channels << "ch/" << samples[track] << "samples";
    std::cout << '\n';
    }
  std::cout << "video corpus: 21/21 frames=" << totalFrames << " duration_s=" << totalSeconds
            << " pcm_samples=" << totalSamples << " all planes and samples equal; two decoder runs equal\n";
  }

class OneCore {
  public:
    OneCore() {
#ifdef __linux__
      require(sched_getaffinity(0,sizeof(previous),&previous)==0,"cannot read benchmark CPU affinity");
      int chosen=-1;
      for(int cpu=0;cpu<CPU_SETSIZE;++cpu)
        if(CPU_ISSET(cpu,&previous) && cpu!=0 && cpu!=1 && cpu!=16 && cpu!=17) { chosen=cpu; break; }
      require(chosen>=0,"no benchmark CPU outside reserved body cores");
      cpu_set_t single; CPU_ZERO(&single); CPU_SET(chosen,&single);
      require(sched_setaffinity(0,sizeof(single),&single)==0,"cannot pin frame benchmark to one core");
#endif
      }
    ~OneCore() {
#ifdef __linux__
      sched_setaffinity(0,sizeof(previous),&previous);
#endif
      }
  private:
#ifdef __linux__
    cpu_set_t previous;
#endif
  };

void benchmark(const fs::path& part) {
  const auto entries=VideoData::readIndex(part);
  OneCore affinity;
  for(const auto* name:{"CREDITS2.BIK","INTRO.BIK","SHIP.BIK"}) {
    const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry) { return entry.source.name==name; });
    require(found!=entries.end() && found->info.width==640 && found->info.height==360,"missing 640x360 benchmark video");
    VideoData::Reader reader(part/found->file);
    constexpr uint32_t count=512;
    std::vector<double> timings; timings.reserve(count);
    uint64_t consumed=0;
    for(uint32_t i=0;i<count;++i) {
      const auto frame=(i*7919u)%found->info.frames;
      const auto start=Clock::now();
      const auto decoded=reader.readFrame(frame);
      timings.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
      consumed+=decoded.planes[0].size();
      }
    double total=0;
    for(const auto ms:timings) total+=ms;
    std::sort(timings.begin(),timings.end());
    const auto average=total/count;
    std::cout << "video frame benchmark: " << found->source.name << " 640x360 one_core=1 random_frames=" << count
              << " mean_ms=" << average << " p95_ms=" << timings[size_t(count)*95/100]
              << " max_ms=" << timings.back() << " consumed_bytes=" << consumed << '\n';
    require(timings.back()<=10,"needs_decision: 640x360 frame read exceeds 10 ms: "+std::to_string(timings.back()));
    }
  }

void flipLastByte(const fs::path& file) {
  std::fstream output(file,std::ios::binary|std::ios::in|std::ios::out);
  output.seekg(-1,std::ios::end);
  const auto previous=output.get();
  require(previous!=std::char_traits<char>::eof(),"cannot read mutation byte");
  output.seekp(-1,std::ios::end); output.put(char(previous^1));
  require(bool(output),"cannot write mutation byte");
  }

void concurrentFirstPublications(const fs::path& gothic,const fs::path& part,const fs::path& root,
                                 const VideoData::Identity& identity,const std::map<std::string,std::string>& expected) {
  const auto store=root/"store", stored=store/"base/videos"/identity.fingerprint;
  const std::array<fs::path,2> caches={root/"cache-a",root/"cache-b"};
  for(const auto& cache:caches) {
    const auto destination=cache/"videos"/identity.fingerprint;
    fs::create_directories(destination);
    for(const auto& entry:fs::directory_iterator(part))
      if(entry.is_regular_file()) fs::create_hard_link(entry.path(),destination/entry.path().filename());
    }
  fs::create_directories(stored.parent_path());
  // Both publishers may already hold a shared lease before the first part exists.
  const int use=open((stored.parent_path()/(identity.fingerprint+".use")).c_str(),O_CREAT|O_RDWR,0600);
  require(use>=0,"cannot open concurrent publication lease");
  if(flock(use,LOCK_SH)!=0) { close(use); throw std::runtime_error("cannot hold concurrent publication lease"); }
  int gate[2];
  if(pipe(gate)!=0) { close(use); throw std::runtime_error("cannot create concurrent publication gate"); }
  std::array<pid_t,2> children={-1,-1};
  std::cout.flush(); std::cerr.flush();
  bool started=true;
  for(size_t i=0;i<children.size();++i) {
    children[i]=fork();
    if(children[i]<0) { started=false; break; }
    if(children[i]==0) {
      close(use); close(gate[1]);
      const auto log=root/("publisher-"+std::to_string(i)+".log");
      const int error=open(log.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0600);
      if(error<0 || dup2(error,STDERR_FILENO)<0) _exit(2);
      close(error);
      char token=0;
      ssize_t count;
      do { count=::read(gate[0],&token,1); } while(count<0 && errno==EINTR);
      close(gate[0]);
      if(count!=1) _exit(3);
      try {
        Environment cacheEnvironment("OPENGOTHIC_BASE_CACHE",caches[i]), storeEnvironment("OPENGOTHIC_BASE_STORE",store);
        auto selected=BaseStore::ensureVideos(gothic,[](std::string_view) {});
        require(selected.path==caches[i]/"videos"/identity.fingerprint,"concurrent publication did not use its own cache");
        if(selected.publication.valid()) selected.publication.get();
        _exit(0);
        }
      catch(const std::exception& error) { std::cerr << error.what() << std::endl; _exit(1); }
      }
    }
  close(gate[0]);
  const char tokens[2]={1,1};
  const auto released=started ? ::write(gate[1],tokens,sizeof(tokens)) : -1;
  close(gate[1]);
  bool succeeded=started && released==ssize_t(sizeof(tokens));
  for(const auto child:children) {
    if(child<=0) continue;
    int status=0; pid_t result;
    do { result=waitpid(child,&status,0); } while(result<0 && errno==EINTR);
    succeeded&=result==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    }
  close(use);
  require(succeeded,"concurrent first video publication process failed");
  for(size_t i=0;i<children.size();++i) {
    std::ifstream log(root/("publisher-"+std::to_string(i)+".log"));
    const std::string events{std::istreambuf_iterator<char>(log),std::istreambuf_iterator<char>()};
    require(events.find("store publication failed")==std::string::npos,"concurrent first video publication: "+events);
    }
  require(VideoData::matches(stored,identity) && hashes(stored)==expected,"concurrent first store copy differs");
  fs::remove_all(root);
  std::cout << "video concurrent publication: two caches, empty shared store, both publishers succeeded\n";
  }

void gameSelection(const fs::path& game, const fs::path& mainBase, const fs::path& root,
                   const fs::path& part, const VideoData::Identity& identity) {
  const auto cache=root/"cache", store=root/"game-store";
  const auto destination=cache/"videos"/identity.fingerprint;
  require(VideoData::identifyFromBase(mainBase).json==VideoData::identify(game).json,
          "native source census and original video selection identities differ");
  size_t imports=0, mainSelections=0;
  const auto observe=[&](std::string_view message) {
    if(message=="video base: importing installation") ++imports;
    if(message.starts_with("base: ")) ++mainSelections;
    };
  const auto checkPointer=[&] {
    const auto pointer=cache/"current"/(BaseImport::sha256(fs::weakly_canonical(game).generic_string())+".json");
    std::ifstream input(pointer,std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    require(bytes.find("\"videos\"")!=std::string::npos && bytes.find("\"base\"")==std::string::npos,
            "game video selection rewrote the main base pointer");
    require(imports==0 && mainSelections==0,"game video selection imported or selected the main base again");
    };
  fs::remove_all(cache/"current");
  {
    auto selected=BaseStore::resolveVideos(game,mainBase,observe);
    require(selected.path==destination,"game video selection without a pointer missed the prepared cache");
    checkPointer();
    }
  {
    Environment storeEnvironment("OPENGOTHIC_BASE_STORE",store);
    fs::create_directories(store/"base/videos");
    fs::rename(destination,store/"base/videos"/identity.fingerprint);
    fs::remove_all(cache/"current");
    {
      auto selected=BaseStore::resolveVideos(game,mainBase,observe);
      require(selected.path==destination && hashes(destination)==hashes(part),
              "game video selection without a pointer did not copy the prepared store part");
      checkPointer();
      }
    fs::remove_all(destination);
    fs::remove_all(store);
    fs::remove_all(cache/"current");
    std::string reason;
    try { (void)BaseStore::resolveVideos(game,mainBase,observe); }
    catch(const std::exception& error) { reason=error.what(); }
    require(reason.find("no ready video base")!=std::string::npos && imports==0 && mainSelections==0,
            "game video cache/store miss must explain the skip without importing: "+reason);
    for(const auto& directory:{cache/"videos",store/"base/videos"})
      for(const auto& entry:fs::directory_iterator(directory))
        require(!entry.is_directory(),"game video cache/store miss created an import directory");
    }
  // Restore the converter-prepared part without another corpus conversion.
  fs::create_directories(destination);
  for(const auto& entry:fs::directory_iterator(part))
    if(entry.is_regular_file()) fs::create_hard_link(entry.path(),destination/entry.path().filename());
  fs::remove_all(cache/"current");
  const int held=::open((cache/".lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
  require(held>=0 && flock(held,LOCK_EX)==0,"cannot hold the main base cache lock");
  std::cout.flush(); std::cerr.flush();
  const auto child=fork(); require(child>=0,"game video cancellation fork failed");
  if(child==0) {
    close(held);
    alarm(2);
    const auto cancelled=std::make_shared<std::atomic_bool>(false);
    std::atomic_bool identifying=false, resume=false;
    std::atomic<size_t> mainEvents=0;
    std::thread worker([&] {
      try {
        auto selected=BaseStore::resolveVideos(game,mainBase,[&](std::string_view message) {
          if(message=="video base: identifying installation") {
            identifying.store(true);
            while(!resume.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          if(message.starts_with("base: ")) ++mainEvents;
          },cancelled);
        }
      catch(const std::exception&) {}
      });
    const auto deadline=Clock::now()+std::chrono::milliseconds(500);
    while(!identifying.load() && Clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // The previous implementation enters the uninterruptible main cache lock after this event.
    const auto started=Clock::now();
    cancelled->store(true);
    resume.store(true);
    worker.join();
    const auto seconds=std::chrono::duration<double>(Clock::now()-started).count();
    std::cout << "game video cancellation with held main cache lock: " << seconds << " s" << std::endl;
    _exit(identifying.load() && mainEvents.load()==0 && seconds<=1 ? 0 : 1);
    }
  int status=0;
  const bool completed=waitpid(child,&status,0)==child;
  close(held);
  require(completed && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "game video cancellation waited for the held main base cache lock (limit 1 s)");
  std::cout << "game video selection: native identity, cache/store without pointers, no-import miss and cancelled main-lock independence verified\n";
  }

void snapshotSelection(const fs::path& gothic, const fs::path& root, const fs::path& part,
                       const VideoData::Identity& identity) {
  const auto game=root/"game", cache=root/"cache";
  fs::create_directories(game);
  fs::path mutableVideo;
  for(const auto& directory:fs::directory_iterator(gothic)) {
    auto name=directory.path().filename().string();
    for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
    if(name!="DATA" && name!="_WORK") continue;
    fs::create_directories(game/directory.path().filename());
    for(const auto& entry:fs::recursive_directory_iterator(directory.path())) {
      const auto target=game/entry.path().lexically_relative(gothic);
      if(entry.is_directory()) { fs::create_directories(target); continue; }
      fs::create_symlink(fs::absolute(entry.path()),target);
      auto extension=entry.path().extension().string();
      for(auto& c:extension) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(extension==".BIK" && (mutableVideo.empty() || entry.file_size()<fs::file_size(mutableVideo))) mutableVideo=target;
      }
    }
  require(!mutableVideo.empty(),"video snapshot fixture needs a loose BIK");
  const auto videoSource=fs::read_symlink(mutableVideo);
  fs::remove(mutableVideo); fs::copy_file(videoSource,mutableVideo);
  const auto destination=cache/"videos"/identity.fingerprint;
  fs::create_directories(destination);
  for(const auto& entry:fs::directory_iterator(part))
    if(entry.is_regular_file()) fs::create_hard_link(entry.path(),destination/entry.path().filename());
  Environment cacheEnvironment("OPENGOTHIC_BASE_CACHE",cache), storeEnvironment("OPENGOTHIC_BASE_STORE","");
  Environment baseEnvironment("OPENGOTHIC_BASE","");
  size_t identifies=0;
  const auto observe=[&](std::string_view message) {
    if(message=="video base: identifying installation") ++identifies;
    };
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies>0,
          "first video selection must identify before writing its pointer");
  identifies=0;
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies==0,
          "matching video snapshot must skip source identification and BIK hashing");
  {
    Environment explicitBase("OPENGOTHIC_BASE",root/"explicit-main");
    require(BaseStore::ensureVideos(game,observe).path==destination && identifies==0,
            "explicit main base must retain the video's snapshot selection");
  }
  fs::last_write_time(mutableVideo,fs::last_write_time(mutableVideo)+std::chrono::seconds(1));
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies>0,
          "changed BIK mtime must take the full video identification path");
  // Main and video use the same pointer document and must retain each other's update.
  const auto mainIdentity=BaseImport::identify(game);
  const auto mainBase=cache/mainIdentity.fingerprint;
  fs::create_directories(mainBase/"sources");
  require(BaseImport::convertSources(game,mainBase/"sources",false)==0,"cannot prepare native source census fixture");
  BaseImport::writeBaseMetadata(mainBase,mainIdentity);
  gameSelection(game,mainBase,root,part,identity);
  BaseImport::Files mounted;
  for(const auto& archive:BaseImport::runtimeArchives(game))
    if(!archive.mod) mounted.emplace_back(archive.name,archive.size);
  // A cold video import must not hold the lock needed to select a ready main base.
  {
    const auto coldCache=root/"cold-cache", coldBase=coldCache/mainIdentity.fingerprint;
    fs::create_directories(coldCache);
    fs::copy(mainBase,coldBase,fs::copy_options::recursive);
    Environment coldEnvironment("OPENGOTHIC_BASE_CACHE",coldCache);
    require(BaseStore::resolve(game,mounted,[](std::string_view) {}).path==coldBase,
            "cold video fixture did not prime the ready main pointer");
    int importing[2], release[2];
    require(pipe(importing)==0 && pipe(release)==0,"cold video import pipes failed");
    std::cout.flush(); std::cerr.flush();
    const auto video=fork(); require(video>=0,"cold video import fork failed");
    if(video==0) {
      close(importing[0]); close(release[1]); alarm(30);
      try {
        (void)BaseStore::ensureVideos(game,[&](std::string_view message) {
          if(message!="video base: importing installation") return;
          require(::write(importing[1],"x",1)==1,"cold video import signal failed");
          char token=0;
          require(::read(release[0],&token,1)==1,"cold video import gate failed");
          throw std::runtime_error("cold video import gate released");
          });
        }
      catch(const std::exception& error) {
        _exit(std::string_view(error.what())=="cold video import gate released" ? 0 : 1);
        }
      _exit(2);
      }
    close(importing[1]); close(release[0]);
    char token=0;
    const bool started=::read(importing[0],&token,1)==1;
    close(importing[0]);
    pid_t main=-1;
    int mainStatus=0;
    bool mainCompleted=false;
    if(started) {
      main=fork();
      if(main==0) {
        close(release[1]); alarm(5);
        try {
          auto selected=BaseStore::resolve(game,mounted,[](std::string_view) {});
          _exit(selected.path==coldBase ? 0 : 1);
          }
        catch(...) { _exit(1); }
        }
      if(main>0) mainCompleted=waitpid(main,&mainStatus,0)==main;
      require(::write(release[1],"x",1)==1,"cold video import release failed");
      }
    close(release[1]);
    int videoStatus=0;
    const bool videoCompleted=waitpid(video,&videoStatus,0)==video;
    require(started && mainCompleted && WIFEXITED(mainStatus) && WEXITSTATUS(mainStatus)==0,
            "ready main base selection waited for the cold video import");
    require(videoCompleted && WIFEXITED(videoStatus) && WEXITSTATUS(videoStatus)==0,
            "cold video import did not stop at its gate");
    std::cout << "cold video import: ready main base selected before import gate release\n";
  }
  int gate[2]; require(pipe(gate)==0,"combined pointer gate failed");
  std::cout.flush(); std::cerr.flush();
  pid_t children[2];
  for(size_t i=0;i<2;++i) {
    children[i]=fork(); require(children[i]>=0,"combined pointer fork failed");
    if(children[i]==0) {
      close(gate[1]); char token=0;
      if(::read(gate[0],&token,1)!=1) _exit(2);
      close(gate[0]);
      try {
        if(i==0) (void)BaseStore::resolve(game,mounted,[](std::string_view) {});
        else (void)BaseStore::ensureVideos(game,[](std::string_view) {});
        _exit(0);
        }
      catch(...) { _exit(1); }
      }
    }
  close(gate[0]); require(::write(gate[1],"xx",2)==2,"combined pointer gate write failed"); close(gate[1]);
  for(const auto child:children) {
    int status=0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "combined pointer writer failed");
    }
  identifies=0;
  fs::path data;
  for(const auto& entry:fs::directory_iterator(game)) {
    auto name=entry.path().filename().string();
    for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
    if(name=="DATA") data=entry.path();
    }
  require(!data.empty(),"video snapshot fixture has no Data");
  fs::rename(data,game/"saved-Data");
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies==0,
          "installation-free video selection must reuse its pointer without source hashing");
  require(BaseStore::resolve(game,{},[](std::string_view) {}).path==mainBase,
          "concurrent main/video pointer writes must retain both parts for offline selection");
  fs::remove_all(destination);
  std::string reason;
  try { (void)BaseStore::ensureVideos(game,observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(!reason.empty(),"pruned video pointer without installation must explain the unavailable part");
  const auto empty=root/"no-pointer";
  fs::create_directory(empty);
  reason.clear();
  try { (void)BaseStore::ensureVideos(empty,observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(!reason.empty(),"video without installation or pointer must retain a skip reason");
  std::cout << "video snapshots: zero source identifies for repeat/explicit/offline; BIK mtime refresh; concurrent main/video pointer; missing and pruned pointer reasons verified\n";
  }

void fingerprintSelection(const fs::path& root, const fs::path& part, const VideoData::Identity& identity) {
  const auto game=root/"game", cache=root/"cache", store=root/"store";
  const auto destination=cache/"videos"/identity.fingerprint;
  fs::create_directories(game);
  fs::create_directories(destination);
  // Keep the large immutable payload linked; mutations below affect only the copied index.
  for(const auto& entry:fs::directory_iterator(part)) {
    const auto target=destination/entry.path().filename();
    if(entry.path().filename()=="videos.json") fs::copy_file(entry.path(),target);
    else if(entry.is_regular_file()) fs::create_hard_link(entry.path(),target);
    }
  Environment cacheEnvironment("OPENGOTHIC_BASE_CACHE",cache), storeEnvironment("OPENGOTHIC_BASE_STORE",store);
  Environment fingerprintEnvironment("OPENGOTHIC_VIDEOS_FINGERPRINT",identity.fingerprint);
  size_t identifies=0;
  const auto observe=[&](std::string_view message) {
    if(message=="video base: identifying installation" || message=="video base: importing installation") ++identifies;
    };
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies==0,
          "video fingerprint must select cache without an installation or identification");
  const auto pointer=cache/"current"/(BaseImport::sha256(fs::weakly_canonical(game).generic_string())+".json");
  const auto checkPointer=[&] {
    std::ifstream input(pointer,std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    require(bytes.find(identity.fingerprint)!=std::string::npos && bytes.find(destination.string())!=std::string::npos,
            "video fingerprint selection must write its game-directory pointer");
    };
  checkPointer();
  fs::create_directories(store/"base/videos");
  fs::rename(destination,store/"base/videos"/identity.fingerprint);
  require(BaseStore::ensureVideos(game,observe).path==destination && identifies==0,
          "video fingerprint must copy store into cache without an installation");
  require(hashes(destination)==hashes(store/"base/videos"/identity.fingerprint),"fingerprint video copy differs");
  checkPointer();
  fs::remove_all(store);
  { std::ofstream broken(destination/"videos.json",std::ios::binary|std::ios::app); broken << ' '; }
  std::string reason;
  try { (void)BaseStore::ensureVideos(game,observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(reason.find("index SHA-256")!=std::string::npos && identifies==0,
          "video fingerprint must reject a corrupt index without importing: "+reason);
  fs::remove_all(destination);
  reason.clear();
  try { (void)BaseStore::ensureVideos(game,observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(reason.find(identity.fingerprint)!=std::string::npos && identifies==0,
          "missing video fingerprint must fail clearly without importing: "+reason);
  {
    Environment disabledVideos("OPENGOTHIC_VIDEOS_FINGERPRINT","");
    reason.clear();
    try { (void)BaseStore::ensureVideos(game,observe); }
    catch(const std::exception& error) { reason=error.what(); }
    require(reason.find("video selection disabled")!=std::string::npos && identifies==0,
            "explicit empty video fingerprint must disable selection without installation access: "+reason);
    }
  std::cout << "video fingerprint selection: cache/store, pointer, missing/corrupt refusal and explicit-empty skip without source identification\n";
  }

void identityInputs(const fs::path& gothic, const fs::path& root, const VideoData::Identity& identity) {
  fs::create_directories(root);
  fs::path nonvideo, video;
  for(const auto& entry:fs::recursive_directory_iterator(gothic)) {
    const auto target=root/entry.path().lexically_relative(gothic);
    if(entry.is_directory()) { fs::create_directories(target); continue; }
    fs::create_symlink(fs::absolute(entry.path()),target);
    auto name=entry.path().filename().string(), parent=entry.path().parent_path().filename().string();
    for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
    for(auto& c:parent) c=char(std::toupper(static_cast<unsigned char>(c)));
    if(name=="GOTHIC.DAT" && parent=="_COMPILED") nonvideo=target;
    if(entry.path().extension()==".BIK" || entry.path().extension()==".bik")
      if(video.empty() || entry.file_size()<fs::file_size(video)) video=target;
    }
  require(!nonvideo.empty() && !video.empty(),"fingerprint fixture lacks DAT or BIK");
  require(VideoData::identify(root).json==identity.json,"mirrored video identity changed");
  const auto mainBefore=BaseImport::identify(root);
  const auto mutableCopy=[&](const fs::path& path) {
    const auto source=fs::read_symlink(path);
    fs::remove(path); fs::copy_file(source,path);
    flipLastByte(path);
    };
  mutableCopy(nonvideo);
  require(VideoData::identify(root).json==identity.json,"nonvideo DAT changed video fingerprint");
  require(BaseImport::identify(root).fingerprint!=mainBefore.fingerprint,"nonvideo DAT did not change main fingerprint");
  mutableCopy(video);
  require(VideoData::identify(root).fingerprint!=identity.fingerprint,"selected BIK change did not change video fingerprint");
  std::cout << "video identities: nonvideo DAT changes main only; selected BIK changes video; format_sha256="
            << VideoData::formatSha256() << '\n';
  }
}

int main() {
  try {
    std::cout << std::fixed << std::setprecision(3);
    const auto* game=std::getenv("GOTHIC_DIR");
    require(game && *game,"GOTHIC_DIR must point to the real Gothic II installation");
    unsetenv("OPENGOTHIC_BASE_FINGERPRINT");
    unsetenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
    if(const auto* reference=std::getenv("OPENGOTHIC_VIDEO_FFMPEG"); reference && *reference) {
      preflightReference(game,reference);
      return 0;
      }
    const auto* home=std::getenv("HOME");
    require(home && *home,"HOME must point to the user's directory");
    const auto* configuredCache=std::getenv("OPENGOTHIC_BASE_CACHE");
    Temporary temporary(configuredCache && *configuredCache ? fs::absolute(configuredCache).parent_path() : fs::path(home)/"gothic-ai");
    const auto cache=temporary.path/"cache", store=temporary.path/"store";
    Environment cacheEnvironment("OPENGOTHIC_BASE_CACHE",cache), storeEnvironment("OPENGOTHIC_BASE_STORE",store);
    const auto identity=VideoData::identify(game);
    require(VideoData::sources(game).size()==21,"expected 21 selected BIK sources");
    std::string events;
    Clock::time_point importStarted, importFinished;
    bool sawImport=false, sawImportFinished=false;
    const auto log=[&](std::string_view line) {
      if(line=="video base: importing installation") { importStarted=Clock::now(); sawImport=true; }
      if(sawImport && !sawImportFinished && line.starts_with("videos: ")) {
        importFinished=Clock::now(); sawImportFinished=true;
        }
      events+=std::string(line)+'\n'; std::cout << line << '\n';
      };
    const auto started=Clock::now();
    auto imported=BaseStore::ensureVideos(game,log);
    const auto selectionSeconds=std::chrono::duration<double>(Clock::now()-started).count();
    const auto part=imported.path;
    if(imported.publication.valid()) imported.publication.get();
    require(events.find("video base: import ")!=std::string::npos,"empty cache/store did not import");
    require(sawImport && sawImportFinished,"corpus import timing events missing");
    const auto importSeconds=std::chrono::duration<double>(importFinished-importStarted).count();
    const auto bytes=directorySize(part);
    std::cout << "video part: files=" << hashes(part).size() << " bytes=" << bytes << " import_seconds=" << importSeconds
              << " selection_seconds=" << selectionSeconds << '\n';
    require(bytes<=3000000000ull,"needs_decision: video part exceeds 3.0 GB: "+std::to_string(bytes));
    require(importSeconds<=90,"needs_decision: corpus import exceeds 90 s: "+std::to_string(importSeconds));
    require(VideoData::matches(store/"base/videos"/identity.fingerprint,identity),"background publication missing");
    benchmark(part);
    compareCorpus(game,part);
    const auto expected=hashes(part);
    const auto repeated=temporary.path/"repeated";
    VideoData::build(game,repeated,log);
    require(hashes(repeated)==expected,"repeated import is not byte-identical");
    fs::remove_all(repeated);
    concurrentFirstPublications(game,part,temporary.path/"concurrent",identity,expected);
    events.clear();
    const auto cachedStarted=Clock::now();
    auto cached=BaseStore::ensureVideos(game,log);
    require(events.find("video base: identifying installation")==std::string::npos,
            "warm video cache snapshot must not invoke identify or hash BIK sources");
    const auto cachedSeconds=std::chrono::duration<double>(Clock::now()-cachedStarted).count();
    const auto backgroundStarted=Clock::now();
    if(cached.publication.valid()) cached.publication.get();
    const auto backgroundSeconds=std::chrono::duration<double>(Clock::now()-backgroundStarted).count();
    std::cout << "video cache hit: selection_seconds=" << cachedSeconds
              << " background_store_seconds=" << backgroundSeconds << '\n';
    require(backgroundSeconds<=1,"background store metadata validation exceeds 1 s");
    require(cached.path==part && events.find("video base: cache ")!=std::string::npos,"second selection did not use cache");
    imported.lease.reset(); cached.lease.reset();
    fs::remove_all(cache);
    events.clear();
    auto copied=BaseStore::ensureVideos(game,log);
    if(copied.publication.valid()) copied.publication.get();
    require(copied.path==part && events.find("video base: store ")!=std::string::npos,"empty cache did not copy store");
    require(hashes(part)==expected,"store copy differs");
    const auto entries=VideoData::readIndex(part);
    require(!entries.empty(),"empty video index");
    const auto indexFile=part/"videos.json";
    std::ifstream indexInput(indexFile,std::ios::binary);
    const std::string indexBytes{std::istreambuf_iterator<char>(indexInput),std::istreambuf_iterator<char>()};
    indexInput.close();
    auto alteredIndex=indexBytes;
    const auto sourcePath=alteredIndex.find("\"path\":\"");
    require(sourcePath!=std::string::npos && sourcePath+8<alteredIndex.size(),"index source path missing");
    alteredIndex[sourcePath+8]=alteredIndex[sourcePath+8]=='X' ? 'Y' : 'X';
    const auto writeIndex=[&](const std::string& bytes) {
      std::ofstream output(indexFile,std::ios::binary|std::ios::trunc);
      output.write(bytes.data(),std::streamsize(bytes.size()));
      require(bool(output),"cannot write index mutation");
      };
    writeIndex(alteredIndex);
    require(!VideoData::matches(part,identity),"parseable corrupted index was accepted by part validation");
    bool indexRejected=false;
    try { (void)VideoData::readIndex(part); } catch(const std::exception&) { indexRejected=true; }
    require(indexRejected,"parseable corrupted index was accepted by readIndex");
    writeIndex(indexBytes);
    const auto damaged=part/entries.front().file;
    flipLastByte(damaged);
    require(VideoData::matches(part,identity),"metadata validation unexpectedly scanned the video payload");
    require(!VideoData::matches(part,identity,true),"corrupted video payload was accepted by full validation");
    bool rejected=false;
    try {
      VideoData::Reader invalid(damaged);
      (void)invalid.readFrame(invalid.info().frames-1);
      } catch(const std::exception&) { rejected=true; }
    require(rejected,"corrupted video frame was accepted by Reader");
    rejected=false;
    try { VideoData::Reader invalid(damaged); invalid.verifyPayload(); } catch(const std::exception&) { rejected=true; }
    require(rejected,"corrupted video payload SHA-256 was accepted");
    fs::resize_file(damaged,fs::file_size(damaged)-1);
    require(!VideoData::matches(part,identity),"wrong video size was accepted by metadata validation");
    copied.lease.reset();
    events.clear();
    auto repaired=BaseStore::ensureVideos(game,log);
    if(repaired.publication.valid()) repaired.publication.get();
    require(events.find("video base: rejected cache directory")!=std::string::npos &&
            events.find("video base: store ")!=std::string::npos,"corrupted cache was not replaced from store");
    require(hashes(repaired.path)==expected,"repaired cache differs");
    fingerprintSelection(temporary.path/"fingerprints",repaired.path,identity);
    snapshotSelection(game,temporary.path/"snapshots",repaired.path,identity);
    identityInputs(game,temporary.path/"identity-game",identity);
    std::cout << "video contract passed: 21/21, all planes/PCM, deterministic decoder and import, import/cache/store, corruption, independent inputs\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "video contract: " << error.what() << '\n';
    return 1;
    }
  }

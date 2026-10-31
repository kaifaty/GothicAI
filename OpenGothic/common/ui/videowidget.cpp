#include "videowidget.h"

#include <Tempest/Painter>
#include <Tempest/TextCodec>
#include <Tempest/Log>
#include <Tempest/Application>
#include <Tempest/Platform>
#include <Tempest/Fence>

#include <cstddef>

#include "videodata.h"
#include "graphics/shaders.h"
#include "gamemusic.h"
#include "gothic.h"

using namespace Tempest;

struct VideoWidget::Sound : Tempest::SoundProducer {
  Sound(SoundContext& c, uint16_t sampleRate, bool isMono)
    :Tempest::SoundProducer(sampleRate, isMono ? 1 : 2), ctx(c), channels(isMono ? 1 : 2) {
    }
  void renderSound(int16_t *out, size_t n) override;

  SoundContext& ctx;
  size_t        channels = 2;
  };

struct VideoWidget::SoundContext {
  SoundContext(Context& ctx, SoundDevice& dev, uint16_t sampleRate, bool isMono):
    ctx(ctx), sampleRate(sampleRate), numChannels(isMono ? 1 : 2) {
    snd = dev.load(std::unique_ptr<VideoWidget::Sound>(new VideoWidget::Sound(*this,sampleRate,isMono)));
    }

  ~SoundContext() {
    snd = SoundEffect();
    }

  void play() {
    snd.play();
    }

  uint64_t tickCount() const {
    auto smp = processedSamples.load();
    return uint64_t(smp*1000)/uint64_t(std::max(sampleRate * numChannels, 1));
    }

  void pushSamples(const std::vector<int16_t>& s) {
    std::lock_guard<std::mutex> guard(syncSamples);
    size_t sz = samples.size();
    samples.resize(sz+s.size());
    std::memcpy(samples.data()+sz, s.data(), s.size()*sizeof(s[0]));
    }

  Context&             ctx;
  Tempest::SoundEffect snd;
  std::mutex           syncSamples;
  std::vector<int16_t> samples;

  std::atomic<uint64_t> processedSamples{0};
  uint16_t             sampleRate = 0;
  uint16_t             numChannels = 0;
  };

void VideoWidget::Sound::renderSound(int16_t *out, size_t n) {
  n = n*channels; // stereo

  std::lock_guard<std::mutex> guard(ctx.syncSamples);
  auto& s = ctx.samples;
  if(s.size()<n)
    return;
  std::memcpy(out, s.data(), n*sizeof(s[0]));
  ctx.samples.erase(ctx.samples.begin(),ctx.samples.begin()+int(n));
  ctx.processedSamples += n;
  }

struct VideoWidget::Context {
  struct Frame {
    struct Plane {
      const uint8_t* pixels = nullptr;
      uint32_t strd = 0;
      uint32_t stride() const { return strd; }
      const uint8_t* data() const { return pixels; }
      uint8_t at(uint32_t x, uint32_t y) const { return pixels[x+y*strd]; }
      };
    Frame(const VideoData::Info& info, VideoData::Frame&& frame) : info(info), data(std::move(frame)) {
      for(size_t i=0; i<planes.size(); ++i)
        planes[i]={data.planes[i].data(),info.planes[i].stride};
      }
    const VideoData::Info& info;
    VideoData::Frame data;
    std::array<Plane,4> planes;
    uint32_t width() const { return info.width; }
    uint32_t height() const { return info.height; }
    const Plane& plane(size_t i) const { return planes[i]; }
    };

  Context(std::unique_ptr<VideoData::Reader>&& f, std::string_view filename) : vid(std::move(f)), filename(filename) {
    sndCtx.resize(vid->info().tracks.size());
    for(size_t i=0; i<sndCtx.size(); ++i) {
      const auto& aud = vid->info().tracks[i];
      sndCtx[i].reset(new SoundContext(*this,sndDev,uint16_t(aud.sampleRate),aud.channels==1));
      }

    const float volume = Gothic::settingsSoundVolume();
    sndDev.setGlobalVolume(volume);
    for(auto& sound:sndCtx)
      sound->play();

    for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
      cmd[i] = Resources::device().commandBuffer();
      }

    frameTime = Application::tickCount();
    Log::i("video start: ",filename,", frames=",vid->info().frames);
    }

  ~Context() {
    for(auto& i:sync)
      i.wait();
    uint64_t samples=0;
    for(auto& sound:sndCtx) {
      sound->snd = SoundEffect();
      samples+=sound->processedSamples.load();
      }
    Log::i("video stop: ",filename,", completed=",isEof(),", frames=",currentFrame,
           ", last_frame=",currentFrame==0 ? -1 : int64_t(currentFrame-1),", samples=",samples);
    }

  void advance(Tempest::Device& device, uint8_t fId) {
    Frame f{vid->info(),vid->nextFrame()};
    ++currentFrame;
    Resources::videoFrameRead(filename);
    yuvToRgba(f, device, fId);
    for(size_t i=0; i<sndCtx.size(); ++i)
      sndCtx[i]->pushSamples(f.data.audio[i]);

    const uint64_t destTick = (uint64_t(1000)*vid->info().fpsDen*currentFrame)/vid->info().fpsNum;
    const uint64_t tickVis  = Application::tickCount() - frameTime;
    const uint64_t tickSnd  = tickSound();
    const uint64_t tick     = std::min(tickVis, tickSnd);

    if(destTick > tick) {
      Application::sleep(uint32_t(destTick-tick));
      return;
      }
    }

  uint64_t tickSound() const {
    uint64_t tickSnd = uint64_t(-1);
    // return tickSnd;

    for(size_t i=0; i<sndCtx.size(); ++i)
      tickSnd = std::min(tickSnd, sndCtx[i]->tickCount());
    return tickSnd;
    }

  void yuvToRgba(const Frame& f, Tempest::Device& device, uint8_t fId) {
    if(uint32_t(frameImg.w())!=f.width() || uint32_t(frameImg.h())!=f.height()) {
      Resources::recycle(std::move(frameImg));
      frameImg = device.attachment(Tempest::RGBA8, f.width(), f.height());
      }
    sync[fId].wait();

    // alignment for ssbo offsets
    auto alignBuf = [](size_t size, size_t align) { return (size+align-1) & ~(align-1); };

    auto& planeY = f.plane(0);
    auto& planeU = f.plane(1);
    auto& planeV = f.plane(2);

    auto  align  = std::max<size_t>(device.properties().ssbo.offsetAlign, 4);
    auto  sizeY  = alignBuf(f.height()*planeY.stride(),     align);
    auto  sizeU  = alignBuf((f.height()/2)*planeU.stride(), align);
    auto  sizeV  = alignBuf((f.height()/2)*planeV.stride(), align);
    auto  size   = sizeY + sizeU + sizeV;

    auto& stage  = staging[fId];
    if(stage.byteSize()!=size) {
      Resources::recycle(std::move(stage));
      stage = device.ssbo(BufferHeap::Upload, Uninitialized, size);
      }
    stage.update(planeY.data(), 0,           (f.height())  *planeY.stride());
    stage.update(planeU.data(), sizeY,       (f.height()/2)*planeU.stride());
    stage.update(planeV.data(), sizeY+sizeU, (f.height()/2)*planeV.stride());

    {
      auto cmd = this->cmd[fId].startEncoding(device);
      cmd.setFramebuffer({{frameImg, Vec4(0), Tempest::Preserve}});

      struct Push { uint32_t strideY; uint32_t strideU; uint32_t strideV; } push = {};
      push.strideY = planeY.stride();
      push.strideU = planeU.stride();
      push.strideV = planeV.stride();

      cmd.setDebugMarker("Bink");
      cmd.setPushData(push);
      cmd.setBinding(0, stage, 0);
      cmd.setBinding(1, stage, sizeY);
      cmd.setBinding(2, stage, sizeY + sizeU);
      cmd.setPipeline(Shaders::inst().bink);
      cmd.draw(nullptr, 0, 3);
    }
    sync[fId] = device.submit(this->cmd[fId]);
    }

  [[deprecated]]
  void yuvToRgba(const Frame& f, Pixmap& pm) {
    if(pm.w()!=f.width() || pm.h()!=f.height())
      pm = Pixmap(f.width(),f.height(),TextureFormat::RGBA8);
    auto& planeY = f.plane(0);
    auto& planeU = f.plane(1);
    auto& planeV = f.plane(2);
    auto  dst    = reinterpret_cast<uint8_t*>(pm.data());

    const uint32_t w = pm.w();
    for(uint32_t y=0; y<pm.h(); ++y)
      for(uint32_t x=0; x<w; ++x) {
        uint8_t* rgb = &dst[(x+y*w)*4];
        float Y = planeY.at(x,  y  );
        float U = planeU.at(x/2,y/2);
        float V = planeV.at(x/2,y/2);

        float r = 1.164f * (Y - 16.f) + 1.596f * (V - 128.f);
        float g = 1.164f * (Y - 16.f) - 0.813f * (V - 128.f) - 0.391f * (U - 128.f);
        float b = 1.164f * (Y - 16.f) + 2.018f * (U - 128.f);

        r = std::max(0.f,std::min(r,255.f));
        g = std::max(0.f,std::min(g,255.f));
        b = std::max(0.f,std::min(b,255.f));

        rgb[0] = uint8_t(r);
        rgb[1] = uint8_t(g);
        rgb[2] = uint8_t(b);
        rgb[3] = 255;
        }
    }

  bool isEof() const {
    return currentFrame>=vid->info().frames;
    }

  std::unique_ptr<VideoData::Reader> vid;
  std::string                   filename;
  uint32_t                      currentFrame = 0;
  uint64_t                      frameTime = 0;

  Tempest::Attachment           frameImg;
  Tempest::CommandBuffer        cmd    [Resources::MaxFramesInFlight];
  Tempest::StorageBuffer        staging[Resources::MaxFramesInFlight];
  Tempest::Fence                sync   [Resources::MaxFramesInFlight];

  Tempest::SoundDevice          sndDev;
  std::vector<std::unique_ptr<SoundContext>> sndCtx;
  };

VideoWidget::VideoWidget() {
  setCursorShape(CursorShape::Hidden);
  }

VideoWidget::~VideoWidget() {
  }

void VideoWidget::pushVideo(std::string_view filename) {
  Resources::startVideoBase();
  std::lock_guard<std::mutex> guard(syncVideo);
  pendingVideo.emplace(filename);
  hasPendingVideo.store(true);
  }

bool VideoWidget::isActive() const {
  return ctx!=nullptr || hasPendingVideo.load();
  }

void VideoWidget::tick() {
  if(ctx!=nullptr && ctx->isEof()) {
    stopVideo();
    }

  if(ctx!=nullptr)
    return;

  if(!hasPendingVideo)
    return;

  std::string filename;
  {
  std::lock_guard<std::mutex> guard(syncVideo);
  if(pendingVideo.empty())
    return;
  filename = std::move(pendingVideo.front());
  pendingVideo.pop();
  if(pendingVideo.size()==0)
    hasPendingVideo.store(false);
  }

  try {
    auto read=Resources::loadVideo(filename);
    if(!read || Gothic::settingsGetI("GAME", "scaleVideos")<0) {
      stopVideo();
      return;
      }
    frame = nullptr;
    ctx.reset(new Context(std::move(read),filename));
    if(!active) {
      active       = true;
      restoreMusic = GameMusic::inst().isEnabled();
      GameMusic::inst().setEnabled(false);
      }
    }
  catch(const std::exception& error) {
    Log::e("unable to play video: \"",filename,"\", ",error.what());
    stopVideo();
    }
  }

void VideoWidget::keyDownEvent(KeyEvent& event) {
  if(event.key==Event::K_ESCAPE) {
    stopVideo();
    }
  }

void VideoWidget::keyUpEvent(KeyEvent&) {
  }

void VideoWidget::mouseDownEvent(Tempest::MouseEvent& event) {
  if(!active) {
    event.ignore();
    return;
    }
#if defined(__MOBILE_PLATFORM__)
  stopVideo();
#endif
  }

void VideoWidget::stopVideo() {
  ctx.reset();
  frame = nullptr;
  if(!hasPendingVideo) {
    if(restoreMusic && !GameMusic::inst().isEnabled())
      GameMusic::inst().setEnabled(true);
    if(active)
      Log::i("video return: music_enabled=",GameMusic::inst().isEnabled());
    active = false;
    }
  update();
  }

void VideoWidget::paint(Tempest::Device& device, uint8_t fId) {
  if(ctx==nullptr)
    return;
  try {
    ctx->advance(device, fId);
    frame = &textureCast<Texture2d&>(ctx->frameImg);
    update();
    }
  catch(const std::exception& e) {
    Log::e("video decoding error. frame: ",ctx->currentFrame,", what: \"", e.what(), "\"");
    stopVideo();
    }
  }

void VideoWidget::paintEvent(PaintEvent& e) {
  if(ctx==nullptr || frame==nullptr)
    return;
  float k  = float(w())/float(frame->w());
  int   vh = int(k*float(frame->h()));

  Painter p(e);
  p.setBrush(Color(0,0,0,1));
  p.drawRect(0,0,w(),h());

  p.setBrush(Brush(*frame,Painter::NoBlend,ClampMode::ClampToEdge));
  p.drawRect(0,(h()-vh)/2,w(),vh,
             0,0,p.brush().w(),p.brush().h());
  }

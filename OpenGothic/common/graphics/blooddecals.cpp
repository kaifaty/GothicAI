#include "blooddecals.h"

#include <Tempest/Device>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "world/world.h"

using namespace Tempest;

namespace {

// A tiny deterministic generator: the atlas must not depend on the standard library's distributions.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed):s(seed*0x9E3779B97F4A7C15ull+1) {}
  uint32_t next() {
    s ^= s<<13; s ^= s>>7; s ^= s<<17;
    return uint32_t(s>>11);
    }
  float uniform()                 { return float(next()&0xFFFFFF)/float(0x1000000); }
  float uniform(float a, float b) { return a+(b-a)*uniform(); }
  int   range(int a, int b)       { return a+int(next()%uint32_t(b-a)); } // [a,b)
  float normal(float sigma) {
    const float u = std::max(uniform(),1e-6f), v = uniform();
    return sigma*std::sqrt(-2.f*std::log(u))*std::cos(6.2831853f*v);
    }
  };

float lattice(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x)*374761393u + uint32_t(y)*668265263u + seed*2246822519u;
  h = (h^(h>>13))*1274126177u;
  return float((h^(h>>16))&0xFFFF)/65535.f;
  }

float valueNoise(float x, float y, uint32_t seed) {
  const int   ix = int(std::floor(x)), iy = int(std::floor(y));
  float fx = x-float(ix), fy = y-float(iy);
  fx = fx*fx*(3-2*fx);
  fy = fy*fy*(3-2*fy);
  const float a = lattice(ix,iy,seed),   b = lattice(ix+1,iy,seed);
  const float c = lattice(ix,iy+1,seed), d = lattice(ix+1,iy+1,seed);
  return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy;
  }

// fbm over the tile, u,v in 0..1
float fbm(float u, float v, uint32_t seed, int octaves, float base) {
  float s = 0, amp = 1, tot = 0, f = base;
  for(int o=0; o<octaves; ++o) {
    s   += amp*valueNoise(u*f, v*f, seed+uint32_t(o)*7919u);
    tot += amp;
    amp *= 0.5f;
    f   *= 2.f;
    }
  return s/tot;
  }

struct Tile {
  uint32_t           n;
  std::vector<float> shape, thick, spread;
  explicit Tile(uint32_t n):n(n),shape(n*n,0.f),thick(n*n,0.f),spread(n*n,0.f) {}
  float x(uint32_t i) const { return (float(i)+0.5f)/float(n)*2.f-1.f; }
  float soft(float d) const { return std::clamp(0.5f - d/(3.f/float(n)), 0.f, 1.f); }
  };

// A round head with a tail ahead of it along `ang` (the way the drop flew).
void drop(Tile& t, float cx, float cy, float r, float ang, float tail, float thick) {
  const float c = std::cos(ang), s = std::sin(ang);
  const float reach = r*(tail+1.5f) + 4.f/float(t.n);
  const int   n  = int(t.n);
  const int   x0 = std::max(0,   int((cx-reach+1)*0.5f*float(n))-1), x1 = std::min(n-1, int((cx+reach+1)*0.5f*float(n))+1);
  const int   y0 = std::max(0,   int((cy-reach+1)*0.5f*float(n))-1), y1 = std::min(n-1, int((cy+reach+1)*0.5f*float(n))+1);
  for(int iy=y0; iy<=y1; ++iy)
    for(int ix=x0; ix<=x1; ++ix) {
      const float dx = t.x(uint32_t(ix))-cx, dy = t.x(uint32_t(iy))-cy;
      const float u  = dx*c+dy*s, v = -dx*s+dy*c;
      float d = std::hypot(u/1.15f, v)-r;
      if(u>0 && u<tail*r) {
        const float k = u/(tail*r);
        d = std::min(d, std::abs(v)-r*std::pow(1.f-k,1.5f));
        }
      const float m = t.soft(d);
      auto& sh = t.shape[size_t(iy)*t.n+size_t(ix)];
      auto& th = t.thick[size_t(iy)*t.n+size_t(ix)];
      sh = std::max(sh,m);
      th = std::max(th,m*thick);
      }
  }

// A hit splash: an irregular main stain, short spines on its rim, drops flung forward (+u).
void splash(Tile& t, uint32_t seed, float spread) {
  Rng rng(seed);
  const float base = -0.25f;
  for(uint32_t iy=0; iy<t.n; ++iy)
    for(uint32_t ix=0; ix<t.n; ++ix) {
      const float x = t.x(ix), y = t.x(iy);
      const float n = fbm((x+1)*0.5f, (y+1)*0.5f, seed*13u, 6, 4.f);
      const float d = std::hypot((x-base)/0.36f, y/0.27f)-1.f + (n-0.5f)*1.1f;
      t.shape[iy*t.n+ix] = t.soft(d*0.45f);
      t.thick[iy*t.n+ix] = std::clamp(-d*1.2f, 0.f, 1.f);
      }
  const int spines = rng.range(14,24);
  for(int k=0; k<spines; ++k) {
    const float a  = rng.normal(1.1f);
    const float x0 = base+std::cos(a)*0.30f, y0 = std::sin(a)*0.22f;
    const float L  = rng.uniform(0.03f,0.10f)*(std::abs(a)<0.6f ? 1.5f : 0.8f);
    const float r  = rng.uniform(0.008f,0.02f);
    drop(t, x0+std::cos(a)*L, y0+std::sin(a)*L, r, a+3.1415927f, L/0.015f, 0.f);
    }
  const int drops = rng.range(40,80);
  for(int k=0; k<drops; ++k) {
    const float a    = rng.normal(spread);
    const float dist = std::pow(rng.uniform(0.12f,0.75f),0.8f)+0.15f;
    const float cx   = base+std::cos(a)*dist, cy = std::sin(a)*dist*0.9f;
    const float u3   = rng.uniform();
    const float r    = 0.004f+0.03f*u3*u3*u3;
    const float tail = rng.uniform(1.f,4.f);
    if(std::abs(cx)>0.94f || std::abs(cy)>0.94f)
      continue;
    drop(t, cx, cy, r, a, tail, 0.3f);
    }
  for(size_t i=0; i<t.thick.size(); ++i)
    t.thick[i] *= t.shape[i];
  }

// A pool: B holds when the spreading edge arrives (0 centre .. 1 final edge), uneven like a floor.
void pool(Tile& t, uint32_t seed) {
  Rng   rng(seed);
  float amp[3], freq[3], phase[3];
  for(int k=0; k<3; ++k) {
    amp[k]   = rng.uniform(0.03f,0.09f);
    freq[k]  = float(rng.range(2,5));
    phase[k] = rng.uniform(0.f,6.28f);
    }
  for(uint32_t iy=0; iy<t.n; ++iy)
    for(uint32_t ix=0; ix<t.n; ++ix) {
      const float x = t.x(ix), y = t.x(iy);
      const float u = (x+1)*0.5f, v = (y+1)*0.5f;
      const float n  = fbm(u, v, seed*7u,     5, 3.f);
      const float n2 = fbm(u, v, seed*7u+99u, 4, 5.f);
      const float ang = std::atan2(y,x);
      float lob = 0;
      for(int k=0; k<3; ++k)
        lob += amp[k]*std::cos(ang*freq[k]+phase[k]);
      const float rad   = std::hypot(x,y);
      const float speed = 1.f + 0.55f*(n-0.5f) + lob;
      float tm = rad/std::max(speed,0.3f)/0.80f;
      tm += std::max(rad-0.88f, 0.f)*8.f; // never reaches the tile border
      const float th = std::pow(std::clamp((1.f-tm)*3.f, 0.f, 1.f), 0.7f)*(0.8f+0.2f*n2);
      const float m  = t.soft((tm-1.f)*0.9f);
      t.shape [iy*t.n+ix] = m;
      t.thick [iy*t.n+ix] = th*m;
      t.spread[iy*t.n+ix] = std::clamp(tm, 0.f, 1.f);
      }
  }

// What thin fresh blood does to the surface's albedo: a multiplier (texture space), so the soil or
// stone under it stays visible as through a real film. The shader darkens it where it is thick.
Vec3 bloodTint(const BloodStains::Stain& s) {
  switch(s.blood) {
    case BloodStains::Blood::Yellow: return Vec3(0.85f,0.72f,0.25f);
    case BloodStains::Blood::Gore:   return Vec3(0.42f,0.30f,0.20f);
    case BloodStains::Blood::Stone:  return Vec3(0.80f,0.78f,0.74f);
    case BloodStains::Blood::Red:    break;
    }
  if(s.surface==BloodStains::Surface::Snow)
    return Vec3(0.85f,0.08f,0.08f);
  return Vec3(0.72f,0.10f,0.08f);
  }

float smooth01(float x) {
  x = std::clamp(x, 0.f, 1.f);
  return x*x*(3-2*x);
  }
}

BloodDecals::BloodDecals() {
  atlas = Resources::device().texture(buildAtlas());
  }

Pixmap BloodDecals::buildAtlas(uint32_t tile) {
  const uint32_t size = tile*AtlasTiles;
  Pixmap  pm(size,size,TextureFormat::RGBA8);
  auto*   px = reinterpret_cast<uint8_t*>(pm.data());
  for(uint32_t id=0; id<AtlasTiles*AtlasTiles; ++id) {
    Tile t(tile);
    if(id<SplashTiles) {
      static const float spread[] = {0.35f, 0.5f, 0.7f};
      splash(t, 100u+id, id<8 ? spread[id%3] : 1.2f);
      } else {
      pool(t, 300u+(id-SplashTiles));
      }
    const uint32_t tx = id%AtlasTiles, ty = id/AtlasTiles;
    for(uint32_t y=0; y<tile; ++y)
      for(uint32_t x=0; x<tile; ++x) {
        const size_t src = size_t(y)*tile+x;
        uint8_t*     dst = px + (size_t(ty*tile+y)*size + tx*tile+x)*4;
        dst[0] = uint8_t(std::lround(t.shape [src]*255.f));
        dst[1] = uint8_t(std::lround(t.thick [src]*255.f));
        dst[2] = uint8_t(std::lround(t.spread[src]*255.f));
        dst[3] = 255;
        }
    }
  return pm;
  }

BloodDecals::Look BloodDecals::look(const BloodStains::Stain& s, int64_t now) {
  const float min = float(std::max<int64_t>(now-s.bornAt,0))/float(BloodStains::MinuteMs);
  Look l;
  // a pool runs out fast, then slows: ~95% of its area in 3-4 game minutes
  l.growth  = s.kind==BloodStains::Kind::Pool ? 1.f-std::exp(-min/1.15f) : 1.f;
  // wet gloss holds a few minutes, gone within the hour
  l.wetness = 1.f-smooth01((min-8.f)/50.f);
  // oxidation: fast in the first hours, dark brown by 12-24 h
  l.dryness = 1.f-std::exp(-min/(5.f*60.f));
  // the last fifth of the lifetime thins the stain out
  const float life = float(BloodStains::lifetimeMs(s))/float(BloodStains::MinuteMs);
  l.opacity = 1.f-smooth01((min-life*0.8f)/(life*0.2f));
  return l;
  }

bool BloodDecals::prepare(uint8_t /*fId*/, const World* world, const SceneGlobals& scene, const Vec3& camera) {
  data.clear();
  count = 0;
  const auto& stains = world->bloodStains();
  if(stains.empty())
    return false;

  const int64_t now   = world->time().toInt();
  const float   range = BloodStains::DrawRangeCm;
  const float   inset = 0.5f/float(256*AtlasTiles);

  // The nearest within range, at most DrawLimit of them: 256 decals filling the screen cost ~0.8 ms
  // on an M1 Pro at 1600x1200, over the 0.2 ms budget; far stains are small and least missed.
  near.clear();
  for(auto& s:stains) {
    if(s.blood==BloodStains::Blood::Stone)
      continue; // golem chips: not drawn yet
    const float d = (s.pos-camera).quadLength();
    if(d > (range+s.radiusCm)*(range+s.radiusCm))
      continue;
    near.push_back({d, &s});
    }
  if(near.size()>BloodStains::DrawLimit) {
    std::nth_element(near.begin(), near.begin()+BloodStains::DrawLimit, near.end(),
                     [](const Near& a, const Near& b) { return a.dist<b.dist; });
    near.resize(BloodStains::DrawLimit);
    }

  for(const auto& it:near) {
    const auto& s = *it.stain;
    const Look lk = look(s, now);
    if(lk.opacity<=0.f)
      continue;

    Vec3 n = Vec3::normalize(s.normal);
    Vec3 u = s.dir - n*Vec3::dotProduct(s.dir,n);
    if(u.quadLength()<1e-4f)
      u = std::abs(n.y)<0.9f ? Vec3::crossProduct(n,Vec3(0,1,0)) : Vec3::crossProduct(n,Vec3(1,0,0));
    u = Vec3::normalize(u);
    const Vec3  w  = Vec3::crossProduct(n,u);
    const float r  = s.radiusCm;
    const float hd = std::clamp(r*0.3f, 6.f, 14.f);

    const uint32_t tile = s.kind==BloodStains::Kind::Pool ? SplashTiles + s.variant%PoolTiles
                                                          : s.variant%SplashTiles;
    const float tw = 1.f/float(AtlasTiles);
    Gpu g = {};
    const Vec3 c = s.pos - scene.originLwc;
    g.world  = Vec4(s.pos.x, s.pos.y, s.pos.z, 0);
    g.center = Vec4(c.x, c.y, c.z, 0);
    g.axisU  = Vec4(u.x*r,  u.y*r,  u.z*r,  0);
    g.axisV  = Vec4(n.x*hd, n.y*hd, n.z*hd, 0);
    g.axisW  = Vec4(w.x*r,  w.y*r,  w.z*r,  0);
    g.uvRect = Vec4(float(tile%AtlasTiles)*tw+inset, float(tile/AtlasTiles)*tw+inset, tw-2*inset, tw-2*inset);
    g.look   = Vec4(lk.growth, lk.wetness, lk.dryness, lk.opacity);
    const Vec3 tint = bloodTint(s);
    g.tint   = Vec4(tint.x, tint.y, tint.z, s.surface==BloodStains::Surface::Earth ? 1.f : 0.f);
    data.push_back(g);
    }
  count = data.size();
  return count>0;
  }

void BloodDecals::draw(Encoder<CommandBuffer>& cmd, uint8_t fId, const SceneGlobals& scene,
                       Attachment& gbufDiffuse, Attachment* gbufOrm,
                       const Attachment& gbufNormal, const ZBuffer& zbuffer) {
  if(count==0)
    return;
  auto& device = Resources::device();
  auto& buf    = ssbo[fId];
  const size_t bytes = data.size()*sizeof(Gpu);
  if(buf.byteSize()<bytes) {
    Resources::recycle(std::move(buf));
    buf = device.ssbo(BufferHeap::Upload, Uninitialized, std::max<size_t>(bytes, 64*sizeof(Gpu)));
    }
  buf.update(data.data(), 0, bytes);

  cmd.setDebugMarker("Blood decals");
  if(gbufOrm!=nullptr)
    cmd.setFramebuffer({{gbufDiffuse, Tempest::Preserve, Tempest::Preserve},
                        {*gbufOrm,    Tempest::Preserve, Tempest::Preserve}}); else
    cmd.setFramebuffer({{gbufDiffuse, Tempest::Preserve, Tempest::Preserve}});
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, zbuffer,    Sampler::nearest());
  cmd.setBinding(2, gbufNormal, Sampler::nearest());
  cmd.setBinding(3, atlas,      Sampler::trillinear());
  cmd.setBinding(4, buf);
  cmd.setPipeline(gbufOrm!=nullptr ? Shaders::inst().bloodDecal : Shaders::inst().bloodDecalAlbedo);
  cmd.draw(nullptr, 0, 36, 0, count);
  }

// TAA by the camera (graphics/taa): the jitter sequence, the reprojection of the history and the camera jump
// rule. CPU only: no device, no game data.
#include "graphics/taa.h"

#include <Tempest/Matrix4x4>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Tempest;

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

constexpr uint32_t Width  = 3840;
constexpr uint32_t Height = 2160;

// the camera's matrices as Camera::mkView builds them
Matrix4x4 mkViewLwc(const Vec3& spin) {
  auto view = Matrix4x4::mkIdentity();
  view.scale(-1,-1,-1);
  auto rot = Matrix4x4::mkIdentity();
  rot.rotateOX(spin.x);
  rot.rotateOY(spin.y);
  rot.rotateOZ(spin.z);
  rot.rotateOY(90);
  view.mul(rot);
  return view;
  }

Taa::View mkView(const Vec3& origin, const Vec3& spin) {
  Taa::View v;
  v.proj.perspective(67.5f, float(Width)/float(Height), 10.f, 100000.f);
  v.viewLwc = mkViewLwc(spin);
  v.origin  = origin;
  return v;
  }

Vec4 apply(const Matrix4x4& m, const Vec4& v) {
  Vec4 r = v;
  m.project(r);
  return r;
  }

// NDC difference in pixels of the target
float pixels(float ax, float ay, float bx, float by) {
  const float dx = (ax-bx)*0.5f*float(Width);
  const float dy = (ay-by)*0.5f*float(Height);
  return std::sqrt(dx*dx + dy*dy);
  }

void jitterSequence() {
  float    sx = 0, sy = 0;
  Vec2     seen[Taa::Phases];
  for(uint32_t i=0; i<Taa::Phases; ++i) {
    const Vec2 j = Taa::jitterPx(i);
    require(std::abs(j.x)<=0.5f && std::abs(j.y)<=0.5f, "jitter beyond half a pixel at phase " + std::to_string(i));
    for(uint32_t k=0; k<i; ++k)
      require(seen[k].x!=j.x || seen[k].y!=j.y, "jitter phase repeats within 8 frames");
    seen[i] = j;
    sx += j.x;
    sy += j.y;
    }
  sx /= float(Taa::Phases);
  sy /= float(Taa::Phases);
  require(std::abs(sx)<0.06f && std::abs(sy)<0.06f, "jitter mean is not about zero");
  // the sequence repeats after 8 frames
  for(uint32_t i=0; i<Taa::Phases; ++i) {
    const Vec2 a = Taa::jitterPx(i), b = Taa::jitterPx(i+Taa::Phases);
    require(a.x==b.x && a.y==b.y, "jitter is not 8 phases");
    }
  // the matrix moves the picture by the offset in pixels
  const Vec2 px  = Taa::jitterPx(3);
  const auto jm  = Taa::jitterMatrix(px, Width, Height);
  const Vec4 c   = apply(jm, Vec4(0.25f, -0.4f, 0.5f, 2.f));
  const float dx = (c.x/c.w - 0.125f)*0.5f*float(Width);
  const float dy = (c.y/c.w + 0.2f  )*0.5f*float(Height);
  require(std::abs(dx-px.x)<1e-3f && std::abs(dy-px.y)<1e-3f, "jitter matrix does not move by the offset");
  std::printf("jitter mean %.4f %.4f px\n", double(sx), double(sy));
  }

void staticCamera() {
  // a camera that stands still: no offset of the history, also far from the world origin
  const Vec3 origins[] = {Vec3(0,0,0), Vec3(-1200,300,800), Vec3(120000,4000,-95000), Vec3(-250000,-8000,310000)};
  float worst = 0;
  for(const Vec3& o:origins) {
    const auto view = mkView(o, Vec3(12,137,0));
    for(uint32_t f=0; f<Taa::Phases; ++f) {
      const auto j = Taa::jitterMatrix(Taa::jitterPx(f), Width, Height);
      const auto m = Taa::reprojection(view, view, j);
      for(float y=-1; y<=1.001f; y+=0.25f)
        for(float x=-1; x<=1.001f; x+=0.25f)
          for(float z:{0.5f, 0.99f, 0.99999f, 1.f}) {
            const Vec4 p = apply(m, Vec4(x,y,z,1));
            require(p.w>0, "static reprojection behind the camera");
            worst = std::max(worst, pixels(p.x/p.w, p.y/p.w, x, y));
            }
      }
    }
  require(worst<1e-3f, "static camera moves the history by " + std::to_string(worst) + " px");
  std::printf("static camera: worst offset %.6f px\n", double(worst));
  }

void movingCamera() {
  // a point at a known place: the reprojection finds it where the previous camera saw it
  const Vec3 prevOrigin = Vec3(120000,4000,-95000);
  const Vec3 curOrigin  = prevOrigin + Vec3(31,-4,17);
  const auto prev = mkView(prevOrigin, Vec3(12,137,0));
  const auto cur  = mkView(curOrigin,  Vec3(13,139.5f,0));
  const auto j    = Taa::jitterMatrix(Taa::jitterPx(5), Width, Height);
  const auto m    = Taa::reprojection(cur, prev, j);

  auto jvp = [&](const Taa::View& v) {
    auto r = j;
    r.mul(v.proj);
    r.mul(v.viewLwc);
    return r;
    };
  const auto curVp  = jvp(cur);
  const auto prevVp = jvp(prev);
  const Vec3 delta  = Vec3(31,-4,17); // curOrigin-prevOrigin, exact

  float worst = 0;
  int   tested = 0;
  for(float a=-60; a<=60; a+=15)
    for(float b=-30; b<=30; b+=15)
      for(float d:{150.f, 900.f, 7000.f, 60000.f}) {
        // relative to the current camera, in front of it
        const auto  rot = mkViewLwc(Vec3(13,139.5f,0));
        auto        inv = rot;
        inv.inverse();
        const float ra = a*float(M_PI)/180.f, rb = b*float(M_PI)/180.f;
        Vec3 dirView = Vec3(std::sin(ra)*std::cos(rb), std::sin(rb), std::cos(ra)*std::cos(rb));
        Vec4 pv = Vec4(dirView.x*d, dirView.y*d, dirView.z*d, 1);
        // the view looks along -z or +z: take whichever side lands in front of the projection
        Vec4 c  = apply(cur.proj, pv);
        if(c.w<=0) {
          pv = Vec4(-pv.x, -pv.y, -pv.z, 1);
          c  = apply(cur.proj, pv);
          }
        const Vec4 lwc   = apply(inv, pv);
        const Vec4 cClip = apply(curVp, lwc);
        const Vec3 ndc   = Vec3(cClip.x/cClip.w, cClip.y/cClip.w, cClip.z/cClip.w);
        if(std::abs(ndc.x)>1 || std::abs(ndc.y)>1)
          continue;
        const Vec4 pLwc  = Vec4(lwc.x+delta.x, lwc.y+delta.y, lwc.z+delta.z, 1);
        const Vec4 pClip = apply(prevVp, pLwc);
        const Vec4 r     = apply(m, Vec4(ndc.x, ndc.y, ndc.z, 1));
        require(r.w>0, "moving reprojection behind the camera");
        worst = std::max(worst, pixels(r.x/r.w, r.y/r.w, pClip.x/pClip.w, pClip.y/pClip.w));
        ++tested;
        }
  require(tested>50, "too few points in view: " + std::to_string(tested));
  require(worst<0.05f, "moving camera reprojects off by " + std::to_string(worst) + " px");
  std::printf("moving camera: %d points, worst error %.4f px\n", tested, double(worst));
  }

// Motion vectors of objects (graphics plan 1b): the residual against the direct projection of p and p+d
void motionResidual() {
  const Vec3 prevOrigin = Vec3(120000,4000,-95000);
  const Vec3 move       = Vec3(31,-4,17);
  const auto prev = mkView(prevOrigin,      Vec3(12,137,0));
  const auto cur  = mkView(prevOrigin+move, Vec3(13,139.5f,0));
  const auto rot  = mkViewLwc(Vec3(13,139.5f,0));
  auto       inv  = rot;
  inv.inverse();

  // the camera's NDC of a world point given relative to the current camera, in double through the lwc chain
  auto ndcOf = [](const Taa::View& v, const Matrix4x4& j, const Vec3& rel) {
    auto m = j;
    m.mul(v.proj);
    m.mul(v.viewLwc);
    const Vec4 c = apply(m, Vec4(rel.x, rel.y, rel.z, 1));
    return Vec4(c.x/c.w, c.y/c.w, c.z/c.w, c.w);
    };

  const Vec3 ds[] = {Vec3(0,0,0), Vec3(3,0,0), Vec3(0,-2.5f,1), Vec3(-6,1,4), Vec3(0,0,12)};
  float worst = 0, worstJitter = 0, zeroWorst = 0;
  int   tested = 0;
  for(bool jittered:{false, true}) {
    const auto j  = jittered ? Taa::jitterMatrix(Taa::jitterPx(5), Width, Height) : Matrix4x4::mkIdentity();
    const auto mc = Taa::motionCamera(cur, prev, true, j);
    auto curVp = j;
    curVp.mul(cur.proj);
    curVp.mul(cur.viewLwc);
    for(float a=-50; a<=50; a+=25)
      for(float b=-25; b<=25; b+=25)
        for(float dist:{200.f, 1500.f, 9000.f}) {
          const float ra = a*float(M_PI)/180.f, rb = b*float(M_PI)/180.f;
          Vec4 pv = Vec4(std::sin(ra)*std::cos(rb)*dist, std::sin(rb)*dist, std::cos(ra)*std::cos(rb)*dist, 1);
          if(apply(cur.proj, pv).w<=0)
            pv = Vec4(-pv.x, -pv.y, -pv.z, 1);
          const Vec4 lwc  = apply(inv, pv);             // relative to the current camera
          const Vec3 rel  = Vec3(lwc.x, lwc.y, lwc.z);
          const Vec4 clip = apply(curVp, Vec4(rel.x, rel.y, rel.z, 1));
          if(std::abs(clip.x/clip.w)>1 || std::abs(clip.y/clip.w)>1)
            continue;
          for(const Vec3& d:ds) {
            const Vec2 r = Taa::motionResidual(mc.reprojectClip, mc.viewProjectPrev, clip, d);
            // directly: the previous camera sees p and p+d; relative to it they are rel+move and rel+move+d
            const Vec4 p0 = ndcOf(prev, j, rel+move);
            const Vec4 p1 = ndcOf(prev, j, rel+move+d);
            const float ex = (p1.x-p0.x)*0.5f, ey = (p1.y-p0.y)*0.5f;
            const float err = std::hypot((r.x-ex)*float(Width), (r.y-ey)*float(Height));
            if(d.x==0 && d.y==0 && d.z==0) {
              require(r.x==0 && r.y==0, "d=0 gives a residual under a moving camera");
              zeroWorst = std::max(zeroWorst, std::hypot(r.x, r.y));
              }
            worst = std::max(worst, err);
            // no jitter in the residual: the same point and move without it
            if(jittered) {
              const auto m0 = Taa::motionCamera(cur, prev, true, Matrix4x4::mkIdentity());
              auto vp0 = cur.proj;
              vp0.mul(cur.viewLwc);
              const Vec2 r0 = Taa::motionResidual(m0.reprojectClip, m0.viewProjectPrev,
                                                  apply(vp0, Vec4(rel.x, rel.y, rel.z, 1)), d);
              worstJitter = std::max(worstJitter, std::hypot((r.x-r0.x)*float(Width), (r.y-r0.y)*float(Height)));
              }
            ++tested;
            }
          }
    }
  require(tested>100, "too few motion points in view: " + std::to_string(tested));
  require(worst<0.01f, "residual is off the direct projection by " + std::to_string(worst) + " px");
  require(worstJitter<0.01f, "jitter leaks into the residual by " + std::to_string(worstJitter) + " px");

  // a cut: no reprojection, the previous camera is this one; a still point gives 0, a moving one its move as this
  // camera sees it
  const auto j   = Taa::jitterMatrix(Taa::jitterPx(2), Width, Height);
  const auto cut = Taa::motionCamera(cur, prev, false, j);
  const auto id  = Matrix4x4::mkIdentity();
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      require(cut.reprojectClip.at(c,r)==id.at(c,r), "a cut reprojects the camera");
  const auto same = Taa::viewProjectPrev(cur, j);
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      require(cut.viewProjectPrev.at(c,r)==same.at(c,r), "a cut takes another previous camera than this one");
  auto curVp = j;
  curVp.mul(cur.proj);
  curVp.mul(cur.viewLwc);
  const Vec3 rel  = Vec3(apply(inv, Vec4(0, 0, 900, 1)).x, apply(inv, Vec4(0, 0, 900, 1)).y, apply(inv, Vec4(0, 0, 900, 1)).z);
  Vec3 relFront   = rel;
  if(apply(curVp, Vec4(rel.x, rel.y, rel.z, 1)).w<=0)
    relFront = Vec3(-rel.x, -rel.y, -rel.z);
  const Vec4 clip = apply(curVp, Vec4(relFront.x, relFront.y, relFront.z, 1));
  const Vec2 still = Taa::motionResidual(cut.reprojectClip, cut.viewProjectPrev, clip, Vec3());
  require(still.x==0 && still.y==0, "a still point moves on a cut");
  const Vec3 d   = Vec3(5,2,-3);
  const Vec2 mv  = Taa::motionResidual(cut.reprojectClip, cut.viewProjectPrev, clip, d);
  const Vec4 q0  = ndcOf(cur, j, relFront);
  const Vec4 q1  = ndcOf(cur, j, relFront+d);
  const float ce = std::hypot((mv.x-(q1.x-q0.x)*0.5f)*float(Width), (mv.y-(q1.y-q0.y)*0.5f)*float(Height));
  require(ce<0.01f, "a moving point on a cut is off its own move by " + std::to_string(ce) + " px");
  std::printf("motion residual: %d cases, worst %.5f px, jitter %.5f px, d=0 %.1g, cut %.5f px\n",
              tested, double(worst), double(worstJitter), double(zeroWorst), double(ce));
  }

void cameraJump() {
  const Vec3 o = Vec3(120000,4000,-95000);
  const auto a = mkView(o, Vec3(10,40,0));
  require(!Taa::isCameraJump(a, a), "a camera standing still is a jump");
  require(!Taa::isCameraJump(mkView(o+Vec3(300,0,-390),Vec3(10,40,0)), a), "4.9 m is a jump");     // 492 cm
  require( Taa::isCameraJump(mkView(o+Vec3(300,0,-410),Vec3(10,40,0)), a), "5.1 m is not a jump"); // 508 cm
  require(!Taa::isCameraJump(mkView(o,Vec3(10,69,0)), a), "29 degrees is a jump");
  require( Taa::isCameraJump(mkView(o,Vec3(10,71,0)), a), "31 degrees is not a jump");
  require( Taa::isCameraJump(mkView(o,Vec3(10,220,0)), a), "a turn about is not a jump");
  require(!Taa::isCameraJump(mkView(o,Vec3(-15,40,0)), a), "25 degrees of pitch is a jump");
  require( Taa::isCameraJump(mkView(o,Vec3(45,40,0)), a), "35 degrees of pitch is not a jump");
  const float r = Taa::rotationAngle(mkViewLwc(Vec3(10,40,0)), mkViewLwc(Vec3(10,52,0)));
  require(std::abs(r-12.f)<0.05f, "rotation angle of 12 degrees reads " + std::to_string(r));
  }
}

// the temporal upscale (graphics plan 1г): Apple's phase count and mip bias, and the longer sequence stays in a pixel
void upscaleSequence() {
  require(Taa::upscalePhases(1728, 959, 3456, 1918)==32, "2x upscale is not 32 phases");
  require(Taa::upscalePhases(2315, 1285, 3456, 1918)==18, "1.5x upscale is not 18 phases");
  require(Taa::upscalePhases(2592, 1438, 3456, 1918)==15, "1.33x upscale is not 15 phases");
  require(Taa::upscalePhases(3456, 1918, 3456, 1918)==Taa::Phases, "no upscale is not TAA's phases");
  require(std::abs(Taa::upscaleMipBias(1728, 3456) + 2.f)<1e-6f, "2x mip bias is not -2");
  require(std::abs(Taa::upscaleMipBias(3456, 3456) + 1.f)<1e-6f, "1x mip bias is not -1");
  for(uint32_t phases : {18u, 32u}) {
    float sx = 0, sy = 0;
    for(uint32_t i=0; i<phases; ++i) {
      const Vec2 j = Taa::jitterPx(i, phases);
      require(std::abs(j.x)<=0.5f && std::abs(j.y)<=0.5f, "upscale jitter beyond half a pixel");
      for(uint32_t k=0; k<i; ++k) {
        const Vec2 o = Taa::jitterPx(k, phases);
        require(o.x!=j.x || o.y!=j.y, "upscale jitter phase repeats");
        }
      const Vec2 again = Taa::jitterPx(i+phases, phases);
      require(again.x==j.x && again.y==j.y, "upscale jitter does not repeat after its phases");
      sx += j.x;
      sy += j.y;
      }
    // Halton from index 1 is not centred: -0.036/-0.033 px at 18 phases, -0.015/-0.019 at 32; the scaler knows each one
    require(std::abs(sx/float(phases))<0.06f && std::abs(sy/float(phases))<0.06f, "upscale jitter mean is not about zero");
    }
  // TAA's sequence is the first 8 of them
  for(uint32_t i=0; i<Taa::Phases; ++i) {
    const Vec2 a = Taa::jitterPx(i), b = Taa::jitterPx(i, 32);
    require(a.x==b.x && a.y==b.y, "TAA jitter changed");
    }
  }

int main() {
  try {
    jitterSequence();
    upscaleSequence();
    staticCamera();
    movingCamera();
    cameraJump();
    motionResidual();
    std::cout << "{\"test\":\"taa\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "taa test failed: " << e.what() << '\n';
    return 1;
    }
  }

#include "camera.h"

#include <Tempest/Application>
#include <Tempest/Log>

#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "game/definitions/cameradefinitions.h"
#include "graphics/mesh/animmath.h"
#include "game/serialize.h"
#include "game/aimcamera.h"
#include "utils/gthfont.h"
#include "utils/dbgpainter.h"
#include "gothic.h"

#include "utils/string_frm.h"

using namespace Tempest;

static float angleMod(float a) {
  a = std::fmod(a,360.f);
  if(a<-180.f)
    a+=360.f;
  if(a>180.f)
    a-=360.f;
  return a;
  }
/*
static Vec3 angleMod(Vec3 a) {
  a.x = angleMod(a.x);
  a.y = angleMod(a.y);
  a.z = angleMod(a.z);
  return a;
  }*/

static zenkit::Quat fromAngles(Vec3 angles) {
  float roll  = float(angles.x*M_PI)/180.f;
  float yaw   = float(angles.y*M_PI)/180.f;
  float pitch = 0.f;

  float cr = std::cos(roll  * 0.5f);
  float sr = std::sin(roll  * 0.5f);
  float cp = std::cos(pitch * 0.5f);
  float sp = std::sin(pitch * 0.5f);
  float cy = std::cos(yaw   * 0.5f);
  float sy = std::sin(yaw   * 0.5f);

  zenkit::Quat q;
  q.w = cr * cp * cy + sr * sp * sy;
  q.x = sr * cp * cy - cr * sp * sy;
  q.y = cr * sp * cy + sr * cp * sy;
  q.z = cr * cp * sy - sr * sp * cy;

  return q;
  }

static Vec3 toAngles(zenkit::Quat q) {
  float roll, pitch, yaw;

  // roll (x-axis rotation)
  float sinr_cosp = 2 * (q.w * q.x + q.y * q.z);
  float cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
  roll = std::atan2(sinr_cosp, cosr_cosp);

  // pitch (y-axis rotation)
  float sinp = std::sqrt(1 + 2 * (q.w * q.y - q.x * q.z));
  float cosp = std::sqrt(1 - 2 * (q.w * q.y - q.x * q.z));
  pitch = 2.f * std::atan2(sinp, cosp) - float(M_PI / 2.0);

  // yaw (z-axis rotation)
  float siny_cosp = 2 * (q.w * q.z + q.x * q.y);
  float cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
  yaw = std::atan2(siny_cosp, cosy_cosp);

  return Vec3(roll,yaw,pitch)*float(180.f/M_PI);
  }

const float Camera::minLength = 0.0001f;

Camera::Camera() {
  }

void Camera::reset() {
  reset(Gothic::inst().player());
  }

void Camera::reset(const Npc* pl) {
  const auto& def = cameraDef();

  userRange    = (def.best_range - def.min_range)/(def.max_range - def.min_range);
  state.range  = userRange*(def.max_range-def.min_range)+def.min_range;
  state.target = pl ? pl->cameraBone() : Vec3();

  state.spin.x  = 0;
  state.spin.y  = pl ? pl->rotation() : 0;
  aimPitchBack  = 0;
  state.spin   += Vec3(def.best_elevation,
                       def.best_azimuth,
                       def.best_rot_z);

  tickThirdPerson(-1.f);
  }

void Camera::save(Serialize &s) {
  s.write(state.range, state.spin, state.target);
  s.write(inter.target, inter.rotOffset);
  s.write(origin,angles,veloTrans);
  s.write(userRange);
  }

void Camera::load(Serialize &s, Npc* pl) {
  reset(pl);
  if(s.version()<55)
    return;
  s.read(state.range, state.spin, state.target);
  s.read(inter.target, inter.rotOffset);
  s.read(origin,angles,veloTrans);
  s.read(userRange);
  }

void Camera::changeZoom(int delta) {
  if(fpEnable)
    return;
  if(delta>0)
    userRange-=0.02f; else
    userRange+=0.02f;
  userRange = std::max(0.f,std::min(userRange,1.f));
  }

void Camera::setViewport(uint32_t w, uint32_t h) {
  const float fov = Gothic::options().cameraFov;
  proj.perspective(fov*fovK, float(w)/float(h), zNear(), zFar());

  // NOTE: usually depth bouds are from 0.95 to 1.0, resilting into ~805675 discrete values
  static float l = 0.951978f, r = 1;
  auto il = reinterpret_cast<uint32_t&>(l);
  auto ir = reinterpret_cast<uint32_t&>(r);
  auto diff = ir-il;
  (void)diff;

  vpWidth   = w;
  vpHeight  = h;
  depthNear = 0;
  }

float Camera::fovY() const {
  return Gothic::options().cameraFov*fovK;
  }

float Camera::zNear() const {
  return nearClip;
  }

void Camera::setNearClip(float zNear) {
  if(nearClip==zNear)
    return;
  nearClip = zNear;
  updateProj();
  }

void Camera::updateProj() {
  if(vpWidth>0 && vpHeight>0)
    proj.perspective(Gothic::options().cameraFov*fovK, float(vpWidth)/float(vpHeight), zNear(), zFar());
  }

float Camera::zFar() const {
  static float far = 100000.0f;
  return far;
  }

void Camera::rotateLeft(uint64_t dt) {
  implMove(KeyEvent::K_Q,dt);
  }

void Camera::rotateRight(uint64_t dt) {
  implMove(KeyEvent::K_E,dt);
  }

void Camera::moveForward(uint64_t dt) {
  implMove(KeyEvent::K_W,dt);
  }

void Camera::moveBack(uint64_t dt) {
  implMove(KeyEvent::K_S,dt);
  }

void Camera::moveLeft(uint64_t dt) {
  implMove(KeyEvent::K_A,dt);
  }

void Camera::moveRight(uint64_t dt) {
  implMove(KeyEvent::K_D,dt);
  }

void Camera::setMode(const Camera::Mode m) {
  if(camMod==m)
    return;

  auto isRegular = [](const Camera::Mode m){
    return m==Normal || m==Inventory || m==Melee || m==Ranged || m==Magic;
    };

  const bool reset = !(isRegular(camMod) && isRegular(m));
  if(camMod==Mode::Cutscene) {
    state.spin   = angles;
    state.target = origin;
    inter.target = origin;
    }

  camMod = m;

  if(camMarvinMod==M_Free || camMarvinMod==M_Freeze)
    return;

  if(camMod==Camera::FirstPerson) {
    if(auto pl = Gothic::inst().player()) {
      state.spin = Vec3(0, pl->rotation(), 0);
      }
    return;
    }

  const auto& def = cameraDef();

  if(reset) {
    state.range = def.best_range;
    if(def.max_range > def.min_range + 0.01f)
      userRange = (def.best_range - def.min_range)/(def.max_range - def.min_range); else
      userRange = 0;
    state.spin = Vec3(0);
    }

  if(auto pl = Gothic::inst().player()) {
    state.spin = Vec3(0, pl->rotation(), 0);
    }

  auto rotBest = Vec3(def.best_elevation,
                      def.best_azimuth,
                      def.best_rot_z);
  state.spin += rotBest;
  }

void Camera::setMarvinMode(Camera::MarvinMode nextMod) {
  if(camMarvinMod==nextMod)
    return;

  if(nextMod==M_Pinned) {
    const auto pl = Gothic::inst().player();

    auto      offset = origin;
    Matrix4x4 rotMat = pl!=nullptr ? pl->cameraMatrix(false) : Matrix4x4::mkIdentity();
    rotMat.inverse();
    rotMat.project(offset);

    pin.origin = offset;
    pin.spin   = angles;
    }
  else if(nextMod==M_Free) {
    state.spin   = angles;
    state.target = origin;
    inter.target = origin;
    }
  camMarvinMod = nextMod;
  }

bool Camera::isMarvin() const {
  return camMarvinMod!=M_Normal;
  }

bool Camera::isFree() const {
  return camMarvinMod==M_Free;
  }

bool Camera::isInWater() const {
  return inWater;
  }

bool Camera::isCutscene() const {
  return camMod==Camera::Mode::Cutscene;
  }

void Camera::setToggleEnable(bool e) {
  tgEnable = e;
  }

bool Camera::isToggleEnabled() const {
  return tgEnable;
  }

void Camera::setInertiaTargetEnable(bool e) {
  inertiaTarget = e;
  }

bool Camera::isInertiaTargetEnabled() const {
  return inertiaTarget;
  }

void Camera::setFirstPerson(bool fp) {
  fpEnable = fp;
  }

bool Camera::isFirstPerson() const {
  return fpEnable;
  }

void Camera::setLookBack(bool lb) {
  if(lbEnable==lb)
    return;
  lbEnable = lb;
  }

void Camera::setAim(bool on, float zoom) {
  if(on && !aimOn) {
    // raised again while still going back: the elevation it was going back to
    aimSaved     = state.spin.x - aimPitchBack;
    aimPitchBack = 0;
    }
  if(!on && aimOn) {
    // back to how it looked before, as the camera blends out (tickAim): at once the camera over the
    // shoulder would swing round with it, a jump of a metre. From where the clamp leaves it, all the way
    state.spin.x = clampRotation(state.spin).x;
    aimPitchBack = state.spin.x - aimSaved;
    }
  aimOn   = on;
  aimZoom = on ? std::clamp(zoom,0.f,1.f) : 0.f;
  }

void Camera::setAimAnchor(const Tempest::Vec3& at) {
  aimAnchor = at;
  }

void Camera::setAimPitch(float deg) {
  state.spin.x = deg;
  }

void Camera::setAimStance(float drop, float side, float shoulder) {
  aimDrop      = drop;
  aimSide      = side;
  aimShoulderK = std::clamp(shoulder,-1.f,1.f);
  }

void Camera::setAimLean(float reach, float drop) {
  aimReach    = std::max(reach,0.f);
  aimLeanDrop = std::max(drop,0.f);
  }

static constexpr float ShoulderSide   = 40.f;  // cm right of the hero
static constexpr float ShoulderHeight = 165.f; // above the feet

Vec3 Camera::aimShoulder(const Vec3& feet, const Vec3& forward) {
  Vec3 right = Vec3::crossProduct(Vec3(0,1,0),Vec3(forward.x,0,forward.z));
  right = right.length()>1e-3f ? Vec3::normalize(right) : Vec3();
  return feet + Vec3(0,ShoulderHeight,0) + right*ShoulderSide;
  }

void Camera::aimRay(Tempest::Vec3& from, Tempest::Vec3& dir) const {
  const auto lp = listenerPosition();
  dir  = lp.front;
  from = lp.pos;
  if(aimBlend>0.f) {
    // what is between the camera and the shoulder (the hero) is not aimed at
    const float k = Vec3::dotProduct(aimFrom-from,dir);
    if(k>0.f)
      from = from + dir*k;
    }
  }

void Camera::tickAim(float dtF) {
  static float blendIn  = 0.2f;  // seconds
  static float blendOut = 0.25f;
  static float zoomRate = 6.f;   // 1/s
  static float fovZoom  = 0.15f; // of the field of view at full draw
  if(dtF>0.f) {
    const float was = aimBlend;
    aimBlend  = aimOn ? std::min(1.f,aimBlend+dtF/blendIn) : std::max(0.f,aimBlend-dtF/blendOut);
    aimZoomS += (aimZoom*aimBlend - aimZoomS)*std::min(1.f,dtF*zoomRate);
    if(!aimOn && aimPitchBack!=0.f) {
      // the elevation goes back with the blend, eased at both ends (the mouse still moves it meanwhile):
      // what is left of it is smoothstep(aimBlend)
      auto  ease = [](float b) { return b*b*(3.f-2.f*b); };
      const float left = was>0.f ? ease(aimBlend)/ease(was) : 0.f;
      const float give = aimPitchBack*(1.f-left);
      state.spin.x -= give;
      aimPitchBack -= give;
      }
    }
  if(aimBlend<=0.f) {
    // the lean over an edge stays while the camera goes back from the aim (no jump), then is gone
    aimReach    = 0;
    aimLeanDrop = 0;
    aimLeanRoom = 1;
    aimRangeRoom = 1;
    aimSteepAdvance = 0;
    aimSteepRoom = 1;
    }
  const float k = 1.f - fovZoom*aimZoomS;
  if(std::abs(k-fovK)>1e-4f) {
    fovK = k;
    updateProj();
    }
  }

void Camera::toggleDebug() {
  dbg = !dbg;
  }

void Camera::setSpin(const PointF &p) {
  state.spin = /*angleMod*/(Vec3(p.x,p.y,0));
  }

void Camera::setTarget(const Tempest::Vec3& pos) {
  state.target = pos;
  }

void Camera::setAngles(const Tempest::PointF& p) {
  angles = /*angleMod*/(Vec3(p.x,p.y,0));
  }

void Camera::setPosition(const Tempest::Vec3& pos) {
  origin = pos;
  }

void Camera::setDialogDistance(float d) {
  dlgRange = d;
  }

void Camera::onRotateMouse(const PointF& dpos) {
  state.spin.x += dpos.x;
  state.spin.y += dpos.y;
  }

Matrix4x4 Camera::projective() const {
  auto ret = proj;
  if(auto w = Gothic::inst().world())
    w->globalFx()->morph(ret);
  return ret;
  }

Matrix4x4 Camera::viewShadowVsm(const Tempest::Vec3& ldir) const {
  return mkViewShadowVsm(inter.target,ldir);
  }

Matrix4x4 Camera::viewShadowVsmLwc(const Tempest::Vec3& ldir) const {
  return mkViewShadowVsm(inter.target-(origin+shake),ldir);
  }

Matrix4x4 Camera::mkViewShadowVsm(const Vec3& cameraPos, const Vec3& ldir) const {
  float smWidth    = 1024; // ~4 pixels per santimeter
  float smDepth    = 10*5120;

  float smWidthInv = 1.f/smWidth;
  float zScale     = 1.f/smDepth;

  auto up = std::abs(ldir.z)<0.999 ? Vec3(0,0,1) : Vec3(1,0,0);
  auto z  = ldir;
  auto x  = Vec3::normalize(Vec3::crossProduct(z, up));
  auto y  = Vec3::crossProduct(x, z);

  auto view = Matrix4x4 {
           x.x, y.x, z.x, 0,
           x.y, y.y, z.y, 0,
           x.z, y.z, z.z, 0,
             0,   0,   0, 1
         };
  view.transpose();
  {
    Matrix4x4 scale;
    scale.identity();
    scale.scale(smWidthInv, smWidthInv, zScale);
    scale.mul(view);
    view = scale;
  }
  view.translate(-cameraPos);

  {
    Matrix4x4 proj;
    proj.identity();
    proj.translate(0.f, 0.f, 0.5f);
    proj.mul(view);
    view = proj;
  }

  return view;
  }

Matrix4x4 Camera::viewShadow(const Vec3& lightDir, size_t layer) const {
  auto  vp       = viewProj();
  float rotation = (angles.y-90);
  // if(layer==0)
  //   return viewShadowVsm(cameraPos,rotation,vp,lightDir);
  return mkViewShadow(inter.target,rotation,vp,lightDir,layer);
  }

Matrix4x4 Camera::viewShadowLwc(const Tempest::Vec3& lightDir, size_t layer) const {
  auto  vp       = viewProjLwc();
  float rotation = (angles.y-90);
  // if(layer==0)
  //   return viewShadowVsm(cameraPos-origin,rotation,vp,lightDir);
  return mkViewShadow(inter.target-origin,rotation,vp,lightDir,layer);
  }

Matrix4x4 Camera::mkViewShadow(const Vec3& cameraPos, float rotation, const Tempest::Matrix4x4& viewProj, const Vec3& lightDir, size_t layer) const {
  Vec3  ldir     = lightDir;
  float eps      = 0.005f;

  if(std::abs(ldir.y)<eps) {
    float k = (1.f-eps*eps)/std::sqrt(ldir.x*ldir.x + ldir.z*ldir.z);
    ldir.y = (ldir.y<0) ? -eps : eps;
    ldir.x *= k;
    ldir.z *= k;
    }

  Vec3 center = cameraPos;
  auto vp = viewProj;
  vp.project(center);

  vp.inverse();
  Vec3 l = {-1,center.y,center.z}, r = {1,center.y,center.z};
  vp.project(l);
  vp.project(r);

  float smWidth    = 0;
  float smDepth    = 5120*5;
  switch(layer) {
    case 0:
      smWidth    = (r-l).length();
      smWidth    = std::max(smWidth,1024.f); // ~4 pixels per santimeter
      smDepth    = 5120;
      break;
    case 1:
      smWidth    = 5120;
      smDepth    = 5120*5;
      break;
    };

  float smWidthInv = 1.f/smWidth;
  float zScale     = 1.f/smDepth;

  Matrix4x4 view;
  view.identity();
  view.rotate(-90, 1, 0, 0);     // +Y -> -Z
  view.rotate(rotation, 0, 1, 0);
  view.scale(smWidthInv, zScale, smWidthInv);
  view.translate(cameraPos);
  view.scale(-1,-1,-1);

  // sun direction
  if(ldir.y!=0.f) {
    float lx = ldir.x/std::abs(ldir.y);
    float lz = ldir.z/std::abs(ldir.y);

    const float ang = -rotation*float(M_PI)/180.f;
    const float c   = std::cos(ang), s = std::sin(ang);

    float dx = lx*c-lz*s;
    float dz = lx*s+lz*c;

    view.set(1,0, dx*smWidthInv);
    view.set(1,1, dz*smWidthInv);
    }

  // stretch shadowmap on light dir
  if(ldir.y!=0.f) {
    // stretch view to camera
    float r0 = std::fmod(rotation, 360.f);
    float r  = std::fmod(std::atan2(ldir.z,ldir.x)*180.f/float(M_PI), 360.f);
    r -= r0;

    float s = std::abs(ldir.y);
    Matrix4x4 proj;
    proj.identity();
    proj.rotate( r, 0, 0, 1);
    proj.translate(-0.5f,0,0);
    proj.scale(s, 1, 1);
    //proj.scale(0.1f, 1, 1);
    proj.translate(0.5f,0,0);
    proj.rotate(-r, 0, 0, 1);
    proj.mul(view);
    view = proj;
    }

  if(layer>0) {
    // projective shadowmap
    Tempest::Matrix4x4 proj;
    proj.identity();

    static float k = -0.4f;
    proj.set(1,3, k);
    proj.mul(view);
    view = proj;
    }

  auto inv = view;
  inv.inverse();
  Vec3 mid = {};
  inv.project(mid);
  view.translate(mid-cameraPos);

  {
    Matrix4x4 proj;
    proj.identity();
    switch(layer) {
      case 0:
        proj.translate(0.f, 0.8f, 0.5f);
        break;
      case 1: {
        proj.translate(0.f, 0.5f, 0.5f);
        break;
        }
      }

    proj.mul(view);
    view = proj;
    }

  return view;
  }

Camera::ListenerPos Camera::listenerPosition() const {
  auto vp = viewProj();
  vp.inverse();

  ListenerPos pos;
  pos.up    = Vec3(0,1,0);
  pos.front = Vec3(0,0,1);
  pos.pos   = Vec3(0,0,0);
  vp.project(pos.up);
  vp.project(pos.front);
  vp.project(pos.pos);

  pos.up    = Vec3::normalize(pos.up    - pos.pos);
  pos.front = Vec3::normalize(pos.front - pos.pos);
  return pos;
  }

const Def::Camera& Camera::cameraDef() const {
  auto& camd = Gothic::cameraDef();
  if(camMod==Dialog)
    return camd.dialogCam();
  if(lbEnable)
    return camd.backCam();
  if(fpEnable && (camMod==Normal || camMod==Melee))
    return camd.fpCam();
  if(camMod==Normal) {
    return camd.stdCam();
    }
  if(camMod==Inventory)
    return camd.inventoryCam();
  if(camMod==Melee)
    return camd.meleeCam();
  if(camMod==Ranged)
    return camd.rangeCam();
  if(camMod==Magic)
    return camd.mageCam();
  if(camMod==Swim)
    return camd.swimCam();
  if(camMod==Dive)
    return camd.diveCam();
  if(camMod==Fall)
    return camd.fallCam();
  if(camMod==Mobsi) {
    std::string_view tag = "", pos = "";
    if(auto pl = Gothic::inst().player())
      if(auto inter = pl->interactive()) {
        tag = inter->schemeName();
        pos = inter->posSchemeName();
        }
    return camd.mobsiCam(tag,pos);
    }
  if(camMod==Death)
    return camd.deathCam();
  return camd.stdCam();
  }

void Camera::implMove(Tempest::Event::KeyType key, uint64_t dt) {
  float dpos      = float(dt);
  float dRot      = dpos/15.f;
  float k         = float(M_PI/180.0);
  float s         = std::sin(-angles.y*k), c=std::cos(-angles.y*k);
  float sx        = std::sin(angles.x*k);
  float cx        = std::cos(angles.x*k);

  if(key==KeyEvent::K_A) {
    origin.x += dpos*s;
    origin.z += dpos*c;
    }
  if(key==KeyEvent::K_D) {
    origin.x -= dpos*s;
    origin.z -= dpos*c;
    }
  if(key==KeyEvent::K_W) {
    origin.x += dpos*c*cx;
    origin.z -= dpos*s*cx;
    origin.y -= dpos*sx;
    }
  if(key==KeyEvent::K_S) {
    origin.x -= dpos*c*cx;
    origin.z += dpos*s*cx;
    origin.y += dpos*sx;
    }
  if(key==KeyEvent::K_Q)
    state.spin.y += dRot;
  if(key==KeyEvent::K_E)
    state.spin.y -= dRot;
  }

Vec3 Camera::followTarget(Vec3 pos, Vec3 dest, float dtF) {
  auto dp  = (dest-pos);
  auto len = dp.length();

  if(dtF<=0.f) {
    return dest;
    }

  if(len<=minLength) {
    return dest;
    }

  // no idea how it trully meant to work in vanilla game
  if(inertiaTarget) {
    static float mul11 = 8.f;
    static float mul12 = 2.f;

    veloTrans = veloTrans+(dp-veloTrans)*std::min(1.f, mul11*dtF);
    pos += veloTrans*std::min(1.f, mul12*dtF);
    return pos;
    } else {
    static float mul21 = 3.f;

    veloTrans = dp;
    pos += veloTrans*std::min(1.f, mul21*dtF);
    return pos;
    }

  /*
    {
    auto diff = dp*k;
    float speed = diff.length()/dtF;
    static float prevSpeed = 0;

    if(false && speed > 1.f)
      Log::i("speed: ", speed, "delta: ", std::abs(prevSpeed-speed));
    prevSpeed = speed;
    }
  */

  return pos;
  }

Tempest::Vec3 Camera::followTrans(Vec3 pos, Tempest::Vec3 dest, float dtF, float velo) {
  if(dtF<0)
    return dest;

  /*
  static uint64_t time = 0;
  if((dest-pos).length()<1 || velo==0) {
    auto tx = Tempest::Application::tickCount();
    if(velo>0)
      Log::d("time = ", tx-time);
    time = tx;
    }
  */
  static float k = 0.25f;
  return pos + (dest-pos)*std::min(1.f, k*velo*dtF);
  }

Tempest::Vec3 Camera::followRot(Vec3 spin, Tempest::Vec3 dest, float dtF, float velo) {
  if(dtF<0.f)
    return dest;

#if 1
  const zenkit::Quat sx = fromAngles(spin);
  const zenkit::Quat dx = fromAngles(dest);

  const zenkit::Quat s  = slerp(sx, dx, std::min(1.f, dtF*velo));
  return toAngles(s);
#else
  const zenkit::Quat d = fromAngles(dest);
  return toAngles(d);
#endif
  }

void Camera::followAng(Vec3& spin, Vec3 dest, float dtF) {
  const auto& def = cameraDef();
  followAng(spin.x, dest.x, def.velo_rot, dtF);
  followAng(spin.y, dest.y, def.velo_rot, dtF);
  }

void Camera::followAng(float& ang, float dest, float speed, float dtF) {
  float da    = angleMod(dest-ang);
  float shift = da*std::min(1.f, speed*dtF);
  if(std::abs(da)<=0.0001f || dtF<0.f) {
    ang = dest;
    return;
    }
  ang += shift;
  }

void Camera::tick(uint64_t dt) {
  if(Gothic::inst().isPause() || (camMarvinMod==M_Freeze && camMod!=Dialog))
    return;

  if(isCutscene()) {
    return; // handle pass thru water ?
    }

  const float dtF = float(dt)/1000.f;

  {
    const auto& def = cameraDef();
    state.range = def.min_range + (def.max_range-def.min_range)*userRange;
  }
  tickAim(dtF);

  // normalize angles in -180..180 range, for convinience
  // dst.spin = angleMod(dst.spin);
  // src.spin = angleMod(src.spin);

  auto prev = origin;

  switch (camMarvinMod) {
    case M_Normal: {
      if(camMod==Camera::FirstPerson) {
        tickFirstPerson(dtF);
        }
      else {
        tickThirdPerson(dtF);
        }
      break;
      }
    case M_Freeze: {
      // nope
      break;
      }
    case M_Free: {
      inter.target = origin;
      angles       = followRot(angles, state.spin, dtF, 10.f);
      break;
      }
    case M_Pinned: {
      const auto pl     = Gothic::inst().player();
      const auto rotMat = pl!=nullptr ? pl->cameraMatrix(false) : Matrix4x4::mkIdentity();

      auto offset = pin.origin;
      rotMat.project(offset);
      origin       = offset;
      inter.target = offset;
      angles       = pin.spin;
      break;
      }
    }

  if(auto w = Gothic::inst().world()) {
    Vec3 sh;
    w->globalFx()->shake(sh);
    shake = shake + (sh-shake)*std::min(1.f, dtF*1000.f);
    }

  if(kickT<0.4f) {
    // up in about 12 ms, gone in about a tenth of a second, no swing back
    kickT  += dtF;
    kickOff = kickAmp*((1.f-std::exp(-kickT/0.012f))*std::exp(-kickT/0.07f));
    } else {
    kickOff = Vec3();
    }

  {
    //NOTE: in vanilla output of velocity is garbage in general, but zero for static camera
    targetVelo = (origin - prev).length()/dtF;
  }

  auto world = Gothic::inst().world();
  if(world!=nullptr) {
    auto  pl     = isFree() ? nullptr : world->player();
    auto& physic = *world->physic();

    if(pl!=nullptr && pl->isSwim()) {
      inWater = (angles.x < -8.f);
      }
    else if(pl!=nullptr && (pl->isInAir() || pl->isJump()) && !pl->isDead()) {
      // NOTE: not quite correct
      inWater = physic.cameraRay(inter.target, origin).waterCol % 2;
      }
    else {
      // NOTE: find a way to avoid persistent tracking
      inWater = inWater ^ (physic.cameraRay(prev, origin).waterCol % 2);
      }
    }
  }

void Camera::tickFirstPerson(float /*dtF*/) {
  const auto pl     = Gothic::inst().player();
  const auto rotMat = pl!=nullptr ? pl->cameraMatrix(true) : Matrix4x4::mkIdentity();

  state.spin   = clampRotation(state.spin);
  inter.target = state.target;

  Vec3 offset = {0,0,20};
  rotMat.project(offset);
  origin = offset;
  angles = state.spin;
  }

void Camera::tickThirdPerson(float dtF) {
  const auto& def = cameraDef();

  auto mkRotMatrix = [this](Vec3 spin){
    auto view = Camera::mkRotation(spin);
    view.inverse();
    return view;
    };

  auto  targetOffset = Vec3(def.target_offset_x,
                            def.target_offset_y,
                            def.target_offset_z);
  auto  rotOffsetDef = Vec3(def.rot_offset_x,
                            def.rot_offset_y,
                            def.rot_offset_z);
  auto  range        = (camMod==Dialog) ? dlgRange : state.range*100.f;

  if(camMod==Dialog) {
    // TODO: DialogCams.zen
    inter.target    = state.target;
    inter.rotOffset = Vec3(0);
    }

  if(camMod!=Dialog) {
    state.spin      = clampRotation(state.spin);
    inter.rotOffset = followRot(inter.rotOffset, rotOffsetDef, dtF, def.velo_rot);
    }

  const auto rotOffsetMat = mkRotMatrix(state.spin);
  if(camMod!=Dialog) {
    rotOffsetMat.project(targetOffset);
    // vanilla clamps target-offset according to collision
    if(def.collision!=0) {
      targetOffset = calcCameraColision(state.target, targetOffset);
      }
    // and has leash-like follow for target+offset
    // ignores def.translate
    inter.target = followTarget(inter.target, state.target+targetOffset, dtF);
    }

  auto dir = Vec3{0,0,1};
  rotOffsetMat.project(dir);

  if(true && def.collision!=0) {
    auto rotation = calcLookAtAngles(inter.target + dir*range, inter.target, inter.rotOffset, state.spin);
    // testd in marvin: collision is calculated from offseted 'target', not from npc
    range = calcCameraColision(inter.target, dir, rotation, range);
    // NOTE: with range < 80, camera gradually moves up in vanilla
    if(range<80.f) {
      range      = 150; // also collision?!
      rotation.x = 80;
      rotation.y = state.spin.y;
      const auto rotOffsetMat = mkRotMatrix(rotation);
      dir = Vec3{0,0,1};
      rotOffsetMat.project(dir);
      }
    }

  if(def.translate!=0) {
    origin = followTrans(origin, inter.target + dir*range, (camMod!=Dialog ? dtF : -1.f), def.velo_trans);
    }

  angles = calcLookAtAngles(origin, inter.target, inter.rotOffset, state.spin);

  if(aimBlend>0.f && camMod!=Dialog) {
    // over the shoulder: the camera looks along the spin through a point beside the head, the center of
    // the screen is where the bow aims (PlayerControl); no lag, the aim follows the mouse
    static float aimRange  = 150.f;
    static float aimZoomIn = 35.f;  // closer at full draw
    Vec3 back = {0,0,1};
    mkRotMatrix(state.spin).project(back);
    // over the shoulder as the hero stands: crouched lower, leaning out to the side, over the left shoulder
    // leaning out to the left
    Vec3 right = Vec3::crossProduct(Vec3(0,1,0),Vec3(-back.x,0,-back.z));
    right = right.length()>1e-3f ? Vec3::normalize(right) : Vec3();
    Vec3 at = aimAnchor + Vec3(0,ShoulderHeight-aimDrop,0) + right*(ShoulderSide*aimShoulderK + aimSide);
    // leaning over an edge the head goes ahead and down, the shoulder with it: past the edge, the target
    // below in view. Not into a roof or a wall: as far as there is room (closer at once, back eased)
    static float leanClear = 20.f; // cm
    static float roomRate  = 4.f;  // 1/s
    Vec3 ahead = Vec3(-back.x,0,-back.z);
    ahead = ahead.length()>1e-3f ? Vec3::normalize(ahead) : Vec3();
    const Vec3  lean    = ahead*aimReach - Vec3(0,aimLeanDrop,0);
    const float leanLen = lean.length();
    if(leanLen>1e-3f) {
      float room = 1;
      if(auto world = Gothic::inst().world()) {
        auto rc = world->physic()->ray(at, at + lean*((leanLen+leanClear)/leanLen));
        if(rc.hasCol)
          room = std::clamp(((rc.v-at).length()-leanClear)/leanLen, 0.f, 1.f);
        }
      if(room<aimLeanRoom)
        aimLeanRoom = room;
      else
        aimLeanRoom += (room-aimLeanRoom)*std::min(1.f,dtF*roomRate);
      at += lean*aimLeanRoom;
      } else {
      aimLeanRoom = 1;
      }
    // At steep downward angles the old shoulder ray runs through the bow and hands.
    // Move its pivot ahead instead of moving along that same obstructed ray. An existing
    // lean already supplies part of the reach; do not add another full reach to it.
    aimSteepAdvance = AimCamera::follow(aimSteepAdvance,state.spin.x,dtF);
    const float advance = std::max(0.f,aimSteepAdvance-aimReach*aimLeanRoom);
    if(advance>1e-3f) {
      const Vec3 shift = ahead*advance;
      const float room = def.collision!=0 ? calcCameraColision(at,shift).length()/advance : 1.f;
      if(room<aimSteepRoom)
        aimSteepRoom = room;
      else
        aimSteepRoom += (room-aimSteepRoom)*std::clamp(dtF*roomRate,0.f,1.f);
      at += shift*aimSteepRoom;
      } else {
      aimSteepRoom = 1;
      }
    // behind the shoulder not into a wall either: closer at once, back out eased (at once the camera went a
    // metre in one frame as the rays behind stopped hitting the wall)
    float      r  = aimRange - aimZoomIn*aimZoomS;
    if(def.collision!=0) {
      const float room = calcCameraColision(at, back, calcLookAtAngles(at+back*r, at, Vec3(), state.spin), r)/r;
      if(room<aimRangeRoom)
        aimRangeRoom = room;
      else
        aimRangeRoom += (room-aimRangeRoom)*std::clamp(dtF*roomRate,0.f,1.f);
      r *= aimRangeRoom;
      }
    const Vec3 o = at + back*std::max(r,20.f);
    const Vec3 a = calcLookAtAngles(o, at, Vec3(), state.spin);
    aimFrom  = at;
    origin   = origin + (o-origin)*aimBlend;
    angles.x = angles.x + (a.x-angles.x)*aimBlend;
    angles.y = angles.y + angleMod(a.y-angles.y)*aimBlend;
    angles.z = angles.z*(1.f-aimBlend);
    }

  static bool dbg = false;
  if(dbg) {
    origin = state.target + targetOffset + dir*range;
    angles = calcLookAtAngles(origin, state.target, inter.rotOffset, state.spin);
    }
  }

float Camera::calcCameraColision(const Tempest::Vec3& target, const Tempest::Vec3& dir, const Tempest::Vec3& angles, float range) const {
  //static float minDist = 20;
  static float padding = 25;
  static int n = 1, nn=1;

  auto world = Gothic::inst().world();
  if(world==nullptr)
    return range;

  const auto origin = target + dir*range;

  Matrix4x4 vinv = projective();
  vinv.mul(mkView(origin,angles));
  vinv.inverse();

  auto& physic = *world->physic();
  auto  dview  = (origin - target);

  raysCasted = 0;
  float distM = range;
  for(int i=-1;i<=n;++i)
    for(int r=-n;r<=n;++r) {
      raysCasted++;
      float u = float(i)/float(nn),v = float(r)/float(nn);
      Tempest::Vec3 r1 = {u,v,depthNear};
      vinv.project(r1);
      auto dr = (r1 - target);
      dr = dr * (range+padding) / (dr.length()+0.00001f);

      auto rc = physic.ray(target, target+dr);
      if(!rc.hasCol)
        continue;

      auto  tr    = (rc.v - target);
      float dist1 = Vec3::dotProduct(dview,tr)/range;

      dist1 = std::max<float>(dist1-padding, 0);
      if(dist1<distM)
        distM = dist1;
      }

  return distM;
  }

Vec3 Camera::calcLookAtAngles(const Tempest::Vec3& origin, const Tempest::Vec3& target, const Vec3& rotOffset, const Vec3& defSpin) const {
  auto  sXZ   = (target - origin);

  float lenXZ = Vec2(sXZ.x,sXZ.z).length();
  float y0    = std::atan2(sXZ.z, sXZ.x)*180.f/float(M_PI);
  float x0    = std::atan2(sXZ.y, lenXZ)*180.f/float(M_PI);

  if(lenXZ < 4.f)
    y0 = defSpin.y;

  return Vec3(-x0,y0,0) - rotOffset;
  }

Vec3 Camera::calcCameraColision(const Tempest::Vec3& from, const Tempest::Vec3& dir) const {
  static float padding = 25;

  auto world = Gothic::inst().world();
  if(world==nullptr)
    return dir;

  const auto& physic = *world->physic();
  const auto  len    = dir.length() + padding;
  const auto  nrm    = Vec3::normalize(dir);

  auto rc = physic.ray(from, from+nrm*len);
  if(!rc.hasCol)
    return dir;

  return nrm*std::max(len*rc.hitFraction-padding, 0.f);
  }

Vec3 Camera::clampRotation(Tempest::Vec3 spin) {
  //NOTE: min elevation is zero for nomal camera. assume that it's ignored by vanilla
  // The advanced manual pivot can be almost above a nearby target. Keep it under the
  // crosshair instead of clamping several degrees short after moving in front of the hands.
  float       maxElev = aimOn || aimBlend>0.f ? 89.f : 85.f;
  float       minElev = aimOn || aimBlend>0.f ? -80 : -60; // a bow aimed by hand: high up too (and going back)
  float       maxAzim = +180;
  float       minAzim = -180;

  const auto pl = Gothic::inst().player();
  if(pl==nullptr)
    return spin;

  const auto plSpin = (Vec3{0,  pl->rotation(), 0});
  spin = spin - plSpin;

  spin.x = std::clamp(spin.x, minElev, maxElev);
  spin.y = std::clamp(spin.y, minAzim, maxAzim);
  return (spin + plSpin);
  }

Matrix4x4 Camera::mkView(const Vec3& pos, const Vec3& spin) const {
  auto view = Matrix4x4::mkIdentity();
  view.scale(-1,-1,-1);

  view.mul(mkRotation(spin));
  view.translate(-pos);

  return view;
  }

Matrix4x4 Camera::mkRotation(const Vec3& spin) const {
  auto view = Matrix4x4::mkIdentity();
  view.rotateOX(spin.x);
  view.rotateOY(spin.y);
  view.rotateOZ(spin.z);
  view.rotateOY(90); // X forward
  return view;
  }

void Camera::debugDraw(DbgPainter& p) {
  if(!dbg)
    return;

  p.setPen(Color(0,1,0));
  p.drawLine(state.target, inter.target);
  p.drawLine(inter.target, origin);

  if(auto pl = Gothic::inst().player()) {
    float a  = pl->rotationRad()+float(M_PI*0.5);
    float c  = std::cos(a), s = std::sin(a);
    auto  ln = Vec3(c,0,s)*25.f;
    p.drawLine(inter.target-ln, inter.target+ln);
    }

  auto& fnt = Resources::font(1.0);
  int   y   = 300+fnt.pixelSize();

  string_frm buf("RaysCasted: ",raysCasted);
  p.drawText(8,y,buf); y += fnt.pixelSize();

  buf = string_frm("PlayerPos : ",state.target.x, ' ', state.target.y, ' ', state.target.z);
  p.drawText(8,y,buf); y += fnt.pixelSize();

  buf = string_frm("targetVelo : ",targetVelo);
  p.drawText(8,y,buf); y += fnt.pixelSize()*4;

  buf = string_frm("Range To Player : ", (inter.target-origin).length());
  p.drawText(8,y,buf); y += fnt.pixelSize();

  buf = string_frm("Azimuth : ", angleMod(angles.y-state.spin.y));
  p.drawText(8,y,buf); y += fnt.pixelSize();
  buf = string_frm("Elevation : ", inter.rotOffset.x+angles.x);
  p.drawText(8,y,buf); y += fnt.pixelSize();
  }

float Camera::azimuth() const {
  return angleMod(state.spin.y-angles.y);
  }

PointF Camera::spin() const {
  return PointF(state.spin.x,state.spin.y);
  }

Vec3 Camera::destTarget() const {
  return state.target;
  }

Matrix4x4 Camera::viewProj() const {
  Matrix4x4 ret=projective();
  ret.mul(view());
  return ret;
  }

Matrix4x4 Camera::view() const {
  return mkView(origin+shake, angles+kickOff);
  }

Matrix4x4 Camera::viewLwc() const {
  return mkView(Vec3(0), angles+kickOff);
  }

void Camera::impact(const Vec3& dir, float deg) {
  const float k    = float(M_PI/180.0);
  const Vec3  left = {std::sin(-angles.y*k), 0, std::cos(-angles.y*k)};
  const float len  = dir.length();
  const float side = len>0.f ? Vec3::dotProduct(dir,left)/len : 0.f;
  kickAmp = kickOff + Vec3(deg, side*deg*0.6f, 0);
  kickT   = 0;
  }

Matrix4x4 Camera::viewProjLwc() const {
  Matrix4x4 ret=projective();
  ret.mul(viewLwc());
  return ret;
  }

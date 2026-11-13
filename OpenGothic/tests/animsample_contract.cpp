// AnimSample (graphics/mesh/animsample): the pose of a clip between its keys. A joint keyed ten times a second and
// shown thirty times a second: on the straight line between two keys (what the engine did, and still does for
// clips keyed 25 times a second) its speed changes in one frame at a key; on the curve it does not. The traps of
// a curve: the pose at a key is the key, a hold stays a hold, a sharp stop does not swing past its key, a clip
// starts from rest and comes to rest at its last key, a rotation's two signs are one rotation. The keys around a time for loops,
// clips that end, and clips played backwards; the root's curve against the line the body travels by.
#include "graphics/mesh/animsample.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Sample = AnimSample::Sample;

void require(bool condition, const std::string& message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message.c_str());
  std::exit(EXIT_FAILURE);
  }

// a joint turned by `deg` about Z, `x` cm along X
Sample key(float deg, float x=0.f) {
  const float h = deg*float(M_PI)/360.f;
  Sample s;
  s.position = asset::Vec3(x,0,0);
  s.rotation = asset::Quat(std::cos(h),0,0,std::sin(h));
  return s;
  }

float degrees(const Sample& s) {
  return 2.f*std::atan2(s.rotation.z,s.rotation.w)*180.f/float(M_PI);
  }

// what the engine did between two keys
Sample line(const Sample& a, const Sample& b, float t) {
  return key(degrees(a)+(degrees(b)-degrees(a))*t, a.position.x+(b.position.x-a.position.x)*t);
  }

// the clip at a time, thirty frames a second from ten keys a second
Sample play(const std::vector<Sample>& clip, uint64_t ms, bool smooth, bool loop=false) {
  const auto k = AnimSample::keys(ms,10.f,clip.size(),loop,false);
  if(!smooth)
    return line(clip[k.a],clip[k.b],k.t);
  return AnimSample::mix(k.hasBefore ? &clip[k.before] : nullptr,clip[k.a],clip[k.b],
                         k.hasAfter ? &clip[k.after] : nullptr,k.t);
  }

// the largest change of the step between two frames, degrees: the stand's an_pop for one joint. The clip as the
// game shows it: its first key before it starts, its last key held after its keys are over
float stepChange(const std::vector<Sample>& clip, bool smooth) {
  float     worst  = 0;
  const int frames = int(float(clip.size()-1)*100.f*30.f/1000.f)+4;
  auto at = [&](int frame) {
    return degrees(play(clip,uint64_t(std::max(frame,0))*1000/30,smooth));
    };
  for(int f=-4; f+2<=frames; ++f)
    worst = std::max(worst,std::abs(at(f+2)-2.f*at(f+1)+at(f)));
  return worst;
  }

void speedCases() {
  // a swing of 30 degrees with a period of 1.6 s, as the trunk goes in the dialog gestures, keyed ten times a second
  std::vector<Sample> clip;
  for(int i=0; i<=32; ++i)
    clip.push_back(key(30.f*std::sin(float(i)*0.1f*2.f*float(M_PI)/1.6f)));

  // the speed just before a key and just after it, degrees a second
  float jumpLine = 0, jumpCurve = 0;
  for(size_t i=2; i+2<clip.size(); ++i) {
    for(const bool smooth:{false,true}) {
      const uint64_t at  = uint64_t(i)*100;
      const float    in  = (degrees(play(clip,at,smooth))  -degrees(play(clip,at-2,smooth)))*500.f;
      const float    out = (degrees(play(clip,at+2,smooth))-degrees(play(clip,at,smooth)))  *500.f;
      (smooth ? jumpCurve : jumpLine) = std::max(smooth ? jumpCurve : jumpLine,std::abs(out-in));
      }
    }
  require(jumpLine>15.f,"on the line the speed must jump at a key (the case the curve is for): "+std::to_string(jumpLine)+" deg/s");
  require(jumpCurve<1.5f,"on the curve the speed must not jump at a key: "+std::to_string(jumpCurve)+" deg/s");

  // shown thirty times a second: the step changes less from frame to frame, also where the clip starts at its
  // full speed (this swing does) and where it ends
  const float was = stepChange(clip,false), now = stepChange(clip,true);
  require(now<0.7f*was,"on the curve the step of a joint must change less between frames: "+std::to_string(now)+
          " deg against "+std::to_string(was)+" on the line");
  // inside the clip, away from its ends, far less
  const std::vector<Sample> inner(clip.begin()+4,clip.begin()+29);  // starts and ends at the swing's turn: at rest
  const float wasIn = stepChange(inner,false), nowIn = stepChange(inner,true);
  require(nowIn<0.6f*wasIn,"inside a clip the step of a joint on the curve must change far less: "+std::to_string(nowIn)+
          " deg against "+std::to_string(wasIn)+" on the line");

  // and the curve is nearer to the swing the keys were taken from
  float offLine = 0, offCurve = 0;
  for(uint64_t ms=100; ms<3100; ms+=10) {
    const float truth = 30.f*std::sin(float(ms)*0.001f*2.f*float(M_PI)/1.6f);
    offLine  = std::max(offLine, std::abs(degrees(play(clip,ms,false))-truth));
    offCurve = std::max(offCurve,std::abs(degrees(play(clip,ms,true)) -truth));
    }
  require(offCurve<0.5f*offLine,"the curve must be nearer to the motion the keys came from: "+std::to_string(offCurve)+
          " deg against "+std::to_string(offLine));
  }

void trapCases() {
  const Sample a = key(10,1), b = key(25,4), before = key(-20,0), after = key(60,9);
  auto same = [](const Sample& x, const Sample& y) {
    return std::abs(degrees(x)-degrees(y))<1e-3f && std::abs(x.position.x-y.position.x)<1e-4f;
    };
  // the pose at a key is the key
  require(same(AnimSample::mix(&before,a,b,&after,0.f),a) && same(AnimSample::mix(&before,a,b,&after,1.f),b),
          "at a key the pose must be the key");

  // a hold stays a hold, whatever moves around it
  for(float t=0; t<=1.f; t+=0.125f) {
    const Sample s = AnimSample::mix(&before,a,a,&after,t);
    require(s.position.x==a.position.x && std::abs(degrees(s)-degrees(a))<1e-4f,"a hold must stay a hold");
    }

  // a sharp stop: in at 40 degrees a key, then still. Plain Catmull-Rom swings 3 degrees past the key
  const Sample s0 = key(0), s1 = key(40,8), s2 = key(40,8), s3 = key(40,8);
  float past = 0, pastX = 0;
  for(float t=0; t<=1.f; t+=0.05f) {
    const Sample in = AnimSample::mix(nullptr,s0,s1,&s2,t), on = AnimSample::mix(&s0,s1,s2,&s3,t);
    past  = std::max({past, degrees(in)-40.f,degrees(on)-40.f});
    pastX = std::max({pastX,in.position.x-8.f,on.position.x-8.f});
    }
  require(past<0.01f && pastX<0.001f,"a joint that stops at a key must not swing past it: "+std::to_string(past)+" deg");
  // and it comes to rest there: the last tenth of the way in is slower than the chord
  const float end = degrees(AnimSample::mix(nullptr,s0,s1,&s2,1.f))-degrees(AnimSample::mix(nullptr,s0,s1,&s2,0.9f));
  require(end<0.5f*4.f,"into a stop the joint must slow down: "+std::to_string(end)+" deg over the last tenth");

  // a slow key next to a fast one: the tangent is three times the slow chord at most
  const Sample f0 = key(0), f1 = key(1), f2 = key(41), f3 = key(81);
  float low = 0;
  for(float t=0; t<=1.f; t+=0.05f)
    low = std::min(low,degrees(AnimSample::mix(&f0,f1,f2,&f3,t))-1.f);
  require(low>-0.01f,"out of a slow key the joint must not dip back first: "+std::to_string(low)+" deg");

  // a clip's ends have no key outside: it starts from rest and comes to rest. The game shows the first key before
  // the clip starts and holds the last one after its keys are over: on the chord's speed the joint would start
  // and stop in one frame
  const Sample c0 = key(0), c1 = key(10), c2 = key(20), c3 = key(30);
  const float first = degrees(AnimSample::mix(nullptr,c0,c1,&c2,0.1f));
  const float last  = 30.f-degrees(AnimSample::mix(&c1,c2,c3,nullptr,0.9f));
  require(first<0.5f && last<0.5f,"a clip must start from rest and come to rest at its last key: "+std::to_string(first)+
          " and "+std::to_string(last)+" deg over its outer tenths, 1 on the chord");
  const Sample mid = AnimSample::mix(nullptr,a,b,nullptr,0.5f);
  require(std::abs(degrees(mid)-17.5f)<0.01f && std::abs(mid.position.x-2.5f)<1e-4f,
          "between two keys with none around the pose must be half way at half the time");

  // q and -q are one rotation
  Sample nb = b, nafter = after;
  nb.rotation     = asset::Quat(-b.rotation.w,-b.rotation.x,-b.rotation.y,-b.rotation.z);
  nafter.rotation = asset::Quat(-after.rotation.w,-after.rotation.x,-after.rotation.y,-after.rotation.z);
  for(float t=0.1f; t<1.f; t+=0.2f) {
    const Sample x = AnimSample::mix(&before,a,b,&after,t), y = AnimSample::mix(&before,a,nb,&nafter,t);
    require(std::abs(degrees(x)-degrees(y))<1e-3f,"the sign of a key's quaternion must not change the pose");
    const float len = std::sqrt(x.rotation.w*x.rotation.w+x.rotation.x*x.rotation.x+x.rotation.y*x.rotation.y+x.rotation.z*x.rotation.z);
    require(std::abs(len-1.f)<1e-5f,"the rotation must be of unit length");
    }
  }

void keyCases() {
  // which clips: under 16 keys a second, three frames at least
  require(AnimSample::sparse(10.f,96) && AnimSample::sparse(15.f,30) && AnimSample::sparse(12.5f,30) && AnimSample::sparse(5.f,10),
          "sparse clips go on the curve");
  require(!AnimSample::sparse(19.f,40) && !AnimSample::sparse(25.f,40) && !AnimSample::sparse(30.f,40),
          "clips keyed 16 times a second or more stay on the line");
  require(!AnimSample::sparse(10.f,2) && !AnimSample::sparse(10.f,1) && !AnimSample::sparse(0.f,10),"a pose or a pair of keys is no curve");

  // a clip that ends: 5 frames, ten a second
  auto k = AnimSample::keys(0,10.f,5,false,false);
  require(k.a==0 && k.b==1 && !k.hasBefore && k.hasAfter && k.after==2 && k.t==0.f,"the first interval has no key before it");
  k = AnimSample::keys(150,10.f,5,false,false);
  require(k.a==1 && k.b==2 && k.hasBefore && k.before==0 && k.hasAfter && k.after==3 && std::abs(k.t-0.5f)<1e-6f,
          "inside the clip the keys around the time are the two before and the two after");
  k = AnimSample::keys(350,10.f,5,false,false);
  require(k.a==3 && k.b==4 && k.before==2 && !k.hasAfter,"the last interval has no key after it");
  k = AnimSample::keys(900,10.f,5,false,false);
  require(k.a==4 && k.b==4,"past its end the clip holds its last key");

  // a loop of 4 frames: the keys wrap, and tell how many cycles on they are
  k = AnimSample::keys(0,10.f,4,true,false);
  require(k.a==0 && k.b==1 && k.before==3 && k.after==2 && k.hasBefore && k.hasAfter &&
          k.cycBefore==-1 && k.cycB==0 && k.cycAfter==0,"a loop's first key has its last one before it, a cycle back");
  k = AnimSample::keys(250,10.f,4,true,false);
  require(k.a==2 && k.b==3 && k.before==1 && k.after==0 && k.cycBefore==0 && k.cycB==0 && k.cycAfter==1,
          "the key after a loop's last one is its first, a cycle on");
  k = AnimSample::keys(750,10.f,4,true,false);
  require(k.a==3 && k.b==0 && k.before==2 && k.after==1 && k.cycB==1 && k.cycAfter==1,"over the seam both keys ahead are a cycle on");

  // played backwards: the same keys from the other end, the cycles the other way
  k = AnimSample::keys(150,10.f,5,false,true);
  require(k.a==3 && k.b==2 && k.before==4 && k.after==1 && k.hasBefore && k.hasAfter,"a clip played backwards runs its keys from the last");
  k = AnimSample::keys(750,10.f,4,true,true);
  require(k.a==0 && k.b==3 && k.before==1 && k.after==2 && k.cycB==-1 && k.cycAfter==-1,
          "a loop played backwards crosses its seam a cycle back");

  // the two keys around the time and the way between them are what Pose::updateFrame took before: the clips on
  // the straight line (25 keys a second: blows, walks) get the same frames
  for(const float fps:{5.f,10.f,12.5f,15.f,25.f,29.56f,30.f})
    for(const size_t count:{size_t(1),size_t(2),size_t(3),size_t(20),size_t(41)})
      for(const bool loop:{false,true})
        for(const bool reverse:{false,true})
          for(uint64_t ms=0; ms<9000; ms+=7) {
            const uint64_t frame = uint64_t(float(ms)*fps);
            uint64_t a = frame/1000, b = frame/1000+1;
            if(loop) {
              a %= count; b %= count;
              } else {
              a = std::min<uint64_t>(a,count-1); b = std::min<uint64_t>(b,count-1);
              }
            if(reverse) {
              a = count-1-a; b = count-1-b;
              }
            const auto now = AnimSample::keys(ms,fps,count,loop,reverse);
            require(now.a==a && now.b==b && now.t==float(frame%1000)/1000.f,
                    "the two keys around a time must be the ones the pose took before: "+std::to_string(ms)+" ms at "+
                    std::to_string(fps)+" keys/s, "+std::to_string(count)+" frames");
            require(now.before<count && now.after<count,"the keys around must be frames of the clip");
            }

  // a loop on the curve has no jump of speed over its seam
  std::vector<Sample> loop;
  for(int i=0; i<16; ++i)
    loop.push_back(key(20.f*std::sin(float(i)*2.f*float(M_PI)/16.f)));
  const float in  = (degrees(play(loop,1600,true,true))-degrees(play(loop,1598,true,true)))*500.f;
  const float out = (degrees(play(loop,1602,true,true))-degrees(play(loop,1600,true,true)))*500.f;
  require(std::abs(out-in)<1.f,"a loop on the curve must keep its speed over the seam: "+std::to_string(in)+" then "+std::to_string(out));
  }

void rootCases() {
  using V = asset::Vec3;
  // a root that goes at one speed is on the line the body travels by
  const V e0(0,0,0), e1(0,0,10), e2(0,0,20), e3(0,0,30);
  for(float t=0; t<=1.f; t+=0.25f) {
    const V off = AnimSample::offLine(&e0,e1,e2,&e3,t);
    require(std::abs(off.x)<1e-4f && std::abs(off.z)<1e-4f,"a root at one speed must be on the body's line");
    }
  // one that speeds up is behind the line between its keys, and on it at the keys
  const V a0(0,0,0), a1(0,0,2), a2(0,0,10), a3(0,0,24);
  const V mid = AnimSample::offLine(&a0,a1,a2,&a3,0.5f);
  require(mid.z<-0.3f && mid.z>-2.f,"a root that speeds up must be behind the line between its keys: "+std::to_string(mid.z));
  const V at0 = AnimSample::offLine(&a0,a1,a2,&a3,0.f), at1 = AnimSample::offLine(&a0,a1,a2,&a3,1.f);
  require(std::abs(at0.z)<1e-5f && std::abs(at1.z)<1e-4f,"at its keys the root must be on the body's line");
  }

}

int main() {
  speedCases();
  trapCases();
  keyCases();
  rootCases();
  std::puts("animsample contract ok");
  return 0;
  }

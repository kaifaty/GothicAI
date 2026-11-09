#include "taa.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

namespace {
// column-major like Tempest::Matrix4x4: m[column][row]; doubles keep the reprojection exact enough to
// give a camera that stands still a zero offset of the history at any resolution
struct Mat {
  double m[4][4] = {};
  };

Mat toMat(const Matrix4x4& a) {
  Mat r;
  for(int c=0; c<4; ++c)
    for(int i=0; i<4; ++i)
      r.m[c][i] = double(a.at(c,i));
  return r;
  }

Matrix4x4 toMatrix(const Mat& a) {
  Matrix4x4 r;
  for(int c=0; c<4; ++c)
    for(int i=0; i<4; ++i)
      r.set(c,i,float(a.m[c][i]));
  return r;
  }

Mat mul(const Mat& a, const Mat& b) {
  Mat r;
  for(int c=0; c<4; ++c)
    for(int i=0; i<4; ++i) {
      double s = 0;
      for(int k=0; k<4; ++k)
        s += a.m[k][i]*b.m[c][k];
      r.m[c][i] = s;
      }
  return r;
  }

Mat translation(double x, double y, double z) {
  Mat r;
  for(int i=0; i<4; ++i)
    r.m[i][i] = 1;
  r.m[3][0] = x;
  r.m[3][1] = y;
  r.m[3][2] = z;
  return r;
  }

// Gauss-Jordan with partial pivoting; a singular matrix gives identity (no reprojection rather than NaN)
Mat inverse(const Mat& src) {
  double a[4][8] = {};
  for(int i=0; i<4; ++i) {
    for(int j=0; j<4; ++j)
      a[i][j] = src.m[j][i]; // row i, column j
    a[i][4+i] = 1;
    }
  for(int col=0; col<4; ++col) {
    int piv = col;
    for(int i=col+1; i<4; ++i)
      if(std::abs(a[i][col])>std::abs(a[piv][col]))
        piv = i;
    if(std::abs(a[piv][col])<1e-300)
      return translation(0,0,0);
    if(piv!=col)
      for(int j=0; j<8; ++j)
        std::swap(a[piv][j],a[col][j]);
    const double k = 1.0/a[col][col];
    for(int j=0; j<8; ++j)
      a[col][j] *= k;
    for(int i=0; i<4; ++i) {
      if(i==col)
        continue;
      const double f = a[i][col];
      if(f==0)
        continue;
      for(int j=0; j<8; ++j)
        a[i][j] -= f*a[col][j];
      }
    }
  Mat r;
  for(int i=0; i<4; ++i)
    for(int j=0; j<4; ++j)
      r.m[j][i] = a[i][4+j];
  return r;
  }
}

float Taa::halton(uint32_t index, uint32_t base) {
  float f = 1, r = 0;
  while(index>0) {
    f /= float(base);
    r += f*float(index%base);
    index /= base;
    }
  return r;
  }

Vec2 Taa::jitterPx(uint32_t frame, uint32_t phases) {
  // from 1: index 0 of Halton is 0 in both dimensions, a corner of the pixel
  const uint32_t i = frame%std::max(phases,1u) + 1;
  return Vec2(halton(i,2) - 0.5f, halton(i,3) - 0.5f);
  }

uint32_t Taa::upscalePhases(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
  const double in  = double(std::max(inW,1u))*double(std::max(inH,1u));
  const double out = double(outW)*double(outH);
  // a hair below the integer: 2x by the area is 32 phases, not 33 from a rounding of the sizes
  const double n   = std::ceil(8.0*out/in - 1e-3);
  return uint32_t(std::clamp(n, double(Phases), 1024.0));
  }

float Taa::upscaleMipBias(uint32_t inW, uint32_t outW) {
  if(inW==0 || outW==0)
    return 0;
  return float(std::log2(double(inW)/double(outW)) - 1.0);
  }

Matrix4x4 Taa::jitterMatrix(const Vec2& px, uint32_t w, uint32_t h) {
  auto m = Matrix4x4::mkIdentity();
  m.translate(2.f*px.x/float(std::max(w,1u)), 2.f*px.y/float(std::max(h,1u)), 0.f);
  return m;
  }

Matrix4x4 Taa::reprojection(const View& cur, const View& prev, const Matrix4x4& jitter) {
  const Mat j     = toMat(jitter);
  const Mat curVp = mul(j, mul(toMat(cur.proj), toMat(cur.viewLwc)));
  // a point relative to this camera, moved to be relative to the previous one
  const Mat shift = translation(double(cur.origin.x) - double(prev.origin.x),
                                double(cur.origin.y) - double(prev.origin.y),
                                double(cur.origin.z) - double(prev.origin.z));
  const Mat prvVp = mul(j, mul(toMat(prev.proj), toMat(prev.viewLwc)));
  return toMatrix(mul(prvVp, mul(shift, inverse(curVp))));
  }

Matrix4x4 Taa::viewProjectPrev(const View& prev, const Matrix4x4& jitter) {
  return toMatrix(mul(toMat(jitter), mul(toMat(prev.proj), toMat(prev.viewLwc))));
  }

Taa::MotionCamera Taa::motionCamera(const View& cur, const View& prev, bool continuous, const Matrix4x4& jitter) {
  MotionCamera ret;
  ret.reprojectClip   = continuous ? reprojection(cur, prev, jitter) : Matrix4x4::mkIdentity();
  ret.viewProjectPrev = viewProjectPrev(continuous ? prev : cur, jitter);
  return ret;
  }

Vec2 Taa::motionResidual(const Matrix4x4& reprojectClip, const Matrix4x4& viewProjectPrev, const Vec4& clipCur,
                         const Vec3& d) {
  // as the shaders do it: prevStatic and prevDelta are the varyings, the division is per fragment
  Vec4 prevStatic = clipCur;
  reprojectClip.project(prevStatic);
  Vec4 prevDelta = Vec4(d.x, d.y, d.z, 0);
  viewProjectPrev.project(prevDelta);
  const float w0 = prevStatic.w, w1 = prevStatic.w + prevDelta.w;
  if(w0<=0 || w1<=0)
    return Vec2();
  // ((s+d)/w1 - s/w0)*0.5 over one denominator, as main.frag: exactly 0 for d=0
  const float den = w0*w1;
  return Vec2((prevDelta.x*w0 - prevStatic.x*prevDelta.w)/den*0.5f,
              (prevDelta.y*w0 - prevStatic.y*prevDelta.w)/den*0.5f);
  }

float Taa::rotationAngle(const Matrix4x4& a, const Matrix4x4& b) {
  // trace(Ra^T Rb) = 1 + 2 cos(angle); a sign flip of both (the view's scale -1) cancels out
  double tr = 0, na = 0, nb = 0;
  for(int c=0; c<3; ++c) {
    for(int i=0; i<3; ++i) {
      tr += double(a.at(c,i))*double(b.at(c,i));
      na += double(a.at(c,i))*double(a.at(c,i));
      nb += double(b.at(c,i))*double(b.at(c,i));
      }
    }
  if(na<=0 || nb<=0)
    return 0;
  // rows of a rotation are unit: the norms are 3 each; scale them away should the view carry a scale
  tr *= 3.0/std::sqrt(na*nb);
  const double c = std::clamp((tr-1.0)*0.5, -1.0, 1.0);
  return float(std::acos(c)*180.0/M_PI);
  }

bool Taa::isCameraJump(const View& cur, const View& prev) {
  const Vec3 d = cur.origin - prev.origin;
  if(d.length()>JumpDistance)
    return true;
  return rotationAngle(cur.viewLwc, prev.viewLwc)>JumpAngle;
  }

#pragma once

#include <algorithm>
#include <limits>
#include <vector>

namespace asset {

struct Vec2 {
  float x = 0, y = 0;

  constexpr Vec2() = default;
  constexpr Vec2(float x, float y):x(x),y(y) {}
  constexpr bool operator==(const Vec2&) const = default;
  };

struct Vec3 {
  float x = 0, y = 0, z = 0;

  constexpr Vec3() = default;
  constexpr Vec3(float x, float y, float z):x(x),y(y),z(z) {}
  explicit constexpr Vec3(float v):Vec3(v,v,v) {}
  constexpr float& operator[](unsigned i) { return (&x)[i]; }
  constexpr const float& operator[](unsigned i) const { return (&x)[i]; }
  constexpr bool operator==(const Vec3&) const = default;
  constexpr Vec3 operator*(float v) const { return {x*v,y*v,z*v}; }
  };

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;

  constexpr Vec4() = default;
  constexpr Vec4(float x, float y, float z, float w):x(x),y(y),z(z),w(w) {}
  constexpr float& operator[](unsigned i) { return (&x)[i]; }
  constexpr const float& operator[](unsigned i) const { return (&x)[i]; }
  };

struct Quat {
  float w = 0, x = 0, y = 0, z = 0;

  constexpr Quat() = default;
  constexpr Quat(float w, float x, float y, float z):w(w),x(x),y(y),z(z) {}
  constexpr bool operator==(const Quat& v) const { return x==v.x && y==v.y && z==v.z && w==v.w; }
  };

struct Mat3 {
  Vec3 columns[3] {};

  constexpr Mat3() = default;
  constexpr Mat3(float m00, float m01, float m02,
                float m10, float m11, float m12,
                float m20, float m21, float m22):columns{{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}} {}
  constexpr Vec3& operator[](unsigned i) { return columns[i]; }
  constexpr const Vec3& operator[](unsigned i) const { return columns[i]; }
  constexpr bool operator==(const Mat3&) const = default;
  static constexpr Mat3 identity() { return {1,0,0,0,1,0,0,0,1}; }
  };

struct Mat4 {
  Vec4 columns[4] {};

  constexpr Mat4() = default;
  constexpr Vec4& operator[](unsigned i) { return columns[i]; }
  constexpr const Vec4& operator[](unsigned i) const { return columns[i]; }
  };

struct Color {
  unsigned char r = 0, g = 0, b = 0, a = 0;

  constexpr Color() = default;
  constexpr Color(unsigned char r, unsigned char g, unsigned char b, unsigned char a):r(r),g(g),b(b),a(a) {}
  constexpr bool operator==(const Color&) const = default;
  constexpr unsigned char& operator[](unsigned i) { return (&r)[i]; }
  constexpr const unsigned char& operator[](unsigned i) const { return (&r)[i]; }
  };

struct Aabb {
  Vec3 min;
  Vec3 max;

  static constexpr Aabb zero() { return {Vec3{0},Vec3{0}}; }
  };

struct Obb {
  Vec3 center;
  Vec3 axes[3];
  Vec3 half_width;
  std::vector<Obb> children;

  Aabb as_bbox() const {
    constexpr float sign[8][3]={{-1,-1,-1},{-1,-1,+1},{-1,+1,-1},{-1,+1,+1},
                               {+1,-1,-1},{+1,-1,+1},{+1,+1,-1},{+1,+1,+1}};
    // Preserve the original corner order, arithmetic and positive minimum.
    Aabb box{Vec3{std::numeric_limits<float>::max()},Vec3{std::numeric_limits<float>::min()}};
    for(const auto& i:sign) {
      auto point=center;
      const auto axis0=axes[0]*half_width.x*i[0];
      const auto axis1=axes[1]*half_width.y*i[1];
      const auto axis2=axes[2]*half_width.z*i[2];
      point.x+=axis0.x+axis1.x+axis2.x;
      point.y+=axis0.y+axis1.y+axis2.y;
      point.z+=axis0.z+axis1.z+axis2.z;
      box.min.x=std::min(box.min.x,point.x); box.max.x=std::max(box.max.x,point.x);
      box.min.y=std::min(box.min.y,point.y); box.max.y=std::max(box.max.y,point.y);
      box.min.z=std::min(box.min.z,point.z); box.max.z=std::max(box.max.z,point.z);
      }
    return box;
    }
  };

}

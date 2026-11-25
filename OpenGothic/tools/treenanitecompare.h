#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>

namespace TreeNanite {

// Tightly packed Vulkan image-to-buffer copies: RG32U, D32, RGBA8, R32U, RGBA8, RG16F.
struct Readback {
  const uint8_t* owner;
  const uint8_t* depth;
  const uint8_t* diffuse;
  const uint8_t* normal;
  const uint8_t* orm;
  const uint8_t* motion;
};

template<class T>
inline T readValue(const uint8_t* bytes) {
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
  }

inline std::array<uint32_t, 2> visibilityAt(const uint8_t* bytes, size_t index) {
  return readValue<std::array<uint32_t, 2>>(bytes + index*8);
  }

struct Coverage {
  uint64_t front = 0;
  uint64_t back  = 0;

  uint64_t won() const { return front + back; }

  void add(std::array<uint32_t, 2> owner) {
    if(owner[0] == 0)
      return;
    if((owner[1] & (1u << 13)) != 0)
      ++front;
    else
      ++back;
    }
};

inline Coverage countCoverage(const uint8_t* owner, size_t pixelCount) {
  Coverage result;
  for(size_t i=0; i<pixelCount; ++i)
    result.add(visibilityAt(owner, i));
  return result;
  }

namespace Detail {

inline std::array<double, 3> decodeNormal(uint32_t packed) {
  // i_octahedral_32 in shader/common.glsl, including the signed normalized clamp.
  const auto snorm = [](uint32_t lane) {
    const int signedLane = int(lane) - ((lane & 0x8000) ? 65536 : 0);
    return std::max(-1.0, signedLane / 32767.0);
    };
  double x = snorm(packed & 0xffff), y = snorm(packed >> 16);
  const double z = 1.0 - std::abs(x) - std::abs(y);
  const double fold = std::max(-z, 0.0);
  x += x > 0.0 ? -fold : fold;
  y += y > 0.0 ? -fold : fold;
  const double inverseLength = 1.0 / std::sqrt(x*x + y*y + z*z);
  return {x*inverseLength, y*inverseLength, z*inverseLength};
  }

inline double normalAngle(uint32_t reference, uint32_t candidate) {
  if(reference == candidate)
    return 0;
  const auto a = decodeNormal(reference);
  const auto b = decodeNormal(candidate);
  const double dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
  return std::acos(std::clamp(dot, -1.0, 1.0)) * (180.0 / 3.14159265358979323846);
  }

inline double decodeHalf(uint16_t packed) {
  const int exponent    = (packed >> 10) & 31;
  const int significand = packed & 1023;
  const double sign     = (packed & 0x8000) ? -1.0 : 1.0;
  if(exponent == 31)
    return significand == 0 ? sign * std::numeric_limits<double>::infinity()
                            : std::numeric_limits<double>::quiet_NaN();
  if(exponent == 0)
    return sign * std::ldexp(double(significand), -24);
  return sign * std::ldexp(double(1024 + significand), exponent - 25);
  }

inline int rgbaError(const uint8_t* reference, const uint8_t* candidate) {
  int error = 0;
  for(size_t channel=0; channel<4; ++channel)
    error = std::max(error, std::abs(int(reference[channel]) - int(candidate[channel])));
  return error;
  }

struct PixelDifference {
  bool   sameOwner  = false;
  bool   sameDepth  = false;
  bool   sameFlags  = false;
  bool   hasMaterial = false;
  int    diffuse    = 0;
  int    orm        = 0;
  double normal     = 0;
  double motion     = 0;

  bool validMotion() const { return std::isfinite(motion); }
  bool passed() const {
    return sameOwner && sameDepth && sameFlags && diffuse <= 1 && orm <= 1 && normal <= 1.0 &&
           validMotion() && motion <= 0.1;
    }
};

struct DifferenceCounts {
  uint64_t pixels                  = 0;
  uint64_t matchedMaterialPixels   = 0;
  uint64_t ownerMismatches         = 0;
  uint64_t depthMismatches         = 0;
  uint64_t flagMismatches          = 0;
  uint64_t diffuseFailures         = 0;
  uint64_t ormFailures             = 0;
  uint64_t normalFailures          = 0;
  uint64_t motionFailures          = 0;
  uint64_t nonfiniteMotionPixels   = 0;
  int      maxDiffuse              = 0;
  int      maxOrm                  = 0;
  double   maxNormalDegrees        = 0;
  double   maxMotionPx             = 0;

  void add(const PixelDifference& diff) {
    ++pixels;
    matchedMaterialPixels += diff.hasMaterial;
    ownerMismatches       += !diff.sameOwner;
    depthMismatches       += !diff.sameDepth;
    flagMismatches        += !diff.sameFlags;
    diffuseFailures       += diff.diffuse > 1;
    ormFailures           += diff.orm > 1;
    normalFailures        += diff.normal > 1.0;
    motionFailures        += !diff.validMotion() || diff.motion > 0.1;
    nonfiniteMotionPixels += !diff.validMotion();
    maxDiffuse            = std::max(maxDiffuse, diff.diffuse);
    maxOrm                = std::max(maxOrm, diff.orm);
    maxNormalDegrees      = std::max(maxNormalDegrees, diff.normal);
    if(diff.validMotion())
      maxMotionPx = std::max(maxMotionPx, diff.motion);
    }

  void writeJsonFields(std::ostream& out) const {
    out << "\"pixels\":" << pixels
        << ",\"matched_material_pixels\":" << matchedMaterialPixels
        << ",\"owner_mismatches\":" << ownerMismatches
        << ",\"depth_mismatches\":" << depthMismatches
        << ",\"flag_mismatches\":" << flagMismatches
        << ",\"diffuse_failures\":" << diffuseFailures
        << ",\"orm_failures\":" << ormFailures
        << ",\"normal_failures\":" << normalFailures
        << ",\"motion_failures\":" << motionFailures
        << ",\"nonfinite_motion_pixels\":" << nonfiniteMotionPixels
        << ",\"max_diffuse\":" << maxDiffuse
        << ",\"max_orm\":" << maxOrm
        << ",\"max_normal_degrees\":" << maxNormalDegrees
        << ",\"max_motion_px\":" << maxMotionPx;
    }
};

}

struct Verification {
  Coverage                 reference;
  Coverage                 candidate;
  uint64_t                 lostPixels           = 0;
  uint64_t                 replacedPixels       = 0;
  uint64_t                 addedPixels          = 0;
  uint64_t                 backgroundMismatches = 0;
  int64_t                  firstFailure         = -1;
  Detail::DifferenceCounts all;
  Detail::DifferenceCounts interior;
  Detail::DifferenceCounts edge;
  Detail::DifferenceCounts subpixel;
  Detail::DifferenceCounts nearClip;
  Detail::DifferenceCounts degenerate;

  bool passed() const { return firstFailure < 0; }

  void writeJsonFields(std::ostream& out) const {
    out << "\"passed\":" << (passed() ? "true" : "false")
        << ",\"reference_pixels\":" << reference.won()
        << ",\"won_pixels\":" << candidate.won()
        << ",\"front_pixels\":" << candidate.front
        << ",\"back_pixels\":" << candidate.back
        << ",\"lost_pixels\":" << lostPixels
        << ",\"replaced_pixels\":" << replacedPixels
        << ",\"added_pixels\":" << addedPixels
        << ",\"background_mismatches\":" << backgroundMismatches
        << ",\"first_failure_pixel\":" << firstFailure << ',';
    all.writeJsonFields(out);
    const auto mask = [&out](const char* label, const Detail::DifferenceCounts& counts) {
      out << ",\"" << label << "\":{";
      counts.writeJsonFields(out);
      out << '}';
      };
    mask("interior", interior);
    mask("edge", edge);
    mask("subpixel", subpixel);
    mask("near_clip", nearClip);
    mask("degenerate", degenerate);
    }
};

// Resolve supplies the R8_UINT diagnostic mask. Its subsets can overlap; a missing candidate has no mask.
// Every pixel participates in ownership and depth checks.
inline Verification compare(Readback reference, Readback candidate, uint32_t width, uint32_t height,
                            const uint8_t* edgeMask) {
  Verification result;
  for(size_t pixel=0; pixel<size_t(width)*height; ++pixel) {
    const auto ownerA = visibilityAt(reference.owner, pixel);
    const auto ownerB = visibilityAt(candidate.owner, pixel);
    const bool coveredA = ownerA[0] != 0, coveredB = ownerB[0] != 0;
    result.reference.add(ownerA);
    result.candidate.add(ownerB);
    result.lostPixels     += coveredA && !coveredB;
    result.replacedPixels += coveredA && coveredB && ownerA != ownerB;
    result.addedPixels    += !coveredA && coveredB;

    const size_t offset = pixel*4;
    const auto depthA   = readValue<uint32_t>(reference.depth + offset);
    const auto depthB   = readValue<uint32_t>(candidate.depth + offset);
    const auto normalA  = readValue<uint32_t>(reference.normal + offset);
    const auto normalB  = readValue<uint32_t>(candidate.normal + offset);
    const auto motionA  = readValue<std::array<uint16_t, 2>>(reference.motion + offset);
    const auto motionB  = readValue<std::array<uint16_t, 2>>(candidate.motion + offset);
    const auto diffuseA = reference.diffuse + offset;
    const auto diffuseB = candidate.diffuse + offset;
    const auto ormA     = reference.orm + offset;
    const auto ormB     = candidate.orm + offset;

    Detail::PixelDifference diff;
    diff.sameOwner   = ownerA == ownerB;
    diff.sameDepth   = depthA == depthB;
    // Diffuse alpha holds the hint bits. The ORM alpha midpoint controls the engine's own-material branch.
    diff.sameFlags   = diffuseA[3] == diffuseB[3] && (ormA[3] >= 128) == (ormB[3] >= 128);
    diff.hasMaterial = coveredA && diff.sameOwner;
    diff.diffuse     = Detail::rgbaError(diffuseA, diffuseB);
    diff.orm         = Detail::rgbaError(ormA, ormB);
    if(diff.hasMaterial) {
      diff.normal = Detail::normalAngle(normalA, normalB);
      const double dx = Detail::decodeHalf(motionA[0]) - Detail::decodeHalf(motionB[0]);
      const double dy = Detail::decodeHalf(motionA[1]) - Detail::decodeHalf(motionB[1]);
      diff.motion = std::hypot(dx*width, dy*height);
      }
    const bool changedBackground = !coveredA && !coveredB &&
        (diff.diffuse != 0 || diff.orm != 0 || normalA != normalB || motionA != motionB);
    result.backgroundMismatches += changedBackground;
    if(result.firstFailure < 0 && (!diff.passed() || changedBackground))
      result.firstFailure = int64_t(pixel);
    result.all.add(diff);

    if(coveredA || coveredB) {
      const uint8_t mask = edgeMask ? edgeMask[pixel] : 0;
      if(mask == 0 && coveredB)
        result.interior.add(diff);
      if(mask & 1u)
        result.edge.add(diff);
      if(mask & 2u)
        result.subpixel.add(diff);
      if(mask & 4u)
        result.nearClip.add(diff);
      if(mask & 8u)
        result.degenerate.add(diff);
      }
    }
  return result;
  }

}

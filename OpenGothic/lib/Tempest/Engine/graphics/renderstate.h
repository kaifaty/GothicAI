#pragma once

#include <cstddef>
#include <cstdint>

namespace Tempest {

class RenderState final {
  public:
    RenderState()=default;

    enum class BlendMode : uint8_t {
      Zero,
      One,
      SrcColor,
      OneMinusSrcColor,
      SrcAlpha,
      OneMinusSrcAlpha,
      DstAlpha,
      OneMinusDstAlpha,
      DstColor,
      OneMinusDstColor,
      SrcAlphaSaturate,
      };

    enum class BlendOp : uint8_t {
      Add,
      Subtract,
      ReverseSubtract,
      Min,
      Max
      };

    enum class ZTestMode : uint8_t {
      Always,
      Never,

      Greater,
      Less,

      GEqual,
      LEqual,

      NoEqual,
      Equal
      };

    enum class CullMode : uint8_t {
      Back=0,
      Front,
      NoCull
      };

    enum ColorMask : uint8_t {
      R   = 1,
      G   = 2,
      B   = 4,
      A   = 8,
      RGB = R|G|B,
      All = R|G|B|A,
      };

    void      setBlendSource(BlendMode s) { blendS=s; }
    void      setBlendDest  (BlendMode d) { blendD=d; }
    void      setBlendOp    (BlendOp   o) { blendO=o; }

    BlendMode blendSource()    const { return blendS; }
    BlendMode blendDest()      const { return blendD; }
    BlendOp   blendOperation() const { return blendO; }

    bool      hasBlend() const { return blendS!=BlendMode::One || blendD!=BlendMode::Zero || !(blendO==BlendOp::Add || blendO==BlendOp::Subtract); }

    void      setZTestMode(ZTestMode z){ zmode=z; }
    ZTestMode zTestMode() const { return zmode; }

    void      setRasterDiscardEnabled(bool e) { discard=e; }
    bool      isRasterDiscardEnabled() const { return discard; }

    void      setZWriteEnabled(bool e) { zdiscard=!e; }
    bool      isZWriteEnabled() const { return !zdiscard; }

    CullMode  cullFaceMode() const { return cull; }
    void      setCullFaceMode(CullMode use) { cull=use; }

    // Channels written to a color attachment (ColorMask bits); attachments 0..7, all channels by default.
    void      setColorWriteMask(size_t attachment, uint8_t mask) {
      const uint32_t sh = uint32_t(attachment*4);
      wmask = (wmask & ~(0xFu<<sh)) | (uint32_t(mask&0xF)<<sh);
      }
    uint8_t   colorWriteMask(size_t attachment) const { return attachment<8 ? uint8_t((wmask>>(attachment*4)) & 0xF) : uint8_t(All); }
    bool      hasColorWriteMask() const { return wmask!=0xFFFFFFFFu; }

  private:
    BlendMode blendS   = BlendMode::One;
    BlendMode blendD   = BlendMode::Zero;
    BlendOp   blendO   = BlendOp::Add;
    ZTestMode zmode    = ZTestMode::Always;
    CullMode  cull     = CullMode::Back;
    bool      discard  = false;
    bool      zdiscard = false;
    uint32_t  wmask    = 0xFFFFFFFFu;
  };

}

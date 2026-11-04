#include "graphics/loadprobe.h"
#if defined(TEMPEST_BUILD_METAL)

#include "mtshader.h"

#include <Tempest/Log>
#include <Tempest/Except>

#include "mtdevice.h"
#include "mtshadercache.h"
#include "gapi/shaderreflection.h"
#include "thirdparty/spirv_cross/spirv_msl.hpp"

#include "libspirv/libspirv.h"

#include <cstdlib>

using namespace Tempest;
using namespace Tempest::Detail;

static uint32_t spvVersion(MTL::LanguageVersion v) {
  const uint32_t major = v >> 16u;
  const uint32_t minor = v & 0xFFFF;
  return spirv_cross::CompilerMSL::Options::make_msl_version(major,minor,0);
  }

static spirv_cross::CompilerMSL::Options mslOptions(const MtDevice& dev) {
  spirv_cross::CompilerMSL::Options optMSL;
#if defined(__OSX__)
  optMSL.platform = spirv_cross::CompilerMSL::Options::macOS;
#else
  optMSL.platform = spirv_cross::CompilerMSL::Options::iOS;
#endif
  optMSL.buffer_size_buffer_index = MtShader::MSL_BUFFER_LENGTH;
  optMSL.msl_version = spvVersion(dev.prop.mslVersion);
  if(dev.prop.descriptors.nonUniformIndexing) {
    optMSL.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
    optMSL.runtime_array_rich_descriptor = true;
    }

  if(dev.prop.mslVersion>=MTL::LanguageVersion2_0) {
    // can relay on threadgroup_barrier(mem_flags::mem_texture) instead
    optMSL.readwrite_texture_fences = false;
    }

  if(!dev.useNativeImageAtomic()) {
    const uint32_t align = dev.linearImageAlignment();
    optMSL.r32ui_linear_texture_alignment = align;
    optMSL.r32ui_alignment_constant_id    = 0;
    }
  return optMSL;
  }

std::string MtShader::cacheIdentity(const MtDevice& dev) {
#if defined(TEMPEST_MSL_TRANSLATOR)
  // Everything the translation depends on besides the SPIR-V: the translator sources (hashed by CMake, so an
  // edit of SPIRV-Cross or of this file changes it) and the device-dependent options.
  const auto opt = mslOptions(dev);
  std::string id = "translator=" TEMPEST_MSL_TRANSLATOR;
  id += "\nplatform="     + std::to_string(int(opt.platform));
  id += "\nmsl="          + std::to_string(opt.msl_version);
  id += "\ntier="         + std::to_string(int(opt.argument_buffers_tier));
  id += "\nrich="         + std::to_string(int(opt.runtime_array_rich_descriptor));
  id += "\nfences="       + std::to_string(int(opt.readwrite_texture_fences));
  id += "\nr32ui_align="  + std::to_string(opt.r32ui_linear_texture_alignment);
  id += "\nr32ui_const="  + std::to_string(opt.r32ui_alignment_constant_id);
  return id;
#else
  (void)dev;
  return std::string();
#endif
  }

// SPIRV-Cross: MSL text and the final binding table (MSL slots filled, duplicates merged). Fills `out` too,
// so the result can go to the cache: binding k of the final table comes from reflection binding out.lay[k].src.
static std::string translate(const MtDevice& dev, const void* source, size_t srcSize,
                             std::vector<ShaderReflection::Binding>& lay, MtShaderCache::Entry& out) {
  spirv_cross::CompilerMSL::Options optMSL = mslOptions(dev);

  spirv_cross::CompilerGLSL::Options optGLSL;
  optGLSL.vertex.flip_vert_y = true;

  spirv_cross::CompilerMSL comp(reinterpret_cast<const uint32_t*>(source),srcSize/4);
  for(auto& cap:comp.get_declared_capabilities()) {
    switch(cap) {
      case spv::CapabilityRayQueryKHR: {
        auto ver = spirv_cross::CompilerMSL::Options::make_msl_version(2,3);
        optMSL.msl_version = std::max(optMSL.msl_version, ver);
        break;
        }
      case spv::CapabilityMeshShadingEXT: {
        auto ver = spirv_cross::CompilerMSL::Options::make_msl_version(3,0);
        optMSL.msl_version = std::max(optMSL.msl_version, ver);
        break;
        }
      default:
        break;
      }
    }
  comp.set_msl_options   (optMSL );
  comp.set_common_options(optGLSL);

  std::string msl = comp.compile();

  out.bufferSizeBuffer = comp.needs_buffer_size_buffer();
  out.reflected        = uint32_t(lay.size());

  for(auto& i:lay) {
    i.mslBinding = comp.get_automatic_msl_resource_binding(i.spvId);
    if(i.cls==ShaderReflection::Texture)
      i.mslBinding2 = comp.get_automatic_msl_resource_binding_secondary(i.spvId);
    if(i.cls==ShaderReflection::ImgR || i.cls==ShaderReflection::ImgRW)
      i.mslBinding2 = comp.get_automatic_msl_resource_binding_secondary(i.spvId);
    if(i.cls==ShaderReflection::Push)
      i.mslSize = ShaderReflection::mslSizeOf(i.spvId,comp);
    }

  std::vector<uint32_t> src(lay.size());
  size_t nsize = 0;
  for(size_t i=0; i<lay.size(); ++i) {
    bool uniq = true;
    if(lay[i].cls!=ShaderReflection::Push) {
      for(size_t r=0; r<i; ++r) {
        if(lay[r].cls==ShaderReflection::Push)
          continue;
        if(lay[i].layout!=lay[r].layout)
          continue;
        uniq = false;
        lay[r].mslBinding  = std::min(lay[r].mslBinding,  lay[i].mslBinding);
        lay[r].mslBinding2 = std::min(lay[r].mslBinding2, lay[i].mslBinding2);
        }
      }
    if(uniq) {
      lay[nsize] = lay[i];
      src[nsize] = uint32_t(i);
      nsize++;
      }
    }
  lay.resize(nsize);

  out.lay.resize(nsize);
  for(size_t i=0; i<nsize; ++i) {
    out.lay[i].src         = src[i];
    out.lay[i].mslBinding  = lay[i].mslBinding;
    out.lay[i].mslBinding2 = lay[i].mslBinding2;
    out.lay[i].mslSize     = lay[i].mslSize;
    }
  out.msl = msl;
  return msl;
  }

// Final binding table from a cache entry and this shader's own reflection; false if they do not fit.
static bool applyCached(const MtShaderCache::Entry& e, std::vector<ShaderReflection::Binding>& lay) {
  if(e.reflected!=lay.size())
    return false;
  std::vector<ShaderReflection::Binding> ret(e.lay.size());
  for(size_t i=0; i<e.lay.size(); ++i) {
    auto& b = e.lay[i];
    if(b.src>=lay.size() || (i>0 && b.src<=e.lay[i-1].src))
      return false;
    ret[i]             = lay[b.src];
    ret[i].mslBinding  = b.mslBinding;
    ret[i].mslBinding2 = b.mslBinding2;
    ret[i].mslSize     = b.mslSize;
    }
  lay = std::move(ret);
  return true;
  }

static bool sameBindings(const std::vector<ShaderReflection::Binding>& a, const std::vector<ShaderReflection::Binding>& b) {
  if(a.size()!=b.size())
    return false;
  for(size_t i=0; i<a.size(); ++i) {
    auto& x = a[i];
    auto& y = b[i];
    if(x.layout!=y.layout || x.cls!=y.cls || x.stage!=y.stage || x.runtimeSized!=y.runtimeSized ||
       x.is3DImage!=y.is3DImage || x.arraySize!=y.arraySize || x.byteSize!=y.byteSize ||
       x.varByteSize!=y.varByteSize || uint32_t(x.spvId)!=uint32_t(y.spvId) ||
       x.mslBinding!=y.mslBinding || x.mslBinding2!=y.mslBinding2 || x.mslSize!=y.mslSize)
      return false;
    }
  return true;
  }

MtShader::MtShader(MtDevice& dev, const void* source, size_t srcSize)
  : Shader(source, srcSize) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();

  std::string msl;
  TempestLoadProbe::PipelineScope convertProbe(TempestLoadProbe::Convert);
  auto&                cache = dev.mslCache;
  MtShaderCache::Key   key;
  MtShaderCache::Entry cached;
  const auto           reflected = lay;
  bool                 hit       = false;
  if(cache.enabled()) {
    key = MtShaderCache::key(source,srcSize);
    hit = cache.find(key,cached) && applyCached(cached,lay);
    if(!hit)
      lay = reflected;
    }

  if(!hit || cache.verify()) {
    try {
      MtShaderCache::Entry live;
      auto                 liveLay = reflected;
      std::string          liveMsl = translate(dev,source,srcSize,liveLay,live);
      if(hit) {
        // OPENGOTHIC_MSL_CACHE_CHECK: the cached result must equal a fresh translation byte for byte.
        const bool same = cached.msl==liveMsl && cached.bufferSizeBuffer==live.bufferSizeBuffer && sameBindings(lay,liveLay);
        if(!same) {
          Log::e("msl cache: MISMATCH for shader \"",dbgShortName(),"\" (msl ",cached.msl==liveMsl ? "same" : "differs",
                 ", bindings ",sameBindings(lay,liveLay) ? "same" : "differ",
                 ", buffer size buffer ",cached.bufferSizeBuffer==live.bufferSizeBuffer ? "same" : "differs",")");
          std::abort();
          }
        cache.verified(true);
        } else {
        lay              = std::move(liveLay);
        bufferSizeBuffer = live.bufferSizeBuffer;
        msl              = std::move(liveMsl);
        cache.put(key,std::move(live));
        }
      }
    catch(const std::bad_alloc&) {
      throw;
      }
    catch(const spirv_cross::CompilerError& err) {
      Log::e("cross-compile error: \"",err.what(),"\"");
      throw std::system_error(Tempest::GraphicsErrc::InvalidShaderModule);
      }
    catch(...) {
      throw std::system_error(Tempest::GraphicsErrc::InvalidShaderModule);
      }
    }
  if(hit) {
    bufferSizeBuffer = cached.bufferSizeBuffer;
    msl              = std::move(cached.msl);
    }

  convertProbe.stop();
  //Log::d(msl);

  auto       opt = NsPtr<MTL::CompileOptions>::init();
  NS::Error* err = nullptr;
  auto       str = NsPtr<NS::String>(NS::String::string(msl.c_str(),NS::UTF8StringEncoding));
  str->retain();
  TempestLoadProbe::PipelineScope pipelineProbe(TempestLoadProbe::Library);
  library = NsPtr<MTL::Library>(dev.impl->newLibrary(str.get(), opt.get(), &err));
  pipelineProbe.stop();

  if(err!=nullptr) {
    const char* e = err->localizedDescription()->utf8String();
    Log::e("msl compile error: \"",e,"\"");
#if !defined(NDEBUG)
    Log::d(msl);
#endif
    throw std::system_error(Tempest::GraphicsErrc::InvalidShaderModule, e);
    }

  auto main = NsPtr<NS::String>(NS::String::string("main0",NS::UTF8StringEncoding));
  main->retain();

  auto cvar = NsPtr<MTL::FunctionConstantValues>::init();
  //cvar->setConstantValues();
  impl = NsPtr<MTL::Function>(library->newFunction(main.get(), cvar.get(), &err));
  }

MtShader::~MtShader() {
  }

#endif

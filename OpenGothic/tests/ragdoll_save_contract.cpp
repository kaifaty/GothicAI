#include "game/ragdollsave.h"
#include "game/serialize.h"
#include "resources.h"
#include "asset/modeltypes.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <zenkit/ModelAnimation.hh>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
  using Snapshot = RagdollSave::Snapshot;
  constexpr uint32_t magic = 0x32444752u;
  constexpr uint16_t schema = 1;
  constexpr uint8_t bodyFlag = 2;
  constexpr const char* bodyEntry = "worlds/NEWWORLD.ZEN/npc/0/ragdoll";

  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  Tempest::Matrix4x4 matrix(float x, float y, float z) {
    auto result = Tempest::Matrix4x4::mkIdentity();
    result.translate(x,y,z);
    return result;
    }

  Snapshot example() {
    Snapshot result;
    Ragdoll::State body;
    body.version = 2;
    body.skeleton = "HUMANS.MDS";
    body.rootTransform = matrix(37124.25f,-2048.5f,-74629.125f);
    body.rootTransform.rotateOY(27.f);
    body.rootTransform.scale(0.9f,1.f,1.f);
    Ragdoll::PartState first;
    first.bone = 0;
    first.transform = matrix(37128.5f,-2015.125f,-74627.5f);
    first.transform.rotateOX(71.5f);
    first.transform.rotateOY(-13.25f);
    auto local = matrix(4.25f,33.375f,1.625f);
    local.rotateOX(71.5f);
    local.rotateOY(-13.25f);
    first.referenceTransform = body.rootTransform*local;
    first.linearVelocity = {12.25f,-34.5f,0.0625f};
    first.angularVelocity = {-0.f,0.125f,-1.25f};
    first.sleeping = false;
    auto last = first;
    last.bone = uint32_t(Resources::MAX_NUM_SKELETAL_NODES-1);
    last.transform = matrix(37170.75f,-2070.25f,-74689.5f);
    last.referenceTransform = body.rootTransform*matrix(46.5f,-21.75f,-60.375f);
    last.linearVelocity = {};
    last.angularVelocity = {};
    last.sleeping = true;
    body.parts = {first,last};
    result.body = std::move(body);
    return result;
    }

  Tempest::Matrix4x4 quaternionMatrix(const Tempest::Vec4& q, const Tempest::Vec3& origin) {
    auto result = matrix(origin.x,origin.y,origin.z);
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    result.set(0,0,1-2*(y*y+z*z)); result.set(0,1,2*(x*y+z*w)); result.set(0,2,2*(x*z-y*w));
    result.set(1,0,2*(x*y-z*w)); result.set(1,1,1-2*(x*x+z*z)); result.set(1,2,2*(y*z+x*w));
    result.set(2,0,2*(x*z+y*w)); result.set(2,1,2*(y*z-x*w)); result.set(2,2,1-2*(x*x+y*y));
    return result;
    }

  Snapshot nativeExample() {
    auto result = example();
    result.body->version = 3;
    for(size_t i=0; i<result.body->parts.size(); ++i) {
      auto& part = result.body->parts[i];
      part.centerOfMassMeters = {std::bit_cast<float>(uint32_t(0x436a76c8)),12.3428936f,-103.688453f};
      part.rotation = i==0 ? Tempest::Vec4(-0.f,.6f,0,.8f) : Tempest::Vec4(-.6f,0,0,.8f);
      part.linearVelocityMeters = i==0 ? Tempest::Vec3(.12345678f,-.2345679f,-0.f) : Tempest::Vec3();
      part.linearVelocity = part.linearVelocityMeters*100.f;
      // Zero local COM is a codec fixture. Real shape COM agreement is tested
      // by the backend, which knows the rebuilt shape and its affine birth frame.
      part.transform = quaternionMatrix(part.rotation,part.centerOfMassMeters*100.f);
      }
    Ragdoll::AncestorState ancestor;
    ancestor.bone = 1;
    ancestor.anchorBone = result.body->parts[0].bone;
    ancestor.referenceTransform = result.body->rootTransform*matrix(-2.5f,-99.95f,7.375f);
    result.body->ancestors.push_back(ancestor);
    return result;
    }

  bool sameMatrix(const Tempest::Matrix4x4& a, const Tempest::Matrix4x4& b) {
    for(int row=0; row<4; ++row)
      for(int column=0; column<4; ++column) {
        const float lhs = a.at(row,column), rhs = b.at(row,column);
        if(std::memcmp(&lhs,&rhs,sizeof(lhs))!=0)
          return false;
        }
    return true;
    }

  bool sameVector(const Tempest::Vec3& a, const Tempest::Vec3& b) {
    return std::memcmp(&a.x,&b.x,sizeof(float))==0 &&
           std::memcmp(&a.y,&b.y,sizeof(float))==0 &&
           std::memcmp(&a.z,&b.z,sizeof(float))==0;
    }

  bool sameQuaternion(const Tempest::Vec4& a, const Tempest::Vec4& b) {
    return std::memcmp(&a.x,&b.x,sizeof(float))==0 &&
           std::memcmp(&a.y,&b.y,sizeof(float))==0 &&
           std::memcmp(&a.z,&b.z,sizeof(float))==0 &&
           std::memcmp(&a.w,&b.w,sizeof(float))==0;
    }

  bool same(const Snapshot& a, const Snapshot& b) {
    if(a.legacyDown!=b.legacyDown || bool(a.body)!=bool(b.body))
      return false;
    if(!a.body)
      return true;
    const auto& lhs = *a.body;
    const auto& rhs = *b.body;
    if(lhs.version!=rhs.version || lhs.skeleton!=rhs.skeleton ||
       !sameMatrix(lhs.rootTransform,rhs.rootTransform) || lhs.parts.size()!=rhs.parts.size())
      return false;
    if(lhs.ancestors.size()!=rhs.ancestors.size() || lhs.legacyAncestors!=rhs.legacyAncestors)
      return false;
    for(size_t i=0; i<lhs.parts.size(); ++i) {
      const auto& l = lhs.parts[i];
      const auto& r = rhs.parts[i];
      if(l.bone!=r.bone || !sameMatrix(l.transform,r.transform) ||
         (lhs.version>=2 && !sameMatrix(l.referenceTransform,r.referenceTransform)) ||
         (lhs.version>=3 && (!sameVector(l.centerOfMassMeters,r.centerOfMassMeters) ||
                            !sameQuaternion(l.rotation,r.rotation) ||
                            !sameVector(l.linearVelocityMeters,r.linearVelocityMeters))) ||
         !sameVector(l.linearVelocity,r.linearVelocity) ||
         !sameVector(l.angularVelocity,r.angularVelocity) || l.sleeping!=r.sleeping)
        return false;
      }
    for(size_t i=0; i<lhs.ancestors.size(); ++i) {
      const auto& l = lhs.ancestors[i];
      const auto& r = rhs.ancestors[i];
      if(l.bone!=r.bone || l.anchorBone!=r.anchorBone || !sameMatrix(l.referenceTransform,r.referenceTransform))
        return false;
      }
    return true;
    }

  std::vector<uint8_t> archive(const std::function<void(Serialize&)>& write) {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    {
      Serialize output(writer);
      write(output);
    }
    return bytes;
    }

  void testAnimationSampleCompatibility() {
    const zenkit::AnimationSample source{{-0.f,12.25f,-34.5f},{.8f,-0.f,.6f,0.f}};
    const asset::AnimationSample own{source.position,source.rotation};
    static_assert(sizeof(own)==sizeof(source));
    const auto bytes=archive([&](Serialize& output) {
      output.setEntry("animation/owned");
      output.write(own);
      output.setEntry("animation/original");
      output.writeBytes(&source,sizeof(source));
      output.setEntry("animation/v39");
      output.write(source.position.x,source.position.y,source.position.z,
                   source.rotation.x,source.rotation.y,source.rotation.z,source.rotation.w);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    require(input.setEntry("animation/owned"),"owned sample entry must exist");
    zenkit::AnimationSample original;
    input.readBytes(&original,sizeof(original));
    require(std::memcmp(&source,&original,sizeof(source))==0,
            "owned sample writes must match original save bytes and retain quaternion WXYZ layout");
    require(input.setEntry("animation/original"),"original sample entry must exist");
    input.setVersion(40);
    asset::AnimationSample restored;
    input.read(restored);
    require(std::memcmp(&source,&restored,sizeof(source))==0,
            "owned samples must read original raw saves without losing signed zero");
    require(input.setEntry("animation/v39"),"legacy sample entry must exist");
    input.setVersion(39);
    input.read(restored);
    require(std::memcmp(&source,&restored,sizeof(source))==0,
            "owned samples must retain legacy XYZW component reads before save version 40");
    }

  template<class T>
  void append(std::vector<uint8_t>& bytes, const T& value) {
    const size_t at = bytes.size();
    bytes.resize(at+sizeof(value));
    std::memcpy(bytes.data()+at,&value,sizeof(value));
    }

  void appendMatrix(std::vector<uint8_t>& bytes, const Tempest::Matrix4x4& value) {
    for(int row=0; row<4; ++row)
      for(int column=0; column<4; ++column)
        append(bytes,value.at(row,column));
    }

  void appendVector(std::vector<uint8_t>& bytes, const Tempest::Vec3& value) {
    append(bytes,value.x);
    append(bytes,value.y);
    append(bytes,value.z);
    }

  std::vector<uint8_t> payload(const Ragdoll::State& body) {
    std::vector<uint8_t> bytes;
    append(bytes,body.version);
    append(bytes,uint32_t(body.skeleton.size()));
    bytes.insert(bytes.end(),body.skeleton.begin(),body.skeleton.end());
    appendMatrix(bytes,body.rootTransform);
    append(bytes,uint32_t(body.parts.size()));
    for(const auto& part:body.parts) {
      append(bytes,part.bone);
      appendMatrix(bytes,part.transform);
      if(body.version>=2)
        appendMatrix(bytes,part.referenceTransform);
      if(body.version>=3) {
        appendVector(bytes,part.centerOfMassMeters);
        append(bytes,part.rotation.x); append(bytes,part.rotation.y);
        append(bytes,part.rotation.z); append(bytes,part.rotation.w);
        appendVector(bytes,part.linearVelocityMeters);
        }
      appendVector(bytes,part.linearVelocity);
      appendVector(bytes,part.angularVelocity);
      append(bytes,uint8_t(part.sleeping ? 1 : 0));
      }
    if(body.version>=3) {
      append(bytes,uint8_t(body.legacyAncestors ? 1 : 0));
      append(bytes,uint32_t(body.ancestors.size()));
      for(const auto& ancestor:body.ancestors) {
        append(bytes,ancestor.bone);
        append(bytes,ancestor.anchorBone);
        appendMatrix(bytes,ancestor.referenceTransform);
        }
      }
    return bytes;
    }

  std::vector<uint8_t> rawArchive(const std::vector<uint8_t>& bytes,
                                uint32_t declaredSize=uint32_t(-1), uint16_t version=schema,
                                uint8_t flags=bodyFlag, uint8_t reserved=0, uint32_t tag=magic,
                                bool extraAfterPayload=false) {
    return archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      output.write(tag,version,flags,reserved,
                   declaredSize==uint32_t(-1) ? uint32_t(bytes.size()) : declaredSize);
      output.writeBytes(bytes.data(),bytes.size());
      if(extraAfterPayload)
        output.write(uint8_t(42));
      });
    }

  void reject(const std::vector<uint8_t>& bytes, const char* message) {
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    require(input.setEntry(bodyEntry),"malformed fixture must contain its ZIP entry");
    auto output = example();
    output.legacyDown = true;
    const auto previous = output;
    require(!RagdollSave::load(input,output),message);
    require(same(output,previous),"failed load must leave the caller's live snapshot unchanged");
    }

  void rejectState(const std::function<void(Ragdoll::State&)>& alter, const char* message) {
    auto invalid = example();
    alter(*invalid.body);
    require(!RagdollSave::valid(invalid),message);
    reject(rawArchive(payload(*invalid.body)),message);
    }

  void rejectNativeState(const std::function<void(Ragdoll::State&)>& alter, const char* message) {
    auto invalid = nativeExample();
    alter(*invalid.body);
    require(!RagdollSave::valid(invalid),message);
    reject(rawArchive(payload(*invalid.body)),message);
    }

  void rejectNativeOutput(const std::function<void(Ragdoll::State&)>& alter, const char* message) {
    auto invalid = nativeExample();
    alter(*invalid.body);
    require(!RagdollSave::valid(invalid),message);
    bool threw = false;
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      try {
        RagdollSave::save(output,invalid);
        }
      catch(const std::invalid_argument&) {
        threw = true;
        }
      });
    require(threw,message);
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    require(!input.setEntry(bodyEntry),"invalid outgoing metadata must not write a partial body entry");
    }

  void testRoundTrip() {
    const auto before = example();
    require(RagdollSave::valid(before),"actual .9/1/1 root with proper affine references and native unit parts must be valid");
    const auto& reference = before.body->parts[0].referenceTransform;
    float referenceDot = 0;
    for(int component=0; component<3; ++component)
      referenceDot += reference.at(0,component)*reference.at(2,component);
    require(std::abs(referenceDot)>0.01f,
            "anisotropic fixture must contain actual reference shear rather than another orthogonal uniform case");
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry("worlds/NEWWORLD.ZEN/npc/0/data");
      output.write(uint32_t(55),float(123.25));
      output.setEntry(bodyEntry);
      RagdollSave::save(output,before);
      output.setEntry("worlds/NEWWORLD.ZEN/npc/0/inventory");
      output.write(uint32_t(17));
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    require(input.directorySize("worlds/NEWWORLD.ZEN/npc/")==1,
            "optional body entry must not create an additional NPC directory");
    require(input.setEntry(bodyEntry),"new save must contain a nonempty body chunk");
    Snapshot after;
    require(RagdollSave::load(input,after),"valid body must load from a ZIP entry");
    require(same(before,after),"positions, native and reference bases, velocities, sleep and bone mapping must round-trip bit exactly");
    input.setEntry("worlds/NEWWORLD.ZEN/npc/0/data");
    uint32_t version = 0;
    float value = 0;
    input.read(version,value);
    require(version==55 && value==123.25f,"existing NPC data layout must remain independent");
    input.setEntry("worlds/NEWWORLD.ZEN/npc/0/inventory");
    input.read(version);
    require(version==17,"existing inventory entry must remain independent");
    }

  void testInactiveAndOldSave() {
    for(bool legacy:{false,true}) {
      Snapshot before;
      before.legacyDown = legacy;
      const auto bytes = archive([&](Serialize& output) {
        output.setEntry(bodyEntry);
        RagdollSave::save(output,before);
        });
      Tempest::MemReader reader(bytes);
      Serialize input(reader);
      require(input.setEntry(bodyEntry),"inactive state needs a header so absence keeps its old-save meaning");
      auto after = example();
      require(RagdollSave::load(input,after) && same(before,after),
              "inactive and legacy-down markers must survive loading without a physical body");
      }
    const auto old = archive([](Serialize& output) {
      output.setEntry("worlds/NEWWORLD.ZEN/npc/0/data");
      output.write(uint32_t(55));
      });
    Tempest::MemReader reader(old);
    Serialize input(reader);
    require(!input.setEntry(bodyEntry),"older ZIP save must report the optional chunk as absent");
    auto unchanged = example();
    const auto previous = unchanged;
    require(!RagdollSave::load(input,unchanged) && same(previous,unchanged),
            "reading a missing chunk must not synthesize or replace a physical pose");
    }

  void testLegacyUniformState() {
    auto before = example();
    before.body->version = 1;
    before.body->rootTransform = matrix(37124.25f,-2048.5f,-74629.125f);
    before.body->rootTransform.rotateOY(27.f);
    before.body->rootTransform.scale(1.125f);
    for(auto& part:before.body->parts)
      part.transform.scale(1.125f);
    require(RagdollSave::valid(before),"legacy v1 uniform visual frames must remain valid");
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,before);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    input.setEntry(bodyEntry);
    Snapshot after;
    require(RagdollSave::load(input,after) && same(before,after),
            "legacy v1 layout without reference matrices must round-trip exactly");
    require(after.body->version==1,"loading a legacy snapshot must not silently relabel its native frames as v2");
    for(const auto& part:after.body->parts)
      require(sameMatrix(part.referenceTransform,part.transform),"legacy unused reference fields must be initialized deterministically");
    auto inconsistent = before;
    inconsistent.body->parts[0].transform.scale(1.01f);
    require(!RagdollSave::valid(inconsistent),"legacy part scale must match the uniform object scale");
    reject(rawArchive(payload(*inconsistent.body)),"legacy mismatched part scale must be rejected on input");
    inconsistent = before;
    inconsistent.body->rootTransform.scale(.9f,1,1);
    require(!RagdollSave::valid(inconsistent),"legacy v1 cannot carry anisotropic visual frames without v2 references");
    reject(rawArchive(payload(*inconsistent.body)),"legacy anisotropic object must be rejected on input");
    }

  void testNativePrecision() {
    // Real public native-origin X from gameplay-v3/npc-sleeping.zip,
    // worlds/newworld.zen/npc/292/ragdoll, part 8, bone 28 (State v2).
    // Its lost original COM/quaternion cannot be reconstructed from that file.
    const float capturedPublicCm = std::bit_cast<float>(uint32_t(0x46b72ccc)); // 23446.3984375
    const float lossyMetres = capturedPublicCm*0.01f;
    const float restoredPublicCm = lossyMetres*100.f;
    require(std::abs(restoredPublicCm-capturedPublicCm)==0.001953125f &&
            std::abs(restoredPublicCm-capturedPublicCm)>0.001f,
            "captured cm/m round trip must expose the genuine binary32 error above the unchanged 0.001 cm gate");
    // A compatible native-origin value derived from that captured public cm:
    // it exports the original value, but the v2 inverse changes one native bit.
    const float compatibleNative = std::bit_cast<float>(uint32_t(0x436a76c8));
    require(compatibleNative*100.f==capturedPublicCm &&
            std::bit_cast<uint32_t>(lossyMetres)!=std::bit_cast<uint32_t>(compatibleNative),
            "saving rounded public cm cannot preserve even a compatible native position's bits");
    const auto before = nativeExample();
    require(RagdollSave::valid(before),"valid v3 native float fields and anisotropic references must be accepted");
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,before);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    input.setEntry(bodyEntry);
    Snapshot after;
    require(RagdollSave::load(input,after) && same(before,after),
            "v3 COM metres, quaternion xyzw, velocities and redundant public frames must round-trip bit exactly");
    require(std::bit_cast<uint32_t>(after.body->parts[0].centerOfMassMeters.x)==0x436a76c8,
            "v3 load must retain stored native coordinates instead of reconstructing them from cm");
    require(sameQuaternion(after.body->parts[0].rotation,before.body->parts[0].rotation),
            "v3 quaternion must not be normalized or regenerated by the codec");

    auto wrongLayout = payload(*before.body);
    const uint32_t olderVersion = 2;
    std::memcpy(wrongLayout.data(),&olderVersion,sizeof(olderVersion));
    reject(rawArchive(wrongLayout),"v2 cannot conceal the v3 native-float extension");
    auto missingNative = payload(*before.body);
    const size_t nativeAt = 2*sizeof(uint32_t)+before.body->skeleton.size()+16*sizeof(float)+
                            sizeof(uint32_t)+sizeof(uint32_t)+32*sizeof(float);
    missingNative.erase(missingNative.begin()+std::ptrdiff_t(nativeAt),
                        missingNative.begin()+std::ptrdiff_t(nativeAt+10*sizeof(float)));
    reject(rawArchive(missingNative),"v3 requires the complete COM/quaternion/native velocity extension");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    rejectNativeState([nan](auto& state) { state.parts[0].centerOfMassMeters.y=nan; },"nonfinite native COM must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].centerOfMassMeters.x=1000000.0625f; },"unreasonable native COM must be rejected");
    rejectNativeState([inf](auto& state) { state.parts[0].rotation.w=inf; },"nonfinite native quaternion must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].rotation={0,0,0,0}; },"zero native quaternion must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].rotation.w*=2; },"nonunit native quaternion must be rejected rather than normalized");
    rejectNativeState([](auto& state) { state.parts[0].rotation={0,0,0,1}; },"native quaternion must agree with the redundant public orientation");
    rejectNativeState([nan](auto& state) { state.parts[0].linearVelocityMeters.z=nan; },"NaN native velocity must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].linearVelocityMeters.x=10000.001f; },"unreasonable native velocity must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].linearVelocityMeters.y+=.01f; },"native/public velocity disagreement must be rejected");
    rejectNativeState([](auto& state) { state.parts[0].linearVelocity.x=std::nextafter(state.parts[0].linearVelocity.x,100.f); },
                      "native/public velocity must retain the exact binary32 export relationship");
    }

  void testAncestors() {
    const auto before = nativeExample();
    require(before.body->ancestors.size()==1 && before.body->ancestors[0].bone!=before.body->parts[0].bone,
            "ancestor fixture must cover an unphysical bone linked to a physical anchor");
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,before);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    input.setEntry(bodyEntry);
    Snapshot after;
    require(RagdollSave::load(input,after) && same(before,after),
            "unphysical ancestor bone, physical anchor and complete affine reference must round-trip bit exactly");

    rejectNativeState([](auto& state) { state.ancestors.push_back(state.ancestors[0]); },"duplicate ancestor bone must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].bone=state.parts[0].bone; },"ancestor must not replace a physical part bone");
    rejectNativeState([](auto& state) { state.ancestors[0].anchorBone=2; },"ancestor anchor must exist among physical parts");
    rejectNativeState([](auto& state) { state.ancestors[0].anchorBone=state.ancestors[0].bone; },"ancestor cannot anchor to itself");
    rejectNativeState([](auto& state) { state.ancestors[0].bone=uint32_t(Resources::MAX_NUM_SKELETAL_NODES); },"out-of-range ancestor bone must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].anchorBone=uint32_t(Resources::MAX_NUM_SKELETAL_NODES); },"out-of-range physical anchor must be rejected");
    rejectNativeState([](auto& state) { state.ancestors.resize(Resources::MAX_NUM_SKELETAL_NODES+1); },"ancestor count must be bounded");
    rejectNativeState([](auto& state) {
      auto additional = state.ancestors[0];
      additional.bone = 2;
      additional.anchorBone = state.ancestors[0].bone;
      state.ancestors.push_back(additional);
      },"another ancestor cannot be used instead of a physical anchor");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    rejectNativeState([nan](auto& state) { state.ancestors[0].referenceTransform.set(3,0,nan); },"nonfinite ancestor reference must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].referenceTransform.set(2,3,.1f); },"nonaffine ancestor reference must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].referenceTransform=matrix(0,0,0); state.ancestors[0].referenceTransform.scale(0,1,1); },"singular ancestor reference must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].referenceTransform=matrix(0,0,0); state.ancestors[0].referenceTransform.scale(-.9f,1,1); },"reflected ancestor reference must be rejected");
    rejectNativeState([](auto& state) { state.ancestors[0].referenceTransform=matrix(0,0,0); state.ancestors[0].referenceTransform.scale(.8f,1,1); },"ancestor reference scale must match its object");
    // Ancestors are absent from the v2 wire format: an independently encoded
    // v2 fixture cannot carry the invalid in-memory fields. Check the writer's
    // refusal before serialization rather than demand that a reader infer them.
    rejectNativeOutput([](auto& state) { state.version=2; },"legacy v2 must not silently drop ancestor metadata on save");
    rejectNativeState([](auto& state) { state.legacyAncestors=true; },"legacy fallback marker requires an empty ancestor list");

    const auto validPayload = payload(*before.body);
    const size_t markerAt = 2*sizeof(uint32_t)+before.body->skeleton.size()+16*sizeof(float)+sizeof(uint32_t)+
                            before.body->parts.size()*(sizeof(uint32_t)+32*sizeof(float)+16*sizeof(float)+sizeof(uint8_t));
    const size_t countAt = markerAt+sizeof(uint8_t);
    auto badMarker = validPayload;
    badMarker[markerAt] = 2;
    reject(rawArchive(badMarker),"legacy ancestor marker must be exactly zero or one");
    badMarker[markerAt] = std::numeric_limits<uint8_t>::max();
    reject(rawArchive(badMarker),"maximum legacy marker byte must be rejected rather than treated as true");
    badMarker[markerAt] = 1;
    reject(rawArchive(badMarker),"legacy ancestor marker must not conceal a new body's reference records");
    auto huge = validPayload;
    const uint32_t hugeCount = std::numeric_limits<uint32_t>::max();
    std::memcpy(huge.data()+countAt,&hugeCount,sizeof(hugeCount));
    reject(rawArchive(huge),"declared ancestor count must be bounded before allocation");
    auto missingCount = validPayload;
    missingCount.resize(countAt);
    reject(rawArchive(missingCount),"v3 requires an explicit ancestor count even when there are no records");
    auto missingMarker = validPayload;
    missingMarker.resize(markerAt);
    reject(rawArchive(missingMarker),"v3 requires the complete typed legacy marker before the ancestor count");
    auto truncated = validPayload;
    truncated.pop_back();
    reject(rawArchive(truncated),"truncated ancestor matrix must not partially restore a pose");
    auto extra = validPayload;
    extra.push_back(42);
    reject(rawArchive(extra),"ancestor payload must consume its entire framed input");

    auto legacy = nativeExample();
    legacy.body->ancestors.clear();
    legacy.body->legacyAncestors = true;
    require(RagdollSave::valid(legacy),"explicit v3 re-export of a legacy body may omit unavailable historic ancestor frames");
    const auto legacyBytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,legacy);
      });
    Tempest::MemReader legacyReader(legacyBytes);
    Serialize legacyInput(legacyReader);
    legacyInput.setEntry(bodyEntry);
    Snapshot legacyAfter;
    require(RagdollSave::load(legacyInput,legacyAfter) && same(legacy,legacyAfter),
            "explicit legacy fallback metadata must remain distinguishable from complete new ancestor metadata");
    legacy.body->version = 2;
    require(!RagdollSave::valid(legacy),"old v2 format must not silently discard a v3 legacy-fallback marker");

    auto maximum = nativeExample();
    maximum.body->parts.resize(1);
    maximum.body->parts[0].bone = uint32_t(Resources::MAX_NUM_SKELETAL_NODES-1);
    maximum.body->ancestors.clear();
    for(size_t bone=0; bone<Resources::MAX_NUM_SKELETAL_NODES-1; ++bone) {
      Ragdoll::AncestorState ancestor;
      ancestor.bone = uint32_t(bone);
      ancestor.anchorBone = maximum.body->parts[0].bone;
      ancestor.referenceTransform = maximum.body->rootTransform*matrix(float(bone),0,0);
      maximum.body->ancestors.push_back(ancestor);
      }
    require(RagdollSave::valid(maximum),"all remaining skeleton bones may be stored as distinct anchored ancestors");
    const auto maximumBytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,maximum);
      });
    Tempest::MemReader maximumReader(maximumBytes);
    Serialize maximumInput(maximumReader);
    maximumInput.setEntry(bodyEntry);
    Snapshot maximumAfter;
    require(RagdollSave::load(maximumInput,maximumAfter) && same(maximum,maximumAfter),
            "largest legal ancestor list must round-trip without creating physical parts");
    }

  void testMalformed() {
    const auto validPayload = payload(*example().body);
    reject(rawArchive(validPayload,uint32_t(validPayload.size()),2),"unknown chunk schema must be rejected");
    reject(rawArchive(validPayload,uint32_t(validPayload.size()),schema,bodyFlag,0,magic^1),"wrong magic must be rejected");
    reject(rawArchive(validPayload,uint32_t(validPayload.size()),schema,bodyFlag|0x80),"unknown flag bits must be rejected");
    reject(rawArchive(validPayload,uint32_t(validPayload.size()),schema,bodyFlag,1),"reserved header bits must be rejected");
    reject(rawArchive({},std::numeric_limits<uint32_t>::max()-1),"huge payload must be rejected before allocation");
    reject(rawArchive({},0),"body flag without a payload must be rejected");
    reject(rawArchive({0},1,schema,0),"inactive header must not conceal a body payload");
    auto truncated = validPayload;
    truncated.pop_back();
    reject(rawArchive(truncated,uint32_t(validPayload.size())),"truncated entry must not restore a partial body");
    auto extra = validPayload;
    extra.push_back(42);
    reject(rawArchive(extra),"trailing framed data must be rejected");
    reject(rawArchive(validPayload,uint32_t(validPayload.size()),schema,bodyFlag,0,magic,true),
           "trailing bytes outside the frame must be rejected");
    auto sleeping = validPayload;
    sleeping.back() = 2;
    reject(rawArchive(sleeping),"sleeping byte must be exactly zero or one");
    auto hugeString = validPayload;
    const uint32_t huge = std::numeric_limits<uint32_t>::max();
    std::memcpy(hugeString.data()+sizeof(uint32_t),&huge,sizeof(huge));
    reject(rawArchive(hugeString),"declared string length must be bounded before allocation");
    const size_t countAt = 2*sizeof(uint32_t)+example().body->skeleton.size()+16*sizeof(float);
    auto hugeCount = validPayload;
    std::memcpy(hugeCount.data()+countAt,&huge,sizeof(huge));
    reject(rawArchive(hugeCount),"declared part count must be bounded before allocation");
    auto wrongVersionLayout = validPayload;
    const uint32_t legacyVersion = 1;
    std::memcpy(wrongVersionLayout.data(),&legacyVersion,sizeof(legacyVersion));
    reject(rawArchive(wrongVersionLayout),"v1 version with v2 reference bytes must be rejected rather than shifted into velocities");
    auto missingReference = validPayload;
    const size_t firstReference = countAt+sizeof(uint32_t)+sizeof(uint32_t)+16*sizeof(float);
    missingReference.erase(missingReference.begin()+std::ptrdiff_t(firstReference),
                           missingReference.begin()+std::ptrdiff_t(firstReference+16*sizeof(float)));
    reject(rawArchive(missingReference),"v2 payload without a complete reference matrix must be rejected");

    rejectState([](auto& state) { state.version=4; },"unknown body-state version must be rejected");
    rejectState([](auto& state) { state.skeleton.clear(); },"body must name its skeleton");
    rejectState([](auto& state) { state.skeleton.assign(513,'A'); },"skeleton name must be bounded");
    rejectState([](auto& state) { state.skeleton.push_back('\0'); },"embedded null skeleton names must be rejected");
    rejectState([](auto& state) { state.parts.clear(); },"body with no parts must be rejected");
    rejectState([](auto& state) { state.parts.resize(Resources::MAX_NUM_SKELETAL_NODES+1); },"too many parts must be rejected");
    rejectState([](auto& state) { state.parts[1].bone=state.parts[0].bone; },"duplicate physical bone must be rejected");
    rejectState([](auto& state) { state.parts[0].bone=uint32_t(Resources::MAX_NUM_SKELETAL_NODES); },"out-of-range bone must be rejected");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    rejectState([nan](auto& state) { state.rootTransform.set(3,0,nan); },"nonfinite root translation must be rejected");
    rejectState([inf](auto& state) { state.parts[0].transform.set(0,0,inf); },"nonfinite part basis must be rejected");
    rejectState([nan](auto& state) { state.parts[0].linearVelocity.x=nan; },"NaN linear velocity must be rejected");
    rejectState([inf](auto& state) { state.parts[0].angularVelocity.z=inf; },"infinite angular velocity must be rejected");
    rejectState([](auto& state) { state.rootTransform.set(3,1,100000008.f); },"unreasonable root coordinate must be rejected");
    rejectState([](auto& state) { state.parts[0].linearVelocity.y=1000001.f; },"unreasonable linear velocity must be rejected");
    rejectState([](auto& state) { state.parts[0].angularVelocity.x=10001.f; },"unreasonable angular velocity must be rejected");
    rejectState([](auto& state) { state.rootTransform=matrix(0,0,0); state.rootTransform.set(0,0,0); },"singular basis must be rejected");
    rejectState([](auto& state) { state.rootTransform=matrix(0,0,0); state.rootTransform.set(0,0,-1); },"reflected basis must be rejected");
    rejectState([](auto& state) { state.rootTransform=matrix(0,0,0); state.rootTransform.set(0,1,.1f); },"skewed basis must be rejected");
    rejectState([](auto& state) { state.parts[0].transform.set(2,3,.1f); },"perspective matrix must be rejected");
    rejectState([](auto& state) { state.rootTransform=matrix(0,0,0); state.rootTransform.scale(101,1,1); },"unreasonable scale must be rejected");
    rejectState([](auto& state) { state.parts[0].transform.scale(0.9f,1,1); },"v2 native frame must be unit rigid rather than visual scaled");
    rejectState([nan](auto& state) { state.parts[0].referenceTransform.set(3,0,nan); },"nonfinite reference translation must be rejected");
    rejectState([inf](auto& state) { state.parts[0].referenceTransform.set(0,1,inf); },"nonfinite reference basis must be rejected");
    rejectState([](auto& state) { state.parts[0].referenceTransform.set(1,3,.1f); },"nonaffine reference must be rejected");
    rejectState([](auto& state) { state.parts[0].referenceTransform.set(3,2,100000008.f); },"unreasonable reference translation must be rejected");
    rejectState([](auto& state) { state.parts[0].referenceTransform=matrix(0,0,0); state.parts[0].referenceTransform.scale(0,1,1); },"singular reference must be rejected");
    rejectState([](auto& state) { state.parts[0].referenceTransform=matrix(0,0,0); state.parts[0].referenceTransform.scale(-.9f,1,1); },"reflected reference must be rejected");
    rejectState([](auto& state) { state.parts[0].referenceTransform=matrix(0,0,0); state.parts[0].referenceTransform.scale(.8f,1,1); },"reference singular values must match object scale");
    rejectState([](auto& state) { state.parts[0].referenceTransform=matrix(0,0,0); state.parts[0].referenceTransform.set(0,1,.1f); },"arbitrary shear must not masquerade as the object's anisotropy");
    }

  void testBoundsAndAtomicSave() {
    auto maximum = nativeExample();
    maximum.body->ancestors.clear();
    maximum.body->legacyAncestors = true;
    maximum.body->skeleton.assign(512,'A');
    maximum.body->rootTransform = matrix(100000000.f,-100000000.f,0);
    maximum.body->rootTransform.scale(0.0001f,100.f,1.f);
    maximum.body->parts.resize(Resources::MAX_NUM_SKELETAL_NODES);
    for(size_t i=0; i<maximum.body->parts.size(); ++i) {
      auto& part = maximum.body->parts[i];
      part.bone = uint32_t(i);
      part.transform = matrix(float(i),0,0);
      part.centerOfMassMeters = {float(i)*.01f,0,0};
      part.rotation = {0,0,0,1};
      part.referenceTransform = matrix(float(i),0,0);
      part.referenceTransform.scale(0.0001f,100.f,1.f);
      part.linearVelocity = {1000000.f,-1000000.f,0};
      part.linearVelocityMeters = {10000.f,-10000.f,0};
      part.angularVelocity = {10000.f,-10000.f,0};
      }
    require(RagdollSave::valid(maximum),"inclusive supported bounds and all skeleton bones must be accepted");
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      RagdollSave::save(output,maximum);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    input.setEntry(bodyEntry);
    Snapshot after;
    require(RagdollSave::load(input,after) && same(after,maximum),"largest legal payload must round-trip");

    auto invalid = example();
    invalid.body->parts[0].linearVelocity.x = std::numeric_limits<float>::quiet_NaN();
    const auto atomic = archive([&](Serialize& output) {
      output.setEntry(bodyEntry);
      output.write(uint32_t(0x13579bdf));
      bool threw = false;
      try {
        RagdollSave::save(output,invalid);
        }
      catch(const std::invalid_argument&) {
        threw = true;
        }
      require(threw,"invalid outgoing snapshot must fail explicitly");
      });
    Tempest::MemReader atomicReader(atomic);
    Serialize atomicInput(atomicReader);
    atomicInput.setEntry(bodyEntry);
    uint32_t marker = 0;
    atomicInput.read(marker);
    require(marker==0x13579bdf,"invalid save must preserve earlier entry data");
    bool exhausted = false;
    try {
      atomicInput.read(marker);
      }
    catch(const std::runtime_error&) {
      exhausted = true;
      }
    require(exhausted,"invalid save must not append a partial chunk header");
    }
  }

namespace {
  void testQuestReturn() {
    constexpr const char* entry = "worlds/NEWWORLD.ZEN/npc/0/ragdoll-quest-return";
    RagdollSave::QuestReturn before;
    before.anchor = {37124.25f,-2048.5f,-74629.125f};
    before.angle = -57.25f;
    before.hasAnchor = true;
    before.armed = true;
    before.hazards = RagdollSave::QuestReturn::water | RagdollSave::QuestReturn::fell;
    const auto bytes = archive([&](Serialize& output) {
      output.setEntry(entry);
      RagdollSave::saveQuestReturn(output,before);
      });
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    require(input.setEntry(entry),"quest return entry must exist");
    RagdollSave::QuestReturn after;
    require(RagdollSave::loadQuestReturn(input,after),"pending dangerous KO must load");
    require(after.anchor==before.anchor && after.angle==before.angle && after.hasAnchor && after.armed &&
            after.hazards==before.hazards,"return point and both hazards must survive a save");

    for(bool truncated : {false,true}) {
      const auto malformed = archive([&](Serialize& output) {
        output.setEntry(entry);
        output.write(uint32_t(1));
        if(!truncated)
          output.write(Tempest::Vec3(std::numeric_limits<float>::quiet_NaN(),0,0),0.f,true,true,uint8_t(1));
        });
      Tempest::MemReader badReader(malformed);
      Serialize badInput(badReader);
      badInput.setEntry(entry);
      auto unchanged = before;
      require(!RagdollSave::loadQuestReturn(badInput,unchanged),"bad return entries must be rejected");
      require(unchanged.anchor==before.anchor && unchanged.armed && unchanged.hazards==before.hazards,
              "bad return entries must not overwrite the last valid point");
      }
    }
  }

int main() {
  testAnimationSampleCompatibility();
  testQuestReturn();
  testRoundTrip();
  testInactiveAndOldSave();
  testLegacyUniformState();
  testNativePrecision();
  testAncestors();
  testMalformed();
  testBoundsAndAtomicSave();
  std::puts("ragdoll save contracts passed");
  return EXIT_SUCCESS;
  }

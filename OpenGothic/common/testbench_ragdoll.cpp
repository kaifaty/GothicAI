#include "testbench_internal.h"
#include "testbench.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>
#include <Tempest/MemReader>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "gothic.h"
#include "camera.h"
#include "graphics/mesh/protomesh.h"
#include "frameprofile.h"
#include "game/gamesession.h"
#include "game/ragdollsave.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include <miniz.h>
#include "world/aiqueue.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

std::string ragdollNativePrecision(const Ragdoll::PartState& part) {
  return ",\"center_of_mass_meters\":"+ragdollVector(part.centerOfMassMeters)+
    ",\"rotationxyzw\":["+ragdollNumber(part.rotation.x)+","+ragdollNumber(part.rotation.y)+","+
      ragdollNumber(part.rotation.z)+","+ragdollNumber(part.rotation.w)+"]"+
    ",\"linear_velocity_meters\":"+ragdollVector(part.linearVelocityMeters);
  }

std::string ragdollAncestorFields(const Ragdoll::State& state) {
  std::string result=",\"legacy_ancestors\":"+std::string(state.legacyAncestors ? "true" : "false")+",\"ancestors\":[";
  for(size_t i=0;i<state.ancestors.size();++i) {
    if(i>0) result+=',';
    const auto& ancestor=state.ancestors[i];
    result+="{\"bone\":"+std::to_string(ancestor.bone)+",\"anchor_bone\":"+std::to_string(ancestor.anchorBone)+
      ",\"reference_transform\":"+ragdollMatrix(ancestor.referenceTransform)+"}";
    }
  return result+"]";
  }

std::string ragdollInventory(const Npc& npc, World& world, Inventory::IteratorType type) {
  std::map<size_t,size_t> counts;
  for(auto item=npc.inventory().iterator(type);item.isValid();++item)
    counts[item->clsId()]+=item.count();
  std::string result="[";
  for(const auto& [id,count]:counts) {
    if(result.size()>1) result+=',';
    const auto* symbol=world.script().findSymbol(id);
    result+="{\"class_id\":"+std::to_string(id)+",\"class_name\":"+
      jsonStr(symbol!=nullptr ? symbol->name() : std::string_view{})+",\"count\":"+std::to_string(count)+"}";
    }
  return result+"]";
  }

std::vector<uint8_t> oldNpcArchive(const std::vector<uint8_t>& bytes) {
  mz_zip_archive reader={}, writer={};
  if(!mz_zip_reader_init_mem(&reader,bytes.data(),bytes.size(),0))
    throw std::runtime_error("NPC archive reader initialization failed");
  if(!mz_zip_writer_init_heap(&writer,0,0)) {
    mz_zip_reader_end(&reader);
    throw std::runtime_error("NPC archive writer initialization failed");
    }
  size_t removed=0;
  for(mz_uint i=0;i<mz_zip_reader_get_num_files(&reader);++i) {
    mz_zip_archive_file_stat stat;
    if(!mz_zip_reader_file_stat(&reader,i,&stat)) {
      mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
      throw std::runtime_error("NPC archive entry read failed");
      }
    const std::string_view name(stat.m_filename);
    if(name.ends_with("/ragdoll") || name.ends_with("/ragdoll-recovery")) {
      ++removed;
      continue;
      }
    if(!mz_zip_writer_add_from_zip_reader(&writer,&reader,i)) {
      mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
      throw std::runtime_error("NPC archive entry copy failed");
      }
    }
  void* data=nullptr;
  size_t size=0;
  const bool finalized=mz_zip_writer_finalize_heap_archive(&writer,&data,&size)!=0;
  mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
  if(!finalized || removed!=2) {
    mz_free(data);
    throw std::runtime_error("old NPC fixture must remove exactly the two optional entries");
    }
  const auto* begin=static_cast<const uint8_t*>(data);
  std::vector<uint8_t> result(begin,begin+size);
  mz_free(data);
  return result;
  }

void ragdollNpcArchive(const std::vector<std::string>& args, World& world, Npc& player) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  const bool saving=args[1]=="save";
  Npc* npc=findNpc(world,player,args.size()>2 ? args[2] : "watched");
  const bool old=!saving && args.size()==5 && args[4]=="old";
  if(npc==nullptr || args.size()<4 || args.size()>5 || (saving && args.size()!=4) ||
     (args.size()==5 && !old) || args[3].empty() || args[3].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
    throw std::runtime_error("expected ragdoll save|restore <npc> <label> [old]");
    }
  const auto id=world.npcId(npc);
  if(id==uint32_t(-1))
    throw std::runtime_error("NPC archive actor does not belong to current world");
  const auto& label=args[3];
  if(saving) {
    if(benchState.savedRagdollNpcs.contains(label))
      throw std::runtime_error("NPC archive label already exists");
    RagdollBench::SavedNpc saved;
    saved.npc=npc; saved.position=npc->position(); saved.hp=npc->attribute(ATR_HITPOINTS);
    saved.dead=npc->isDead(); saved.unconscious=npc->isUnconscious();
    saved.enabled=Gothic::settingsGetI("GAME","ragdoll")!=0;
    const auto& probe=npc==&player ? Pose::playerProbe : Pose::subjectProbe;
    if(!probe.valid || probe.skeleton==nullptr || npc!=&context.subjectOf(player))
      throw std::runtime_error("NPC archive fixture needs the watched actor's complete bone probe");
    for(size_t i=0;i<probe.numBones;++i) {
      const auto& name=probe.skeleton->nodes[i].name;
      saved.bones.emplace_back(name,npc->mapBone(name));
      }
    {
    MemWriter writer(saved.bytes);
    Serialize out(writer); out.setContext(&world); out.setVersion(Serialize::Current);
    npc->save(out,id,"/npc/");
    }
    // Inspect what the real NPC saver wrote, including a dormant body while disabled.
    {
    MemReader reader(saved.bytes); Serialize in(reader); in.setContext(&world); in.setVersion(Serialize::Current);
    RagdollSave::Snapshot snapshot;
    if(!in.setEntry("worlds/",world.name(),"/npc/",id,"/ragdoll") || !RagdollSave::load(in,snapshot))
      throw std::runtime_error("actual NPC save did not write a valid optional body entry");
    saved.body=std::move(snapshot.body);
    }
    std::ofstream file(outDir+"/npc-"+label+".zip",std::ios::binary);
    file.write(reinterpret_cast<const char*>(saved.bytes.data()),std::streamsize(saved.bytes.size()));
    if(!file)
      throw std::runtime_error("unable to write the actual NPC archive fixture");
    event("ragdoll_npc_archive","\"operation\":\"save\",\"label\":"+jsonStr(label)+
          ",\"bytes\":"+std::to_string(saved.bytes.size())+",\"bone_count\":"+std::to_string(saved.bones.size())+
          ",\"saved_body\":"+(saved.body ? "true" : "false")+
          ",\"feature_enabled\":"+(saved.enabled ? "true" : "false")+
          ",\"t_game_ms\":"+std::to_string(world.tickCount()));
    benchState.savedRagdollNpcs.emplace(label,std::move(saved));
    say("ragdoll save "+label);
    return;
    }
  auto found=benchState.savedRagdollNpcs.find(label);
  if(found==benchState.savedRagdollNpcs.end() || found->second.npc!=npc)
    throw std::runtime_error("NPC archive must restore the same saved actor lifetime");
  const auto& saved=found->second;
  const auto bytes=old ? oldNpcArchive(saved.bytes) : saved.bytes;
  {
  MemReader reader(bytes); Serialize in(reader); in.setContext(&world); in.setVersion(Serialize::Current);
  npc->load(in,id,"/npc/"); npc->postValidate();
  }
  npc->prepareRagdoll(); npc->updateAnimation(0,true);
  const bool enabled=Gothic::settingsGetI("GAME","ragdoll")!=0;
  const auto* body=npc->ragdollBody();
  bool matches=npc->attribute(ATR_HITPOINTS)==saved.hp && npc->isDead()==saved.dead && npc->isUnconscious()==saved.unconscious;
  const bool expectedBody=!old && enabled && saved.body.has_value();
  matches=matches && (body!=nullptr)==expectedBody;
  float bodyPosition=0, bodyBasis=0, linear=0, angular=0;
  if(expectedBody && body!=nullptr) {
    const auto actual=body->state(); const auto& expected=*saved.body;
    matches=matches && actual.version==expected.version && actual.skeleton==expected.skeleton && actual.parts.size()==expected.parts.size();
    for(size_t i=0;i<16;++i) {
      const float delta=std::abs(actual.rootTransform.data()[i]-expected.rootTransform.data()[i]);
      matches=matches && std::isfinite(delta);
      if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
      }
    if(actual.parts.size()==expected.parts.size())
      for(size_t p=0;p<actual.parts.size();++p) {
        const auto& a=actual.parts[p]; const auto& e=expected.parts[p];
        matches=matches && a.bone==e.bone && a.sleeping==e.sleeping;
        if(actual.version>=3) {
          matches=matches && a.centerOfMassMeters==e.centerOfMassMeters && a.rotation==e.rotation &&
            a.linearVelocityMeters==e.linearVelocityMeters;
          }
        for(size_t i=0;i<16;++i) {
          const float delta=std::abs(a.transform.data()[i]-e.transform.data()[i]);
          matches=matches && std::isfinite(delta);
          if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
          if(actual.version>=2) {
            const float referenceDelta=std::abs(a.referenceTransform.data()[i]-e.referenceTransform.data()[i]);
            matches=matches && std::isfinite(referenceDelta);
            if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,referenceDelta); else bodyBasis=std::max(bodyBasis,referenceDelta);
            }
          }
        linear=std::max(linear,(a.linearVelocity-e.linearVelocity).length());
        angular=std::max(angular,(a.angularVelocity-e.angularVelocity).length());
        matches=matches && std::isfinite((a.linearVelocity-e.linearVelocity).length()) && std::isfinite((a.angularVelocity-e.angularVelocity).length());
        }
    if(actual.version>=3) {
      matches=matches && actual.legacyAncestors==expected.legacyAncestors && actual.ancestors.size()==expected.ancestors.size();
      if(actual.ancestors.size()==expected.ancestors.size())
        for(size_t a=0;a<actual.ancestors.size();++a) {
          const auto& got=actual.ancestors[a]; const auto& saved=expected.ancestors[a];
          matches=matches && got.bone==saved.bone && got.anchorBone==saved.anchorBone;
          for(size_t i=0;i<16;++i) {
            const float delta=std::abs(got.referenceTransform.data()[i]-saved.referenceTransform.data()[i]);
            matches=matches && std::isfinite(delta);
            if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
            }
          }
      }
    matches=matches && bodyPosition<=.001f && bodyBasis<=.00001f && linear<=.001f && angular<=.00001f;
    }
  const bool sameMode=!old && saved.enabled==enabled;
  float boneDelta=0;
  for(const auto& [name,before]:saved.bones) {
    const float delta=(npc->mapBone(name)-before).length();
    matches=matches && std::isfinite(delta);
    boneDelta=std::max(boneDelta,delta);
    }
  if(sameMode) matches=matches && boneDelta<=.001f && (npc->position()-saved.position).length()<=.001f;
  event("ragdoll_npc_archive","\"operation\":\"restore\",\"label\":"+jsonStr(label)+
        ",\"old_without_optional_entries\":"+(old ? "true" : "false")+
        ",\"matched\":"+(matches ? "true" : "false")+",\"body_expected\":"+(expectedBody ? "true" : "false")+
        ",\"body_present\":"+(body!=nullptr ? "true" : "false")+
        ",\"same_feature_mode\":"+(sameMode ? "true" : "false")+
        ",\"max_world_bone_delta_cm\":"+ragdollNumber(boneDelta)+
        ",\"max_part_position_delta_cm\":"+ragdollNumber(bodyPosition)+
        ",\"max_part_basis_delta\":"+ragdollNumber(bodyBasis)+
        ",\"max_linear_velocity_delta_cm_s\":"+ragdollNumber(linear)+
        ",\"max_angular_velocity_delta_rad_s\":"+ragdollNumber(angular)+
        ",\"t_game_ms\":"+std::to_string(world.tickCount()));
  if(!matches)
    throw std::runtime_error("actual NPC archive restore differs from saved pose, velocities, sleep or state");
  say("ragdoll restore "+label+(old ? " old" : ""));
  }

void ragdollSessionArchive(const std::vector<std::string>& args, World& world, Npc& player, uint64_t initialDt) {
  if(args.size()!=4 || args[3].empty() ||
     args[3].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("expected ragdoll session-save|session-snapshot|session-initial <npc> <label>");
  Npc* npc=findNpc(world,player,args[2]);
  const auto id=world.npcId(npc);
  const auto* skeleton=npc!=nullptr ? npc->visualSkeleton() : nullptr;
  if(npc==nullptr || id==uint32_t(-1) || skeleton==nullptr || skeleton->nodes.empty())
    throw std::runtime_error("session archive needs an exact actor with a loaded skeleton");
  const bool saving=args[1]=="session-save";
  const bool initial=args[1]=="session-initial";
  if(initial && initialDt!=0)
    throw std::runtime_error("initial session snapshot must precede the first positive simulation step");
  const auto* body=npc->ragdollBody();
  const std::string prefix=outDir+"/session-"+args[3];
  std::string fields="\"schema_version\":1,\"operation\":"+jsonStr(saving ? "save" : initial ? "initial" : "snapshot")+
    ",\"world\":"+jsonStr(world.name())+",\"actor_id\":"+std::to_string(id)+
    ",\"actor\":"+jsonStr(TestBench::who(npc))+",\"symbol\":"+jsonStr(symName(world,npc))+
    ",\"handle_id\":"+std::to_string(npc->handle().id)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"initial_dt\":"+(initial ? std::to_string(initialDt) : "null")+
    ",\"feature_enabled\":"+(Gothic::settingsGetI("GAME","ragdoll")!=0 ? "true" : "false")+
    ",\"dead\":"+(npc->isDead() ? "true" : "false")+
    ",\"unconscious\":"+(npc->isUnconscious() ? "true" : "false")+
    ",\"melee_knockdown\":"+(npc->isMeleeKnockedDown() ? "true" : "false")+
    ",\"recovering\":"+(npc->isRagdollBlending() ? "true" : "false")+
    ",\"hp\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+
    ",\"inventory\":"+ragdollInventory(*npc,world)+
    ",\"position_cm\":"+ragdollVector(npc->position())+
    ",\"object_transform\":"+ragdollMatrix(npc->transform())+
    ",\"sleeping\":"+(body!=nullptr && body->isSleeping() ? "true" : "false")+
    ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+",\"world_bones\":[";
  for(size_t i=0;i<skeleton->nodes.size();++i) {
    if(i>0) fields+=',';
    const auto& name=skeleton->nodes[i].name;
    fields+="{\"bone\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(npc->mapBone(name))+"}";
    }
  fields+="],\"snapshot\":";
  if(body!=nullptr) {
    const auto state=body->state();
    fields+="{\"version\":"+std::to_string(state.version)+",\"skeleton\":"+jsonStr(state.skeleton)+
      ",\"root\":"+ragdollMatrix(state.rootTransform)+",\"parts\":[";
    for(size_t i=0;i<state.parts.size();++i) {
      if(i>0) fields+=',';
      const auto& part=state.parts[i];
      fields+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
        ",\"reference_transform\":"+ragdollMatrix(part.referenceTransform)+
        (state.version>=3 ? ragdollNativePrecision(part) : "")+
        ",\"linear_velocity_cm_s\":"+ragdollVector(part.linearVelocity)+
        ",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
        ",\"sleeping\":"+(part.sleeping ? "true" : "false")+"}";
      }
    fields+="]"+(state.version>=3 ? ragdollAncestorFields(state) : "")+"}";
    } else {
    fields+="null";
    }
  if(saving) {
    std::vector<uint8_t> bytes;
    {
    MemWriter writer(bytes);
    Serialize out(writer);
    Pixmap preview(1,1,TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    Gothic::inst().gameSession()->save(out,"ragdoll "+args[3],preview);
    }
    std::ofstream file(prefix+".sav",std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    if(!file)
      throw std::runtime_error("cannot write full GameSession archive");
    fields+=",\"archive\":"+jsonStr("session-"+args[3]+".sav")+",\"archive_bytes\":"+std::to_string(bytes.size());
    }
  std::ofstream json(prefix+".json",std::ios::binary);
  json << '{' << fields << "}\n";
  if(!json)
    throw std::runtime_error("cannot write full session snapshot");
  event("ragdoll_session_archive",fields);
  say("ragdoll "+args[1]+" "+args[3]);
  }

void writeRagdoll(Npc& player) {
  const auto context=ragdollContext();
  Npc& subject=context.subjectOf(player);
  const Ragdoll* body=subject.ragdollBody();
  const auto stats=subject.world().physic()->physicsStats();
  const int mode=body!=nullptr ? 3 : 0; // Documented modes: off, kinematic, active, limp.
  std::fprintf(context.csv,",%.6f,%zu,%llu,%d,%d,%d",double(stats.stepMilliseconds),stats.bodies,
               (unsigned long long)stats.rays,mode,body!=nullptr ? 1 : 0,
               int(body!=nullptr && body->isSleeping()));

  auto id=context.bodyIds.find(&subject);
  if(id==context.bodyIds.end())
    id=context.bodyIds.emplace(&subject,++context.nextBodyId).first;
  const auto& probe=context.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  const auto observed=subject.bodyObservationState();
  std::string fields="\"schema_version\":1,\"actor_id\":"+std::to_string(id->second)+
    ",\"actor\":"+jsonStr(TestBench::who(&subject))+",\"step\":"+std::to_string(context.step)+
    ",\"t_game_ms\":"+std::to_string(subject.world().tickCount())+
    ",\"dead\":"+(subject.isDead() ? "true" : "false")+
    ",\"unconscious\":"+(subject.isUnconscious() ? "true" : "false")+
    ",\"recovering\":"+(subject.isRagdollBlending() ? "true" : "false")+
    ",\"in_air\":"+(subject.isInAir() ? "true" : "false")+
    ",\"ai_wait_ms\":"+std::to_string(observed.aiWaitMs)+
    ",\"ani_wait_ms\":"+std::to_string(observed.aniWaitMs)+
    ",\"output_wait_ms\":"+std::to_string(observed.outputWaitMs)+
    ",\"animations\":"+jsonStr(subject.animNames())+
    ",\"hp\":"+std::to_string(subject.attribute(ATR_HITPOINTS))+
    ",\"position_cm\":"+ragdollVector(subject.position())+
    ",\"mode\":"+std::to_string(mode)+",\"weight\":"+(body!=nullptr ? "1" : "0")+
    ",\"sleeping\":"+(body!=nullptr && body->isSleeping() ? "true" : "false")+
    ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+
    ",\"world_bones\":[";
  // Pose::probe is the animation before the procedural passes. mapBone reads the final
  // visual world transform, so this measurement includes NPC movement at recovery.
  if(probe.valid && probe.skeleton!=nullptr)
    for(size_t i=0;i<probe.numBones;++i) {
      if(i>0) fields+=',';
      const auto& name=probe.skeleton->nodes[i].name;
      fields+="{\"bone\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(subject.mapBone(name))+"}";
      }
  fields+="],\"snapshot\":";
  if(body!=nullptr) {
    const auto snapshot=body->state();
    fields+="{\"version\":"+std::to_string(snapshot.version)+",\"skeleton\":"+jsonStr(snapshot.skeleton)+
      ",\"root\":"+ragdollMatrix(snapshot.rootTransform)+",\"parts\":[";
    for(size_t i=0;i<snapshot.parts.size();++i) {
      if(i>0) fields+=',';
      const auto& part=snapshot.parts[i];
      fields+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
        ",\"reference_transform\":"+ragdollMatrix(part.referenceTransform)+
        (snapshot.version>=3 ? ragdollNativePrecision(part) : "")+
        ",\"linear_velocity_cm_s\":"+ragdollVector(part.linearVelocity)+
        ",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
        ",\"sleeping\":"+(part.sleeping ? "true" : "false")+"}";
      }
    fields+="]"+(snapshot.version>=3 ? ragdollAncestorFields(snapshot) : "")+"}";
    } else {
    fields+="null";
    }
  event("ragdoll_frame",fields);
  }

bool arenaProjected(const Vec3& point) {
  const auto* camera=Gothic::inst().camera();
  if(camera==nullptr) return false;
  float x=point.x,y=point.y,z=point.z,w=1;
  camera->viewProj().project(x,y,z,w);
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w) &&
         w>0 && std::abs(x)<=.90f*w && std::abs(y)<=.90f*w && z>=0 && z<=w;
  }

// A conservative final-pose envelope, actual loaded mesh bounds, and anatomical
// landscape/object sight lines. The untimed PNG independently verifies rendering.
bool arenaMeshVisible(World& world, const Npc& npc) {
  const auto context=ragdollContext();
  const auto* skeleton=npc.visualSkeleton();
  const auto bounds=npc.bounds();
  if(!TestBench::ragdollBodyDrawable(npc) || skeleton==nullptr || skeleton->nodes.empty() || !std::isfinite(bounds.rConservative) || bounds.rConservative<=0)
    return false;
  const auto object=npc.transform();
  for(int c=0;c<8;++c) {
    Vec3 p{bounds.bbox[c&1].x,bounds.bbox[(c>>1)&1].y,bounds.bbox[(c>>2)&1].z};
    object.project(p);
    if(!arenaProjected(p)) return false;
    }
  for(const auto& bone:skeleton->nodes) {
    const auto p=npc.mapBone(bone.name);
    for(int c=0;c<8;++c)
      if(!arenaProjected(p+Vec3((c&1) ? 50.f : -50.f,(c&2) ? 50.f : -50.f,(c&4) ? 50.f : -50.f)))
        return false;
    }
  for(const auto name:{"BIP01 PELVIS","BIP01 HEAD","BIP01 L FOOT","BIP01 R FOOT"})
    if(skeleton->findNode(name)==size_t(-1) || hidden(world,context.camPos,npc.mapBone(name)+Vec3(0,5,0)))
      return false;
  return true;
  }

bool arenaCheapVisible(const Npc& npc, size_t index) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  if(!TestBench::ragdollBodyDrawable(npc) || index>=benchState.ragdollArenaRadii.size()) return false;
  const double radius=benchState.ragdollArenaRadii[index];
  const auto center=npc.position();
  const auto* camera=Gothic::inst().camera();
  if(camera==nullptr || !std::isfinite(radius) || radius<=0) return false;
  const auto vp=camera->viewProj();
  float x=center.x,y=center.y,z=center.z,w=1;
  vp.project(x,y,z,w);
  if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) || w<=0 ||
     std::abs(x)>w || std::abs(y)>w || z<0 || z>w) return false;
  // Conservative sphere NONREJECTION; full vertex/occlusion visibility is proven
  // separately by all untimed posed envelopes. Never test the larger cube corners.
  for(int plane=0;plane<6;++plane) {
    double coefficients[4]={};
    for(int column=0;column<4;++column) {
      if(plane<4) coefficients[column]=.90*vp.at(column,3)+(plane%2==0 ? 1. : -1.)*vp.at(column,plane/2);
      else coefficients[column]=plane==4 ? vp.at(column,2) : double(vp.at(column,3))-vp.at(column,2);
      if(!std::isfinite(coefficients[column])) return false;
      }
    const double normal=std::sqrt(coefficients[0]*coefficients[0]+coefficients[1]*coefficients[1]+coefficients[2]*coefficients[2]);
    const double distance=coefficients[0]*center.x+coefficients[1]*center.y+coefficients[2]*center.z+coefficients[3];
    if(!std::isfinite(distance) || distance< -radius*normal) return false;
    }
  return true;
  }

RagdollBench::PerfSample ragdollCoverageSample(World& world) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  RagdollBench::PerfSample sample;
  sample.physicalBeforeImpulse=benchState.perfPhysicalBeforeImpulse;
  sample.sleepingBeforeImpulse=benchState.perfSleepingBeforeImpulse;
  sample.actors=context.crew.size();
  const auto* camera=Gothic::inst().camera();
  const auto vp=camera!=nullptr ? camera->viewProj() : Matrix4x4::mkIdentity();
  for(size_t actor=0;actor<context.crew.size();++actor) {
    const auto* npc=context.crew[actor];
    if(npc->isDead()) ++sample.corpses;
    const auto* body=npc->ragdollBody();
    if(body!=nullptr) {
      ++sample.physical; sample.parts+=body->partCount();
      if(body->isSleeping()) ++sample.sleeping;
      }
    const auto center=npc->centerPosition();
    float x=center.x,y=center.y,z=center.z,w=1;
    vp.project(x,y,z,w);
    if(camera!=nullptr && w>0 && std::abs(x)<=w && std::abs(y)<=w && z>=0 && z<=w)
      ++sample.centersInView;
    if(benchState.ragdollArenaCamera && (context.ragdollPerf ? arenaCheapVisible(*npc,actor) : arenaMeshVisible(world,*npc))) ++sample.visibleMeshes;
    }
  return sample;
  }

void sampleRagdollPerformance(World& world) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  FrameProfile::sample(context.recName,context.recFrame);
  auto sample=ragdollCoverageSample(world);
  sample.segment=context.recName; sample.frame=context.recFrame; sample.wallMs=benchState.perfWallMs;
  const auto stats=world.physic()->physicsStats();
  sample.stepMs=stats.stepMilliseconds; sample.worldBodies=stats.bodies; sample.rays=stats.rays;
  benchState.ragdollPerformance.push_back(std::move(sample));
  }

bool arenaModeMatches(const RagdollBench::PerfSample& sample, std::string_view mode) {
  const size_t physical=mode=="disabled" ? 0 : 20;
  const size_t sleeping=mode=="sleeping" ? 20 : 0;
  return sample.actors==20 && sample.corpses==20 && sample.centersInView==20 && sample.visibleMeshes==20 &&
         sample.physical==physical && sample.physicalBeforeImpulse==physical && sample.sleeping==sleeping &&
         sample.sleepingBeforeImpulse==sleeping && sample.parts==(physical==0 ? 0 : 480);
  }

void arenaModelProof(World& world, const Npc& npc, size_t frame) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  const auto* skeleton=npc.visualSkeleton();
  if(skeleton==nullptr) return;
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low{infinity,infinity,infinity},high{-infinity,-infinity,-infinity};
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x); low.y=std::min(low.y,point.y); low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x); high.y=std::max(high.y,point.y); high.z=std::max(high.z,point.z);
    };
  const auto bounds=npc.bounds(); const auto object=npc.transform();
  for(int corner=0;corner<8;++corner) {
    Vec3 p{bounds.bbox[corner&1].x,bounds.bbox[(corner>>1)&1].y,bounds.bbox[(corner>>2)&1].z};
    object.project(p); add(p);
    }
  for(const auto& bone:skeleton->nodes) {
    const auto p=npc.mapBone(bone.name); add(p-Vec3(50,50,50)); add(p+Vec3(50,50,50));
    }
  Vec3 projectedLow{infinity,infinity,infinity},projectedHigh{-infinity,-infinity,-infinity};
  bool projected=true;
  const auto viewProjection=Gothic::inst().camera()->viewProj();
  for(int corner=0;corner<8;++corner) {
    float x=(corner&1) ? high.x : low.x,y=(corner&2) ? high.y : low.y,z=(corner&4) ? high.z : low.z,w=1;
    viewProjection.project(x,y,z,w);
    if(w<=0) { projected=false; continue; }
    x/=w; y/=w; z/=w;
    projected=projected && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
              std::abs(x)<=.90f && std::abs(y)<=.90f && z>=0 && z<=1;
    projectedLow.x=std::min(projectedLow.x,x); projectedLow.y=std::min(projectedLow.y,y); projectedLow.z=std::min(projectedLow.z,z);
    projectedHigh.x=std::max(projectedHigh.x,x); projectedHigh.y=std::max(projectedHigh.y,y); projectedHigh.z=std::max(projectedHigh.z,z);
    }
  if(!projected) benchState.ragdollCoverageFailed=true;
  const auto iterator=std::find(context.crew.begin(),context.crew.end(),&npc);
  const bool cheap=iterator!=context.crew.end() && arenaCheapVisible(npc,size_t(iterator-context.crew.begin()));
  const float radius=iterator!=context.crew.end() ? benchState.ragdollArenaRadii[size_t(iterator-context.crew.begin())] : 0;
  bool contains=true;
  for(int corner=0;corner<8;++corner) {
    const Vec3 point{(corner&1) ? high.x : low.x,(corner&2) ? high.y : low.y,(corner&4) ? high.z : low.z};
    contains=contains && (point-npc.position()).length()<=radius;
    }
  if(!cheap || !contains) benchState.ragdollCoverageFailed=true;
  event("ragdoll_coverage_model","\"coverage_frame\":"+std::to_string(frame)+",\"actor_id\":"+
        std::to_string(world.npcId(&npc))+",\"model_bound_radius_cm\":"+std::to_string(bounds.rConservative)+
        ",\"bone_count\":"+std::to_string(skeleton->nodes.size())+",\"world_min_cm\":"+ragdollVector(low)+
        ",\"world_max_cm\":"+ragdollVector(high)+",\"ndc_min\":"+ragdollVector(projectedLow)+
        ",\"ndc_max\":"+ragdollVector(projectedHigh)+",\"projected_with_margin\":"+(projected ? "true" : "false")+
        ",\"body_drawable\":"+(TestBench::ragdollBodyDrawable(npc) ? "true" : "false")+
        ",\"proxy_radius_cm\":"+std::to_string(radius)+",\"actor_origin_cm\":"+ragdollVector(npc.position())+
        ",\"proxy_contains_envelope\":"+(contains ? "true" : "false")+
        ",\"cheap_proxy_visible\":"+(cheap ? "true" : "false")+
        ",\"visible\":"+(arenaMeshVisible(world,npc) ? "true" : "false"));
  }

bool chooseRagdollArena(World& world, Npc& player) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  if(!context.crew.empty()) return false;
  std::vector<const WayPoint*> candidates;
  world.findWayPoint(player.position(),[&](const WayPoint& wp) { candidates.push_back(&wp); return false; });
  const auto origin=player.position();
  std::sort(candidates.begin(),candidates.end(),[&](const auto* a,const auto* b) {
    const auto da=(a->pos-origin).quadLength(),db=(b->pos-origin).quadLength();
    return da==db ? a->name<b->name : da<db;
    });
  size_t rejected=0;
  for(const auto* waypoint:candidates) {
    std::vector<Vec3> places;
    float lowest=std::numeric_limits<float>::infinity(),highest=-lowest;
    bool valid=true;
    for(int i=0;i<20 && valid;++i) {
      Vec3 place=waypoint->pos+Vec3(float(i%5)*250.f-500.f,0,float(i/5)*250.f-375.f);
      const auto floor=world.physic()->landRay(place+Vec3(0,300,0),900);
      if(!floor.hasCol || floor.n.y<.98f) { valid=false; break; }
      place.y=floor.v.y;
      for(float dx:{-150.f,0.f,150.f}) for(float dz:{-150.f,0.f,150.f}) {
        const auto support=world.physic()->landRay(place+Vec3(dx,300,dz),600);
        if(!support.hasCol || support.n.y<.98f || std::abs(support.v.y-place.y)>20.f) valid=false;
        }
      if(world.physic()->ray(place+Vec3(0,40,0),place+Vec3(0,3200,0)).hasCol) valid=false;
      lowest=std::min(lowest,place.y); highest=std::max(highest,place.y);
      places.push_back(place);
      }
    if(!valid || places.size()!=20 || highest-lowest>40.f) { ++rejected; continue; }
    const Vec3 target{waypoint->pos.x,(lowest+highest)*.5f+80.f,waypoint->pos.z};
    const Vec3 eye=target+Vec3(0,2600,1100);
    for(const auto& place:places)
      if(hidden(world,eye,place+Vec3(0,30,0))) valid=false;
    if(!valid) { ++rejected; continue; }
    benchState.ragdollArenaPlaces=std::move(places); benchState.ragdollArenaWaypoint=waypoint->name;
    player.setPosition(target+Vec3(0,-80,-1200)); player.updateTransform();
    benchState.ragdollArenaTarget=target;
    event("ragdoll_arena_site","\"waypoint\":"+jsonStr(waypoint->name)+",\"geometry_rejections\":"+
          std::to_string(rejected)+",\"center\":"+ragdollVector(target)+",\"ground_min_cm\":"+
          std::to_string(lowest)+",\"ground_max_cm\":"+std::to_string(highest)+",\"places\":20");
    return true;
    }
  return false;
  }

bool arenaFullSchema(World& world, std::string_view mode) {
  const auto context=ragdollContext();
  auto& benchState=context.ragdoll;
  if(context.crew.size()!=20 || !benchState.ragdollArenaCamera) return false;
  std::unordered_set<size_t> ids;
  std::vector<uint32_t> topology;
  for(const auto* npc:context.crew) {
    const auto* skeleton=npc->visualSkeleton(); const auto* body=npc->ragdollBody();
    if(!ids.insert(world.npcId(npc)).second || skeleton==nullptr || skeleton->name()!="HUMANS.MDS" ||
       symName(world,npc)!="BDT_1013_BANDIT_L" || !npc->isDead() || !arenaMeshVisible(world,*npc)) return false;
    if(mode=="disabled") { if(body!=nullptr) return false; continue; }
    if(body==nullptr || body->partCount()!=24 || body->boneIndices().size()!=24) return false;
    const auto& bones=body->boneIndices();
    if(topology.empty()) topology.assign(bones.begin(),bones.end());
    if(!std::equal(topology.begin(),topology.end(),bones.begin())) return false;
    }
  return true;
  }

bool ragdollCommand(const RagdollContext& context,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& benchState=context.ragdoll;
  const auto& c=a[0];
  if(c=="ragdoll" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","ragdoll",a[1]=="on" ? 1 : 0);
    say("ragdoll "+a[1]);
    event("ragdoll_toggle","\"enabled\":"+std::string(a[1]=="on" ? "true" : "false"));
    }
  else if(c=="ragdoll" && a.size()>1 && (a[1]=="save" || a[1]=="restore")) {
    try {
      ragdollNpcArchive(a,w,pl);
      } catch(const std::exception& error) {
      say("ragdoll archive failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }
  else if(c=="ragdoll" && a.size()>1 && (a[1]=="session-save" || a[1]=="session-snapshot" || a[1]=="session-initial")) {
    try {
      if(a[1]=="session-initial")
        throw std::runtime_error("session-initial must be the first scenario command at dt=0");
      ragdollSessionArchive(a,w,pl);
      } catch(const std::exception& error) {
      say("ragdoll session failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="loot") {
    try {
      Npc* npc=findNpc(w,pl,a[2]);
      if(!context.ragdollTrace || npc==nullptr || npc==&pl || !npc->isDead() || npc->ragdollBody()==nullptr || pl.isDown())
        throw std::runtime_error("loot fixture requires trace=1 and a physical corpse with a live player");
      const auto inventory=ragdollInventory(*npc,w);
      const auto lootable=ragdollInventory(*npc,w,Inventory::T_Ransack);
      if(lootable=="[]")
        throw std::runtime_error("loot fixture needs nonempty actual ransack inventory");
      ctrl.clearFocus(); ctrl.tickFocus();
      const auto focus=ctrl.focus();
      const bool matched=focus.npc==npc;
      event("ragdoll_loot_focus","\"expected_actor_id\":"+std::to_string(w.npcId(npc))+
        ",\"selected_actor_id\":"+std::to_string(w.npcId(focus.npc))+
        ",\"actor\":"+jsonStr(TestBench::who(npc))+
        ",\"matched\":"+(matched ? "true" : "false")+
        ",\"position_cm\":"+ragdollVector(npc->position())+",\"lootable_inventory\":"+lootable);
      if(!matched)
        throw std::runtime_error("ordinary player focus did not select the exact moved corpse");
      benchState.expectedRagdollLoot=npc; benchState.ragdollLootDidOpen=false;
      const bool interacted=ctrl.interact(*npc);
      benchState.expectedRagdollLoot=nullptr;
      if(!interacted || !benchState.ragdollLootDidOpen || inventory!=ragdollInventory(*npc,w))
        throw std::runtime_error("ordinary rendered ransack did not open the exact corpse or changed inventory");
      say("ragdoll loot "+symName(w,npc)+" verified exact native Ransack target; closed synchronously without transfer");
      } catch(const std::exception& error) {
      benchState.expectedRagdollLoot=nullptr;
      say("ragdoll loot failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }
  else if(c=="ragdoll" && a.size()==4 && a[1]=="queued-standup") {
    Npc* npc=findNpc(w,pl,a[2]);
    const float delay=num(a,3,0);
    if(npc==nullptr || npc->isDead() || !npc->isUnconscious() || npc->ragdollBody()==nullptr ||
       !std::isfinite(delay) || delay<1 || delay>30000 || std::floor(delay)!=delay) {
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    npc->setExternalControl(false);
    // Keep ZS_Unconscious: ending it would let prepareRagdoll auto-recover before the queue.
    npc->clearAiQueue();
    npc->aiPush(AiQueue::aiWait(uint64_t(delay)));
    npc->aiPush(AiQueue::aiStandup());
    event("ragdoll_queued_standup","\"actor\":"+jsonStr(TestBench::who(npc))+
          ",\"actor_id\":"+std::to_string(w.npcId(npc))+",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"delay_ms\":"+std::to_string(uint64_t(delay))+
          ",\"control\":\"normal AI_Wait then AI_StandUp queue; no direct recovery\"");
    say("ragdoll queued-standup "+symName(w,npc));
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="remove") {
    Npc* npc=findNpc(w,pl,a[2]);
    if(npc==nullptr || npc==&pl || npc->ragdollBody()==nullptr) {
      say("ragdoll remove failed: expected a physical NPC corpse");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    const auto parts=npc->ragdollBody()->partCount();
    const auto before=w.physic()->physicsStats().bodies;
    w.removeNpc(*npc);
    const auto after=w.physic()->physicsStats().bodies;
    const bool removed=npc->ragdollBody()==nullptr && before>=parts && after==before-parts;
    event("ragdoll_remove","\"parts\":"+std::to_string(parts)+",\"bodies_before\":"+
          std::to_string(before)+",\"bodies_after\":"+std::to_string(after)+",\"matched\":"+(removed ? "true" : "false"));
    if(!removed)
      event("ragdoll_error","\"reason\":\"removing NPC retained native ragdoll parts\"");
    if(context.questNpc==npc) context.questNpc=nullptr;
    context.crew.erase(std::remove(context.crew.begin(),context.crew.end(),npc),context.crew.end());
    if(benchState.waitRagdollNpc==npc) benchState.waitRagdollNpc=nullptr;
    if(context.subject==npc) exec("watch player",w,pl,ctrl);
    say("ragdoll remove "+std::to_string(parts)+" parts");
    }
  else if(c=="ragdoll" && a.size()==2 && a[1]=="arena") {
    if(!chooseRagdollArena(w,pl)) arenaAbort("no geometry-valid twenty-actor arena");
    else exec("clear 2500",w,pl,ctrl);
    }
  else if(c=="ragdoll" && a.size()==2 && a[1]=="awake20") {
    if(context.crew.size()!=20 || std::any_of(context.crew.begin(),context.crew.end(),[](const auto* npc) {
         return npc->ragdollBody()==nullptr || npc->ragdollBody()->partCount()!=24;
         })) { arenaAbort("awake20 requires twenty full physical humans"); return true; }
    for(auto* npc:context.crew)
      const_cast<Ragdoll*>(npc->ragdollBody())->setAllowSleeping(false);
    event("ragdoll_benchmark_sleep_policy","\"actors\":20,\"allow_sleeping\":false,\"scope\":\"benchmark ACTIVE only; normal gameplay unchanged\"");
    }
  else if(c=="ragdoll" && a.size()==4 && a[1]=="birth-track") {
    benchState.birthActor=findNpc(w,pl,a[2]); benchState.birthLabel=a[3]; benchState.birthHistory.clear();
    if(benchState.birthActor==nullptr || benchState.birthActor->isDown()) arenaAbort("birth-track requires a live actor");
    }
  else if(c=="ragdoll" && a.size()==5 && a[1]=="birth-down") {
    auto* npc=findNpc(w,pl,a[2]);
    if(npc==nullptr || npc!=benchState.birthActor || benchState.birthHistory.size()<4 || (a[3]!="dead" && a[3]!="ko")) {
      arenaAbort("birth-down requires four ordinary live history samples and a matching actor"); return true;
      }
    const auto current=benchState.birthHistory.back(),previous=benchState.birthHistory[benchState.birthHistory.size()-2];
    const auto elapsed=current.tick-previous.tick;
    if(elapsed==0 || elapsed>250 || current.tick!=w.tickCount()) { arenaAbort("birth history must end at the actual current world tick"); return true; }
    const auto velocity=(current.position-previous.position)*(1000.f/float(elapsed));
    std::string history="[";
    for(const auto& sample:benchState.birthHistory) {
      if(history.size()>1) history+=',';
      history+="{\"t_game_ms\":"+std::to_string(sample.tick)+",\"in_air\":"+(sample.inAir ? "true" : "false")+
               ",\"position_cm\":"+ragdollVector(sample.position)+"}";
      }
    history+=']'; benchState.birthActor=nullptr;
    npc->setLastHitBy(&pl);
    if(!npc->setRagdollDown(a[3]=="dead")) { arenaAbort("actual production down path refused birth oracle"); return true; }
    npc->prepareRagdoll();
    const auto* body=npc->ragdollBody();
    if(body==nullptr || body->partCount()!=24) { arenaAbort("birth oracle requires a complete native human"); return true; }
    const auto state=body->state();
    std::string velocities="[";
    for(const auto& part:state.parts) { if(velocities.size()>1) velocities+=','; velocities+=ragdollVector(part.linearVelocity); }
    velocities+=']';
    event("ragdoll_birth","\"label\":"+jsonStr(a[4])+",\"actor_id\":"+std::to_string(w.npcId(npc))+
          ",\"t_game_ms\":"+std::to_string(w.tickCount())+",\"mode\":"+jsonStr(a[4]=="stationary" ? "stationary" : a[3])+
          ",\"history\":"+history+",\"expected_velocity_cm_s\":"+ragdollVector(velocity)+",\"parts\":24,\"velocities_cm_s\":"+
          velocities+",\"birth_pelvis_y_cm\":"+std::to_string(body->pelvisTransform().data()[13]));
    ragdollSessionArchive({"ragdoll","session-snapshot",a[2],a[4]},w,pl,0);
    benchState.birthContinuation=npc; benchState.birthContinuationLabel=a[4]; benchState.birthTick=w.tickCount();
    }
  else if(c=="ragdoll" && a.size()==3 && (a[1]=="verify20" || a[1]=="coverage")) {
    const auto& mode=a[2];
    if((mode!="disabled" && mode!="active" && mode!="sleeping") || !arenaFullSchema(w,mode)) {
      arenaAbort("twenty complete visible model/schema precondition failed"); return true;
      }
    if(a[1]=="coverage") {
      if(context.ragdollPerf) { arenaAbort("untimed coverage requires OPENGOTHIC_RAGDOLL_PERF=0"); return true; }
      benchState.ragdollCoverageMode=mode; benchState.ragdollCoverageFrames=0; benchState.ragdollCoverageActive=true;
      } else event("ragdoll_arena_verified","\"mode\":"+jsonStr(mode)+",\"actors\":20,\"visible_meshes\":20");
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="churn" && (a[2]=="on" || a[2]=="off")) {
    benchState.ragdollChurn=a[2]=="on";
    benchState.ragdollPulseFrame=0;
    say("ragdoll churn "+a[2]);
    }
  else return false;
  return true;
  }

}

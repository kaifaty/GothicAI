#include "testbench_internal.h"
#include "testbench.h"

#include <Tempest/SystemApi>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "npcdayjournal.h"
#include "camera.h"
#include "game/gamescript.h"
#include "game/archerai.h"
#include "game/playercontrol.h"
#include "graphics/drawcommands.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/talkgestures.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/eyes.h"
#include "graphics/mesh/lean.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/animblend.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/sound.h"
#include "world/world.h"
#include "gothic.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

// the faces a mood shows (the bridge's say.mood)
struct FaceColumn { const char* column; const char* anim; };
const FaceColumn FaceColumns[] = {
  {"face_neutral","S_NEUTRAL"}, {"face_friendly","S_FRIENDLY"}, {"face_angry","S_ANGRY"},
  {"face_hostile","S_HOSTILE"}, {"face_afraid","S_FRIGHTENED"}, {"face_pain","T_HURT"},
  };
// the facial gestures of the dictionary (say.cues, the own head's profile): cue_<name>
const char* const CueColumns[] = {"flash","doubt","smirk","press","disgust","squint","frown","chin","wince","glance","roll"};

void writeHeader() {
  std::fprintf(bench.csv,"seg,frame,t,x,y,z,rot,speed,bs,walk,occl,ik_valid,ik_on,ik_hardoff,ik_w,nlegs,bad,pelvis,chest,pelvis_target,pelvis_limit,floor");
  for(const char* s:{"l","r","fl","fr"}) {
    std::fprintf(bench.csv,",%s_plant,%s_has,%s_gy,%s_nx,%s_ny,%s_nz,%s_toehas,%s_toey,%s_smooth,%s_err,%s_knee,%s_tilt,%s_roll,%s_yaw,%s_out,%s_slack",
                 s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s);
    for(const char* j:{"hip","kneea","anka","toea","kneef","ankf","toef"})
      std::fprintf(bench.csv,",%s_%s_x,%s_%s_y,%s_%s_z",s,j,s,j,s,j);
    }
  std::fprintf(bench.csv,",g_on,g_src,g_w,g_err,g_yaw,g_pitch,g_yawgoal,g_pitchgoal,g_head,g_chest,g_waist,g_animyaw,g_animpitch,g_hipsyaw,g_fyaw,g_fpitch,pg_on,pg_src,pg_w,pg_err,pg_yaw");
  std::fprintf(bench.csv,",ln_on,ln_roll,ln_pitch,ln_speed,ln_turn,ln_slope");
  std::fprintf(bench.csv,",an_pop,an_bone,an_speed,an_layers,bl_on,bl_active,bl_started,bl_angle,bl_time");
  std::fprintf(bench.csv,",a_on,a_w,a_yaw,a_pitch,a_animyaw,a_animpitch,a_turn,a_bend,a_err,a_steady,a_lean,a_side,ik_crouch,m_crouch,m_peek,m_cant,m_walk,aw_w,aw_speed,aw_twist,aw_chest,aw_drop");
  std::fprintf(bench.csv,",m_stage,m_draw,m_hold,m_fatigue,m_breath,m_holding,m_short,m_disturb,m_amp,m_sway_yaw,m_sway_pitch,m_off,m_spread,m_cross");
  std::fprintf(bench.csv,",bd_w,bd_draw,bd_span,bd_travel,bd_miss,bd_follow,bd_fit,bd_offset,bd_angle,hd_hand,hd_fore,hd_arrow,bd_hold,bd_nock,rh_x,rh_y,rh_z,bd_held,bd_overgrip,bd_atmiddle,bd_nockgap,bd_square,bd_cant,bd_bowmove,bd_bowmiss,bd_back,bd_kick,bd_turn");
  std::fprintf(bench.csv,",lip_valid,lip_on,lip_t,lip_level,lip_mouth,lip_open,lip_f0,lip_f1,lip_alpha,lip_int,lip_picks");
  std::fprintf(bench.csv,",ge_on,ge_talk,ge_id,ge_mood,ge_starts,ge_cuts,ge_stops,ge_nods,ge_pitch,ge_yaw,ge_roll");
  std::fprintf(bench.csv,",ge_listen,ge_think,ge_lnods,ge_aside");
  std::fprintf(bench.csv,",bk_on,bk_w,bk_n,bk_pause,bk_shift,bk_double");
  for(auto& f:FaceColumns)
    std::fprintf(bench.csv,",%s",f.column);
  std::fprintf(bench.csv,",rig_on,rig_anims,rig_targets,rig_us,rig_n,rig_up_us");
  std::fprintf(bench.csv,",ey_on,ey_kind,ey_ly,ey_lp,ey_ry,ey_rp,ey_err,ey_over,ey_move,ey_sac,ey_big,ey_dy,ey_down,ey_hp");
  for(auto* c:CueColumns)
    std::fprintf(bench.csv,",cue_%s",c);
  std::fprintf(bench.csv,",cue_leak");
  std::fprintf(bench.csv,",lip_tone,br_w,br_acc,br_moves");
  std::fprintf(bench.csv,",f_on,f_bend,f_yaw,f_twist,f_head,f_hits,f_str,f_ht");
  std::fprintf(bench.csv,",hp_player,hp_subject");
  std::fprintf(bench.csv,",stamina_player,stamina_subject");
  std::fprintf(bench.csv,",ai_on,ai_want,ai_sigma,ai_errx,ai_erry,ai_lead,ai_pv,ai_ps");
  std::fprintf(bench.csv,",mr_w,mr_dev,mr_hits,physics_ms");
  // the watched one's quiver (graphics/mesh/quiver): cm the armour and the legs go into it, the arrows shown,
  // the mouth in the pelvis bone's frame, how far out it was pushed, the arrows over the mouth and the bottom
  std::fprintf(bench.csv,",qv_on,qv_arrows,qv_armor,qv_leg,qv_arm,qv_mx,qv_my,qv_mz,qv_out,qv_top,qv_bottom,qv_swing,qv_back,qv_outward,qv_us,qv_twist,qv_bow,qv_2h,qv_blade,qv_yield,qv_legt,qv_legl,qv_gnd,qv_gndm,qv_gmode,qv_gon,qv_botup,qv_gndt,qv_need,qv_needb,qv_needo,qv_needbody,qv_trunk,qv_clear,qv_rays,qv_rayus,qv_cpu,qv_ax,qv_ay,qv_az");
  if(ragdollTrace)
    std::fprintf(bench.csv,",ph_ms,ph_bodies,ph_rays,rd_mode,rd_weight,rd_sleep");
  if(bench.routine.questTrace)
    std::fprintf(bench.csv,",ig_immortal,ig_enabled,ig_anchor,ig_armed,ig_hazards,ig_returns,ig_reason,ig_anchor_x,ig_anchor_y,ig_anchor_z,ig_x,ig_y,ig_z,ig_drop_cm,ig_in_water,ig_ko,ig_wall_ms");
  if(bench.living.livingFixture)
    std::fprintf(bench.csv,",lb_models,lb_alive,lb_kinematic,lb_parts,lb_dynamic,lb_dynamic_parts,lb_duplicates,lb_world_bodies,lb_ph_ms");
  std::fprintf(bench.csv,"\n");
  }

// the animated pose of the watched one (or the hero): the largest second difference of a joint over
// three frames (cm; a pop), which joint, the fastest joint (cm/s) and the playing animations
void writeAnim() {
  const auto&  a    = bench.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  const size_t n    = a.valid ? a.numBones : 0;
  float        pop  = 0, speed = 0;
  size_t       bone = size_t(-1);
  if(bench.anim2.size()==n && bench.anim1.size()==n) {
    for(size_t i=0; i<n; ++i) {
      const float d2 = (a.joint[i] - bench.anim1[i]*2.f + bench.anim2[i]).length();
      if(d2>pop) {
        pop  = d2;
        bone = i;
        }
      speed = std::max(speed,(a.joint[i] - bench.anim1[i]).length()*float(Fps));
      }
    }
  bench.anim2 = std::move(bench.anim1);
  bench.anim1.assign(a.joint,a.joint+n);

  std::string name = "-";
  if(bone!=size_t(-1) && a.skeleton!=nullptr && bone<a.skeleton->nodes.size())
    name = a.skeleton->nodes[bone].name;
  for(auto& c:name)
    if(c==' ' || c==',')
      c = '_';
  std::fprintf(bench.csv,",%.2f,%s,%.0f,%s",pop,name.c_str(),speed,a.layers.empty() ? "-" : a.layers.c_str());
  std::fprintf(bench.csv,",%d,%u,%u,%.1f,%.3f",int(AnimBlend::isEnabled()),a.blend.active,a.blend.started,a.blend.angle,a.blend.time);
  }

}

namespace testbench_detail {

void writeHeartbeat(const World& world) {
  if(envStr("OPENGOTHIC_BENCH_OUT")==nullptr)
    return;
  const auto now = std::chrono::steady_clock::now();
  if(now<bench.heartbeatNext)
    return;
  bench.heartbeatNext = now+std::chrono::seconds(10);
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now-bench.heartbeatStart).count();
  const auto command = std::min(std::max(bench.pc,size_t(1)),bench.prog.size());
  char gameTime[80];
  std::snprintf(gameTime,sizeof(gameTime),"day %lld %02lld:%02lld",(long long)world.time().day(),
                (long long)world.time().hour(),(long long)world.time().minute());
  const auto path = outDir+"/heartbeat.json";
  std::ofstream file(path+".tmp");
  file << "{\"elapsed_seconds\":" << elapsed << ",\"frame\":" << bench.step
       << ",\"command_index\":" << command << ",\"command_count\":" << bench.prog.size()
       << ",\"command\":" << jsonStr(command>0 ? bench.prog[command-1] : "")
       << ",\"game_time\":" << jsonStr(gameTime);
  if(NpcDayJournal::running())
    file << ",\"npcday_elapsed_minutes\":" << double(world.time().toInt()-bench.npcCommands.npcDayStart)/60000.0
         << ",\"npcday_target_minutes\":" << bench.npcCommands.npcDayMinutes;
  file << "}\n";
  file.close();
  if(file) {
    std::error_code error;
    std::filesystem::rename(path+".tmp",path,error);
    }
  }

void finish() {
  if(auto* world=Gothic::inst().world())
    NpcDayJournal::stop(*world,"bench_end");
  stopBodyLive();
  npcCharacterStopMotionTrace(bench.character,Gothic::inst().world());
  endAnimationControl();
  animationControlTotals();
  if(bench.speaking) {
    std::string reply;
    aiCommand("ai ptt cancel",reply);
    bench.speaking = false;
    }
  stopRecording();
  if(!bench.streamingSamples.empty()) {
    if(auto* file=std::fopen((outDir+"/streaming.csv").c_str(),"w")) {
      std::fprintf(file,"segment,frame,update_ms,work_ms,near,far,outside,near_transitions,far_transitions,publication_ms,published,released,page_used_bytes,page_free_bytes,page_retired_bytes,page_count,tex_live_bytes,tex_retired_bytes,tex_peak_bytes,tex_live_n,tex_evicted,tex_reloaded,tex_landscape_bytes,tex_landscape_n,tex_static_bytes,tex_static_n,tex_dynamic_bytes,tex_dynamic_n,tex_other_bytes,tex_other_n,tex_full_bytes,tex_tail_bytes,tex_promoted,tex_demoted,tex_promotion_queue,tex_near_tail_n,tex_near_tail_seconds,tex_level0_bytes,tex_level1_bytes,tex_level2_bytes,tex_level3_bytes,tex_level4_bytes,tex_level5_bytes,tex_shortfall_n,tex_shortfall_seconds,tex_jumps,tex_jumps_open,tex_jump_sync_max_ms,tex_jump_zero_max_ms,vob_near,vob_far,vob_outside,vob_created,vob_released,vob_pending,vob_publication_ms,vob_near_missing,vob_near_missing_seconds,npc_full,npc_simplified,npc_tier_transitions,npc_full_tick_ms,npc_simplified_tick_ms\n");
      for(const auto& row:bench.streamingSamples) {
        std::fprintf(file,"%s,%llu,%.9g,%.9g,%zu,%zu,%zu,%llu,%llu,",row.segment.c_str(),
          (unsigned long long)row.frame,row.updateMs,row.workMs,row.near,row.far,row.outside,
          (unsigned long long)row.nearTransitions,(unsigned long long)row.farTransitions);
        for(size_t i=0;i<row.publicationTimesMs.size();++i)
          std::fprintf(file,"%s%.9g",i==0 ? "" : ";",row.publicationTimesMs[i]);
        std::fprintf(file,",%llu,%llu,%zu,%zu,%zu,%zu",(unsigned long long)row.published,(unsigned long long)row.released,
          row.pageUsedBytes,row.pageFreeBytes,row.pageRetiredBytes,row.pageCount);
        const auto& textures = row.textures;
        std::fprintf(file,",%llu,%llu,%llu,%llu,%llu,%llu",
          (unsigned long long)textures.liveBytes,(unsigned long long)textures.retiredBytes,
          (unsigned long long)textures.peakBytes,(unsigned long long)textures.count,
          (unsigned long long)textures.evicted,(unsigned long long)textures.reloaded);
        for(size_t i=0;i<textures.categoryBytes.size();++i)
          std::fprintf(file,",%llu,%llu",(unsigned long long)textures.categoryBytes[i],
            (unsigned long long)textures.categoryCount[i]);
        std::fprintf(file,",%llu,%llu,%llu,%llu,%llu,%llu,%.9g",
          (unsigned long long)textures.fullBytes,(unsigned long long)textures.tailBytes,
          (unsigned long long)textures.promoted,(unsigned long long)textures.demoted,
          (unsigned long long)textures.promotionQueue,(unsigned long long)textures.nearTailCount,textures.nearTailSeconds);
        for(const auto bytes:textures.levelBytes)
          std::fprintf(file,",%llu",(unsigned long long)bytes);
        std::fprintf(file,",%llu,%.9g",(unsigned long long)textures.shortfallCount,textures.shortfallSeconds);
        std::fprintf(file,",%llu,%llu,%.9g,%.9g",(unsigned long long)textures.jumps,(unsigned long long)textures.jumpsOpen,
          textures.jumpSyncMsMax,textures.jumpZeroMsMax);
        std::fprintf(file,",%zu,%zu,%zu,%llu,%llu,%zu,%.9g,%zu,%.9g",
          row.vobNear,row.vobFar,row.vobOutside,(unsigned long long)row.vobCreated,
          (unsigned long long)row.vobReleased,row.vobPending,row.vobPublicationMs,
          row.vobNearMissing,row.vobNearMissingSeconds);
        std::fprintf(file,",%zu,%zu,%llu,%.9g,%.9g\n",row.npcFull,row.npcSimplified,
          (unsigned long long)row.npcTierTransitions,row.npcFullTickMs,row.npcSimplifiedTickMs);
        }
      std::fclose(file);
      } else event("streaming_error","\"reason\":\"cannot write residency samples\"");
    }
  if(!bench.character.characterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,frame,t_game_ms,dt_ms,x_cm,y_cm,z_cm,move_held,in_air,falling,swimming,cv_enabled,cv_active,ground_vx_cm_s,ground_vy_cm_s,ground_vz_cm_s,step_ms,wall_ms,captured,normal_clock,visual_x_cm,visual_y_cm,visual_z_cm\n");
      for(const auto& row:bench.character.characterFrames)
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.9g,%.9g,%.9g\n",
          row.route.c_str(),(unsigned long long)row.frame,(unsigned long long)row.time,(unsigned long long)row.dt,
          double(row.root.x),double(row.root.y),double(row.root.z),int(row.moving),int(row.air),int(row.fall),int(row.swim),
          int(row.enabled),int(row.active),double(row.groundVelocity.x),double(row.groundVelocity.y),double(row.groundVelocity.z),
          double(row.physics),double(row.wall),int(row.captured),int(row.normal),
          double(row.visualRoot.x),double(row.visualRoot.y),double(row.visualRoot.z));
      std::fclose(file);
      } else event("character_error","\"reason\":\"cannot write native character frames\"");
    }
  if(!bench.character.npcCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/npc-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"phase,frame,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,interactive,step_ms,wall_ms,nearby_humans,nearby_cv,moving_humans,moving_cv,captured,normal_clock%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.character.npcCharacterFrames) {
        std::fprintf(file,"%s,%llu,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%.9g,%.9g,%u,%u,%u,%u,%d,%d",
          row.phase.c_str(),(unsigned long long)row.frame,(unsigned long long)row.time,(unsigned long long)row.dt,
          (unsigned long long)row.npc,double(row.root.x),double(row.root.y),double(row.root.z),
          double(row.visual.x),double(row.visual.y),double(row.visual.z),int(row.enabled),int(row.active),int(row.moving),
          int(row.air),int(row.interactive),double(row.physics),double(row.wall),row.nearby,row.virtualNearby,row.movingNearby,row.movingVirtualNearby,int(row.captured),int(row.normal));
        writeCharacterMoveFrame(file,row.move);
        }
      std::fclose(file);
      } else event("npc_character_error","\"reason\":\"cannot write native NPC character frames\"");
    }
  if(!bench.monster.monsterCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,falling,swimming,animation,body_yaw_deg%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.monster.monsterCharacterFrames) {
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,\"%s\",%.9g",
          row.route.c_str(),(unsigned long long)row.time,(unsigned long long)row.dt,(unsigned long long)row.npc,
          double(row.root.x),double(row.root.y),double(row.root.z),double(row.visual.x),double(row.visual.y),double(row.visual.z),
          int(row.enabled),int(row.active),int(row.moving),int(row.air),int(row.fall),int(row.swim),row.animation.c_str(),double(row.bodyYaw));
        writeCharacterMoveFrame(file,row.move);
        }
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster frames\"");
    }
  if(!bench.monster.monsterCharacterPerformance.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-performance.csv").c_str(),"w")) {
      std::fprintf(file,"t_game_ms,dt_ms,step_ms,wall_ms,nearby_monsters,nearby_cv,moving,maximum_distance_cm,captured,normal_clock\n");
      for(const auto& row:bench.monster.monsterCharacterPerformance)
        std::fprintf(file,"%llu,%llu,%.9g,%.9g,%u,%u,%u,%.9g,%d,%d\n",
          (unsigned long long)row.time,(unsigned long long)row.dt,double(row.physics),double(row.wall),row.nearby,
          row.virtualNearby,row.moving,double(row.maximumDistance),int(row.captured),int(row.normal));
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster performance samples\"");
    }
  bench.monster.monsterCharacterProps.clear();
  bench.monster.monsterCharacterLedgeBody=DynamicWorld::Item();bench.monster.monsterCharacterLedgeShape.reset();
  bench.character.characterProp.reset();bench.character.characterPlatform.reset();bench.character.characterPlatformVisual.reset();
  bench.destruction.destructionFixtures.clear();
  bench.furnitureParts.loadBodies.clear();
  if(bench.npcDuel.active) {
    Gothic::inst().onCombatHit.ubind(&bench.npcDuel,&Bench::NpcDuel::hit);
    bench.npcDuel.active=false;
    }
  if(bench.bodyImpact.observing) {
    Gothic::inst().onHeroDeed.ubind(&bench.bodyImpact,&Bench::BodyImpact::deed);
    Gothic::inst().onObservedPerception.ubind(&bench.bodyImpact,&Bench::BodyImpact::perception);
    Gothic::inst().onExternalPerception.ubind(&bench.bodyImpact,&Bench::BodyImpact::perception);
    bench.bodyImpact.observing=false;
    }
  if(bench.placedItems.watchingTheft) {
    Gothic::inst().onHeroDeed.ubind(&bench.placedItems,&Bench::PlacedItems::deed);
    Gothic::inst().onObservedPerception.ubind(&bench.placedItems,&Bench::PlacedItems::perception);
    bench.placedItems.watchingTheft=false;
  }
  bench.movables.movableFixtures.clear();
  if(ragdollPerf) {
    std::FILE* perf=std::fopen((outDir+"/ragdoll-performance.csv").c_str(),"w");
    if(perf!=nullptr) {
      std::fprintf(perf,"seg,frame,wall_ms,step_ms,world_bodies,rays,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_mesh_proxies\n");
      for(const auto& sample:bench.ragdoll.ragdollPerformance)
        std::fprintf(perf,"%s,%llu,%.9g,%.9g,%zu,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",sample.segment.c_str(),
                     (unsigned long long)sample.frame,double(sample.wallMs),double(sample.stepMs),sample.worldBodies,
                     (unsigned long long)sample.rays,sample.actors,sample.corpses,sample.physical,sample.sleeping,sample.parts,sample.centersInView,
                     sample.physicalBeforeImpulse,sample.sleepingBeforeImpulse,sample.visibleMeshes);
      std::fclose(perf);
      } else {
      event("ragdoll_error","\"reason\":\"cannot write deferred performance samples\"");
      }
    }
  if(!bench.ragdoll.ragdollCoverage.empty()) {
    if(auto* file=std::fopen((outDir+"/ragdoll-coverage.csv").c_str(),"w")) {
      std::fprintf(file,"mode,frame,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_meshes\n");
      for(const auto& sample:bench.ragdoll.ragdollCoverage)
        std::fprintf(file,"%s,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",sample.segment.c_str(),
                     (unsigned long long)sample.frame,sample.actors,sample.corpses,sample.physical,sample.sleeping,
                     sample.parts,sample.centersInView,sample.physicalBeforeImpulse,sample.sleepingBeforeImpulse,sample.visibleMeshes);
      std::fclose(file);
      }
    }
  if(bench.csv!=nullptr)
    std::fclose(bench.csv);
  bench.csv  = nullptr;
  if(bench.events!=nullptr)
    std::fclose(bench.events);
  bench.events = nullptr;
  if(realtime)
    say("realtime: " + std::string(bench.lagMs>200 ? "fell behind the wall clock by " + std::to_string(bench.lagMs) + " ms"
                                                   : "kept to the wall clock"));
  bench.done = true;
  DrawCommands::hiddenTypes    = 0;
  DrawCommands::hidePfxObjects = false;
  if(auto c = Gothic::inst().camera()) {
    c->setMarvinMode(Camera::M_Normal);
    c->setNearClip(10);
    }
  say("done");
  if(!keepRunning && !Resources::isHeadless())
    SystemApi::exit();
  }

std::string ragdollNumber(float value) {
  if(!std::isfinite(value))
    return "null"; // Valid JSON, with an explicit invalid measurement for the strict reader.
  char buffer[40];
  std::snprintf(buffer,sizeof(buffer),"%.9g",double(value));
  return buffer;
  }

std::string ragdollVector(const Vec3& value) {
  return "["+ragdollNumber(value.x)+","+ragdollNumber(value.y)+","+ragdollNumber(value.z)+"]";
  }

std::string ragdollMatrix(const Matrix4x4& value) {
  std::string result="[";
  for(size_t i=0;i<16;++i) {
    if(i>0) result+=',';
    result+=ragdollNumber(value.data()[i]);
    }
  return result+"]";
  }

void stunDriftSample(World& world, uint64_t elapsed, uint64_t requested) {
  const auto& drift=bench.npcCommands.stunDrift;
  const auto& target=*drift.target;
  const auto delta=target.position()-drift.origin;
  const auto separation=drift.attacker->position()-target.position();
  event("stun_drift","\"actor\":"+jsonStr(symName(world,&target))+
    ",\"attacker\":"+jsonStr(symName(world,drift.attacker))+",\"elapsed_ms\":"+std::to_string(elapsed)+
    ",\"requested_ms\":"+std::to_string(requested)+",\"position_cm\":"+ragdollVector(target.position())+
    ",\"attacker_position_cm\":"+ragdollVector(drift.attacker->position())+
    ",\"axis\":"+ragdollVector(drift.axis)+",\"toward_cm\":"+ragdollNumber(Vec3::dotProduct(delta,drift.axis))+
    ",\"horizontal_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+",\"height_cm\":"+ragdollNumber(delta.y)+
    ",\"separation_cm\":"+ragdollNumber(std::hypot(separation.x,separation.z))+
    ",\"stunned\":"+(target.isMeleeStunned() ? "true" : "false")+
    ",\"slide\":"+(target.isSlide() ? "true" : "false")+
    ",\"in_air\":"+(target.isInAir() ? "true" : "false")+
    ",\"melee_stun_beat\":"+(target.meleeStunSecondBeat() ? "true" : "false")+
    ",\"external\":"+(target.isExternalControl() ? "true" : "false")+
    ",\"animation\":"+jsonStr(target.animNames()));
  }

void spellFields(World& world) {
  if(!bench.movables.spellTrace || bench.movables.spellSampleTick==world.tickCount()) return;
  bench.movables.spellSampleTick=world.tickCount();
  std::string actors="[",items="[";
  for(size_t i=0;i<bench.movables.spellActors.size();++i) {
    auto& actor=bench.movables.spellActors[i];auto& npc=*actor.npc;
    const auto* living=npc.livingBody();const auto* corpse=npc.ragdollBody();
    const auto* body=living?living:corpse;
    std::vector<Matrix4x4> native;
    const bool nativePose=body && body->getPose(native);
    if(i==0 && bench.movables.spellCase.starts_with("ice")) {
      if(npc.spellFrozen()) {npc.setExternalControl(true);bench.movables.spellWasFrozen=true;}
      else if(bench.movables.spellWasFrozen) {npc.setExternalControl(false);bench.movables.spellWasFrozen=false;}
      }
    if(i) actors+=',';
    actors+="{\"index\":"+std::to_string(i)+",\"role\":"+jsonStr(actor.role)+",\"mass_kg\":"+ragdollNumber(npc.physicsMass())+
      ",\"position_cm\":"+ragdollVector(npc.position())+",\"spell_velocity_cm_s\":"+ragdollVector(npc.spellVelocity())+
      ",\"state\":"+jsonStr(stateName(world,npc))+",\"state_time\":"+std::to_string(npc.stateTime())+
      ",\"frozen\":"+(npc.spellFrozen()?"true":"false")+",\"whirling\":"+(npc.spellWhirling()?"true":"false")+
      ",\"in_air\":"+(npc.isInAir()?"true":"false")+
      ",\"dead\":"+(npc.isDead()?"true":"false")+",\"living_parts\":"+std::to_string(living?living->partCount():0)+
      ",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
      ",\"kinematic\":"+(body && body->isKinematic()?"true":"false")+
      ",\"sleeping\":"+(body && body->isSleeping()?"true":"false")+",\"bones\":[";
    const auto* sk=npc.visualSkeleton();
    if(sk)
      for(std::string_view name:{"BIP01 PELVIS","BIP01 SPINE2","BIP01 L UPPERARM","BIP01 L CALF"}) {
        const auto bone=arrowBone(*sk,name);if(bone==size_t(-1)) continue;
        if(actors.back()!='[') actors+=',';
        actors+="{\"id\":"+std::to_string(bone)+",\"render\":"+ragdollMatrix(npc.renderedBone(bone));
        if(nativePose && bone<native.size()) actors+=",\"native\":"+ragdollMatrix(native[bone]);
        actors+='}';
        }
    actors+="]}";
    }
  for(size_t i=0;i<bench.movables.spellItems.size();++i) {
    const auto& item=*bench.movables.spellItems[i];if(i) items+=',';
    items+="{\"index\":"+std::to_string(i)+",\"position_cm\":"+ragdollVector(item.position())+
      ",\"mass_kg\":"+ragdollNumber(item.physicsMass())+",\"velocity_cm_s\":"+ragdollVector(item.physicsVelocity())+
      ",\"sleeping\":"+(item.physicsSleeping()?"true":"false")+",\"dynamic\":"+(item.isDynamic()?"true":"false")+"}";
    }
  if(bench.movables.spellGuardTrace && !bench.movables.spellActors.empty()) {
    const auto& npc=*bench.movables.spellActors[0].npc;
    const auto& q=npc.ragdollQuestReturnState();
    const auto returns=npc.ragdollQuestReturnCount();
    event("spell_guard_frame","\"position_cm\":"+ragdollVector(npc.position())+
      ",\"case\":"+jsonStr(bench.movables.spellGuardCase)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"return_from_cm\":"+ragdollVector(npc.ragdollQuestReturnFrom())+
      ",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
      ",\"human\":"+(npc.isHuman()?"true":"false")+",\"hero\":"+(npc.isPlayer()?"true":"false")+
      ",\"immortal\":"+(npc.isImmortal()?"true":"false")+
      ",\"in_air\":"+(npc.isInAir()?"true":"false")+",\"falling\":"+(npc.isFalling()?"true":"false")+
      ",\"standing\":"+(npc.isStanding()?"true":"false")+
      ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
      ",\"hazards\":"+std::to_string(q.hazards)+",\"returns\":"+std::to_string(returns));
    if(returns>bench.movables.spellGuardObservedReturns) {
      const auto from=npc.ragdollQuestReturnFrom();
      const auto floor=world.physic()->landRay(from+Vec3(0,200,0),6000);
      const auto water=world.physic()->waterRay(from,200.f);
      event("spell_guard_landing","\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(npc.position())+
        ",\"case\":"+jsonStr(bench.movables.spellGuardCase)+",\"hazards\":"+std::to_string(q.hazards)+
        ",\"has_water\":"+(water.hasCol?"true":"false")+
        ",\"water_y_cm\":"+ragdollNumber(water.wdepth)+
        ",\"water_depth_cm\":"+ragdollNumber(water.wdepth-from.y)+
        ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"returns\":"+std::to_string(returns)+
        ",\"has_floor\":"+(floor.hasCol?"true":"false")+
        ",\"floor_cm\":"+(floor.hasCol?ragdollVector(floor.v):"null")+
        ",\"floor_normal\":"+(floor.hasCol?ragdollVector(floor.n):"null"));
      bench.movables.spellGuardObservedReturns=returns;
      }
    }
  const auto stats=world.physic()->physicsStats();
  event("spell_physics_frame","\"case\":"+jsonStr(bench.movables.spellCase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","spell_physics")?"true":"false")+
    ",\"fields\":"+std::to_string(world.spellFieldCount())+",\"world_bodies\":"+std::to_string(stats.bodies)+
    ",\"physics_ms\":"+ragdollNumber(stats.stepMilliseconds)+",\"actors\":"+actors+"],\"items\":"+items+"]");
  }

void writeRow(Npc& pl) {
  if(bench.csv==nullptr) {
    bench.csv = std::fopen((outDir+"/metrics.csv").c_str(),"w");
    if(bench.csv==nullptr)
      return;
    writeHeader();
    }
  auto&      p   = FootIk::playerProbe;
  const Vec3 pos = pl.position();
  std::fprintf(bench.csv,"%s,%llu,%llu,%.2f,%.2f,%.2f,%.1f,%.1f,%u,%u,%u,%d,%d,%d,%.3f,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f",
               bench.recName.c_str(),(unsigned long long)bench.recFrame,(unsigned long long)(bench.time-bench.recStart),
               pos.x,pos.y,pos.z,pl.rotation(),bench.speed,unsigned(pl.bodyStateMasked()),unsigned(pl.walkMode()),unsigned(bench.occl),
               int(p.valid),int(p.enable),int(p.hardOff),p.weight,unsigned(p.numLegs),unsigned(p.badBones),p.pelvis,p.chest,p.target,p.limit,p.floorY);
  for(auto& l:p.leg) {
    std::fprintf(bench.csv,",%.3f,%d,%.2f,%.3f,%.3f,%.3f,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f",
                 l.plant,int(l.hasCol),l.groundY,l.n.x,l.n.y,l.n.z,int(l.toeCol),l.toeY,l.smoothY,l.err,l.kneeDev,l.tilt,l.roll,l.yaw,l.outward,l.slack);
    const Vec3* pts[] = {&l.final[0],&l.anim[1],&l.anim[2],&l.anim[3],&l.final[1],&l.final[2],&l.final[3]};
    for(auto v:pts)
      std::fprintf(bench.csv,",%.2f,%.2f,%.2f",v->x,v->y,v->z);
    }
  // gaze of the watched npc (or the hero) and of the hero
  const auto& g  = bench.subject!=nullptr ? Gaze::subjectProbe : Gaze::playerProbe;
  const auto& pg = Gaze::playerProbe;
  // where the face looks in the world: a jerk is a jump of this, whatever the body does
  const float fyaw   = std::atan2(g.facing.z,g.facing.x)*180.f/float(M_PI);
  const float fpitch = std::asin(std::clamp(g.facing.y,-1.f,1.f))*180.f/float(M_PI);
  std::fprintf(bench.csv,",%d,%d,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%.3f,%.2f,%.2f",
               int(g.active),int(g.source),g.weight,g.error,g.yaw,g.pitch,g.yawGoal,g.pitchGoal,g.headYaw,g.chestYaw,g.waistYaw,
               g.animYaw,g.animPitch,g.hipsYaw,fyaw,fpitch,int(pg.active),int(pg.source),pg.weight,pg.error,pg.yaw);
  // lean of the watched npc (or the hero)
  const auto& ln = bench.subject!=nullptr ? Lean::subjectProbe : Lean::playerProbe;
  std::fprintf(bench.csv,",%d,%.2f,%.2f,%.1f,%.1f,%.1f",int(ln.active),ln.roll,ln.pitch,ln.speed,ln.turn,ln.slope);
  writeAnim();
  // aim of the watched npc (or the hero)
  const auto& am = bench.subject!=nullptr ? Aim::subjectProbe : Aim::playerProbe;
  std::fprintf(bench.csv,",%d,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",int(am.active),am.weight,am.yaw,am.pitch,
               am.animYaw,am.animPitch,am.turn,am.bend,am.error);
  std::fprintf(bench.csv,",%d,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f",int(am.steady),am.lean,am.side,FootIk::playerProbe.crouch,
               double(pl.aimCrouch()),double(pl.aimPeek()),double(pl.aimCant()));
  // steps with the bow drawn
  const auto& aw = AimWalk::playerProbe;
  std::fprintf(bench.csv,",%.2f,%.2f,%.1f,%.1f,%.1f,%.1f",double(pl.aimWalk()),aw.weight,aw.speed,aw.twist,aw.chest,aw.drop);
  // the manual aim: the bow's stage, the sway, how far the arrow points off the aim
  const auto& mp = Archer::playerProbe;
  // the crosshair: how far it is open, degrees; how far it moved on the screen since the last frame,
  // percent of the screen's height
  Vec3  cross;
  float spread = 0;
  auto  cam    = Gothic::inst().camera();
  bool  hasCross = cam!=nullptr && bench.ctrl!=nullptr && bench.ctrl->crosshair(cross,spread);
  float moved  = -1;
  if(hasCross) {
    // as the game draws it: the center of the screen, unless the bench aims elsewhere
    float x = cross.x, y = cross.y, z = cross.z, w = 1;
    cam->viewProj().project(x,y,z,w);
    if(bench.ctrl->isCrosshairCentered()) {
      x = 0;
      y = 0;
      w = 1;
      }
    hasCross = w>1e-3f;
    const Vec3 at = hasCross ? Vec3(x/w,y/w,0) : Vec3();
    if(hasCross && bench.crossValid)
      moved = std::hypot((at.x-bench.crossPrev.x)*bench.aspect,at.y-bench.crossPrev.y)*50.f;
    bench.crossPrev = at;
    }
  bench.crossValid = hasCross;
  std::fprintf(bench.csv,",%d,%.3f,%.2f,%.3f,%.2f,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",mp.valid ? int(mp.stage) : -1,mp.draw,mp.hold,
               mp.fatigue,mp.breath,int(mp.holding),int(mp.short_),mp.disturb,mp.amp,mp.yaw,mp.pitch,manualAimOff(pl),
               hasCross ? spread*180.f/float(M_PI) : -1.f,moved);
  // the hand drawing the string
  const auto& bd = BowDraw::playerProbe;
  std::fprintf(bench.csv,",%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",bd.valid ? bd.weight : -1.f,bd.draw,bd.span,bd.travel,bd.miss,
               bd.follow,bd.fit,bd.offset,bd.angle);
  std::fprintf(bench.csv,",%.1f,%.1f,%.1f,%d,%.2f",bd.head,bd.fore,bd.arrow,int(bd.holding),bd.nock);
  std::fprintf(bench.csv,",%.2f,%.2f,%.2f",bd.handAt.x,bd.handAt.y,bd.handAt.z);
  std::fprintf(bench.csv,",%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",bd.aligned,bd.overGrip,bd.atMiddle,bd.nockGap,bd.square,bd.cant,bd.bowMove,bd.bowMiss);
  // the release: the drawing hand off where it let go, the bow hand thrown back, the bow turned in it
  std::fprintf(bench.csv,",%.2f,%.2f,%.2f",bd.back,bd.kick,bd.turn);
  // lips of the watched one
  const auto& lp = LipSync::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%llu,%.3f,%.3f,%.3f,%u,%u,%.2f,%.3f,%u",
               int(lp.valid),int(lp.active),(unsigned long long)lp.time,lp.level,lp.mouth,lp.open,
               lp.shape.frame0,lp.shape.frame1,lp.shape.alpha,lp.shape.intensity,lp.picks);
  // its hands while it speaks
  const auto& ge = TalkGestures::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%u,%u,%u,%u,%u,%u,%.2f,%.2f,%.2f",int(ge.engaged),int(ge.speaking),unsigned(ge.gesture),
               unsigned(ge.mood),ge.starts,ge.cuts,ge.stops,ge.nods,double(ge.head.pitch*180.f/float(M_PI)),
               double(ge.head.yaw*180.f/float(M_PI)),double(ge.head.roll*180.f/float(M_PI)));
  std::fprintf(bench.csv,",%d,%d,%u,%.3f",int(ge.listening),int(ge.thinking),ge.listenNods,ge.aside.weight);
  // its blinks: how closed the eyes are now, counters
  const auto& bk = EyeBlink::subjectProbe;
  std::fprintf(bench.csv,",%d,%.3f,%u,%u,%u,%u",int(bk.valid && bk.enabled),subjectOf(pl).faceWeight("R_EYESBLINK"),
               bk.blinks,bk.pauses,bk.shifts,bk.doubles);
  // faces of the watched one, as shown (fading in and out)
  Npc& s = subjectOf(pl);
  for(auto& f:FaceColumns)
    std::fprintf(bench.csv,",%.3f",s.faceWeight(f.anim));
  // the own head's face (FaceRig): active anims, targets with a weight, the last blend and upload, blends so far
  const auto& fr = FaceRig::subjectProbe;
  std::fprintf(bench.csv,",%d,%u,%u,%.1f,%u,%.1f",int(fr.valid),fr.anims,fr.targets,double(fr.blendUs),fr.blends,
               double(fr.uploadUs));
  // its eyes (Eyes): the fixation point's kind, degrees in the socket (left, right), from the point and beyond the
  // limits, in a saccade, saccades and the big ones so far; the point above the eyes (cm), below their level and
  // the head's forward above it (degrees, world)
  const auto& ey = Eyes::subjectProbe;
  std::fprintf(bench.csv,",%d,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%u,%u,%.1f,%.2f,%.2f",int(ey.valid),unsigned(ey.kind),
               double(ey.yaw[0]),double(ey.pitch[0]),double(ey.yaw[1]),double(ey.pitch[1]),double(ey.error),double(ey.over),
               int(ey.moving),ey.saccades,ey.big,double(ey.rise),double(ey.down),double(ey.head));
  // its gestures as shown, 0..1
  for(auto* c:CueColumns)
    std::fprintf(bench.csv,",%.3f",s.cueWeight(c));
  // and a leak: its strongest weight as shown
  std::fprintf(bench.csv,",%.3f",s.leakWeight());
  // the tone of its voice now (semitones from the line's median, nan: none) and its brows on the accents: as shown
  // (+ up, - down), accents of the line found, moves of the brows
  std::fprintf(bench.csv,",%.2f,%.3f,%u,%u",double(lp.tone),double(fr.brows),fr.accents,fr.browMoves);
  // flinch of the watched one (or the hero)
  const auto& fl = bench.subject!=nullptr ? Flinch::subjectProbe : Flinch::playerProbe;
  std::fprintf(bench.csv,",%d,%.2f,%.1f,%.2f,%.2f,%u,%.2f,%.2f",int(fl.active),fl.bend,fl.bendYaw,fl.twist,fl.head,
               fl.hits,fl.strength,fl.height);
  // hit points: the hero's, the watched one's (a fight's course)
  std::fprintf(bench.csv,",%d,%d",pl.attribute(ATR_HITPOINTS),bench.subject!=nullptr ? bench.subject->attribute(ATR_HITPOINTS) : -1);
  std::fprintf(bench.csv,",%.3f,%.3f",double(pl.stamina()),bench.subject!=nullptr ? double(bench.subject->stamina()) : -1.0);
  // the watched npc's archer by hand (game/archerai)
  const auto& ai = ArcherAi::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%.3f,%.3f,%.3f,%.0f,%.2f,%.2f",int(ai.valid && ai.active),int(ai.want),ai.sigma,ai.err.x,ai.err.y,
               ai.lead,ai.pv,ai.ps);
  {
    // the watched npc's motor reaction to a blow: its weight, how far the shown body leaves the animation (cm)
    const Npc&     s      = subjectOf(pl);
    const Ragdoll* living = s.livingBody();
    float          weight = 0, dev = 0;
    if(living!=nullptr && living->reactionWeight()>0.f) {
      weight = living->reactionWeight();
      for(const auto b:living->reactionBones()) {
        Vec3 shown, anim;
        s.renderedBone(b).project(shown);
        s.animationBone(b).project(anim);
        dev = std::max(dev,(shown-anim).length());
        }
      }
    std::fprintf(bench.csv,",%.3f,%.2f,%u,%.6f",weight,dev,unsigned(s.meleeReactionProbe().applied),
                 double(s.world().physic()->physicsStats().stepMilliseconds));
  }
  {
    const auto q = subjectOf(pl).quiverProbe();
    std::fprintf(bench.csv,",%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f",int(q.on),q.arrows,q.armor,q.leg,
                 q.arm,q.mouth.x,q.mouth.y,q.mouth.z,q.out,q.top,q.bottom,q.swing,q.back,q.outward,
                 double(subjectOf(pl).quiverCostUs()));
    std::fprintf(bench.csv,",%.2f,%.2f,%.2f,%.2f,%.2f,%.1f",q.twist,q.bow,q.longsword,q.blade,q.yield,q.legAt);
    std::fprintf(bench.csv,",%.2f",q.legLeft);
    // cm the tube and its mouth go under the world (its collision, a ray down over each point of the outer side)
    float gnd = 0, gndMouth = 0, gndTube = 0;
    Quiver::measureGround = true;
    if(q.on) {
      constexpr int Rings = 9, Around = 12;
      const auto pts = subjectOf(pl).quiverSurface(Rings,Around);
      auto* phys = Gothic::inst().world()->physic();
      for(size_t i=0; i<pts.size(); ++i) {
        const auto& v = pts[i];
        const auto  r = phys->rayStatic({v.x,v.y+150.f,v.z},{v.x,v.y,v.z});
        if(!r.hasCol || r.v.y<=v.y)
          continue;
        gnd = std::max(gnd,r.v.y-v.y);
        if(i>=size_t(2*Around))
          gndTube = std::max(gndTube,r.v.y-v.y); // from the third ring, 16.75 cm down, on
        if(i<size_t(Around))
          gndMouth = std::max(gndMouth,r.v.y-v.y);
        }
      }
    // cm the bottom's centre is over the ground under it (NAN: no ground under it)
    float botUp = NAN;
    if(q.on) {
      const auto pts = subjectOf(pl).quiverSurface(9,12);
      if(pts.size()==size_t(9*12)) {
        Vec3 c;
        for(size_t i=8*12; i<pts.size(); ++i)
          c += pts[i];
        c = c/12.f;
        const auto r = Gothic::inst().world()->physic()->rayStatic({c.x,c.y+150.f,c.z},{c.x,c.y-300.f,c.z});
        if(r.hasCol)
          botUp = c.y-r.v.y;
        }
      }
    std::fprintf(bench.csv,",%.2f,%.2f,%d,%d",gnd,gndMouth,q.ground,int(q.groundOn));
    std::fprintf(bench.csv,",%.2f",botUp);
    std::fprintf(bench.csv,",%.2f,%.1f,%.1f,%.1f,%.2f",gndTube,q.needAngle,q.needBack,q.needOut,q.needBody);
    const Npc& qs = subjectOf(pl);
    std::fprintf(bench.csv,",%.2f,%.1f,%d,%.1f,%.1f",q.trunk,double(qs.quiverClearance()),qs.quiverRays(),
                 double(qs.quiverRayUs()),double(qs.quiverCpuUs()));
    std::fprintf(bench.csv,",%.5f,%.5f,%.5f",q.axis.x,q.axis.y,q.axis.z);
  }
  if(ragdollTrace)
    writeRagdoll(pl);
  if(bench.routine.questTrace && bench.routine.questNpc!=nullptr)
    questFields(*Gothic::inst().world(),*bench.routine.questNpc,true);
  if(bench.living.livingFixture)
    livingFields(*Gothic::inst().world());
  livingSeatFields(*Gothic::inst().world());
  std::fprintf(bench.csv,"\n");
  }

}

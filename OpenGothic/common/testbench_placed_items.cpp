#include "testbench_internal.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <tuple>

#include "game/archery.h"
#include "game/gamescript.h"
#include "game/herodeed.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "gothic.h"
#include "graphics/mesh/protomesh.h"
#include "resources.h"
#include "world/bullet.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void PlacedItems::deed(const HeroDeed& value) {
  if(value.kind!="take_item")return;
  ++theftDeeds;theftOwner=value.owner.npc;theftOwnerGuild=value.ownerGuild;theftItem=value.item;
  theftRoom=value.room;theftRoomOwner=value.roomOwner.npc;theftRoomGuild=value.roomGuild;
  for(const auto& witness:value.witnesses)theftWitnesses.push_back(witness.npc);
}

void PlacedItems::perception(Npc& self,Npc&,Npc*,PercType value) {
  if(value==PERC_ASSESSTHEFT)theftRecipients.push_back(self.handle().symbol_index());
}

namespace {

std::tuple<Vec3,size_t,size_t> placedItemsDensest(World& world) {
  Vec3 center;size_t maximum=0,eligible=0;
  const auto& all=world.placedItemCensus();
  for(const auto& candidate:all) {
    const auto at=movablePosition(candidate.origin);
    size_t count=0,admitted=0;
    for(const auto& other:all)if((movablePosition(other.origin)-at).quadLength()<=2000.f*2000.f) {
      ++count;admitted+=other.validation.enabled;
    }
    if(count>maximum || (count==maximum && admitted>eligible)) {maximum=count;eligible=admitted;center=at;}
  }
  return {center,maximum,eligible};
}

std::string placedItemRow(World& world,const WorldObjects::PlacedItem& row) {
  const auto& validation=row.validation;
  return "\"world\":"+jsonStr(world.name())+",\"zen_id\":"+std::to_string(row.zenId)+
    ",\"instance\":"+std::to_string(row.instance)+",\"visual\":"+jsonStr(row.visual)+
    ",\"origin_cm\":"+ragdollVector(movablePosition(row.origin))+",\"origin_transform\":"+ragdollMatrix(row.origin)+
    ",\"disposition\":"+jsonStr(validation.disposition)+",\"enabled\":"+(validation.enabled?"true":"false")+
    ",\"support_gap_cm\":"+(std::isfinite(validation.supportGapCm)?ragdollNumber(validation.supportGapCm):"null")+
    ",\"static_depth_cm\":"+ragdollNumber(validation.staticDepthCm)+
    ",\"furniture_depth_cm\":"+ragdollNumber(validation.furnitureDepthCm)+
    ",\"item_depth_cm\":"+ragdollNumber(validation.itemDepthCm)+
    ",\"support_category\":"+std::to_string(validation.supportCategory)+",\"support_label\":"+jsonStr(validation.supportLabel)+
    ",\"support_normal\":"+ragdollVector(validation.supportNormal)+
    ",\"hull_low_point_cm\":"+ragdollVector(validation.hullLowPointCm)+
    ",\"support_face_normal\":"+ragdollVector(validation.supportFaceNormal)+
    ",\"support_slope_deg\":"+(std::isfinite(validation.supportSlopeDeg)?ragdollNumber(validation.supportSlopeDeg):"null")+
    ",\"support_zen_id\":"+
    (validation.supportZenId==uint32_t(-1)?"null":std::to_string(validation.supportZenId))+
    ",\"support_item_index\":"+(validation.supportItemIndex==size_t(-1)?"null":std::to_string(validation.supportItemIndex));
}

}

// Body-level equivalence of the census cache paths (WorldObjects::physicsDigest).
// Lines are compared byte for byte between runs.
static void placedItemsDigest(World& world,std::string_view phase) {
  const auto digest=world.physicsDigest();
  const std::string row="\"phase\":"+jsonStr(phase)+",\"world\":"+jsonStr(world.name())+
    ",\"bodies\":"+std::to_string(digest.native.bodies)+",\"active\":"+std::to_string(digest.native.active)+
    ",\"bodies_sha256\":"+jsonStr(digest.native.digest)+",\"movables\":"+std::to_string(digest.native.movables)+
    ",\"owners_sha256\":"+jsonStr(digest.native.owners)+",\"item_bodies\":"+std::to_string(digest.itemBodies)+
    ",\"items_sha256\":"+jsonStr(digest.items);
  std::ofstream(outDir+"/placed-items-digest.jsonl",std::ios::app)<<'{'+row+"}\n";
  event("placed_items_digest",row);
}

void placedItemsCensus(World& world) {
  std::ofstream file(outDir+"/placed-items-census.jsonl");
  if(!file) {arenaAbort("cannot write placed Item census");return;}
  size_t eligible=0,gaps[3]{};std::map<std::string,size_t> reasons;
  for(const auto& row:world.placedItemCensus()) {
    file<<'{'+placedItemRow(world,row)+"}\n";
    ++reasons[row.validation.disposition];eligible+=row.validation.enabled;
    if(row.validation.enabled) {
      const auto gap=std::max(0.f,row.validation.supportGapCm);
      ++gaps[gap<1.f?0:gap<2.f?1:2];
    }
  }
  std::string counts="{";
  for(const auto& [reason,count]:reasons) {if(counts.size()>1)counts+=',';counts+=jsonStr(reason)+':'+std::to_string(count);}
  const auto [center,total,count]=placedItemsDensest(world);
  event("placed_items_census","\"world\":"+jsonStr(world.name())+",\"total\":"+
    std::to_string(world.placedItemCensus().size())+",\"eligible\":"+std::to_string(eligible)+
    ",\"classification_ms\":"+ragdollNumber(float(world.placedItemValidationMilliseconds()))+
    ",\"reason_counts\":"+counts+"},\"gaps_0_1\":"+std::to_string(gaps[0])+
    ",\"gaps_1_2\":"+std::to_string(gaps[1])+",\"gaps_2_4\":"+std::to_string(gaps[2])+
    ",\"densest_center_cm\":"+ragdollVector(center)+",\"densest_total_within_20m\":"+std::to_string(total)+
    ",\"densest_eligible_within_20m\":"+std::to_string(count));
  placedItemsDigest(world,"census");
}

namespace {

void placedItemsNextBatch(PlacedItems& state,World& world,Npc& player) {
  const auto& all=world.placedItemCensus();
  const auto first=movablePosition(all[state.batches[state.batch].front()].origin);
  // A 12 m cube stays inside the 20 m observer radius. Hold the observer above
  // it throughout the probe so the hero cannot be the source of an impulse.
  state.observer=Vec3((std::floor(first.x/1200.f)+.5f)*1200.f,
    (std::floor(first.y/1200.f)+.5f)*1200.f+900.f,(std::floor(first.z/1200.f)+.5f)*1200.f);
  player.clearAiQueue();player.stopWalking();player.clearSpeed();
  player.setPosition(state.observer);player.updateTransform();
  state.probeNeighbours.clear();
  for(size_t i=0;i<all.size();++i) {
    const auto& row=all[i];
    if(!row.validation.enabled || !row.item)continue;
    if((movablePosition(row.origin)-state.observer).quadLength()<=2200.f*2200.f) {
      // Install every admitted neighbouring support before waking this cell.
      // The product's four-body frame budget is measured by the separate
      // performance bench and must not remove a support from this probe.
      row.item->setPhysicsDisable();row.item->setObjMatrix(row.origin);
      row.item->wakePhysics();
      if(!row.item->isDynamic() || !row.item->physicsSleeping()) {
        arenaAbort("placed Item probe cannot install a sleeping neighbouring hull");state.probing=false;return;
      }
      state.probeNeighbours.push_back(i);
    }
    else if(row.item->isDynamic() && row.item->physicsSleeping())row.item->setPhysicsDisable();
  }
  for(const auto index:state.batches[state.batch]) {
    const auto& row=all[index];
    if(!row.item) {arenaAbort("placed Item probe lost an authored Item");state.probing=false;return;}
    if(!row.item->activatePhysics()) {arenaAbort("placed Item probe cannot activate an admitted hull");state.probing=false;return;}
  }
  state.started=world.tickCount();state.startedStep=world.physic()->simulationStep();
  event("placed_items_probe_batch","\"batch\":"+std::to_string(state.batch)+",\"items\":"+
    std::to_string(state.batches[state.batch].size())+",\"neighbour_bodies\":"+std::to_string(state.probeNeighbours.size())+
    ",\"observer_cm\":"+ragdollVector(state.observer));
}

}

void placedItemsProbe(PlacedItems& state,World& world,Npc& player) {
  state.batches.clear();state.batch=state.probed=state.under5=0;state.maximumShift=0;
  const auto& all=world.placedItemCensus();
  state.maximumTravel.assign(all.size(),0.f);state.npcTouched.assign(all.size(),false);
  // Keep authored actors stationary: this probe measures wake settling without
  // a routine-walking NPC or the observer supplying the first impulse.
  for(uint32_t i=0;i<world.npcCount();++i)if(auto* npc=world.npcById(i);npc && npc!=&player) {
    npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();npc->clearSpeed();npc->setExternalControl(true);
  }
  std::map<std::tuple<int,int,int>,std::vector<size_t>> cells;
  for(size_t i=0;i<all.size();++i)if(all[i].validation.enabled) {
    const auto at=movablePosition(all[i].origin);
    cells[{int(std::floor(at.x/1200.f)),int(std::floor(at.y/1200.f)),int(std::floor(at.z/1200.f))}].push_back(i);
  }
  for(auto& [key,indices]:cells)state.batches.push_back(std::move(indices));
  if(state.batches.empty()) {arenaAbort("placed Item probe has no admitted candidates");return;}
  std::ofstream(outDir+"/placed-items-probe.jsonl");
  state.probing=true;placedItemsNextBatch(state,world,player);
}

void placedItemsDiagnose(PlacedItems& state,World& world,Npc& player,uint32_t zenId,bool control) {
  const auto& all=world.placedItemCensus();
  const auto target=std::find_if(all.begin(),all.end(),[&](const auto& row){return row.zenId==zenId;});
  if(target==all.end() || !target->item) {
    arenaAbort("placed Item diagnosis requires an authored ZEN-ID");return;
  }
  if(!target->validation.enabled) {
    const bool woke=target->item->wakePhysics(),activated=target->item->activatePhysics();
    const bool pass=!woke && !activated && !target->item->isDynamic();
    event("placed_items_diagnose_excluded",placedItemRow(world,*target)+
      ",\"dynamic\":"+(target->item->isDynamic()?"true":"false")+",\"pass\":"+(pass?"true":"false"));
    if(!pass)arenaAbort("excluded placed Item acquired a native body");
    return;
  }
  const size_t targetIndex=size_t(target-all.begin());
  const auto cell=[](const Vec3& at) {
    return std::tuple(int(std::floor(at.x/1200.f)),int(std::floor(at.y/1200.f)),int(std::floor(at.z/1200.f)));
  };
  const auto targetCell=cell(movablePosition(target->origin));
  state.batches.assign(1,{});state.batch=0;
  for(size_t i=0;i<all.size();++i)
    if(all[i].validation.enabled && cell(movablePosition(all[i].origin))==targetCell)
      state.batches[0].push_back(i);
  for(uint32_t i=0;i<world.npcCount();++i)if(auto* npc=world.npcById(i);npc && npc!=&player) {
    npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();npc->clearSpeed();npc->setExternalControl(true);
  }
  placedItemsNextBatch(state,world,player);
  if(!target->item->isDynamic())return;
  size_t removedItems=0,removedFurniture=0;
  if(control) {
    for(size_t i=0;i<all.size();++i)if(i!=targetIndex && all[i].item) {
      removedItems+=all[i].item->isDynamic();
      all[i].item->setPhysicsDisable();
      auto matrix=all[i].origin;matrix.translate(0,-100000.f,0);
      all[i].item->setObjMatrix(matrix);
    }
    for(const auto& body:world.physic()->movableVobSnapshot())if(body.enabled) {
      auto matrix=body.transform;matrix.translate(0,-100000.f,0);
      if(!world.physic()->setMovableVobTransform(body.zenId,matrix)) {
        arenaAbort("placed Item diagnosis cannot remove furniture collision");return;
      }
      ++removedFurniture;
    }
  }
  if(!world.physic()->startItemTrace(*target->item)) {
    arenaAbort("placed Item diagnosis cannot start native trace");return;
  }
  const auto firstStep=world.physic()->simulationStep();
  float maximum=0;
  // Less than one fixed-step duration prevents two native steps from being
  // hidden between trace samples, independently of the accumulator's phase.
  while(world.physic()->simulationStep()-firstStep<300) {
    world.physic()->tick(16);
    maximum=std::max(maximum,(target->item->position()-movablePosition(target->origin)).length());
  }
  world.physic()->stopItemTrace();
  const auto samples=world.physic()->takeItemTrace();
  std::ofstream file(outDir+"/placed-items-diagnose.jsonl");
  if(!file) {arenaAbort("cannot write placed Item diagnosis trace");return;}
  const auto triangle=[](const auto& hit) {
    return "\"triangle\":"+std::string(hit.triangle?"true":"false")+
      ",\"native_subshape_id\":"+std::to_string(hit.nativeSubShapeId)+
      ",\"source_triangle\":"+std::to_string(hit.sourceTriangle)+
      ",\"triangle_normal\":"+ragdollVector(hit.triangleNormal)+
      ",\"triangle_vertices_cm\":["+ragdollVector(hit.triangleVertices[0])+','+
        ragdollVector(hit.triangleVertices[1])+','+ragdollVector(hit.triangleVertices[2])+']'+
      ",\"triangle_edges_cm\":["+ragdollNumber(hit.triangleEdgesCm[0])+','+
        ragdollNumber(hit.triangleEdgesCm[1])+','+ragdollNumber(hit.triangleEdgesCm[2])+']';
  };
  const auto traceContact=[&](const auto& contact,bool solver) {
    return "{\"body_id\":"+std::to_string(contact.bodyId)+",\"zen_id\":"+std::to_string(contact.zenId)+
      ",\"category\":"+std::to_string(contact.category)+",\"visual\":"+jsonStr(contact.visual)+
      ",\"depth_cm\":"+ragdollNumber(contact.depth)+",\"normal\":"+ragdollVector(contact.normal)+
      ",\"point_cm\":"+ragdollVector(contact.point)+",\"other_point_cm\":"+ragdollVector(contact.otherPoint)+','+triangle(contact)+
      (solver?",\"native_step\":"+std::to_string(contact.nativeStep)+",\"persisted\":"+(contact.persisted?"true":"false"):std::string())+'}';
  };
  uint64_t previous=firstStep,maximumGap=0;
  size_t triggers=0,tested=0,solverCount=0;
  for(const auto& sample:samples) {
    maximumGap=std::max(maximumGap,sample.nativeStep-previous);previous=sample.nativeStep;
    std::string contacts="[",solverContacts="[",sweeps="[";
    for(const auto& contact:sample.contacts) {
      if(contacts.size()>1)contacts+=',';
      contacts+=traceContact(contact,false);
    }
    for(const auto& contact:sample.solverContacts) {
      if(solverContacts.size()>1)solverContacts+=',';
      solverContacts+=traceContact(contact,true);
      ++solverCount;
    }
    for(const auto& sweep:sample.recoverySweeps) {
      if(sweeps.size()>1)sweeps+=',';
      tested+=sweep.tested;triggers+=sweep.triggered;
      sweeps+="{\"native_step\":"+std::to_string(sweep.nativeStep)+",\"item_body_id\":"+std::to_string(sweep.itemBodyId)+
        ",\"body_id\":"+std::to_string(sweep.bodyId)+",\"category\":"+std::to_string(sweep.category)+
        ",\"tested\":"+(sweep.tested?"true":"false")+",\"hit\":"+(sweep.hit?"true":"false")+
        ",\"triggered\":"+(sweep.triggered?"true":"false")+",\"status\":"+jsonStr(sweep.status)+
        ",\"from_cm\":"+ragdollVector(sweep.from)+",\"to_cm\":"+ragdollVector(sweep.to)+
        ",\"point_cm\":"+ragdollVector(sweep.point)+",\"normal\":"+ragdollVector(sweep.normal)+
        ",\"fraction\":"+ragdollNumber(sweep.fraction)+",\"before_cm\":"+ragdollNumber(sweep.beforeCm)+
        ",\"after_cm\":"+ragdollNumber(sweep.afterCm)+','+triangle(sweep)+'}';
    }
    file<<"{\"world\":"+jsonStr(world.name())+",\"zen_id\":"+std::to_string(zenId)+
      ",\"control\":"+(control?"true":"false")+",\"step\":"+std::to_string(sample.nativeStep)+
      ",\"index\":"+std::to_string(sample.index)+",\"native_steps\":"+std::to_string(sample.nativeSteps)+
      ",\"before_step\":"+(sample.beforeStep?"true":"false")+",\"seconds\":"+ragdollNumber(sample.seconds)+
      ",\"body_id\":"+std::to_string(sample.bodyId)+",\"position_cm\":"+ragdollVector(sample.position)+
      ",\"com_cm\":"+ragdollVector(sample.centerOfMass)+",\"velocity_cm_s\":"+ragdollVector(sample.velocity)+
      ",\"bounds_min_cm\":"+ragdollVector(sample.bounds[0])+",\"bounds_max_cm\":"+ragdollVector(sample.bounds[1])+
      ",\"active\":"+(sample.active?"true":"false")+",\"sleeping\":"+(sample.sleeping?"true":"false")+
      ",\"contacts\":"+contacts+"],\"solver_contacts\":"+solverContacts+"],\"recovery_sweeps\":"+sweeps+"]}\n";
  }
  const auto steps=world.physic()->simulationStep()-firstStep;
  event("placed_items_diagnose",placedItemRow(world,*target)+",\"control\":"+(control?"true":"false")+
    ",\"section_items\":"+std::to_string(state.batches[0].size())+",\"removed_item_bodies\":"+std::to_string(removedItems)+
    ",\"removed_furniture\":"+std::to_string(removedFurniture)+",\"native_steps\":"+std::to_string(steps)+
    ",\"samples\":"+std::to_string(samples.size())+",\"maximum_native_step_gap\":"+std::to_string(maximumGap)+
    ",\"max_shift_cm\":"+ragdollNumber(maximum)+",\"final_shift_cm\":"+
    ragdollNumber((target->item->position()-movablePosition(target->origin)).length())+
    ",\"solver_contacts\":"+std::to_string(solverCount)+
    ",\"recovery_rays_tested\":"+std::to_string(tested)+",\"recovery_triggers\":"+std::to_string(triggers)+
    ",\"trace_complete\":"+(!samples.empty() && maximumGap<=1 && steps>=300?"true":"false"));
}

void placedItemsFields(PlacedItems& state,World& world,Npc& player,PlayerControl& ctrl,std::vector<Hold>& holds) {
  if(state.probing) {
    const auto& all=world.placedItemCensus();
    player.stopWalking();player.clearSpeed();player.setPosition(state.observer);player.updateTransform();
    for(uint32_t i=0;i<world.npcCount();++i)if(auto* npc=world.npcById(i);npc && npc!=&player &&
      (npc->position()-state.observer).quadLength()<2200.f*2200.f) {npc->stopWalking();npc->clearSpeed();}
    const auto sample=[&] {
      for(const auto index:state.batches[state.batch]) {
        const auto& row=all[index];
        if(!row.item) {arenaAbort("placed Item vanished during its wake probe");return false;}
        const auto shift=(row.item->position()-movablePosition(row.origin)).length();
        if(!std::isfinite(shift)) {arenaAbort("placed Item probe produced a nonfinite displacement");return false;}
        state.maximumTravel[index]=std::max(state.maximumTravel[index],shift);
        state.npcTouched[index]=state.npcTouched[index] || row.item->physicsTouchedByNpc();
      }
      return true;
    };
    if(!sample()) {state.probing=false;return;}
    // One complete section per outer frame. Advance the real fixed-step Jolt
    // world and observe after every native tick; rendering need not pace five
    // seconds of physics, and routine NPCs remain frozen for this pure probe.
    while(world.physic()->simulationStep()-state.startedStep<300) {
      world.physic()->tick(17);
      if(!sample()) {state.probing=false;return;}
    }
    const auto nativeSteps=world.physic()->simulationStep()-state.startedStep;
    std::ofstream file(outDir+"/placed-items-probe.jsonl",std::ios::app);
    if(!file) {arenaAbort("cannot append placed Item wake probe");state.probing=false;return;}
    for(const auto index:state.batches[state.batch]) {
      const auto& row=all[index];const float maximum=state.maximumTravel[index];
      ++state.probed;state.under5+=maximum<5.f;state.maximumShift=std::max(state.maximumShift,maximum);
      file<<'{'+placedItemRow(world,row)+",\"batch\":"+std::to_string(state.batch)+
        ",\"max_shift_cm\":"+ragdollNumber(maximum)+",\"final_shift_cm\":"+
        ragdollNumber((row.item->position()-movablePosition(row.origin)).length())+
        ",\"elapsed_ms\":"+std::to_string(nativeSteps*1000/60)+
        ",\"game_elapsed_ms\":"+std::to_string(world.tickCount()-state.started)+
        ",\"npc_contact\":"+(state.npcTouched[index]?"true":"false")+
        ",\"native_steps\":"+std::to_string(nativeSteps)+
        ",\"sleeping\":"+(row.item->physicsSleeping()?"true":"false")+"}\n";
    }
    // Restore all nearby placements, including sleeping supports in adjacent
    // cells, so no later section inherits another section's Item motion.
    for(const auto index:state.probeNeighbours) {
      const auto& row=all[index];
      row.item->setPhysicsDisable();row.item->setObjMatrix(row.origin);
    }
    if(++state.batch<state.batches.size())placedItemsNextBatch(state,world,player);
    else {
      state.probing=false;
      placedItemsDigest(world,"probe");
      event("placed_items_probe_summary","\"world\":"+jsonStr(world.name())+",\"total\":"+
        std::to_string(state.probed)+",\"under_5cm\":"+std::to_string(state.under5)+",\"max_shift_cm\":"+
        ragdollNumber(state.maximumShift)+",\"batches\":"+std::to_string(state.batches.size()));
    }
  }

  if(state.tracing)for(const auto index:state.observed) {
    const auto& row=world.placedItemCensus()[index];
    if(row.item)state.travel[index]=std::max(state.travel[index],(row.item->position()-state.reference[index]).length());
  }
  if(state.walking) {
    const auto position=player.position(),delta=position-state.walkStart;
    state.walkMaximumStep=std::max(state.walkMaximumStep,std::abs(position.y-state.walkPrevious.y));state.walkPrevious=position;
    state.walkMaximumCrossTrack=std::max(state.walkMaximumCrossTrack,std::abs(delta.x*state.walkDirection.z-delta.z*state.walkDirection.x));
    if(Vec3::dotProduct(delta,state.walkDirection)>=(state.walkGoal-state.walkStart).length()-20.f) {
      state.walkArrived=world.tickCount();state.walking=false;
      release(ctrl,KeyCodec::Forward);
      holds.erase(std::remove_if(holds.begin(),holds.end(),[](const Hold& hold){return hold.a==KeyCodec::Forward;}),holds.end());
      player.stopWalking();player.clearSpeed();
    }
    else player.setDirection(state.walkDirection);
  }
  if(state.performance) {
    const auto now=std::chrono::steady_clock::now();
    const float wall=state.hasFrame?std::chrono::duration<float,std::milli>(now-state.frameWall).count():0.f;
    state.frameWall=now;state.hasFrame=true;
    const auto step=world.physic()->simulationStep(),advanced=step-state.lastNative;state.lastNative=step;
    size_t bodies=0;for(const auto& row:world.placedItemCensus())bodies+=row.item && row.item->isDynamic();
    const auto stats=world.physic()->physicsStats();
    state.samples.push_back({world.tickCount(),advanced,stats.stepMilliseconds,wall,stats.bodies,bodies,world.placedItemsCreatedThisFrame()});
  }
}

void placedItemsPerformance(PlacedItems& state,World& world,Npc& player) {
  const auto [center,total,count]=placedItemsDensest(world);
  const auto floor=world.physic()->landRay(center+Vec3(0,300,0),600.f);
  if(!count || !floor.hasCol) {arenaAbort("placed Item densest region has no supported observer point");return;}
  player.clearAiQueue();player.stopWalking();player.clearSpeed();
  player.setPosition(floor.v);player.updateTransform();
  state.started=world.tickCount();state.samples.clear();state.lastNative=world.physic()->simulationStep();
  state.frameWall=std::chrono::steady_clock::now();state.hasFrame=true;state.performance=true;
  event("placed_items_performance_begin","\"world\":"+jsonStr(world.name())+
    ",\"enabled\":"+(Gothic::physicsFlags().placedItems?"true":"false")+
    ",\"center_cm\":"+ragdollVector(center)+",\"observer_cm\":"+ragdollVector(floor.v)+",\"total_within_20m\":"+std::to_string(total)+
    ",\"eligible_within_20m\":"+std::to_string(count));
}

void placedItemsPerformanceReport(PlacedItems& state,World& world) {
  state.performance=false;
  std::ofstream file(outDir+"/placed-items-performance.csv");
  if(!file || state.samples.empty()) {arenaAbort("placed Item performance has no samples or output");return;}
  file<<"t_game_ms,native_steps,physics_ms,frame_ms,bodies,placed_bodies,proximity_created\n";
  std::vector<float> physics;float worst=0;size_t maximumBodies=0,maximumPlaced=0,maximumCreated=0;
  for(const auto& sample:state.samples) {
    const auto elapsed=sample.tick-state.started;
    file<<elapsed<<','<<sample.nativeSteps<<','<<sample.physicsMs<<','<<sample.frameMs<<','<<sample.bodies<<','<<sample.placedBodies<<','<<sample.created<<'\n';
    worst=std::max(worst,sample.frameMs);maximumBodies=std::max(maximumBodies,sample.bodies);
    maximumPlaced=std::max(maximumPlaced,sample.placedBodies);maximumCreated=std::max(maximumCreated,sample.created);
    if(elapsed>=2000 && sample.nativeSteps)physics.push_back(sample.physicsMs);
  }
  if(physics.empty()) {arenaAbort("placed Item performance has no native samples after warmup");return;}
  std::sort(physics.begin(),physics.end());
  event("placed_items_performance","\"world\":"+jsonStr(world.name())+
    ",\"enabled\":"+(Gothic::physicsFlags().placedItems?"true":"false")+
    ",\"samples\":"+std::to_string(physics.size())+",\"physics_p95_ms\":"+
    ragdollNumber(physics[size_t(.95*double(physics.size()-1))])+",\"worst_frame_ms\":"+ragdollNumber(worst)+
    ",\"maximum_bodies\":"+std::to_string(maximumBodies)+",\"maximum_placed_bodies\":"+std::to_string(maximumPlaced)+
    ",\"maximum_proximity_created_in_frame\":"+std::to_string(maximumCreated)+",\"world_load_ms\":"+
    ragdollNumber(float(world.loadMilliseconds()))+",\"classification_ms\":"+ragdollNumber(float(world.placedItemValidationMilliseconds())));
}

namespace {

void placedItemsObserve(PlacedItems& state,World& world,const std::vector<size_t>& indices) {
  state.observed=indices;state.reference.clear();state.travel.clear();
  state.tracing=true;
  for(const auto index:indices)if(const auto* item=world.placedItemCensus()[index].item)state.reference[index]=item->position();
}

}

void placedItemsFloor(PlacedItems& state,World& world,Npc& player) {
  const auto& all=world.placedItemCensus();
  // Prefer ordinary food, as in item-wake, without changing its authored pose.
  for(bool foodOnly:{true,false})for(size_t i=0;i<all.size();++i) {
    const auto& row=all[i];if(!row.validation.enabled || !row.item)continue;
    auto& item=*row.item;
    if(foodOnly && !(uint32_t(item.mainFlag())&ITM_CAT_FOOD))continue;
    const auto* mesh=Resources::loadMesh(row.visual);if(!mesh)continue;
    auto low=Vec3(std::numeric_limits<float>::infinity()),high=-low;
    const auto* bounds=mesh->bbox();
    for(unsigned corner=0;corner<8;++corner) {
      Vec3 point(bounds[corner&1].x,bounds[(corner>>1)&1].y,bounds[(corner>>2)&1].z);row.origin.project(point);
      low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
      high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
    }
    if(high.y-low.y>55.f)continue;
    const auto center=(low+high)*.5f;
    const auto floor=world.physic()->landRay(center+Vec3(0,100,0),300.f);
    if(!floor.hasCol || floor.n.y<.98f || std::abs(low.y-floor.v.y)>6.f)continue;
    for(const auto direction:{Vec3(1,0,0),Vec3(-1,0,0),Vec3(0,0,1),Vec3(0,0,-1)}) {
      auto start=center-direction*180.f,goal=center+direction*180.f;start.y=goal.y=floor.v.y;
      const Vec3 across(-direction.z,0,direction.x);bool clear=true;
      for(unsigned step=0;step<=12 && clear;++step)for(float lateral:{-30.f,0.f,30.f}) {
        const auto point=start+(goal-start)*(float(step)/12.f)+across*lateral;
        const auto ground=world.physic()->landRay(point+Vec3(0,90,0),180.f);
        if(!ground.hasCol || ground.n.y<.98f || std::abs(ground.v.y-floor.v.y)>5.f) {clear=false;break;}
      }
      for(float lateral:{-30.f,0.f,30.f})for(float height:{20.f,100.f,170.f})
        if(clear && world.physic()->ray(start+across*lateral+Vec3(0,height,0),goal+across*lateral+Vec3(0,height,0)).hasCol)clear=false;
      if(!clear)continue;
      state.target=i;state.walkStart=start;state.walkGoal=goal;state.walkDirection=direction;
      // Match item-wake's local NPC removal without disturbing the tavern's
      // actors needed later for the native pickup and theft checks.
      std::vector<Npc*> nearby;
      for(uint32_t id=0;id<world.npcCount();++id)
        if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-center).length()<2000.f)
          nearby.push_back(npc);
      for(auto* npc:nearby)world.removeNpc(*npc);
      player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();player.closeWeapon(true);
      player.setPosition(start);player.setDirection(direction);player.updateTransform();
      state.walkPrevious=start;state.walkMaximumStep=state.walkMaximumCrossTrack=0;state.walkArrived=0;
      event("placed_items_floor_setup",placedItemRow(world,row)+",\"owned\":"+
        ((item.handle().owner!=0 || item.handle().owner_guild!=0)?"true":"false")+
        ",\"owner\":"+std::to_string(item.handle().owner)+",\"owner_guild\":"+std::to_string(item.handle().owner_guild)+
        ",\"flags\":"+std::to_string(item.itemFlag())+",\"count\":"+std::to_string(item.count())+
        ",\"start_cm\":"+ragdollVector(start)+",\"goal_cm\":"+ragdollVector(goal)+
        ",\"npcs_removed\":"+std::to_string(nearby.size())+",\"npc_clear_radius_cm\":2000");
      return;
    }
  }
  arenaAbort("placed Item floor preparation found no admitted source Item with a clear supported hero lane");
}

void placedItemsWalk(PlacedItems& state,World& world,Npc& player) {
  if(state.target==size_t(-1)) {arenaAbort("placed Item walking needs a prepared floor object");return;}
  const auto* item=world.placedItemCensus()[state.target].item;
  if(!item) {arenaAbort("placed Item walking source disappeared");return;}
  const auto settled=player.position();
  // Match character begin: held input starts at the prepared coordinate after
  // the idle settling interval in both modes.
  player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();
  player.setPosition(state.walkStart);player.setDirection(state.walkDirection);player.updateTransform();
  state.walkPrevious=state.walkStart;state.walkMaximumStep=state.walkMaximumCrossTrack=0;state.walkArrived=0;
  state.walkBodyBefore=item->isDynamic();state.walkSince=world.tickCount();state.walking=true;
  placedItemsObserve(state,world,{state.target});
  event("placed_items_walk_begin","\"settled_before_cm\":"+ragdollVector(settled)+
    ",\"start_cm\":"+ragdollVector(player.position()));
}

void placedItemsWalkReport(PlacedItems& state,World& world,Npc& player) {
  state.walking=false;state.tracing=false;
  const auto& row=world.placedItemCensus()[state.target];
  const auto shift=state.travel[state.target];const bool enabled=Gothic::physicsFlags().placedItems;
  const auto elapsed=(state.walkArrived?state.walkArrived:world.tickCount())-state.walkSince;
  const bool pass=state.walkArrived && elapsed<=2500 && state.walkMaximumCrossTrack<25.f && state.walkMaximumStep<20.f &&
    (!enabled || shift>.1f);
  event("placed_items_walk",placedItemRow(world,row)+",\"placed_items\":"+(enabled?"true":"false")+
    ",\"character_virtual\":"+(Gothic::physicsFlags().characterVirtual?"true":"false")+
    ",\"npc_character_virtual\":"+(Gothic::physicsFlags().npcCharacterVirtual?"true":"false")+
    ",\"body_before_walk\":"+(state.walkBodyBefore?"true":"false")+
    ",\"body_after_walk\":"+(row.item && row.item->isDynamic()?"true":"false")+
    ",\"npc_contact\":"+(row.item && row.item->physicsTouchedByNpc()?"true":"false")+
    ",\"arrived\":"+(state.walkArrived?"true":"false")+",\"elapsed_ms\":"+std::to_string(elapsed)+
    ",\"max_shift_cm\":"+ragdollNumber(shift)+",\"hero_travel_cm\":"+ragdollNumber((player.position()-state.walkStart).length())+
    ",\"max_cross_track_cm\":"+ragdollNumber(state.walkMaximumCrossTrack)+",\"max_vertical_step_cm\":"+
    ragdollNumber(state.walkMaximumStep)+",\"pass\":"+(pass?"true":"false"));
}

void placedItemsArrowReady(PlacedItems& state,World& world,Npc& player) {
  if(state.target==size_t(-1)) {arenaAbort("placed Item arrow needs the prepared authored floor object");return;}
  const auto& row=world.placedItemCensus()[state.target];
  if(!row.item) {arenaAbort("placed Item arrow target was removed");return;}
  row.item->setObjMatrix(row.origin);
  auto observer=state.walkStart;
  const auto floor=world.physic()->landRay(observer+Vec3(0,200,0),600.f);
  if(!floor.hasCol) {arenaAbort("placed Item arrow observer has no real floor");return;}
  observer.y=floor.v.y;player.clearAiQueue();player.stopWalking();player.clearSpeed();
  player.setPosition(observer);player.updateTransform();
}

void placedItemsArrow(PlacedItems& state,World& world,Npc& player) {
  if(state.target==size_t(-1) || !world.placedItemCensus()[state.target].item) {arenaAbort("placed Item arrow target unavailable");return;}
  const auto& row=world.placedItemCensus()[state.target];
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("placed Item arrow native instance unavailable");return;}
  const auto bodies=world.physic()->waterBodySnapshot();
  const auto native=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.item==row.item;});
  if(native==bodies.end()) {arenaAbort("placed Item arrow requires the prepared native collision body");return;}
  const auto target=(native->collisionBounds[0]+native->collisionBounds[1])*.5f,from=target-state.walkDirection*150.f;
  Item arrow(world,symbol,Item::T_Inventory);
  auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(Archery::launch(from,target,DynamicWorld::bulletSpeed)-from),DynamicWorld::bulletSpeed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  placedItemsObserve(state,world,{state.target});
  event("placed_items_arrow_launch",placedItemRow(world,row)+",\"from_cm\":"+ragdollVector(from)+",\"target_cm\":"+ragdollVector(target)+
    ",\"collision_bounds_min_cm\":"+ragdollVector(native->collisionBounds[0])+",\"collision_bounds_max_cm\":"+ragdollVector(native->collisionBounds[1])+
    ",\"dynamic_before_shot\":"+(row.item->isDynamic()?"true":"false")+",\"sleeping_before_shot\":"+(row.item->physicsSleeping()?"true":"false"));
}

void placedItemsArrowReport(PlacedItems& state,World& world) {
  state.tracing=false;
  const auto& row=world.placedItemCensus()[state.target];const auto shift=state.travel[state.target];
  const bool pass=shift>.1f;
  event("placed_items_arrow",placedItemRow(world,row)+",\"placed_items\":"+(Gothic::physicsFlags().placedItems?"true":"false")+
    ",\"max_shift_cm\":"+ragdollNumber(shift)+",\"pass\":"+(pass?"true":"false"));
}

namespace {

bool placedItemOwned(Npc& player,const Item& item) {
  const auto point=item.position();
  const auto ownership=HeroDeed::snapshot(player,"take_item",nullptr,&item,nullptr,0,&point);
  return !ownership->owner.npc.empty() || ownership->ownerGuild>int32_t(GIL_NONE) || !ownership->roomOwner.npc.empty() ||
         (ownership->roomGuild>int32_t(GIL_NONE) && ownership->roomGuild!=int32_t(GIL_PUBLIC));
}

}

void placedItemsTable(PlacedItems& state,World& world,Npc& player,bool owned) {
  const auto* anchor=owned?nullptr:world.findWayPoint("NW_TAVERNE_IN_05");
  if(!owned && !anchor) {arenaAbort("placed Item table preparation requires the Dead Harpy tavern");return;}
  const auto& all=world.placedItemCensus();size_t best=0;std::optional<DynamicWorld::MovableVobSnapshot> chosen;
  for(const auto& table:world.physic()->movableVobSnapshot()) {
    if(!table.enabled || table.className!="table" ||
       (!owned && (movablePosition(table.origin)-anchor->position()).length()>1800.f))continue;
    auto inverse=table.origin;inverse.inverse();std::vector<size_t> indices;size_t admitted=0,excluded=0,ownedAdmitted=0;
    for(size_t i=0;i<all.size();++i) {
      const auto& row=all[i];if(!row.item)continue;
      auto position=row.item->midPosition();inverse.project(position);
      if(position.x<table.localBounds[0].x-10.f || position.x>table.localBounds[1].x+10.f ||
         position.z<table.localBounds[0].z-10.f || position.z>table.localBounds[1].z+10.f ||
         position.y<table.localBounds[1].y-15.f || position.y>table.localBounds[1].y+50.f)continue;
      indices.push_back(i);admitted+=row.validation.enabled;excluded+=!row.validation.enabled;
      ownedAdmitted+=owned && row.validation.enabled && placedItemOwned(player,*row.item);
    }
    if(admitted && (!owned || (excluded && ownedAdmitted)) && indices.size()>best) {
      best=indices.size();chosen=table;state.observed=std::move(indices);
    }
  }
  if(!chosen) {
    arenaAbort(owned?"placed Item owned table preparation requires admitted owned and excluded authored Items on one native table":
                     "placed Item table preparation found no native tavern table with admitted authored Items");return;
  }
  state.tableZen=chosen->zenId;state.tableOrigin=chosen->origin;
  const auto center=movablePosition(chosen->origin);auto observer=center+Vec3(500,0,500);
  const auto floor=world.physic()->landRay(observer+Vec3(0,200,0),600.f);
  if(!floor.hasCol) {arenaAbort("placed Item tavern observer has no supported floor");return;}
  observer.y=floor.v.y;player.clearAiQueue();player.stopWalking();player.clearSpeed();player.setPosition(observer);player.updateTransform();
  event("placed_items_table_setup","\"table_zen_id\":"+std::to_string(state.tableZen)+",\"table_visual\":"+jsonStr(chosen->visual)+
    ",\"items\":"+std::to_string(state.observed.size())+",\"purpose\":"+jsonStr(owned?"owned":"tavern")+
    ",\"tavern_waypoint\":"+jsonStr(anchor?std::string_view(anchor->name):std::string_view()));
}

void placedItemsTablePush(PlacedItems& state,World& world,Npc& player) {
  if(state.tableZen==uint32_t(-1)) {arenaAbort("placed Item table push needs preparation");return;}
  const auto table=furnitureBody(world,state.tableZen);
  if(!table) {arenaAbort("placed Item native tavern table disappeared");return;}
  auto center=(table->localBounds[0]+table->localBounds[1])*.5f,point=center;point.y=table->localBounds[1].y;
  const bool narrowX=table->localBounds[1].x-table->localBounds[0].x<table->localBounds[1].z-table->localBounds[0].z;
  if(narrowX)point.x=table->localBounds[1].x;else point.z=table->localBounds[1].z;
  table->transform.project(center);table->transform.project(point);
  auto direction=point-center;direction.y=0;direction=Vec3::normalize(direction);
  const auto force=direction*(6.f*table->mass);
  placedItemsObserve(state,world,state.observed);
  const auto pushed=world.physic()->applyMovableVobImpulse(center,140.f,[force,mass=table->mass](const Vec3&,float targetMass){
    return std::abs(targetMass-mass)<.001f?force:Vec3();},&player,&point);
  const auto reacted=world.physic()->applyMovableVobImpulse(center,140.f,[force,mass=table->mass](const Vec3&,float targetMass){
    return std::abs(targetMass-mass)<.001f?-force:Vec3();},&player,&center);
  event("placed_items_table_push","\"table_zen_id\":"+std::to_string(state.tableZen)+",\"impulses\":"+std::to_string(pushed+reacted));
  if(!pushed || !reacted)arenaAbort("placed Item native table impulse couple failed");
}

void placedItemsTableReport(PlacedItems& state,World& world) {
  state.tracing=false;size_t admittedMoved=0,excludedMoved=0;std::string items="[";
  for(const auto index:state.observed) {
    const auto& row=world.placedItemCensus()[index];const auto shift=state.travel[index];
    admittedMoved+=row.validation.enabled && shift>.1f;excludedMoved+=!row.validation.enabled && shift>.01f;
    if(items.size()>1)items+=',';
    items+='{'+placedItemRow(world,row)+",\"max_shift_cm\":"+ragdollNumber(shift)+'}';
  }
  const auto table=furnitureBody(world,state.tableZen);const auto tableShift=table?(movablePosition(table->transform)-movablePosition(state.tableOrigin)).length():0.f;
  const bool enabled=Gothic::physicsFlags().placedItems,pass=table && tableShift>1.f && (!enabled || (admittedMoved>0 && excludedMoved==0));
  event("placed_items_table","\"placed_items\":"+std::string(enabled?"true":"false")+",\"table_zen_id\":"+
    std::to_string(state.tableZen)+",\"table_shift_cm\":"+ragdollNumber(tableShift)+",\"admitted_moved\":"+
    std::to_string(admittedMoved)+",\"excluded_moved\":"+std::to_string(excludedMoved)+",\"items\":"+items+
    "],\"pass\":"+(pass?"true":"false"));
}

void placedItemsSnapshot(World& world,std::string_view label,uint64_t initialDt) {
  if(label=="initial" && initialDt!=0) {arenaAbort("placed Item cold snapshot must precede the first native step");return;}
  std::ofstream file(outDir+"/placed-items-"+std::string(label)+".jsonl");
  if(!file) {arenaAbort("cannot write placed Item save observation");return;}
  size_t count=0,linked=0,dynamic=0,sleeping=0,shifted=0,unchanged=0;
  std::unordered_map<uint32_t,const WorldObjects::PlacedItem*> authored;
  for(const auto& row:world.placedItemCensus())authored.emplace(row.zenId,&row);
  for(uint32_t i=0;auto* item=world.itmById(i);++i) {
    ++count;linked+=item->isPlacedItem();dynamic+=item->isDynamic();sleeping+=item->isDynamic() && item->physicsSleeping();
    const auto source=authored.find(item->placedItemZenId());
    const bool hasOrigin=item->isPlacedItem() && source!=authored.end();
    const float shift=hasOrigin?(item->position()-movablePosition(source->second->origin)).length():0.f;
    if(hasOrigin) {shifted+=shift>.01f;unchanged+=shift<=.01f;}
    file<<"{\"world\":"+jsonStr(world.name())+",\"index\":"+std::to_string(i)+",\"instance\":"+std::to_string(item->clsId())+
      ",\"visual\":"+jsonStr(item->handle().visual)+",\"zen_id\":"+(item->isPlacedItem()?std::to_string(item->placedItemZenId()):"null")+
      ",\"placed\":"+(item->isPlacedItem()?"true":"false")+
      ",\"shift_from_origin_cm\":"+(hasOrigin?ragdollNumber(shift):"null")+
      ",\"position_cm\":"+ragdollVector(item->position())+
      ",\"transform\":"+ragdollMatrix(item->transform())+",\"dynamic\":"+(item->isDynamic()?"true":"false")+
      ",\"sleeping\":"+(item->physicsSleeping()?"true":"false")+",\"owner\":"+std::to_string(item->handle().owner)+
      ",\"owner_guild\":"+std::to_string(item->handle().owner_guild)+",\"flags\":"+std::to_string(item->itemFlag())+
      ",\"count\":"+std::to_string(item->count())+"}\n";
  }
  event("placed_items_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"initial_dt\":"+std::to_string(initialDt)+",\"items\":"+std::to_string(count)+",\"linked\":"+std::to_string(linked)+
    ",\"dynamic\":"+std::to_string(dynamic)+",\"sleeping\":"+std::to_string(sleeping)+
    ",\"placed_shifted\":"+std::to_string(shifted)+",\"placed_unchanged\":"+std::to_string(unchanged));
}

void placedItemsSave(World& world) {
  std::vector<uint8_t> bytes;
  {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(out,"placed Items",preview);}
  std::ofstream file(outDir+"/session-placed-items.sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file) {arenaAbort("cannot write complete placed Item GameSession archive");return;}
  placedItemsSnapshot(world,"saved");
  event("placed_items_session_save","\"archive\":\"session-placed-items.sav\",\"bytes\":"+std::to_string(bytes.size()));
}

void placedItemsPickupReady(PlacedItems& state,World& world,Npc& player) {
  const auto& all=world.placedItemCensus();
  const auto ownedMoved=[&](size_t index) {
    const auto& row=all[index];
    return row.item && row.validation.enabled && placedItemOwned(player,*row.item) &&
      (row.item->position()-movablePosition(row.origin)).length()>.1f;
  };
  if(state.target==size_t(-1) || !ownedMoved(state.target)) {
    const auto found=std::find_if(state.observed.begin(),state.observed.end(),ownedMoved);
    if(found==state.observed.end()) {arenaAbort("placed Item pickup requires a moved owned authored floor or tabletop object");return;}
    state.target=*found;
  }
  const auto& row=all[state.target];
  const auto point=row.item->position();auto observer=point-state.walkDirection*70.f;
  const auto floor=world.physic()->landRay(observer+Vec3(0,200,0),600.f);
  if(!floor.hasCol) {arenaAbort("placed Item pickup observer has no native floor");return;}
  observer.y=floor.v.y;player.closeWeapon(true);player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();
  player.setPosition(observer);player.setDirection(point-observer);player.updateTransform();
  event("placed_items_pickup_ready",placedItemRow(world,row)+",\"shift_from_origin_cm\":"+
    ragdollNumber((row.item->position()-movablePosition(row.origin)).length()));
}

void placedItemsPickup(PlacedItems& state,World& world,Npc& player,PlayerControl& ctrl) {
  const auto& row=world.placedItemCensus()[state.target];
  if(!row.item) {arenaAbort("placed Item pickup source disappeared");return;}
  state.pickupSymbol=row.item->clsId();state.pickupCount=row.item->count();state.pickupBefore=player.itemCount(state.pickupSymbol);
  state.theftDeeds=0;state.theftWitnesses.clear();state.theftRecipients.clear();state.theftOwner.clear();state.theftItem.clear();
  state.theftRoom.clear();state.theftRoomOwner.clear();state.theftRoomGuild=-1;
  Gothic::inst().onHeroDeed.bind(&state,&PlacedItems::deed);
  Gothic::inst().onObservedPerception.bind(&state,&PlacedItems::perception);state.watchingTheft=true;
  const auto fields=placedItemRow(world,row)+",\"owner\":"+std::to_string(row.item->handle().owner)+
    ",\"owner_guild\":"+std::to_string(row.item->handle().owner_guild)+",\"flags\":"+std::to_string(row.item->itemFlag())+
    ",\"count\":"+std::to_string(state.pickupCount)+",\"shift_from_origin_cm\":"+
    ragdollNumber((row.item->position()-movablePosition(row.origin)).length());
  state.pickupAccepted=ctrl.interact(*row.item);
  event("placed_items_pickup_begin",fields+",\"accepted\":"+(state.pickupAccepted?"true":"false"));
}

void placedItemsPickupReport(PlacedItems& state,World& world,Npc& player) {
  if(state.watchingTheft) {
    Gothic::inst().onHeroDeed.ubind(&state,&PlacedItems::deed);
    Gothic::inst().onObservedPerception.ubind(&state,&PlacedItems::perception);state.watchingTheft=false;
  }
  std::sort(state.theftWitnesses.begin(),state.theftWitnesses.end());std::sort(state.theftRecipients.begin(),state.theftRecipients.end());
  std::string witnesses="[",recipients="[";
  for(const auto& name:state.theftWitnesses) {if(witnesses.size()>1)witnesses+=',';witnesses+=jsonStr(name);}
  for(const auto id:state.theftRecipients) {if(recipients.size()>1)recipients+=',';recipients+=std::to_string(id);}
  const auto after=player.itemCount(state.pickupSymbol);
  const bool owned=!state.theftOwner.empty() || state.theftOwnerGuild>int32_t(GIL_NONE) || !state.theftRoomOwner.empty() ||
                   (state.theftRoomGuild>int32_t(GIL_NONE) && state.theftRoomGuild!=int32_t(GIL_PUBLIC));
  const bool pass=state.pickupAccepted && after==state.pickupBefore+state.pickupCount && state.theftDeeds==1 && owned;
  event("placed_items_pickup","\"placed_items\":"+std::string(Gothic::physicsFlags().placedItems?"true":"false")+
    ",\"inventory_before\":"+std::to_string(state.pickupBefore)+",\"inventory_after\":"+std::to_string(after)+
    ",\"count\":"+std::to_string(state.pickupCount)+",\"deeds\":"+std::to_string(state.theftDeeds)+
    ",\"item\":"+jsonStr(state.theftItem)+",\"owner\":"+jsonStr(state.theftOwner)+",\"owner_guild\":"+
    std::to_string(state.theftOwnerGuild)+",\"room\":"+jsonStr(state.theftRoom)+",\"room_owner\":"+jsonStr(state.theftRoomOwner)+
    ",\"room_guild\":"+std::to_string(state.theftRoomGuild)+",\"witnesses\":"+witnesses+"],\"perception_recipients\":"+recipients+
    "],\"pass\":"+(pass?"true":"false"));
}

bool placedItemsCommand(PlacedItems& state,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  const std::string& c = a[0];
  if(c=="placed-items" && a.size()==2 && a[1]=="floor")placedItemsFloor(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="walk")placedItemsWalk(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="walk-report")placedItemsWalkReport(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="arrow-ready")placedItemsArrowReady(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="arrow")placedItemsArrow(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="arrow-report")placedItemsArrowReport(state,w);
  else if(c=="placed-items" && a.size()==2 && a[1]=="table")placedItemsTable(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="owned-table")placedItemsTable(state,w,pl,true);
  else if(c=="placed-items" && a.size()==2 && a[1]=="table-push")placedItemsTablePush(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="table-report")placedItemsTableReport(state,w);
  else if(c=="placed-items" && a.size()==2 && a[1]=="save")placedItemsSave(w);
  else if(c=="placed-items" && a.size()==2 && a[1]=="snapshot")placedItemsSnapshot(w,"before");
  else if(c=="placed-items" && a.size()==2 && a[1]=="initial")placedItemsSnapshot(w,"initial",w.tickCount());
  else if(c=="placed-items" && a.size()==2 && a[1]=="pickup-ready")placedItemsPickupReady(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="pickup")placedItemsPickup(state,w,pl,ctrl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="pickup-report")placedItemsPickupReport(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="census")placedItemsCensus(w);
  else if(c=="placed-items" && a.size()==2 && a[1]=="probe")placedItemsProbe(state,w,pl);
  else if(c=="placed-items" && (a.size()==3 || (a.size()==4 && a[3]=="control")) && a[1]=="diagnose")
    placedItemsDiagnose(state,w,pl,uint32_t(std::stoul(a[2])),a.size()==4);
  else if(c=="placed-items" && a.size()==2 && a[1]=="performance")placedItemsPerformance(state,w,pl);
  else if(c=="placed-items" && a.size()==2 && a[1]=="performance-report")placedItemsPerformanceReport(state,w);
  else return false;
  return true;
  }

}

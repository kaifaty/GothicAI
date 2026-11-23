#pragma once

// Test-only access keeps the production creation entry point unchanged.
struct NpcDefinitionContract {
  static auto& definitions(GameScript& script) { return script.npcDefinitions; }
  static auto& placements(GameScript& script) { return script.placementDefinitions; }
  static auto& activities(GameScript& script) { return script.activityDefinitions; }
  static auto& aiState(Npc& npc) { return npc.aiState; }
  static auto& activityEvents(Npc& npc) { return npc.activityEvents; }
  static bool& activityEventsOn(GameScript& script) { return script.activityEvents; }
  static auto& previousState(Npc& npc) { return npc.aiPrevState; }
  static void tickRoutine(Npc& npc) { npc.aiState.loopNextTime=npc.world().tickCount(); npc.tickRoutine(); }
  static void tickScheduledRoutine(Npc& npc) { npc.tickRoutine(); }
  // The engine's next queued action, as the NPC's tick takes it.
  static void nextAiAction(Npc& npc) { npc.nextAiAction(npc.aiQueue,0); }
  static void tickActivityDeadline(Npc& npc) { npc.tickActivityDeadline(); }
  static const AiQueue& queue(const Npc& npc) { return npc.aiQueue; }
  static AiQueue& queue(Npc& npc) { return npc.aiQueue; }
  static uint64_t& refusalDeadline(Npc& npc) { return npc.refuseTalkMilis; }
  static auto& perceptions(Npc& npc) { return npc.perception; }
  static bool walking(const Npc& npc) { return !npc.go2.empty(); }
  static const auto& physicsPosition(const Npc& npc) { return npc.physic.position(); }
  // An action at the head of the queue, as an interruption ahead of the queued use.
  static void pushFront(Npc& npc, AiQueue::AiAction&& a) { npc.aiQueue.pushFront(std::move(a)); }
  static const auto& items(GameScript& script) { return script.itemDefinitions; }
  static const auto& routines(GameScript& script) { return script.routineDefinitions; }
  static auto& random(GameScript& script) { return script.randGen; }
  // the views of items (graphics/itemview): the sword and the bow on a body, a lying item's
  static const ItemView& sword(const Npc& npc) { return npc.visual.sword.view; }
  static const ItemView& bow(const Npc& npc) { return npc.visual.bow.view; }
  static const ItemView* lying(const Item& item) {
    const auto& v=item.visual;
    return v.type==ObjVisual::M_Mesh && v.mesh.worldItem ? &v.mesh.item : nullptr;
    }
  static StreamedVisual& visual(Item& item) { return item.visual; }
  static const ItemView& stuckArrow(const Npc& npc, size_t i) { return npc.visual.stuckArrows[i].view; }
  // where a stuck arrow is by its own bone and offset: the bone's rendered matrix times the pin's local one
  static Tempest::Vec3 pinAt(const Npc& npc, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const auto m=npc.visual.renderedBone(pin.bone)*pin.local;
    return {m.at(3,0),m.at(3,1),m.at(3,2)};
    }
  // a stuck arrow's record in main's own bytes (mdlvisual_quiver.cpp, APR1 version 1), read by the current loader
  static bool loadMainArrow(Npc& npc, World& world, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const std::string skeleton(npc.visual.skeleton->name()),bone(npc.visual.skeleton->nodes[pin.bone].name),visual=pin.visual;
    const auto local=pin.local;
    std::vector<uint8_t> raw;
    {
      Tempest::MemWriter writer(raw);
      Serialize out(writer);
      out.setEntry("contract/arrows");
      const auto name=[&](const std::string& v) {
        const uint32_t n=uint32_t(v.size());
        out.writeBytes(&n,sizeof(n));
        out.writeBytes(v.data(),v.size());
        };
      const uint32_t magic=0x31525041,count=1;
      const uint16_t version=1;
      out.writeBytes(&magic,sizeof(magic));
      out.writeBytes(&version,sizeof(version));
      name(skeleton);
      out.writeBytes(&count,sizeof(count));
      name(visual);
      name(bone);
      out.writeBytes(&local,sizeof(local));
    }
    Tempest::MemReader reader(raw);
    Serialize in(reader);
    return in.setEntry("contract/arrows") && npc.visual.loadStuckArrows(in,world);
    }
  static auto& isolatedRandom(GameScript& script) { return script.isolatedRandom; }
  // an object's model and its effects of the animations' events (MdlVisual::startEffect)
  static const MdlVisual& model(const Interactive& mob) { return mob.objVisual().mdl.view; }
  static const auto& effects(const MdlVisual& view) { return view.effects; }
  static size_t bone(const MdlVisual& view, std::string_view name) { return view.skeleton->findNode(name); }
  static const AnimationSolver& solver(const MdlVisual& view) { return view.solver; }
  };

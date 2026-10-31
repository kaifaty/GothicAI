#include "simulationstep.h"

#include "playercontrol.h"
#include "bodyobservations.h"
#include "bodycontroller.h"
#include "bodytrajectory.h"
#include "bodyreplay.h"
#include "frameprofile.h"
#include "gothic.h"
#include "camera.h"
#include "testbench.h"
#include "world/objects/npc.h"

void SimulationStep::worldPlayerStep(uint64_t dt, PlayerControl& player,
                                      const std::function<void()>& afterFocus) {
  BodyController::pumpWatchdogs();
  BodyObservations::beginStep(BodyObservations::currentStep()+1);
  TestBench::beforeWorldTick(dt);
  Gothic::inst().tick(dt);
  player.tickFocus();
  if(afterFocus)
    afterFocus();
  player.tickMove(dt);
  }

bool SimulationStep::finishStep(uint64_t dt, PlayerControl& player,
                                 const std::function<void()>& updateCamera) {
  Gothic::inst().updateAnimation(dt);
  if(updateCamera)
    updateCamera();
  else
    observerCamera(dt);
  BodyController::postStep();
  BodyTrajectory::postStep();
  { FrameProfile::Scope timing(FrameProfile::Zone::Observations); BodyObservations::sampleAll(); }
  const bool capture = TestBench::tick(dt,player);
  if(dt>0)
    BodyReplay::postStep();
  return capture;
  }

void SimulationStep::observerCamera(uint64_t dt) {
  auto camera = Gothic::inst().camera();
  auto player = Gothic::inst().player();
  if(camera==nullptr)
    return;

  if(player!=nullptr && !camera->isCutscene() && !camera->isFree()) {
    camera->setAimAnchor(player->position());
    auto spin = camera->spin();
    if(player->interactive()==nullptr && !player->isDown() && !player->isMeleeKnockedDown())
      spin.y = player->rotation();
    if(player->isDive() && !camera->isMarvin())
      spin.x = -player->rotationY();
    camera->setSpin(spin);
    camera->setTarget(player->cameraBone(camera->isFirstPerson()));
    }
  if(dt==0)
    return;

  if(camera->isToggleEnabled() && !camera->isCutscene()) {
    auto mode = Camera::Normal;
    if(!camera->isFree()) {
      if(camera->isFirstPerson())
        mode = Camera::FirstPerson;
      else if(player!=nullptr) {
        if(player->interactive()!=nullptr)
          mode = Camera::Mobsi;
        else if(player->isDead())
          mode = Camera::Death;
        else if(player->isMeleeKnockedDown())
          mode = Camera::Normal;
        else if(player->isDive())
          mode = Camera::Dive;
        else if(player->isSwim())
          mode = Camera::Swim;
        else if(player->isFallingDeep())
          mode = Camera::Fall;
        else {
          switch(player->weaponState()) {
            case WeaponState::Fist:
            case WeaponState::W1H:
            case WeaponState::W2H: mode = Camera::Melee; break;
            case WeaponState::Bow:
            case WeaponState::CBow:
              mode = Gothic::inst().version().game==2 ? Camera::Ranged : Camera::Normal;
              break;
            case WeaponState::Mage: mode = Camera::Melee; break;
            case WeaponState::NoWeapon: break;
            }
          }
        }
      }
    camera->setMode(mode);
    }
  camera->tick(dt);
  }

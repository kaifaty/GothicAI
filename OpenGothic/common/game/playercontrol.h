#pragma once

#include "world/focus.h"
#include "utils/keycodec.h"
#include "constants.h"
#include "aimstep.h"
#include "meleedodge.h"

#include <array>
#include <optional>

class DialogMenu;
class InventoryMenu;
class DbgPainter;
class World;
class Interactive;
class Npc;
class Item;
class Camera;
class Gothic;

class PlayerControl final {
  public:
    PlayerControl(); // CPU simulation observer; inventory and dialog UI are optional.
    PlayerControl(DialogMenu& dlg, InventoryMenu& inv);
    ~PlayerControl();

    void  onKeyPressed (KeyCodec::Action a, Tempest::Event::KeyType key, KeyCodec::Mapping mapping);
    void  onKeyReleased(KeyCodec::Action a, KeyCodec::Mapping mapping);
    bool  isPressed(KeyCodec::Action a) const;
    void  onRotateMouse(float dAngleX, float dAngleY);
    // the mouse wheel; true: taken (aiming a bow by hand: down cants it for a quick shot, up straightens it)
    bool  onMouseWheel(int delta);

    void  drawVobRay(DbgPainter& p) const;

    void  changeZoom(int delta);
    void  tickFocus();
    void  clearFocus();

    bool  interact(Interactive& it);
    bool  interact(Npc&         other);
    bool  interact(Item&        item);

    void  clearInput();

    void  setTarget(Npc* other);
    void  actionFocus(Npc& other);
    void  emptyFocus();

    Focus focus() const;
    bool  hasActionFocus() const;

    bool  tickMove(uint64_t dt);
    bool  tickCameraMove(uint64_t dt);

    // the bow raised in the manual aim (game/archery): the mouse aims
    bool  isManualAim() const { return manualAiming; }
    // the test bench aims there instead of the center of the screen
    void  setAimOverride(bool on, const Tempest::Vec3& at = {});
    // the crosshair of the manual aim: where it aims, how far off the arrow may go now (radians: the
    // crosshair opens that much); false: not aiming by hand. centered: it aims at the center of the
    // screen (else the test bench aims elsewhere)
    bool  crosshair(Tempest::Vec3& at, float& spread) const;
    bool  isCrosshairCentered() const { return !aimOverride; }

  private:
    enum WeaponAction : uint8_t {
      WeaponClose,
      WeaponMele,
      WeaponBow,
      Weapon3,
      Weapon4,
      Weapon5,
      Weapon6,
      Weapon7,
      Weapon8,
      Weapon9,
      Weapon10,

      Last,
      };

    enum FocusAction : uint8_t {
      ActForward=0,
      ActBack   =1,
      ActLeft   =2,
      ActRight  =3,
      ActGeneric=4,
      ActMove   =5,
      ActKill   =6,
      };

    using Action=KeyCodec::Action;

    struct AxisStatus { 
        /// Main direction (e.g. W or Up arrow)
        std::array<bool, KeyCodec::NumMappings> main = {};
        
        /// Reverse direction (e.g. S or Down arrow)
        std::array<bool, KeyCodec::NumMappings> reverse = {};

        /// Current axis value (scale from -1 to 1)
        auto value() const -> float {
          return
              (this->anyMain() ? 1.f : 0.f)
            + (this->anyReverse() ? -1.f : 0.f);
          }

        /// True if only one of directions is active
        /// (e.g. false if none or both directions are active).
        auto any() const -> bool {
          return this->value() != 0;
          }

        void reset() {
          this->main.fill(false);
          this->reverse.fill(false);
          }

      private:
        /// Is any key pressed that activates the main direction
        /// (e.g. W or Up Arrow in Forward-Backward axis)
        auto anyMain() const -> bool {
          for(auto elem : main) {
            if(elem) return true;
            }
          return false;
          }
        
        /// Is any key pressed that activates the reverse direction
        /// (e.g. S or Down arrow in Forward-Backward axis)
        auto anyReverse() const -> bool {
          for(auto elem : reverse) {
            if(elem) return true;
            }
          return false;
          }
      };

    struct MovementStatus {
      AxisStatus forwardBackward;  
      AxisStatus strafeRightLeft;
      AxisStatus turnRightLeft;

      /// Resets all axes to their default state.
      void reset() {
        this->forwardBackward.reset();
        this->strafeRightLeft.reset();
        this->turnRightLeft.reset();
        }
      } movement;
    
    bool           ctrl[Action::Last]={};
    bool           wctrl[WeaponAction::Last]={};
    bool           actrl[7]={};

    WeaponAction   wctrlLast = WeaponAction::WeaponMele; //!< Reminder for weapon toggle.

    bool           cacheFocus=false;
    Focus          currentFocus;
    float          rotMouse=0;
    float          rotMouseY=0;
    bool           casting = false;
    size_t         pickLockProgress = 0;

    float          runAngleDest   = 0.f;
    uint64_t       turnAniSmooth  = 0;
    int            rotationAni    = 0;
    bool           g2Ctrl         = false;

    Npc*           threatened     = nullptr; //!< last npc the armed player aimed at (PERC_ASSESSTHREAT)
    uint64_t       threatenedAt   = 0;

    bool           manualAiming   = false;   //!< the bow raised in the manual aim
    bool           aimOverride    = false;
    Tempest::Vec3  aimOverrideAt  = {};
    Tempest::Vec3  aimAt          = {};      //!< where the crosshair is
    float          aimSpread      = 0;       //!< how far it is open, radians, smoothed
    float          aimSpreadVel   = 0;       //!< how fast it opens, radians per second
    bool           aimCrouch      = false;   //!< crouched to aim (the sneak key while the bow is up)
    float          crouchK        = 0;       //!< crouched 0..1, smoothed
    float          peekK          = 0;       //!< leaning out round a corner -1 (left)..1 (right), smoothed
    bool           aimCant        = false;   //!< the bow canted for quick shots (the wheel; kept till changed)
    float          cantK          = 0;       //!< canted 0..1, smoothed
    float          walkK          = 0;       //!< stepping with the bow drawn 0..1, smoothed
    AimStep        aimStep;
    bool           aimStepStopped = false;

    // The melee dodge asked for at the press of its key, with the way the movement keys pointed then;
    // implMove plays it (Npc::meleeDodge) or drops it.
    std::optional<MeleeDodge::Direction> dodgeRequest;

    DialogMenu*    dlg = nullptr;
    InventoryMenu* inv = nullptr;

    void           setupSettings();
    void           assessThreat(Npc& pl, Npc& other);
    bool           canInteract() const;
    // The dodge key dodges now: [GAME] melee_dodge, a melee weapon or the fists drawn, the hero's camera
    bool           dodgeKeyDodges(const Npc& pl) const;
    void           marvinF8(uint64_t dt);
    void           marvinK(uint64_t dt);
    void           marvinO();
    void           toggleWalkMode();
    void           toggleSneakMode();
    void           moveFocus(FocusAction act);
    Focus          findFocus(const Focus* prev) const;

    void           clrDraw();
    void           implMove(uint64_t dt);
    void           implMoveMobsi(Npc& pl, uint64_t dt);
    void           processPickLock(Npc& pl, Interactive& inter, KeyCodec::Action key);
    void           processLadder(Npc& pl, Interactive& inter, KeyCodec::Action key);
    void           quitPicklock(Npc& pl);
    void           setPos(std::array<float,3> a, uint64_t dt, float speed);
    void           assignRunAngle(Npc& pl, float rotation, uint64_t dt);
    void           setAnimRotate (Npc& pl, float rotation, int anim, bool force, uint64_t dt);
    void           processAutoRotate(Npc& pl, float& rot, uint64_t dt);
    bool           implManualAim(Npc& pl, uint64_t dt);
    void           endManualAim(Npc& pl);
    Npc*           aimRayHit(Npc& pl, Tempest::Vec3& at) const;


    //////////////////////////////////
    // Helper functions for movement
    //////////////////////////////////

    auto wantsToMoveForward() const -> bool {
      return movement.forwardBackward.value() > 0.f;
      }
    auto wantsToMoveBackward() const -> bool {
      return movement.forwardBackward.value() < 0.f;
      }

    auto wantsToStrafeRight() const -> bool {
      return movement.strafeRightLeft.value() > 0.f;
      }
    auto wantsToStrafeLeft() const -> bool {
      return movement.strafeRightLeft.value() < 0.f;
      }

    auto wantsToTurnRight() const -> bool {
      return movement.turnRightLeft.value() > 0.f;
      }
    auto wantsToTurnLeft() const -> bool {
      return movement.turnRightLeft.value() < 0.f;
      }

    /// @brief Analyses the input action mapping and updates the movement status accordingly.
    ///        Meant to be used when key is pressed or released.
    /// @param actionMapping - the pressed/released action
    /// @param pressed - true if the key was pressed, false if it was released
    auto handleMovementAction(KeyCodec::ActionMapping actionMapping, bool pressed) -> void;
  };

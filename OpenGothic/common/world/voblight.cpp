#include "voblight.h"

#include <cmath>

#include "world.h"
#include "graphics/voblight.h"

VobLight::VobLight(Vob* parent, World& world, const asset::VLight& vob, Flags flags)
  : Vob(parent,world,vob,flags) {
#if 0
  if(!(flags&Startup))
    return;
#endif

  light = world.addLight(vob);
  const auto offset = VobLayer::lightOffset(vob);
  lightOffset = Tempest::Vec3(offset[0],offset[1],offset[2]);
  if(!vob.is_static && !vob.on) {
    light.setEnabled(false);
    return;
    }

  if(vob.is_static && !vob.on) {
    //light.setEnabled(false);
    //return;
    }

  if(vob.is_static &&
     (vob.preset=="NW_STANDART_DARKBLUE" ||
      vob.preset=="DEFAULTLIGHT_DARKBLUE" || vob.preset=="INROOM_DARKBLUE")) {
    // ambient light-spam in many caves/houses
    light.setEnabled(false);
    }
  if(vob.is_static && vob.preset.find("AMBIENCE_")==0) {
    light.setEnabled(false);
    }
  if(vob.is_static && vob.preset.find("_STATIC")!=std::string::npos) {
    //light.setEnabled(false);
    }
  if(vob.is_static && vob.preset=="SCHMUTZ") {
    light.setEnabled(false);
    }
  if(vob.is_static && vob.range>3000.0) {
    // unmotivated light blobs in old-mine ("MINE_DARK"/"MINE_DARKLIGHT")
    light.setEnabled(false);
    }
  // a burning lamp of the world itself (not of a fire's vob tree) inside a house goes to bed with it
  if((flags&Static)!=None && light.isEnabled() && world.view()!=nullptr) {
    // a fire's lamp carries its particles (the hearth's FIRE.pfx): it burns through the night
    bool fire = false;
    for(const auto& c:vob.children)
      fire |= (c!=nullptr && c->visual!=nullptr && c->visual->type==asset::VisualType::PARTICLE_EFFECT);
    world.view()->houseLamps().adopt(light, position()+lightOffset, vob.id, fire);
    }
  }

VobLight::~VobLight() {
  if(world.view()!=nullptr)
    world.view()->houseLamps().drop(light);
  }

void VobLight::moveEvent() {
  light.setPosition(position()+lightOffset);
  // the light mask turns with the light's own axes: the yaw of its X axis in the world
  auto t = transform();
  Tempest::Vec3 o = {}, x = {1,0,0};
  t.project(o);
  t.project(x);
  light.setYaw(std::atan2(x.z-o.z, x.x-o.x));
  }

void VobLight::fillAttachmentSnapshot(AttachmentSnapshot& out) const {
  Vob::fillAttachmentSnapshot(out);
  out.isLight = true;
  out.hasRenderedPosition = light.renderPosition(out.renderedPosition);
  }

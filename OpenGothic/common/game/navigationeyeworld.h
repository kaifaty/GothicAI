#pragma once

#include "physics/dynamicworld.h"

class Npc;

DynamicWorld::NavigationRayResult navigationRay(const DynamicWorld& physics, const Tempest::Vec3& from,
                                                const Tempest::Vec3& to, const Npc* except);

#pragma once

#include "game/knownroads.h"
#include "physics/navmesh.h"

#include <iosfwd>
#include <string>
#include <vector>

class World;
class Npc;

namespace NavDemosInternal {
using Tempest::Vec3;
constexpr float Pi=3.14159265358979323846f;

NavMesh::Point meters(const Vec3& p);
Vec3 cm(const NavMesh::Point& p);
void jsonNumber(std::ostream& out,double value);
void vector(std::ostream& out,const Vec3& p);
std::array<float,3> local(const Vec3& p,float yaw);
Vec3 demoPoint(World& world,const std::string& name);
const char* transitionName(NavMesh::LinkType type);
void writeV2Plan(std::ostream& out,const Vec3& position,const Vec3& goal,
                 const KnownRoads::Guidance& guidance,const NavMesh::Path& path,bool direct);
void commandProbes(const std::vector<std::string>& a,World& w,const std::string& output,bool& handled);
}

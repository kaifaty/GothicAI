#pragma once

#include <Tempest/Vec>
#include "worlddata.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace PortalRooms {

// BSP leaves can extend from an indoor floor to the sky outside a tower or
// ship. Room surfaces (is_sector) and portal caps bound the actual room volume;
// ordinary landscape polygons such as a ship's yard are not room ceilings.
class Volumes final {
  public:
    void reset(asset::BspTreeType mode,std::vector<WorldData::Room> volumes) {
      indoorWorld=mode==asset::BspTreeType::INDOOR;
      rooms=std::move(volumes);
      }

    bool isIndoor(const Tempest::Vec3& point) const {
      if(indoorWorld) return true;
      for(const auto& room:rooms) {
        if(point.x<room.min.x || point.x>room.max.x ||
           point.y<room.min.y || point.y>room.max.y ||
           point.z<room.min.z || point.z>room.max.z) continue;
        // Three oblique rays avoid an edge or coincident portal changing the
        // answer. Coincident intersections count once, including back faces.
        const std::array<Tempest::Vec3,3> directions={Tempest::Vec3(.371f,.529f,.763f),
                                                   Tempest::Vec3(-.683f,.413f,.601f),
                                                   Tempest::Vec3(.227f,.851f,-.473f)};
        unsigned inside=0;
        for(const auto& direction:directions) {
          std::vector<double> intersections;
          for(const auto& triangle:room.triangles) {
            const double distance=intersect(point,direction,triangle);
            if(distance>=0.) intersections.push_back(distance);
            }
          std::sort(intersections.begin(),intersections.end());
          size_t crossings=0;
          double previous=-1.;
          for(const auto distance:intersections) {
            if(previous<0. || distance-previous>.01) ++crossings;
            previous=distance;
            }
          inside+=crossings%2;
          }
        if(inside>=2) return true;
        }
      return false;
      }

  private:
    using Triangle=WorldData::Triangle;
    using Room=WorldData::Room;
    bool indoorWorld=false;
    std::vector<Room> rooms;

    static double intersect(const Tempest::Vec3& from,const Tempest::Vec3& direction,const Triangle& triangle) {
      struct Double3 {
        double x,y,z;
        explicit Double3(const Tempest::Vec3& v):x(v.x),y(v.y),z(v.z) {}
        explicit Double3(const zenkit::Vec3& v):x(v.x),y(v.y),z(v.z) {}
        Double3(double x,double y,double z):x(x),y(y),z(z) {}
        Double3 operator-(const Double3& b) const { return {x-b.x,y-b.y,z-b.z}; }
        double dot(const Double3& b) const { return x*b.x+y*b.y+z*b.z; }
        Double3 cross(const Double3& b) const { return {y*b.z-z*b.y,z*b.x-x*b.z,x*b.y-y*b.x}; }
        };
      const Double3 start(from),ray(direction),a(triangle.point[0]),b(triangle.point[1]),c(triangle.point[2]);
      const auto edge1=b-a,edge2=c-a,p=ray.cross(edge2);
      const double determinant=edge1.dot(p);
      if(std::abs(determinant)<1.e-9) return -1.;
      const auto offset=start-a;
      const double u=offset.dot(p)/determinant;
      if(u<-.0000001 || u>1.0000001) return -1.;
      const auto q=offset.cross(edge1);
      const double v=ray.dot(q)/determinant;
      if(v<-.0000001 || u+v>1.0000001) return -1.;
      const double distance=edge2.dot(q)/determinant;
      return distance>=0. ? distance : -1.;
      }
  };
}

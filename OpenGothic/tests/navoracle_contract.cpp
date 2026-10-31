#include "physics/navmesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Point = NavMesh::Point;

void check(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
  }

void triangle(std::vector<float>& vertices,std::vector<int>& indices,Point a,Point b,Point c) {
  const int base=int(vertices.size()/3);
  for(const auto& p:{a,b,c}) vertices.insert(vertices.end(),p.begin(),p.end());
  indices.insert(indices.end(),{base,base+1,base+2});
  }

void floor(NavMesh::Geometry& geometry,float x0,float z0,float x1,float z1,float height=0,bool water=false) {
  auto& vertices=water ? geometry.waterVertices : geometry.vertices;
  auto& indices=water ? geometry.waterTriangles : geometry.triangles;
  triangle(vertices,indices,{x0,height,z0},{x0,height,z1},{x1,height,z1});
  triangle(vertices,indices,{x0,height,z0},{x1,height,z1},{x1,height,z0});
  }

void ramp(NavMesh::Geometry& geometry,float x0,float x1,float z0,float z1,float bottom,float top) {
  triangle(geometry.vertices,geometry.triangles,{x0,bottom,z0},{x0,bottom,z1},{x1,top,z1});
  triangle(geometry.vertices,geometry.triangles,{x0,bottom,z0},{x1,top,z1},{x1,top,z0});
  }

const NavMesh::RasterCell& cell(const NavMesh::RasterLayer& layer,int right,int forward) {
  check(right>=0 && forward>=0 && size_t(right)<layer.size && size_t(forward)<layer.size,
        "test requested an out-of-bounds raster cell");
  return layer.cells[size_t(forward)*layer.size+size_t(right)];
  }

Point worldCell(const Point& feet,float yaw,const NavMesh::RasterLayer& layer,int right,int forward) {
  const float r=(float(right)+.5f-float(layer.size)*.5f)*layer.cellSize;
  const float f=(float(forward)+.5f-float(layer.size)*.5f)*layer.cellSize;
  const float angle=yaw*3.14159265358979323846f/180.f;
  return {feet[0]+f*std::cos(angle)-r*std::sin(angle),feet[1],
          feet[2]+f*std::sin(angle)+r*std::cos(angle)};
  }

void validate(const NavMesh::LocalRaster& value) {
  check(value.near.size==64 && value.near.cellSize==.25f && value.near.cells.size()==4096,
        "near oracle must retain 64x64 native quarter-metre cells");
  check(value.far.size==100 && value.far.cellSize==2 && value.far.cells.size()==10000,
        "far oracle must retain 100x100 native two-metre cells");
  for(const auto* layer:{&value.near,&value.far})for(const auto& sample:layer->cells) {
    check(!sample.heightBoundary || !sample.traversable,"a height-boundary cell must never be free ground");
    check(!sample.traversable || (sample.heightValid && sample.polygon!=0),
          "free oracle cells require exact polygon coverage and a ground height");
    check(!sample.heightValid || (std::isfinite(sample.groundHeight) && sample.polygon!=0),
          "covered oracle cells require finite world height and polygon identity");
    if(!value.supportValid)
      check(!sample.traversable && !sample.heightValid && sample.polygon==0,
            "missing exact body support must not borrow another floor");
    }
  check(!value.supportValid || value.supportPolygon!=0,"valid support must retain its exact ground polygon");
  if(value.supportValid) {
    check(std::isfinite(value.supportSnapDistance) && std::isfinite(value.supportHorizontalDistance) &&
          std::isfinite(value.supportVerticalOffset) && value.supportSnapDistance>=0 && value.supportHorizontalDistance>=0,
          "support-anchor snap metrics must be finite distances in world metres");
    check(std::abs(value.supportSnapDistance-std::hypot(value.supportHorizontalDistance,value.supportVerticalOffset))<.0001f,
          "support-anchor 3D distance must retain both horizontal and vertical offsets");
    }
  }

NavMesh::LocalRaster raster(const NavMesh& mesh,const Point& feet,float yaw=0) {
  auto result=mesh.rasterLocal(feet,yaw);
  validate(result);
  return result;
  }

void height(const NavMesh::RasterCell& value,float expected,const std::string& context) {
  check(value.heightValid && std::abs(value.groundHeight-expected)<.21f,
        context+": oracle selected the wrong physical floor height");
  }

NavMesh build(const NavMesh::Geometry& geometry,const NavMesh::Params& params,
              const std::filesystem::path& cache,const std::string& name) {
  NavMesh mesh;
  mesh.buildOrLoad(geometry,params,cache,name);
  check(mesh.stats().complete,"fixture mesh did not finish building: "+name);
  return mesh;
  }

struct Cleanup final {
  std::filesystem::path path=std::filesystem::temp_directory_path()/
      ("gothic-navoracle-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~Cleanup() {std::error_code error;std::filesystem::remove_all(path,error);}
  };
}

int main() {
  Cleanup cache;
  try {
    unsigned tests=0;
    NavMesh::Params params;
    params.tileSize=32;params.cellSize=.1f;

    NavMesh empty;
    check(!raster(empty,{0,0,0}).supportValid,"unbuilt mesh must not invent body support");
    ++tests;

    NavMesh::Geometry flatGeometry;
    floor(flatGeometry,0,0,20,20);
    auto flat=build(flatGeometry,params,cache.path,"flat");
    const auto flatRaster=raster(flat,{10,0,10});
    check(flatRaster.supportValid,"contained flat floor must support the actual body");
    check(std::all_of(flatRaster.near.cells.begin(),flatRaster.near.cells.end(),[](const auto& value) {
            return value.traversable && value.heightValid && !value.heightBoundary;
            }),"ordinary flat polygons and tile edges must remain free without a second erosion");
    height(cell(flatRaster.near,32,32),0,"flat near");
    height(cell(flatRaster.far,49,49),0,"flat far");
    // x=16 is exactly five tile widths (.1m * 32). Reaching that endpoint
    // may leave Detour's ray corridor in the linked polygon on the other side
    // of the shared edge; it is still ordinary covered flat ground.
    const auto tileBoundary=raster(flat,{1,0,10});
    const auto& boundaryCell=cell(tileBoundary.far,50,57);
    check(boundaryCell.traversable && !boundaryCell.heightBoundary,
          "shared flat tile boundary must not be painted as a cliff by endpoint polygon identity");
    check(!cell(flatRaster.far,55,55).heightValid && !cell(flatRaster.far,55,55).traversable,
          "far cells beyond exact geometry must stay uncovered");
    check(!raster(flat,{1000,0,1000}).supportValid,"off-mesh body must not snap to a remote floor");
    ++tests;

    const auto boundaryAnchor=raster(flat,{.05f,0,10});
    check(boundaryAnchor.supportValid && !boundaryAnchor.supportExact &&
          boundaryAnchor.supportHorizontalDistance>0 && boundaryAnchor.supportHorizontalDistance<=params.nearestHorizontal,
          "body just outside mesh erosion must retain its bounded nearest-ground support anchor");
    check(std::abs(boundaryAnchor.supportVerticalOffset)<=params.climb+2*params.cellHeight,
          "nearest-ground support must retain the physical floor-height bound");
    check(cell(boundaryAnchor.near,32,36).traversable,
          "a bounded boundary anchor must retain adjacent exactly covered free ground");
    ++tests;

    NavMesh::Geometry rectangleGeometry;
    floor(rectangleGeometry,0,0,30,8);
    auto rectangle=build(rectangleGeometry,params,cache.path,"rotation");
    const Point rectangleFeet={15,0,4};
    const auto facingX=raster(rectangle,rectangleFeet,0),facingZ=raster(rectangle,rectangleFeet,90);
    check(cell(facingX.near,32,60).traversable && !cell(facingX.near,60,32).heightValid,
          "yaw zero must map forward to world +X and right to world +Z");
    check(!cell(facingZ.near,32,60).heightValid && cell(facingZ.near,60,32).traversable,
          "yaw ninety must rotate forward to +Z and right to -X");
    ++tests;

    NavMesh::Geometry bridgeGeometry;
    floor(bridgeGeometry,0,0,8,8);floor(bridgeGeometry,12,0,20,8);
    floor(bridgeGeometry,7,3.5f,13,4.5f);
    auto bridge=build(bridgeGeometry,params,cache.path,"narrow-bridge");
    const Point bridgeFeet={7,0,4};
    const auto bridgeRaster=raster(bridge,bridgeFeet);
    check(cell(bridgeRaster.near,32,44).traversable,"a surviving narrow bridge must remain walkable");
    const auto outsideBridge=worldCell(bridgeFeet,0,bridgeRaster.near,34,44);
    const auto nearbyBridge=bridge.groundLocation(outsideBridge);
    check(nearbyBridge.known && nearbyBridge.horizontalDistance>.1f,
          "bridge fixture must prove that the ordinary nearest-ground query snaps across its edge");
    check(!cell(bridgeRaster.near,34,44).heightValid && !cell(bridgeRaster.near,34,44).traversable,
          "nearest bridge polygon must not fill an uncovered cell beside the bridge");
    ++tests;

    NavMesh::Geometry gapGeometry;
    floor(gapGeometry,0,0,8,8);floor(gapGeometry,9,0,17,8);
    auto gap=build(gapGeometry,params,cache.path,"real-gap");
    const Point gapFeet={7,0,4};
    const auto gapRaster=raster(gap,gapFeet);
    const auto gapPoint=worldCell(gapFeet,0,gapRaster.near,32,38);
    check(gap.groundLocation(gapPoint).known,"gap fixture must remain inside nearest-poly query extents");
    check(!cell(gapRaster.near,32,38).heightValid && !cell(gapRaster.near,32,38).traversable,
          "real gap must stay uncovered despite successful nearest-poly lookup");
    const auto anchoredGap=raster(gap,gapPoint);
    check(anchoredGap.supportValid && !anchoredGap.supportExact &&
          anchoredGap.supportHorizontalDistance<=params.nearestHorizontal,
          "body in a narrow gap may use a bounded bank anchor without claiming exact body coverage");
    check(!cell(anchoredGap.near,32,32).heightValid && !cell(anchoredGap.near,32,32).traversable,
          "a support anchor must not turn uncovered gap cells into borrowed bank polygons");
    ++tests;

    NavMesh::Geometry stackedGeometry;
    floor(stackedGeometry,0,0,20,20);floor(stackedGeometry,0,0,20,20,4);
    auto stacked=build(stackedGeometry,params,cache.path,"stacked");
    const auto lower=raster(stacked,{10,0,10}),upper=raster(stacked,{10,4,10});
    check(lower.supportValid && upper.supportValid && lower.supportPolygon!=upper.supportPolygon,
          "same XZ on two storeys must retain distinct actual support polygons");
    height(cell(lower.near,40,40),0,"lower stacked storey");
    height(cell(upper.near,40,40),4,"upper stacked storey");
    check(cell(lower.near,40,40).traversable && cell(upper.near,40,40).traversable,
          "the actual supported storey must remain traversable");
    check(!raster(stacked,{10,2,10}).supportValid,"body between storeys must not borrow nearest vertical ground");
    ++tests;

    NavMesh::Geometry cliffGeometry;
    floor(cliffGeometry,0,0,8,10,4);floor(cliffGeometry,8.4f,0,20,10);
    auto cliff=build(cliffGeometry,params,cache.path,"disconnected-cliff");
    const auto cliffRaster=raster(cliff,{6,4,5});
    const auto& bottom=cell(cliffRaster.near,32,48);
    height(bottom,0,"disconnected cliff bottom");
    check(!bottom.traversable,"ground below a disconnected cliff must not join the supported walk component");
    height(cell(cliffRaster.near,32,32),4,"cliff actual support sheet");
    ++tests;

    // Gradually reached high terrain must survive without an initial-Y cutoff.
    // There is no second floor below this plateau to make its height ambiguous.
    NavMesh::Geometry rampGeometry;
    floor(rampGeometry,0,0,3,20);
    ramp(rampGeometry,2,10,6,8,0,4);
    floor(rampGeometry,10,6,22,18,4);
    auto rampMesh=build(rampGeometry,params,cache.path,"connected-ramp");
    check(rampMesh.findPath({1,0,6},{16,4,11},false).complete,"ramp fixture must connect low and high ground without links");
    const auto fromBelow=raster(rampMesh,{1,0,6});
    height(cell(fromBelow.far,52,57),4,"high ground reached gradually by ramp");
    check(cell(fromBelow.far,52,57).traversable,"connected high terrain must not be cut off by the body's initial Y");
    ++tests;

    // Both overlapping floors now belong to the SAME ground component via
    // the remote ramp. The actual support's ground-walk rank must select the
    // floor, and an adjacent XZ height seam must not become a local shortcut.
    NavMesh::Geometry connectedFloorsGeometry;
    floor(connectedFloorsGeometry,0,0,30,24);
    ramp(connectedFloorsGeometry,2,10,6,8,0,4);
    floor(connectedFloorsGeometry,10,6,22,18,4);
    auto connectedFloors=build(connectedFloorsGeometry,params,cache.path,"connected-stacked-floors");
    const Point lowerFeet={16,0,11},upperFeet={16,4,11};
    const auto lowerLocation=connectedFloors.groundLocation(lowerFeet);
    const auto upperLocation=connectedFloors.groundLocation(upperFeet);
    check(lowerLocation.known && upperLocation.known && lowerLocation.component==upperLocation.component &&
          lowerLocation.polygon!=upperLocation.polygon,
          "remote ramp fixture must join two distinct overlapping floors in the same ground component");
    const auto connectedLower=raster(connectedFloors,lowerFeet),connectedUpper=raster(connectedFloors,upperFeet);
    height(cell(connectedLower.near,40,40),0,"same-component floor selected from lower support");
    height(cell(connectedUpper.near,40,40),4,"same-component floor selected from upper support");
    check(cell(connectedLower.near,40,40).traversable && cell(connectedUpper.near,40,40).traversable,
          "walk-distance ranking must preserve the actual sheet even when components are identical");
    const auto onBridge=raster(connectedFloors,{16,4,17});
    const auto& highEdge=cell(onBridge.near,34,32);
    const auto& lowEdge=cell(onBridge.near,35,32);
    height(highEdge,4,"bridge seam upper cell");height(lowEdge,0,"bridge seam lower cell");
    check(highEdge.heightBoundary && lowEdge.heightBoundary && !highEdge.traversable && !lowEdge.traversable,
          "both covered neighbours at a bridge drop must block a false same-component shortcut");
    ++tests;

    NavMesh::Geometry waterGeometry;
    floor(waterGeometry,0,0,20,20);floor(waterGeometry,8,0,12,20,2,true);
    auto water=build(waterGeometry,params,cache.path,"water-bottom");
    const auto waterRaster=raster(water,{3,0,10});
    check(waterRaster.supportValid && !cell(waterRaster.near,32,60).traversable,
          "deep-water bottom must not become free oracle ground");
    check(!raster(water,{10,0,10}).supportValid,"submerged excluded bottom must not support the body oracle");
    ++tests;

    const Point takeoff={3,0,4},landing={14,0,4};
    const auto candidates=gap.gapCandidates(takeoff,landing);
    check(!candidates.empty(),"off-mesh fixture must expose the real gap banks");
    auto linkedParams=params;
    NavMesh::OffMeshLink jump;
    jump.start=candidates.front().start;jump.end=candidates.front().end;
    jump.type=NavMesh::LinkType::Jump;jump.id=42;linkedParams.links.push_back(jump);
    auto linked=build(gapGeometry,linkedParams,cache.path,"off-mesh-gap");
    const auto crossed=linked.findPath(takeoff,landing);
    check(crossed.complete && crossed.transitions.size()==1 && !linked.findPath(takeoff,landing,false).complete,
          "off-mesh fixture must permit a proven jump while retaining disconnected ground banks");
    const auto linkedRaster=raster(linked,takeoff);
    const auto& farIsland=cell(linkedRaster.far,50,55);
    height(farIsland,0,"off-mesh destination island");
    check(!farIsland.traversable,"jump link must never turn the destination island into ground-only free cells");
    ++tests;

    std::cout<<"{\"contract\":\"navoracle\",\"tests\":"<<tests
             <<",\"checks\":\"native raster, exact coverage, rotation, bridge, gap, floors, cliff, ramp, water, offmesh\"}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr<<"navoracle contract: "<<error.what()<<'\n';
    return 1;
    }
  }

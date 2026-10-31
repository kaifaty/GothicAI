#pragma once

// Include after VHACD.h with its implementation enabled. This reference uses
// the same exact triangle/box overlap predicate, independently of decomposition.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct VoxelReference {
  double pitchCm = 0, volumeCm3 = 0;
  std::array<size_t,3> grid = {};
  size_t surfaceCells = 0, interiorCells = 0;
  };

static VoxelReference voxelReference(const std::vector<double>& points,
                                     const std::vector<uint32_t>& indices) {
  if(points.empty() || points.size()%3!=0 || indices.empty() || indices.size()%3!=0)
    throw std::runtime_error("invalid voxel reference mesh");
  std::array<double,3> lower = {points[0],points[1],points[2]}, upper = lower;
  for(size_t i=0;i<points.size();++i) {
    if(!std::isfinite(points[i])) throw std::runtime_error("non-finite voxel reference position");
    lower[i%3]=std::min(lower[i%3],points[i]);
    upper[i%3]=std::max(upper[i%3],points[i]);
    }
  std::array<double,3> extent;
  for(size_t axis=0;axis<3;++axis) extent[axis]=upper[axis]-lower[axis];
  VoxelReference result;
  result.pitchCm=std::max(1.0,*std::max_element(extent.begin(),extent.end())/253.0);
  std::array<double,3> origin;
  for(size_t axis=0;axis<3;++axis) {
    // Mathematically extent/pitch <= 253. Clamp its ceil against roundoff.
    result.grid[axis]=std::min(size_t(253),size_t(std::ceil(extent[axis]/result.pitchCm)))+3;
    origin[axis]=lower[axis]-1.5*result.pitchCm;
    }
  const auto nx=result.grid[0], ny=result.grid[1], nz=result.grid[2];
  const auto plane=nx*ny, total=plane*nz;
  // 0: unvisited empty, 1: triangle surface, 2: exterior empty.
  std::vector<uint8_t> cells(total,0);
  const VHACD::Vect3 halfSize(.5);
  for(size_t triangle=0;triangle<indices.size();triangle+=3) {
    std::array<VHACD::Vect3,3> vertices;
    for(size_t corner=0;corner<3;++corner) {
      const size_t index=indices[triangle+corner];
      if(index>=points.size()/3) throw std::runtime_error("voxel reference triangle out of range");
      for(size_t axis=0;axis<3;++axis)
        vertices[corner][axis]=(points[index*3+axis]-origin[axis])/result.pitchCm;
      }
    std::array<size_t,3> begin,end;
    for(size_t axis=0;axis<3;++axis) {
      const auto minimum=std::min({vertices[0][axis],vertices[1][axis],vertices[2][axis]});
      const auto maximum=std::max({vertices[0][axis],vertices[1][axis],vertices[2][axis]});
      // Closed boxes on an exact grid boundary both intersect the triangle.
      begin[axis]=size_t(std::max(0.0,std::ceil(minimum)-1));
      end[axis]=std::min(result.grid[axis]-1,size_t(std::floor(maximum)));
      }
    for(size_t z=begin[2];z<=end[2];++z)
      for(size_t y=begin[1];y<=end[1];++y)
        for(size_t x=begin[0];x<=end[0];++x) {
          auto& cell=cells[z*plane+y*nx+x];
          if(cell==0 && VHACD::TriBoxOverlap(VHACD::Vect3(double(x)+.5,double(y)+.5,double(z)+.5),
                                           halfSize,vertices[0],vertices[1],vertices[2])) {
            cell=1;
            ++result.surfaceCells;
            }
          }
    }
  std::vector<uint32_t> queue;
  const auto visit=[&](size_t index) {
    if(cells[index]==0) {
      cells[index]=2;
      queue.push_back(uint32_t(index));
      }
    };
  for(size_t z=0;z<nz;++z)
    for(size_t y=0;y<ny;++y) {
      visit(z*plane+y*nx);
      visit(z*plane+y*nx+nx-1);
      }
  for(size_t z=0;z<nz;++z)
    for(size_t x=0;x<nx;++x) {
      visit(z*plane+x);
      visit(z*plane+(ny-1)*nx+x);
      }
  for(size_t y=0;y<ny;++y)
    for(size_t x=0;x<nx;++x) {
      visit(y*nx+x);
      visit((nz-1)*plane+y*nx+x);
      }
  for(size_t head=0;head<queue.size();++head) {
    const size_t index=queue[head], x=index%nx, y=index/nx%ny, z=index/plane;
    if(x>0) visit(index-1);
    if(x+1<nx) visit(index+1);
    if(y>0) visit(index-nx);
    if(y+1<ny) visit(index+nx);
    if(z>0) visit(index-plane);
    if(z+1<nz) visit(index+plane);
    }
  result.interiorCells=size_t(std::count(cells.begin(),cells.end(),uint8_t(0)));
  result.volumeCm3=double(result.surfaceCells+result.interiorCells)*
                    result.pitchCm*result.pitchCm*result.pitchCm;
  return result;
  }

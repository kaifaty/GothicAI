#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace TreeNanite {

struct Contours {
  struct Face {
    std::array<uint32_t, 3> vertices;
    uint32_t chart;
    };
  struct Shape { uint32_t firstMeshlet, meshletCount; };
  struct Meshlet { uint32_t firstVertex, firstPrimitive, vertexCount, primitiveCount; };

  static constexpr uint32_t MipBase = 96, MipCount = 257, MipSteps = 32;
  std::array<std::vector<Face>, 4> faces;
  std::vector<Shape> shapes;
  std::vector<Meshlet> meshlets;
  std::vector<uint32_t> barycentrics, primitives;

  static Contours load(const std::filesystem::path& path) {
    static_assert(std::endian::native == std::endian::little && sizeof(Face) == 16 &&
                  sizeof(Shape) == 8 && sizeof(Meshlet) == 16);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto bytes = in.tellg();
    in.seekg(0);
    std::array<uint32_t, 12> head{};
    if(!in.read(reinterpret_cast<char*>(head.data()), sizeof(head)))
      throw std::runtime_error("Cannot read route contour header: " + path.string());
    const auto [magic, version, mipBase, mipCount, chartCount, meshletCount, baryCount, primCount,
                n1, n2, n3, n4] = head;
    const uint64_t shapeCount = uint64_t(chartCount)*mipCount;
    if(magic != 0x4c544346u || version != 1 || mipBase != MipBase || mipCount != MipCount ||
       chartCount == 0 || shapeCount > UINT32_MAX)
      throw std::runtime_error("Invalid route contour header: " + path.string());
    const uint64_t expected = sizeof(head) + sizeof(Face)*(uint64_t(n1)+n2+n3+n4) +
                              sizeof(Shape)*shapeCount + sizeof(Meshlet)*uint64_t(meshletCount) +
                              sizeof(uint32_t)*(uint64_t(baryCount)+primCount);
    if(uint64_t(bytes) != expected)
      throw std::runtime_error("Invalid route contour section sizes: " + path.string());

    Contours data;
    const auto read = [&]<class T>(std::vector<T>& values, size_t count) {
      values.resize(count);
      if(count != 0 && !in.read(reinterpret_cast<char*>(values.data()), std::streamsize(count*sizeof(T))))
        throw std::runtime_error("Truncated route contour section");
      };
    const std::array counts{n1, n2, n3, n4};
    for(size_t i = 0; i < counts.size(); ++i) {
      read(data.faces[i], counts[i]);
      for(const auto& face : data.faces[i])
        if(face.chart >= chartCount)
          throw std::runtime_error("Route contour face chart is outside the cache");
      }
    read(data.shapes, shapeCount);
    read(data.meshlets, meshletCount);
    read(data.barycentrics, baryCount);
    read(data.primitives, primCount);

    for(const auto& shape : data.shapes)
      if(uint64_t(shape.firstMeshlet) + shape.meshletCount > meshletCount)
        throw std::runtime_error("Route contour chart range is invalid");
    for(const auto& mesh : data.meshlets) {
      if(mesh.vertexCount == 0 || mesh.vertexCount > 64 || mesh.primitiveCount == 0 || mesh.primitiveCount > 64 ||
         uint64_t(mesh.firstVertex) + mesh.vertexCount > baryCount ||
         uint64_t(mesh.firstPrimitive) + mesh.primitiveCount > primCount)
        throw std::runtime_error("Route contour meshlet range is invalid");
      for(uint32_t i = 0; i < mesh.primitiveCount; ++i) {
        const uint32_t v = data.primitives[mesh.firstPrimitive + i];
        if((v >> 24u) || (v & 255u) >= mesh.vertexCount || ((v >> 8u) & 255u) >= mesh.vertexCount ||
           ((v >> 16u) & 255u) >= mesh.vertexCount)
          throw std::runtime_error("Route contour primitive exceeds its meshlet");
        }
      }
    for(uint32_t w : data.barycentrics)
      if((w & 65535u) + (w >> 16u) > 65535u)
        throw std::runtime_error("Route contour barycentric weight is outside the source face");
    return data;
    }
  };

}

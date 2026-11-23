#include "islandterrain.h"

#include "islandnoise.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace island {

namespace {

constexpr float Pi = 3.14159265358979f;

constexpr float rad(float degrees) {
  return degrees*Pi/180.f;
  }

// --- the shore: a closed line around the origin, r = R(angle) -------------------------------------------------
// Land about 286 m across. The second harmonic stretches the island along a diagonal of the field, where the sea
// has room; along the axes the shore stays 40 m and more from the field's edge.
constexpr float ShoreRadius = 143.f;

// --- the shore profile, by the distance to the shoreline --------------------------------------------------------
constexpr float BeachSlope  = 0.0437f; // 2.5 degrees at the water: fine sand (1:23)
constexpr float MeadowSlope = 0.02f;
constexpr float BermRise    = 0.9f;    // the back of the beach, on top of the slopes: 1.7 m over the sea at 25 m
constexpr float SeaDeepen   = 0.0022f; // per metre squared: the bottom reaches -5 m 40 m out and levels off at -10 m

// --- the hill: off the centre, gentle towards the north-west, steep (rock) towards the south-east ---------------
constexpr float HillX = 40.f, HillZ = -30.f;
constexpr float HillHeight = 20.f;
constexpr float HillRadius = 57.f, HillRadiusSwing = 19.f; // 76 m on the gentle side, 38 m on the steep one
constexpr float HillGentle = rad(140.f);
// A wide low apron under the hill, shifted to the gentle side: the stream runs down its fall line.
constexpr float ApronX = HillX-13.8f, ApronZ = HillZ+11.6f;
constexpr float ApronHeight = 7.f, ApronRadius = 120.f;
constexpr float CragHeight = 1.6f, CragRadius = 10.f;

// --- the stream ---------------------------------------------------------------------------------------------------
constexpr float SpringAngle  = rad(118.f); // from the hill's centre
constexpr float SpringReach  = 0.86f;      // of the hill's radius: the foot of its slope
constexpr float BedHalf      = 0.7f;       // the flat bed
constexpr float BankHalf     = 1.4f;       // the top of the banks: a channel 2.8 m wide, water 2.3 m
constexpr float BankHeight   = 0.45f;
constexpr float ValleySlope  = 0.3f;       // the sides of the valley the stream has cut, outside the banks
constexpr float ValleyReach  = 16.f;       // metres from the centre line the valley may reach
constexpr float MeanderWave  = 29.f;       // metres: 12 channel widths (Leopold & Wolman: 10-14)
constexpr float MeanderSwing = 4.5f;
constexpr float MouthSlope   = 0.02f, SteepSlope = 0.065f;
constexpr float MouthBed     = -0.35f;     // at the shoreline: the sea fills the mouth
constexpr float RibbonHalf   = 1.25f;      // the water's ribbon reaches into the banks

// --- material borders ---------------------------------------------------------------------------------------------
constexpr float WetWidth   = 6.5f;   // metres of wet sand above the shoreline
constexpr float BeachWidth = 24.f;   // the grass starts here, give or take 4 m
constexpr float RockSlope  = 0.675f; // 34 degrees: the angle of repose

// Metres of the world per repeat of the material's texture.
constexpr float TileMetres[GroundCount] = {3.f, 3.f, 4.f, 5.f, 2.5f, 4.f, 4.f};

constexpr uint32_t SeedMeadow = 11, SeedHill = 23, SeedCrag = 37, SeedWet = 41, SeedBeach = 53, SeedRock = 67,
                   SeedCap = 71;

float smoothMin(float a, float b, float k) {
  const float h = std::max(k-std::abs(a-b), 0.f)/k;
  return std::min(a, b) - h*h*k*0.25f;
  }

float bump(float q) {
  if(q>=1.f)
    return 0.f;
  const float v = 1.f-q*q;
  return v*v;
  }

float prescribedSlope(float upstream) {
  return MouthSlope + (SteepSlope-MouthSlope)*smoothstep(30.f, 60.f, upstream);
  }

// A mesh in the making: triangles over shared points of the ground plane, cut by one field after another.
struct Mesher {
  std::vector<float>                   x, z;
  std::vector<std::array<uint32_t,3>>  tri;
  std::vector<uint8_t>                 side; // bit k: the triangle lies where field k is positive

  // Marching triangles: every triangle the zero line of the field crosses is split along it. A cut point belongs
  // to its edge, so both triangles at the edge get the same one and the mesh stays closed.
  template<class Field>
  void cut(unsigned bit, const Field& field) {
    std::vector<float> value(x.size());
    for(size_t i=0; i<x.size(); ++i)
      value[i] = field(x[i], z[i]);

    std::unordered_map<uint64_t,uint32_t> onEdge;
    auto crossing = [&](uint32_t negative, uint32_t positive) {
      const uint64_t key = uint64_t(std::min(negative, positive))<<32 | std::max(negative, positive);
      if(const auto it = onEdge.find(key); it!=onEdge.end())
        return it->second;
      const float ax = x[negative], az = z[negative], bx = x[positive], bz = z[positive];
      float ta = 0, tb = 1, fa = value[negative], fb = value[positive];
      float t = 0.5f;
      for(int pass=0; pass<4; ++pass) {
        t = fa!=fb ? ta + (tb-ta)*fa/(fa-fb) : 0.5f*(ta+tb);
        if(pass==3)
          break;
        const float f = field(ax+(bx-ax)*t, az+(bz-az)*t);
        if(f<0) {
          ta = t;
          fa = f;
          } else {
          tb = t;
          fb = f;
          }
        }
      // no slivers at the corners: the line may miss its level by 6 cm on a metre's edge
      t = std::min(0.94f, std::max(0.06f, t));
      const uint32_t id = uint32_t(x.size());
      x.push_back(ax+(bx-ax)*t);
      z.push_back(az+(bz-az)*t);
      onEdge.emplace(key, id);
      return id;
      };
    auto distance2 = [&](uint32_t a, uint32_t b) {
      const float dx = x[a]-x[b], dz = z[a]-z[b];
      return dx*dx+dz*dz;
      };

    std::vector<std::array<uint32_t,3>> tri2;
    std::vector<uint8_t>                side2;
    tri2.reserve(tri.size()+tri.size()/8);
    side2.reserve(tri.size()+tri.size()/8);
    const uint8_t mask = uint8_t(1u<<bit);
    for(size_t t=0; t<tri.size(); ++t) {
      const auto v = tri[t];
      const bool p[3] = {value[v[0]]>=0, value[v[1]]>=0, value[v[2]]>=0};
      const int  n    = int(p[0])+int(p[1])+int(p[2]);
      if(n==0 || n==3) {
        tri2.push_back(v);
        side2.push_back(uint8_t(side[t] | (n==3 ? mask : 0)));
        continue;
        }
      const bool lonePositive = n==1;
      unsigned lone = 0;
      for(unsigned c=0; c<3; ++c)
        if(p[c]==lonePositive)
          lone = c;
      const uint32_t a = v[lone], b = v[(lone+1)%3], c = v[(lone+2)%3];
      const uint32_t ab = lonePositive ? crossing(b, a) : crossing(a, b);
      const uint32_t ac = lonePositive ? crossing(c, a) : crossing(a, c);
      const uint8_t  loneSide  = uint8_t(side[t] | (lonePositive ? mask : 0));
      const uint8_t  otherSide = uint8_t(side[t] | (lonePositive ? 0 : mask));
      tri2.push_back({a, ab, ac});
      side2.push_back(loneSide);
      // the quad ab-b-c-ac by its shorter diagonal
      if(distance2(ab, c)<=distance2(b, ac)) {
        tri2.push_back({ab, b, c});
        tri2.push_back({ab, c, ac});
        } else {
        tri2.push_back({ab, b, ac});
        tri2.push_back({b, c, ac});
        }
      side2.push_back(otherSide);
      side2.push_back(otherSide);
      }
    tri  = std::move(tri2);
    side = std::move(side2);
    }
  };

enum Field : unsigned { FieldBank, FieldBed, FieldWet, FieldBeach, FieldRock };

}

const char* groundName(Ground ground) {
  switch(ground) {
    case SandDry:     return "ISLAND_SAND_DRY";
    case SandWet:     return "ISLAND_SAND_WET";
    case Grass:       return "ISLAND_GRASS";
    case Rock:        return "ISLAND_ROCK";
    case StreamBed:   return "ISLAND_STREAMBED";
    case SeaWater:    return "ISLAND_SEA";
    case StreamWater: return "ISLAND_STREAM";
    case GroundCount: break;
    }
  return "";
  }

float Terrain::shore(float x, float z) const {
  const float r = std::sqrt(x*x+z*z);
  const float a = std::atan2(z, x);
  const float radius = ShoreRadius + 10.f*std::cos(2.f*a-0.5f*Pi) + 4.f*std::cos(3.f*a+0.6f) +
                       2.5f*std::cos(5.f*a+2.1f) + 1.5f*std::cos(7.f*a+4.f);
  const float turn   = -20.f*std::sin(2.f*a-0.5f*Pi) - 12.f*std::sin(3.f*a+0.6f) -
                       12.5f*std::sin(5.f*a+2.1f) - 10.5f*std::sin(7.f*a+4.f);
  const float lean   = turn/radius; // the shoreline against the circle through the point
  return (radius-r)/std::sqrt(1.f+lean*lean);
  }

float Terrain::base(float x, float z) const {
  // far inland the profile is level, so the angle's seam at the origin does not show
  const float s = std::min(shore(x, z), 110.f);
  if(s<0) {
    const float u = -s;
    const float v = BeachSlope*u + SeaDeepen*u*u;
    return -(v<8.f ? v : 8.f+2.f*(1.f-std::exp(-(v-8.f)/2.f)));
    }
  float y = MeadowSlope*60.f*(1.f-std::exp(-s/60.f)) + (BeachSlope-MeadowSlope)*22.f*(1.f-std::exp(-s/22.f)) +
            BermRise*smoothstep(9.f, 25.f, s);
  const float inland = smoothstep(26.f, 60.f, s);
  if(inland<=0)
    return y;

  const float hx = x-HillX, hz = z-HillZ;
  const float hr = std::sqrt(hx*hx+hz*hz);
  const float radius = HillRadius + HillRadiusSwing*std::cos(std::atan2(hz, hx)-HillGentle);
  // noise bends the contour lines: not a cone
  const float q = hr/radius*(1.f+0.1f*valueNoise(x/35.f, z/35.f, 0, SeedHill));
  const float ax = x-ApronX, az = z-ApronZ;
  float rise = HillHeight*bump(q) + ApronHeight*bump(std::sqrt(ax*ax+az*az)/ApronRadius);
  rise += CragHeight*bump(hr/CragRadius)*(0.6f+0.4f*valueNoise(x/4.f, z/4.f, 0, SeedCrag));
  rise += 0.5f*fbm(x/45.f, z/45.f, 0, SeedMeadow, 3);
  return y + inland*rise;
  }

float Terrain::slope(float x, float z) const {
  const float dx = (base(x+1.f, z)-base(x-1.f, z))*0.5f;
  const float dz = (base(x, z+1.f)-base(x, z-1.f))*0.5f;
  return std::sqrt(dx*dx+dz*dz);
  }

Terrain::Foot Terrain::foot(float x, float z) const {
  Foot ret;
  if(x<streamMin[0] || x>streamMax[0] || z<streamMin[1] || z>streamMax[1])
    return ret;
  float best = 1e18f;
  for(size_t i=0; i+1<centre.size(); ++i) {
    const auto& a = centre[i];
    const auto& b = centre[i+1];
    const float ex = b.x-a.x, ez = b.z-a.z;
    const float t  = saturate(((x-a.x)*ex+(z-a.z)*ez)/(ex*ex+ez*ez));
    const float dx = x-(a.x+ex*t), dz = z-(a.z+ez*t);
    const float d2 = dx*dx+dz*dz;
    if(d2<best) {
      best     = d2;
      ret.bed  = mix(a.bed, b.bed, t);
      ret.bank = mix(a.bank, b.bank, t);
      }
    }
  ret.dist = std::sqrt(best);
  return ret;
  }

float Terrain::height(float x, float z) const {
  const float ground = base(x, z);
  const Foot  f      = foot(x, z);
  if(f.dist>=ValleyReach)
    return ground;
  const float valley = f.bed + f.bank*smoothstep(BedHalf, BankHalf, f.dist) +
                       std::max(0.f, f.dist-BankHalf)*ValleySlope;
  // towards its reach the valley gives way to the ground, whatever the cross slope
  return mix(smoothMin(ground, valley, 0.2f), ground, smoothstep(ValleyReach-6.f, ValleyReach, f.dist));
  }

float Terrain::field(unsigned id, float x, float z) const {
  switch(id) {
    case FieldBank:
      return foot(x, z).dist-BankHalf;
    case FieldBed:
      return foot(x, z).dist-BedHalf;
    case FieldWet:
      return shore(x, z) - (WetWidth+1.2f*valueNoise(x/9.f, z/9.f, 0, SeedWet));
    case FieldBeach:
      return shore(x, z) - (BeachWidth+4.f*fbm(x/20.f, z/20.f, 0, SeedBeach, 2));
    case FieldRock: {
      if(shore(x, z)<45.f)
        return -1.f;
      // steeper than sand can lie, and the top of the hill
      const float steep = (slope(x, z)-RockSlope)*10.f + 0.5f*valueNoise(x/7.f, z/7.f, 0, SeedRock);
      const float hx = x-HillX, hz = z-HillZ;
      const float cap = 1.f - std::sqrt(hx*hx+hz*hz)/9.f + 0.35f*valueNoise(x/3.5f, z/3.5f, 0, SeedCap);
      return std::max(steep, cap);
      }
    }
  return 0;
  }

Terrain::Terrain() {
  // The centre line: from the spring at the foot of the hill's slope down the apron's fall line to the sea, with
  // a slight bow and meanders that fade towards both ends.
  const float reach = SpringReach*(HillRadius + HillRadiusSwing*std::cos(SpringAngle-HillGentle));
  const float sx = HillX+reach*std::cos(SpringAngle), sz = HillZ+reach*std::sin(SpringAngle);
  float dx = sx-ApronX, dz = sz-ApronZ;
  const float dl = std::sqrt(dx*dx+dz*dz);
  dx /= dl;
  dz /= dl;
  float toShore = 0;
  while(shore(sx+dx*toShore, sz+dz*toShore)>0 && toShore<400.f)
    toShore += 0.25f;
  const float length = toShore+9.f; // the channel runs out on the sea bottom
  auto at = [&](float u, float& px, float& pz) {
    const float side = 6.f*std::sin(Pi*u/length) +
                       MeanderSwing*std::sin(2.f*Pi*u/MeanderWave)*smoothstep(0.f, 14.f, u)*
                       (1.f-smoothstep(toShore-34.f, toShore-8.f, u));
    px = sx + dx*u - dz*side;
    pz = sz + dz*u + dx*side;
    };
  // a point every metre of the line's own length
  {
    float px = 0, pz = 0, lx = 0, lz = 0, run = 0;
    at(0, lx, lz);
    centre.push_back({lx, lz, 0, 0});
    for(float u=0.02f; u<=length; u+=0.02f) {
      at(u, px, pz);
      run += std::sqrt((px-lx)*(px-lx)+(pz-lz)*(pz-lz));
      lx = px;
      lz = pz;
      if(run>=1.f) {
        centre.push_back({px, pz, 0, 0});
        run -= 1.f;
        }
      }
  }
  const size_t n = centre.size();
  if(n<40)
    throw std::runtime_error("island: the stream is too short");

  // metres upstream of the shoreline, negative in the sea
  size_t mouth = n-1;
  while(mouth>0 && shore(centre[mouth].x, centre[mouth].z)<0)
    --mouth;
  // The bed: a profile that only descends (2 % at the mouth, 6.5 % on the slope), lowered wherever the ground at
  // the banks lies lower still, and never rising again downstream.
  std::vector<float> prescribed(n);
  for(size_t i=n; i-->0; ) {
    const float upstream = float(mouth)-float(i);
    if(upstream<=0)
      prescribed[i] = MouthBed+0.01f*upstream;
    else
      prescribed[i] = prescribed[i+1]+prescribedSlope(upstream-0.5f);
    }
  float bed = 1e9f;
  for(size_t i=0; i<n; ++i) {
    auto& p = centre[i];
    const auto& before = centre[i>0 ? i-1 : 0];
    const auto& after  = centre[i+1<n ? i+1 : n-1];
    float tx = after.x-before.x, tz = after.z-before.z;
    const float tl = std::sqrt(tx*tx+tz*tz);
    tx /= tl;
    tz /= tl;
    p.bank = BankHeight*smoothstep(-7.f, -1.f, float(mouth)-float(i));
    const float lowest = std::min(base(p.x, p.z),
                                  std::min(base(p.x-tz*1.5f, p.z+tx*1.5f), base(p.x+tz*1.5f, p.z-tx*1.5f)));
    bed   = std::min(bed, std::min(prescribed[i], lowest-p.bank));
    p.bed = bed;
    }

  streamMin[0] = streamMin[1] = 1e9f;
  streamMax[0] = streamMax[1] = -1e9f;
  for(const auto& p:centre) {
    streamMin[0] = std::min(streamMin[0], p.x-ValleyReach);
    streamMin[1] = std::min(streamMin[1], p.z-ValleyReach);
    streamMax[0] = std::max(streamMax[0], p.x+ValleyReach);
    streamMax[1] = std::max(streamMax[1], p.z+ValleyReach);
    }
  // the valley must close within its reach: the ground beyond it is not cut
  for(const auto& p:centre)
    if(base(p.x, p.z)-(p.bed+p.bank) > (ValleyReach-BankHalf-2.f)*ValleySlope)
      throw std::runtime_error("island: the stream's valley is deeper than its reach");
  }

asset::WorldMesh Terrain::mesh() const {
  const int side = int(2.f*FieldHalf/GridStep);
  const int row  = side+1;
  Mesher m;
  m.x.reserve(size_t(row)*size_t(row)*5/4);
  m.z.reserve(size_t(row)*size_t(row)*5/4);
  for(int j=0; j<row; ++j)
    for(int i=0; i<row; ++i) {
      m.x.push_back(-FieldHalf+float(i)*GridStep);
      m.z.push_back(-FieldHalf+float(j)*GridStep);
      }
  m.tri.reserve(size_t(side)*size_t(side)*2);
  for(int j=0; j<side; ++j)
    for(int i=0; i<side; ++i) {
      const uint32_t a = uint32_t(j*row+i), b = a+1, c = a+uint32_t(row), d = c+1;
      // the engine's front face: clockwise seen from above. Diagonals alternate, so no direction is preferred.
      if(((i+j)&1)==0) {
        m.tri.push_back({a, b, c});
        m.tri.push_back({b, d, c});
        } else {
        m.tri.push_back({a, b, d});
        m.tri.push_back({a, d, c});
        }
      }
  m.side.assign(m.tri.size(), 0);
  for(unsigned id:{unsigned(FieldBank), unsigned(FieldBed), unsigned(FieldWet), unsigned(FieldBeach),
                   unsigned(FieldRock)})
    m.cut(id, [&](float x, float z) { return field(id, x, z); });

  asset::WorldMesh out;
  out.name = "ISLAND.ZEN";
  for(uint32_t g=0; g<GroundCount; ++g) {
    asset::Material mat;
    mat.name    = groundName(Ground(g));
    mat.texture = mat.name+".TGA";
    mat.color   = {255, 255, 255, 255};
    const bool water = g==SeaWater || g==StreamWater;
    mat.group      = water ? asset::MaterialGroup::WATER :
                     (g==Rock || g==StreamBed) ? asset::MaterialGroup::STONE : asset::MaterialGroup::EARTH;
    mat.alpha_func = water ? asset::AlphaFunction::BLEND : asset::AlphaFunction::NONE;
    out.materials.push_back(std::move(mat));
    }

  // every point of the ground lies on the height function, the cut points too: the banks follow the stream's own
  // lines, not the grid's
  const size_t groundPoints = m.x.size();
  std::vector<float>       y(groundPoints);
  std::vector<asset::Vec3> normal(groundPoints);
  for(size_t i=0; i<groundPoints; ++i) {
    const float x = m.x[i], z = m.z[i];
    y[i] = height(x, z);
    const float e  = 0.25f;
    const float gx = (height(x+e, z)-height(x-e, z))/(2.f*e);
    const float gz = (height(x, z+e)-height(x, z-e))/(2.f*e);
    const float l  = std::sqrt(gx*gx+gz*gz+1.f);
    normal[i] = {-gx/l, 1.f/l, -gz/l};
    out.vertices.push_back({x*100.f, y[i]*100.f, z*100.f});
    }

  std::unordered_map<uint64_t,uint32_t> features;
  auto feature = [&](uint32_t vertex, uint32_t ground, const asset::Vec3& nrm) {
    const uint64_t key = uint64_t(vertex)<<8 | ground;
    if(const auto it = features.find(key); it!=features.end())
      return it->second;
    asset::VertexFeature f;
    const auto& v = out.vertices[vertex];
    f.texture = {v.x*0.01f/TileMetres[ground], v.z*0.01f/TileMetres[ground]};
    f.light   = 0xFFFFFFFF;
    f.normal  = nrm;
    const uint32_t id = uint32_t(out.features.size());
    out.features.push_back(f);
    features.emplace(key, id);
    return id;
    };
  auto triangle = [&](uint32_t ground, uint32_t a, uint32_t b, uint32_t c, const asset::Vec3* nrm) {
    for(uint32_t v:{a, b, c}) {
      out.polygons.vertex_indices.push_back(v);
      out.polygons.feature_indices.push_back(feature(v, ground, nrm!=nullptr ? *nrm : normal[v]));
      }
    out.polygons.material_indices.push_back(ground);
    out.polygons.lightmap_indices.push_back(-1);
    out.polygons.flags.push_back({});
    };

  for(size_t t=0; t<m.tri.size(); ++t) {
    const uint8_t s = m.side[t];
    auto positive = [&](Field f) { return (s & (1u<<f))!=0; };
    const Ground ground = !positive(FieldBank)  ? StreamBed :
                          positive(FieldRock)   ? Rock :
                          !positive(FieldWet)   ? SandWet :
                          !positive(FieldBeach) ? SandDry : Grass;
    triangle(ground, m.tri[t][0], m.tri[t][1], m.tri[t][2], nullptr);
    }

  const asset::Vec3 up = {0, 1, 0};
  auto point = [&](float x, float level, float z) {
    out.vertices.push_back({x*100.f, level*100.f, z*100.f});
    return uint32_t(out.vertices.size()-1);
    };

  // The sea: a level sheet in 8 m cells wherever the ground of the cell dips under it.
  {
    const int cell  = 8;
    const int cells = side/cell;
    std::vector<uint32_t> corner(size_t(cells+1)*size_t(cells+1), 0xFFFFFFFF);
    auto cornerAt = [&](int i, int j) {
      auto& id = corner[size_t(j)*size_t(cells+1)+size_t(i)];
      if(id==0xFFFFFFFF)
        id = point(-FieldHalf+float(i*cell)*GridStep, 0.f, -FieldHalf+float(j*cell)*GridStep);
      return id;
      };
    for(int j=0; j<cells; ++j)
      for(int i=0; i<cells; ++i) {
        float lowest = 1e9f;
        for(int v=0; v<=cell; ++v)
          for(int u=0; u<=cell; ++u)
            lowest = std::min(lowest, y[size_t(j*cell+v)*size_t(row)+size_t(i*cell+u)]);
        if(lowest>=0.05f)
          continue;
        const uint32_t a = cornerAt(i, j), b = cornerAt(i+1, j), c = cornerAt(i, j+1), d = cornerAt(i+1, j+1);
        triangle(SeaWater, a, b, c, &up);
        triangle(SeaWater, b, d, c, &up);
        }
  }

  // The stream: a ribbon over the bed from the spring's pool down to where the sea takes over.
  {
    const size_t n = centre.size();
    auto level = [&](size_t i) { return centre[i].bed + StreamWaterDepth*centre[i].bank/BankHeight; };
    size_t end = 0;
    while(end+1<n && level(end+1)>0.03f)
      ++end;
    std::vector<std::array<uint32_t,3>> ribbon(end+1);
    float leftX = 0, leftZ = 0, backX = 0, backZ = 0;
    for(size_t i=0; i<=end; ++i) {
      const auto& before = centre[i>0 ? i-1 : 0];
      const auto& after  = centre[i+1<n ? i+1 : n-1];
      float tx = after.x-before.x, tz = after.z-before.z;
      const float tl = std::sqrt(tx*tx+tz*tz);
      tx /= tl;
      tz /= tl;
      if(i==0) {
        leftX = tz;
        leftZ = -tx;
        backX = -tx;
        backZ = -tz;
        }
      const auto& p = centre[i];
      ribbon[i] = {point(p.x+tz*RibbonHalf, level(i), p.z-tx*RibbonHalf), point(p.x, level(i), p.z),
                   point(p.x-tz*RibbonHalf, level(i), p.z+tx*RibbonHalf)};
      }
    // clockwise seen from above, as the ground
    auto wound = [&](uint32_t a, uint32_t b, uint32_t c) {
      const auto& va = out.vertices[a];
      const auto& vb = out.vertices[b];
      const auto& vc = out.vertices[c];
      const float turn = (vb.z-va.z)*(vc.x-va.x) - (vb.x-va.x)*(vc.z-va.z);
      if(turn<0)
        triangle(StreamWater, a, b, c, &up);
      else
        triangle(StreamWater, a, c, b, &up);
      };
    for(size_t i=0; i<end; ++i)
      for(unsigned k=0; k<2; ++k) {
        wound(ribbon[i][k], ribbon[i][k+1], ribbon[i+1][k]);
        wound(ribbon[i][k+1], ribbon[i+1][k+1], ribbon[i+1][k]);
        }
    // the pool at the spring: half a disc behind the first point, from one edge round the back to the other
    const auto& s = centre[0];
    const int fan = 8;
    uint32_t last = ribbon[0][0];
    for(int k=1; k<=fan; ++k) {
      uint32_t next = ribbon[0][2];
      if(k<fan) {
        const float a = Pi*float(k)/float(fan);
        const float c = std::cos(a)*RibbonHalf, b = std::sin(a)*RibbonHalf;
        next = point(s.x+leftX*c+backX*b, level(0), s.z+leftZ*c+backZ*b);
        }
      wound(ribbon[0][1], last, next);
      last = next;
      }
  }

  out.bbox.min = asset::Vec3(1e9f);
  out.bbox.max = asset::Vec3(-1e9f);
  for(const auto& v:out.vertices)
    for(unsigned c=0; c<3; ++c) {
      out.bbox.min[c] = std::min(out.bbox.min[c], v[c]);
      out.bbox.max[c] = std::max(out.bbox.max[c], v[c]);
      }
  return out;
  }

const Terrain& terrain() {
  static const Terrain instance;
  return instance;
  }

}

#include "islandtextures.h"

#include "islandimage.h"
#include "islandnoise.h"
#include "islandterrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace island {

namespace {

constexpr float Tau = 6.28318530718f;

// Noise over `cells` lattice cells a tile: it wraps where the tile does.
float tiled(float u, float v, int cells, uint32_t seed) {
  return valueNoise(u*float(cells), v*float(cells), cells, seed);
  }

float tiledFbm(float u, float v, int cells, uint32_t seed, int octaves, float gain = 0.5f) {
  return fbm(u*float(cells), v*float(cells), cells, seed, octaves, gain);
  }

Cells tiledCells(float u, float v, int cells, uint32_t seed) {
  return cellNoise(u*float(cells), v*float(cells), cells, seed);
  }

struct Rgb {
  float r = 0, g = 0, b = 0;
  };

Rgb blend(const Rgb& a, const Rgb& b, float t) {
  return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t)};
  }

Rgb shade(const Rgb& a, float k) {
  return {a.r*k, a.g*k, a.b*k};
  }

// One texel of a material: its relief in metres, linear albedo, roughness and occlusion.
struct Texel {
  float height = 0;
  Rgb   albedo;
  float rough = 1;
  float ao    = 1;
  };

// --- sand: wind ripples 12 cm apart, grain, pebbles and shell fragments; a 3 m tile --------------------------------
Texel sand(float u, float v, bool wet) {
  const float large  = tiledFbm(u, v, 2, 101, 3);
  const float tint   = tiledFbm(u, v, 3, 113, 2);
  const float warp   = tiledFbm(u, v, 3, 127, 3);
  const float patch  = smoothstep(-0.35f, 0.35f, tiledFbm(u, v, 4, 131, 2));
  const float phase  = Tau*(24.f*v + 1.3f*warp + 0.3f*tiled(u, v, 9, 137));
  const float ripple = 0.5f + 0.4f*std::sin(phase) + 0.1f*std::sin(2.f*phase+0.9f);
  // ripples stand in patches and lie flat between them; the sea has smoothed the wet strip
  const float amp    = (wet ? 0.45f : 1.f)*mix(0.3f, 1.f, patch);
  const float grain  = 0.6f*tiled(u, v, 384, 139) + 0.4f*tiled(u, v, 160, 149);

  Texel t;
  t.height = 0.012f*amp*ripple + 0.0008f*grain;
  t.albedo = wet ? Rgb{0.27f, 0.21f, 0.14f} : Rgb{0.50f, 0.40f, 0.27f};
  t.albedo.r *= 1.f+0.03f*tint;
  t.albedo.b *= 1.f-0.05f*tint;
  t.albedo = shade(t.albedo, 1.f + 0.045f*large + 0.05f*grain + 0.07f*amp*(ripple-0.5f));
  t.rough  = wet ? 0.5f+0.06f*large : 0.95f;
  t.ao     = 1.f - 0.2f*amp*(1.f-ripple);

  // pebbles and shells of 3-7 cm, in a quarter of the 23 cm cells
  const Cells pebble = tiledCells(u, v, 13, 151);
  if((pebble.id & 0xffU)<60U) {
    const uint32_t kind   = hash32(pebble.id ^ 0x5bd1e995U);
    const float    radius = 0.06f + 0.10f*hashUnit(kind);
    const float    d      = pebble.f1/radius;
    if(d<1.f) {
      const float dome  = std::sqrt(1.f-d*d);
      const bool  shell = (kind & 3U)==0U;
      const float tone  = 0.75f + 0.5f*hashUnit(hash32(kind));
      const Rgb   stone = shell ? Rgb{0.62f, 0.56f, 0.46f} : Rgb{0.26f, 0.24f, 0.22f};
      t.height += 0.07f*radius*dome;
      t.albedo  = blend(t.albedo, shade(stone, (wet ? 0.6f : 1.f)*tone), smoothstep(0.f, 0.25f, dome));
      t.rough   = wet ? 0.35f : 0.7f;
      t.ao      = 0.7f+0.3f*dome;
      } else if(d<1.4f) {
      t.ao *= 0.72f + 0.28f*(d-1.f)/0.4f; // the stone's foot
      }
    }
  // specks of a centimetre: dark grains and shell crumbs
  const Cells speck = tiledCells(u, v, 61, 157);
  if((speck.id & 0xffU)<70U) {
    const uint32_t kind = hash32(speck.id ^ 0x2545f491U);
    const float    d    = speck.f1/(0.10f + 0.12f*hashUnit(kind));
    if(d<1.f) {
      const Rgb crumb = (kind & 1U)!=0U ? Rgb{0.15f, 0.12f, 0.10f} : Rgb{0.66f, 0.62f, 0.54f};
      t.albedo = blend(t.albedo, shade(crumb, wet ? 0.6f : 1.f), 0.8f*(1.f-d*d));
      t.height += 0.002f*(1.f-d*d);
      }
    }
  return t;
  }

Texel sandDry(float u, float v) { return sand(u, v, false); }
Texel sandWet(float u, float v) { return sand(u, v, true); }

// --- grass: blades as short streaks in eight directions, tufts, dry and dark patches, a flower here and there;
// a 4 m tile ---------------------------------------------------------------------------------------------------------
Texel grass(float u, float v) {
  // A streak is fine along `a` and long along `b`. Whole numbers of u and v in `a` keep it wrapping at the edge.
  struct Lay {
    float au, av; // a = au*u + av*v
    bool  alongU; // b = u, else v
    int   fine, coarse;
    };
  static const Lay lays[] = {
    {1.f, 0.f, false, 250, 30}, {0.f, 1.f, true, 250, 30},  {1.f, 1.f, false, 176, 40},  {1.f, -1.f, false, 176, 40},
    {2.f, 1.f, false, 112, 34}, {1.f, -2.f, true, 112, 34}, {1.f, 2.f, true, 112, 34},   {2.f, -1.f, false, 112, 34},
    };
  float blades = 0;
  uint32_t seed = 211;
  for(const auto& lay:lays) {
    const float a = lay.au*u + lay.av*v;
    const float b = lay.alongU ? u : v;
    const float streak = valueNoise(a*float(lay.fine), b*float(lay.coarse), lay.fine, lay.coarse, seed);
    // every direction leads in patches of its own
    const float lead = 0.55f + 0.45f*tiled(u, v, 7, seed+1000U);
    blades = std::max(blades, lead*smoothstep(-0.05f, 0.65f, streak));
    seed += 17U;
    }
  // The hexagons of `antitile` are smaller than the tile (GrassCell): each shows a part of it, so the patches below
  // fall into them by chance. A patch as wide as the tile would come once to a hexagon and draw their lattice.
  const float large = tiledFbm(u, v, 2, 307, 3);
  const float mid   = tiledFbm(u, v, 6, 311, 3);
  const Cells tuft  = tiledCells(u, v, 9, 313);
  const float dome  = 1.f - smoothstep(0.15f, 0.6f, tuft.f1);
  const float dry   = 0.38f*smoothstep(0.12f, 0.5f, tiledFbm(u, v, 6, 317, 3, 0.6f));
  const float lush  = smoothstep(0.1f, 0.5f, tiledFbm(u, v, 5, 337, 3));

  Texel t;
  t.height = 0.035f*blades + 0.03f*dome;
  t.albedo = blend({0.085f, 0.17f, 0.032f}, {0.19f, 0.33f, 0.08f}, blades);
  t.albedo = blend(t.albedo, {t.albedo.r*0.8f, t.albedo.g*0.9f, t.albedo.b*0.95f}, lush);
  t.albedo = blend(t.albedo, shade({0.31f, 0.29f, 0.12f}, 0.6f+0.4f*blades), dry);
  t.albedo = shade(t.albedo, 1.f + 0.05f*large + 0.03f*mid + 0.05f*(dome-0.4f));
  t.rough  = 0.88f + 0.05f*(blades-0.5f);
  t.ao     = 0.55f + 0.45f*std::max(blades, 0.8f*dome);

  // a flower in one cell of sixteen: daisy white or buttercup yellow, 2-3 cm
  const Cells flower = tiledCells(u, v, 34, 331);
  if((flower.id & 0xfU)==0U) {
    const uint32_t kind = hash32(flower.id ^ 0x68e31da4U);
    const float    d    = flower.f1/(0.09f + 0.06f*hashUnit(kind));
    if(d<1.f) {
      const Rgb petal = (kind & 1U)!=0U ? Rgb{0.72f, 0.72f, 0.62f} : Rgb{0.70f, 0.52f, 0.06f};
      t.albedo = blend(t.albedo, petal, 1.f-d*d*d);
      t.height += 0.01f;
      }
    }
  return t;
  }

// --- rock: weathered beds with broken fractures, pitted relief, lichen and stains; a 5 m tile ----------------------
Texel rock(float u, float v) {
  // the beds and the fractures bend with a warp that wraps as the tile does
  const float wu = u + 0.05f*tiledFbm(u, v, 3, 401, 3);
  const float wv = v + 0.05f*tiledFbm(u, v, 3, 409, 3);
  // A fracture is the zero line of a noise: long and winding, and it dies out where the second noise is low. A net
  // of closed cells reads as a turtle's shell on a boulder.
  const float lineA  = std::abs(tiledFbm(wu, wv, 3, 419, 4));
  const float liveA  = smoothstep(-0.15f, 0.2f, tiledFbm(u, v, 4, 421, 2));
  const float lineB  = std::abs(tiledFbm(wu, wv, 7, 423, 3));
  const float liveB  = smoothstep(0.f, 0.3f, tiledFbm(u, v, 5, 427, 2));
  const float crack  = 1.f - liveA*(1.f-smoothstep(0.f, 0.028f, lineA));
  const float fine   = 1.f - liveB*(1.f-smoothstep(0.f, 0.035f, lineB));
  const float relief = tiledFbm(u, v, 8, 431, 5, 0.55f);
  const float pits   = tiledFbm(u, v, 72, 433, 2);
  // beds of 30-60 cm with a step at each: the phase runs fast across a step and slowly over the bed's face
  const float bed    = 9.f*wv + 0.8f*tiledFbm(u, v, 4, 439, 3);
  const float face   = bed-std::floor(bed);
  const float step   = smoothstep(0.f, 0.12f, face)*(1.f-smoothstep(0.88f, 1.f, face));
  const float tone   = tiledFbm(u, v, 3, 441, 3);
  const float lichen = 0.55f*smoothstep(0.2f, 0.5f, tiledFbm(u, v, 7, 443, 4))*smoothstep(-0.2f, 0.3f, relief);
  const float stain  = 0.2f*smoothstep(0.15f, 0.6f, tiledFbm(u, v, 4, 449, 3));

  Texel t;
  t.height = 0.05f*(0.5f+0.5f*relief) + 0.02f*tone + 0.004f*pits + 0.014f*step*face - 0.04f*(1.f-crack) -
             0.012f*(1.f-fine);
  t.albedo = shade({0.31f, 0.285f, 0.25f}, 1.f + 0.07f*relief + 0.1f*tone + 0.05f*(face-0.5f) + 0.04f*pits);
  t.albedo = blend(t.albedo, {0.37f, 0.38f, 0.25f}, lichen);
  t.albedo = shade(t.albedo, (1.f-stain)*(0.55f+0.45f*crack)*(0.82f+0.18f*fine)*(0.85f+0.15f*step));
  t.rough  = mix(0.92f, 0.76f, crack) + 0.04f*pits;
  t.ao     = (0.55f+0.45f*crack)*(0.82f+0.18f*fine)*(0.9f+0.1f*smoothstep(-0.5f, 0.3f, relief));
  return t;
  }

// --- the stream's bed: rounded stones of 15-27 cm lying apart on pebbles and dark sand, all wet; a 2.5 m tile ------
Texel streamBed(float u, float v) {
  const Cells pebble = tiledCells(u, v, 37, 509);
  const float small  = smoothstep(0.02f, 0.25f, pebble.f2-pebble.f1)*(1.f - 0.5f*std::min(1.f, pebble.f1*2.f));
  const float speck  = tiled(u, v, 128, 521);
  const float silt   = tiledFbm(u, v, 5, 523, 3);
  const Rgb   gravel = shade({0.19f, 0.17f, 0.14f}, 0.8f + 0.4f*hashUnit(pebble.id));

  Texel t;
  t.height = 0.012f*small;
  t.albedo = shade(gravel, (0.72f+0.28f*small)*(1.f+0.12f*silt));
  t.rough  = 0.6f;
  t.ao     = 0.7f+0.3f*small;

  // a stone in three cells of five, of its own size: a paving of equal stones is a road, not a stream
  const Cells stone = tiledCells(u, v, 9, 503);
  const uint32_t kind = hash32(stone.id ^ 0x1b873593U);
  if(kind%5U<3U) {
    const float radius = 0.27f + 0.2f*hashUnit(hash32(kind ^ 0x51ed270bU));
    // not past the cell's border: the stone ends before its neighbour begins
    const float d = std::max(stone.f1/radius, 1.f-(stone.f2-stone.f1)/0.12f);
    if(d<1.f) {
      const float dome = std::sqrt(1.f-d*d);
      // most stones grey, a fifth of them rust brown
      const Rgb grey = blend({0.15f, 0.145f, 0.14f}, {0.25f, 0.235f, 0.21f}, hashUnit(kind));
      const Rgb body = (kind & 7U)<2U ? Rgb{0.24f, 0.17f, 0.11f} : grey;
      const float on = smoothstep(0.f, 0.3f, dome);
      t.height = std::max(t.height, 0.07f*radius/0.47f*std::min(1.f, dome*1.5f));
      t.albedo = blend(t.albedo, shade(body, 1.f+0.07f*speck+0.08f*silt), on);
      t.rough  = mix(t.rough, 0.4f, on);
      t.ao     = mix(t.ao, 0.75f+0.25f*dome, on);
      } else if(d<1.25f) {
      t.ao *= 0.75f + 0.25f*(d-1.f)/0.25f; // the stone's foot
      }
    }
  return t;
  }

// --- wood: four boards across u, the grain along v, seams, knots, sun-bleached streaks; a 1 m tile ----------------
Texel wood(float u, float v) {
  const float across = u*4.f;
  const float board  = std::floor(across);
  const float bu     = across-board;
  const uint32_t id  = hash32(uint32_t(int32_t(board))*0x27d4eb2fU + 601U);
  // every board is cut from its own place of the log: the grain is shifted by whole lattice cells along v
  const float shift  = float(id & 7U);
  const float wave   = 0.012f*valueNoise(across*3.f, v*3.f+shift, 12, 3, 607);
  float grain = 0, norm = 0, amp = 1;
  int fine = 96, coarse = 5;
  for(int o=0; o<3; ++o) {
    grain += amp*valueNoise((u+wave)*float(fine), (v+shift)*float(coarse), fine, coarse, 613U+uint32_t(o)*29U);
    norm  += amp;
    amp   *= 0.55f;
    fine  *= 2;
    coarse *= 2;
    }
  grain /= norm;
  const float seam  = smoothstep(0.f, 0.035f, std::min(bu, 1.f-bu));
  const float tone  = hashUnit(hash32(id))-0.5f;
  const float grey  = 0.35f*smoothstep(-0.2f, 0.5f, valueNoise(across*2.f, v*2.f+shift, 8, 2, 631));

  Texel t;
  t.height = 0.003f*grain - 0.008f*(1.f-seam);
  t.albedo = shade({0.26f, 0.17f, 0.10f}, 1.f + 0.3f*tone + 0.2f*grain);
  t.albedo = blend(t.albedo, shade({0.25f, 0.23f, 0.20f}, 1.f+0.15f*grain), grey);
  t.rough  = 0.8f + 0.05f*grain;
  t.ao     = 0.45f + 0.55f*seam;

  // a knot in a third of the boards: dark heart, the grain's rings around it
  if((id & 0x300U)==0U) {
    const float ku = 0.3f + 0.4f*hashUnit(hash32(id ^ 0x9e3779b9U));
    const float kv = hashUnit(hash32(id ^ 0x7f4a7c15U));
    float dv = v-kv;
    dv -= std::round(dv);
    const float du = (bu-ku)*0.25f;
    const float d  = std::sqrt(du*du + dv*dv*0.3f)/0.022f;
    if(d<1.6f) {
      const float ring = 0.5f+0.5f*std::sin(d*14.f);
      t.albedo = shade(t.albedo, mix(0.4f + 0.2f*ring, 1.f, smoothstep(0.6f, 1.6f, d)));
      t.height -= 0.002f*(1.f-smoothstep(0.f, 1.f, d));
      }
    }
  t.albedo = shade(t.albedo, 0.55f+0.45f*seam);
  return t;
  }

struct Spec {
  const char* name;       // the key of materials.json without ".TGA"
  const char* file;       // the stem of its maps
  int         size;
  float       tile;       // metres a repeat
  Texel     (*pattern)(float u, float v);
  float       luminance;  // the mean linear albedo to reach: the table of the note
  float       spread;     // how far a quarter of the tile may stray from it
  const char* antitile;
  float       cell;       // the hexagon of `antitile`, tiles across
  };

// Brightness wanders at most 10 % over the ground seen from above (the note's table). The stream's bed is a strip
// 1.4 m wide of stones of different rock, and the wood is boards of different trees: a fifth for them.
const Spec Specs[] = {
  {"ISLAND_SAND_DRY",  "sand_dry",  1024, 3.f,  sandDry,   0.40f, 0.10f, "offset", 0.62f},
  {"ISLAND_SAND_WET",  "sand_wet",  1024, 3.f,  sandWet,   0.22f, 0.10f, "offset", 0.62f},
  {"ISLAND_GRASS",     "grass",     1024, 4.f,  grass,     0.22f, 0.10f, "rotate", 0.62f},
  {"ISLAND_ROCK",      "rock",      1024, 5.f,  rock,      0.27f, 0.10f, "rotate", 0.62f},
  {"ISLAND_STREAMBED", "streambed", 1024, 2.5f, streamBed, 0.16f, 0.20f, "rotate", 0.62f},
  {"ISLAND_WOOD",      "wood",      512,  1.f,  wood,      0.16f, 0.20f, "off",    1.f},
  };

MaterialNote writeMaps(const std::filesystem::path& dir, const Spec& spec) {
  const int n = spec.size;
  Image albedo(n, n), normal(n, n), orm(n, n);
  std::vector<float> height(size_t(n)*size_t(n));
  parallelRows(n, [&](int y) {
    for(int x=0; x<n; ++x) {
      const Texel t = spec.pattern((float(x)+0.5f)/float(n), (float(y)+0.5f)/float(n));
      height[size_t(y)*size_t(n)+size_t(x)] = t.height;
      float* a = albedo.at(x, y);
      a[0] = t.albedo.r;
      a[1] = t.albedo.g;
      a[2] = t.albedo.b;
      a[3] = 1.f;
      float* o = orm.at(x, y);
      o[0] = saturate(t.ao);
      o[1] = saturate(t.rough);
      o[2] = 0.f; // no metal on the island
      o[3] = 1.f;
      }
    });

  // the table's albedo, whatever the pattern's own mean came to
  const float gain = spec.luminance/meanLuminance(albedo);
  for(size_t i=0; i<albedo.px.size(); i+=4)
    for(size_t c=0; c<3; ++c)
      albedo.px[i+c] = std::min(1.f, albedo.px[i+c]*gain);

  MaterialNote note;
  note.name      = spec.name;
  note.luminance = meanLuminance(albedo);
  const int quarter = n/4;
  for(int by=0; by<4; ++by)
    for(int bx=0; bx<4; ++bx) {
      double sum = 0;
      for(int y=0; y<quarter; ++y)
        for(int x=0; x<quarter; ++x) {
          const float* a = albedo.at(bx*quarter+x, by*quarter+y);
          sum += 0.2126*double(a[0]) + 0.7152*double(a[1]) + 0.0722*double(a[2]);
          }
      const float mean = float(sum/double(quarter*quarter));
      note.spread = std::max(note.spread, std::abs(mean/note.luminance-1.f));
      }
  if(note.spread>spec.spread)
    throw std::runtime_error(std::string("island: the brightness of ")+spec.name+" wanders by "+
                             std::to_string(note.spread)+", more than "+std::to_string(spec.spread));

  // the normal from the height by Sobel's filter, the texel's size from the tile's
  const float texel = spec.tile/float(n);
  parallelRows(n, [&](int y) {
    auto h = [&](int x, int yy) {
      return height[size_t((yy+n)%n)*size_t(n)+size_t((x+n)%n)];
      };
    for(int x=0; x<n; ++x) {
      const float gx = (h(x+1, y-1)+2.f*h(x+1, y)+h(x+1, y+1) - h(x-1, y-1)-2.f*h(x-1, y)-h(x-1, y+1))/(8.f*texel);
      const float gy = (h(x-1, y+1)+2.f*h(x, y+1)+h(x+1, y+1) - h(x-1, y-1)-2.f*h(x, y-1)-h(x+1, y-1))/(8.f*texel);
      const float l  = std::sqrt(gx*gx+gy*gy+1.f);
      float* p = normal.at(x, y);
      p[0] = -gx/l;
      p[1] = -gy/l;
      p[2] = 1.f/l;
      p[3] = 1.f;
      }
    });

  const std::string stem = (dir/"island"/spec.file).string();
  writeDds(stem+"_albedo.dds", albedo, Encoding::Colour);
  writeDds(stem+"_normal.dds", normal, Encoding::Normal);
  writeDds(stem+"_orm.dds",    orm,    Encoding::Data);
  return note;
  }

std::string number(float value) {
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%.2f", double(value));
  return text;
  }

std::string mapsJson(const char* file) {
  const std::string stem = std::string("island/")+file;
  return "\"albedo\":\""+stem+"_albedo.dds\",\"normal\":\""+stem+"_normal.dds\",\"orm\":\""+stem+"_orm.dds\"";
  }

Image filled(int size, const std::function<void(float u, float v, float* px)>& texel) {
  Image image(size, size);
  parallelRows(size, [&](int y) {
    for(int x=0; x<size; ++x)
      texel((float(x)+0.5f)/float(size), (float(y)+0.5f)/float(size), image.at(x, y));
    });
  return image;
  }

}

std::vector<MaterialNote> writeMaterials(const std::filesystem::path& dir) {
  std::vector<MaterialNote> notes;
  std::string json = "{\n";
  for(const auto& spec:Specs) {
    notes.push_back(writeMaps(dir, spec));
    json += std::string("  \"")+spec.name+".TGA\": {"+mapsJson(spec.file)+",\"antitile\":\""+spec.antitile+
            "\",\"antitile_cell\":"+number(spec.cell)+"},\n";
    }
  // loose stones: the rock's maps by world position, so a stone of any shape and size is not stretched
  json += std::string("  \"")+BoulderMaterial+".TGA\": {"+mapsJson("rock")+",\"projection\":{\"tile_m\":2.5}},\n";

  // The water is clear: the shader refracts the bottom and scatters the sun, a colour of its own it takes from
  // nothing. The map only gives the materials their names; turbidity stays 0.
  const Image water = filled(16, [](float, float, float* px) {
    px[0] = 0.02f;
    px[1] = 0.09f;
    px[2] = 0.12f;
    px[3] = 1.f;
    });
  writeDds(dir/"island/water_albedo.dds", water, Encoding::Colour);
  json += std::string("  \"")+groundName(SeaWater)+".TGA\": {\"albedo\":\"island/water_albedo.dds\"},\n";
  json += std::string("  \"")+groundName(StreamWater)+".TGA\": {\"albedo\":\"island/water_albedo.dds\"}\n}\n";

  std::ofstream out(dir/"materials.json", std::ios::binary | std::ios::trunc);
  out << json;
  if(!out.flush())
    throw std::runtime_error("island: unable to write "+(dir/"materials.json").string());
  return notes;
  }

void writeSky(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  // The shader adds rgb * alpha of the two layers' mean and takes a fifth of the day's alpha; clouds_lut reads the
  // last mip's alpha as the sky's cover. White everywhere, the cover in alpha alone.
  const Image heaps = filled(512, [](float u, float v, float* px) {
    const float cover = smoothstep(0.05f, 0.5f, tiledFbm(u, v, 4, 701, 6, 0.55f));
    const float body  = mix(1.f, 0.7f, cover*cover); // the thick middle is greyer
    px[0] = body;
    px[1] = body;
    px[2] = std::min(1.f, body*1.03f);
    px[3] = cover;
    });
  const Image wisps = filled(512, [](float u, float v, float* px) {
    const float ridge = 1.f - 2.2f*std::abs(tiledFbm(u, v, 3, 709, 5));
    const float where = smoothstep(-0.2f, 0.3f, tiledFbm(u, v, 2, 719, 2));
    px[0] = px[1] = px[2] = 1.f;
    px[3] = 0.6f*smoothstep(0.5f, 0.95f, ridge)*where;
    });
  const Image stars = filled(512, [](float u, float v, float* px) {
    float light = 0;
    Rgb   colour = {1.f, 1.f, 1.f};
    const Cells star = tiledCells(u, v, 56, 727);
    if((star.id & 3U)!=0U) {
      const uint32_t kind = hash32(star.id ^ 0x3c6ef372U);
      const float    size = 0.03f + 0.07f*hashUnit(kind)*hashUnit(kind);
      light  = (0.35f+0.65f*hashUnit(hash32(kind)))*(1.f-smoothstep(0.3f*size, size, star.f1));
      colour = (kind & 7U)==0U ? Rgb{1.f, 0.85f, 0.7f} : (kind & 7U)==1U ? Rgb{0.8f, 0.88f, 1.f} : colour;
      }
    px[0] = colour.r*light;
    px[1] = colour.g*light;
    px[2] = colour.b*light;
    px[3] = light;
    });
  const Image haze = filled(512, [](float u, float v, float* px) {
    px[0] = px[1] = 0.45f;
    px[2] = 0.5f;
    px[3] = 0.5f*smoothstep(0.1f, 0.6f, tiledFbm(u, v, 3, 733, 5));
    });
  // the sprite is added to the sky as it is: a small disc and a glow that dies before the sprite's edge
  const Image sun = filled(256, [](float u, float v, float* px) {
    const float r     = 2.f*std::sqrt((u-0.5f)*(u-0.5f)+(v-0.5f)*(v-0.5f));
    const float disc  = 1.f-smoothstep(0.07f, 0.1f, r);
    const float glow  = 0.45f*std::exp(-7.f*r)*(1.f-smoothstep(0.55f, 0.95f, r));
    const float light = std::min(1.f, disc+glow);
    px[0] = light;
    px[1] = light*0.97f;
    px[2] = light*0.88f;
    px[3] = light;
    });
  const Image moon = filled(256, [](float u, float v, float* px) {
    const float r    = 2.f*std::sqrt((u-0.5f)*(u-0.5f)+(v-0.5f)*(v-0.5f));
    const float disc = 1.f-smoothstep(0.78f, 0.82f, r);
    const float seas = smoothstep(-0.1f, 0.4f, fbm(u*5.f, v*5.f, 0, 739, 4));
    const float limb = std::sqrt(std::max(0.f, 1.f-r*r/0.67f));
    const float body = disc*(0.55f+0.45f*limb)*mix(0.85f, 0.55f, seas);
    px[0] = body;
    px[1] = body*0.98f;
    px[2] = body*0.94f;
    px[3] = disc;
    });
  writeDds(dir/"SKYDAY_LAYER0_A0.dds",   heaps, Encoding::ColourAlpha);
  writeDds(dir/"SKYDAY_LAYER1_A0.dds",   wisps, Encoding::ColourAlpha);
  writeDds(dir/"SKYNIGHT_LAYER0_A0.dds", stars, Encoding::ColourAlpha);
  writeDds(dir/"SKYNIGHT_LAYER1_A0.dds", haze,  Encoding::ColourAlpha);
  writeDds(dir/"UNSUN5.dds",             sun,   Encoding::DataAlpha);
  writeDds(dir/"MOON.dds",               moon,  Encoding::DataAlpha);
  }

}

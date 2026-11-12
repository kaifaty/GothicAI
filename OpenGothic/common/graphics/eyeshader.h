#pragma once

#include <cstdlib>

// The eye shader of own heads (reports/content/2026-11-hero-head/eye-shader.md): a material with `eye` in
// materials.json gets the wet highlight and the lid's shade, the face rig computes that shade per vertex of the
// eyeballs (FaceRig, the profile's eyes.lid_shade). OPENGOTHIC_EYE_SHADER=0 turns both off: the eye is lit as before.
inline bool eyeShaderEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("OPENGOTHIC_EYE_SHADER");
    return e==nullptr || e[0]!='0';
    }();
  return on;
  }

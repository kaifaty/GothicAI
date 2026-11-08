#ifndef RCAS_GLSL
#define RCAS_GLSL

// Sharpening after the TAA resolve: RCAS ("robust contrast adaptive sharpening") of AMD FidelityFX FSR 1,
// MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc. (FsrRcasF in ffx_fsr1.h), without its noise
// removal. The lobe is limited by the min and max of the cross, so a contrast edge (a lamp post against the sky)
// gets no halo. RCAS works in 0..1: the HDR frame goes through an invertible Reinhard of its max channel.
//
// The licence of ffx_fsr1.h, for the RCAS part:
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// FSR_RCAS_LIMIT: the strongest negative lobe that cannot clip
const float RcasLimit = 0.25 - 1.0/16.0;

float rcasMax3(vec3 c) { return max(c.r, max(c.g, c.b)); }

vec3 rcasToUnit(vec3 c)   { return c/(1.0 + rcasMax3(c)); }
vec3 rcasFromUnit(vec3 c) { return c/max(1.0 - rcasMax3(c), 1e-4); }

vec3 rcasFetch(sampler2D src, ivec2 at, ivec2 size) {
  return rcasToUnit(max(texelFetch(src, clamp(at, ivec2(0), size-1), 0).rgb, vec3(0)));
  }

// src: linear HDR, pre-exposed; sharpness 0..1, 0 copies the frame
vec3 rcas(sampler2D src, ivec2 pix, float sharpness) {
  const ivec2 size = textureSize(src, 0);
  const vec3  raw  = max(texelFetch(src, pix, 0).rgb, vec3(0));
  if(sharpness<=0.0)
    return raw;

  //    b
  //  d e f
  //    h
  const vec3 b = rcasFetch(src, pix + ivec2( 0,-1), size);
  const vec3 d = rcasFetch(src, pix + ivec2(-1, 0), size);
  const vec3 e = rcasToUnit(raw);
  const vec3 f = rcasFetch(src, pix + ivec2( 1, 0), size);
  const vec3 h = rcasFetch(src, pix + ivec2( 0, 1), size);

  const vec3 mn4 = min(min(b, d), min(f, h));
  const vec3 mx4 = max(max(b, d), max(f, h));

  // the lobe that keeps the result inside the cross' range, per channel; the most limiting one wins
  const vec3 hitMin = min(mn4, e)/(4.0*mx4 + 1e-5);
  const vec3 hitMax = (1.0 - max(mx4, e))/(4.0*mn4 - 4.0 - 1e-5);
  const vec3 lobeC  = max(-hitMin, hitMax);
  const float lobe  = max(-RcasLimit, min(rcasMax3(lobeC), 0.0))*sharpness;

  const vec3 c = (lobe*(b + d + f + h) + e)/(4.0*lobe + 1.0);
  return rcasFromUnit(clamp(c, vec3(0), vec3(0.9999)));
  }

#endif

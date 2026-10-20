// Scalar-only material measurements. Both the original decoder and Python's
// own-image adapter call measure(); original pixel buffers never cross this API.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace material_measures {
inline double linear(uint8_t byte) {
  const double v=double(byte)/255;
  return v<=0.04045 ? v/12.92 : std::pow((v+0.055)/1.055,2.4);
}
struct Result {
  double periodU=0, periodV=0, periodConfidenceU=0, periodConfidenceV=0;
  int direction=0; // 0:none, 1:along U, 2:along V; direction of the long details
  double directionConfidence=0, angle=0;
  std::array<double,3> oklab{}, meanRgb{};
  double logSpread=0, detailK3=0, detailK9=0, detailK27=0;
};

inline std::pair<double,double> period(const std::vector<double>& image,int w,int h,bool u) {
  const int n=u?w:h, lines=u?h:w;
  if(n<8) return {0,0};
  const int frequencies=n/4; // reject details narrower than four analysis samples
  std::vector<double> powers(frequencies+1,0);
  constexpr double tau=6.2831853071795864769;
  // Sum per-line spectra, rather than spectrum of a row/column mean: staggered
  // masonry and offset shingles must not cancel one another's phase.
  for(int line=0;line<lines;++line) {
    double mean=0;
    for(int p=0;p<n;++p) mean+=u?image[line*w+p]:image[p*w+line];
    mean/=n;
    for(int k=1;k<=frequencies;++k) {
      double re=0,im=0;
      for(int p=0;p<n;++p) {
        const double sample=(u?image[line*w+p]:image[p*w+line])-mean;
        const double angle=tau*k*p/n;
        re+=sample*std::cos(angle); im+=sample*std::sin(angle);
      }
      powers[k]+=re*re+im*im;
    }
  }
  double total=0;
  for(int k=1;k<=frequencies;++k) total+=powers[k];
  if(total/(lines*n*n)<1e-6) return {0,0};
  int peak=0;
  double best=0;
  // k=1 is a broad field, not a repeated detail. Select a local spectral peak
  // above its two-neighbour baseline; otherwise 1/f shading would masquerade
  // as one enormous stone. The prominence gate also rejects a flat noise PSD.
  for(int k=2;k<=frequencies;++k) {
    const double left=powers[k-1],right=k<frequencies?powers[k+1]:left;
    const double baseline=(left+right)*0.5;
    if(powers[k]<=left || powers[k]<=right || powers[k]<1.5*baseline || powers[k]/total<0.025) continue;
    const double excess=powers[k]-baseline;
    if(excess>best) { best=excess; peak=k; }
  }
  // Zero means measured absence of a dominant peak, never a fabricated count.
  return peak?std::pair<double,double>{1.0/peak,best/total}:std::pair<double,double>{0,0};
}

inline Result measure(const uint8_t* rgba,int width,int height) {
  if(!rgba || width<=0 || height<=0) throw std::runtime_error("invalid RGBA dimensions");
  Result result;
  double count=0;
  // Mean visible-pixel linear RGB, then its OKLab coordinates. This is the
  // mean color that per-channel linear gains can reproduce exactly.
  for(int i=0;i<width*height;++i) {
    if(rgba[4*i+3]<128) continue;
    const double r=linear(rgba[4*i]),g=linear(rgba[4*i+1]),b=linear(rgba[4*i+2]);
    result.meanRgb[0]+=r; result.meanRgb[1]+=g; result.meanRgb[2]+=b;
    ++count;
  }
  if(!count) throw std::runtime_error("no pixels surviving alpha cutoff");
  for(int c=0;c<3;++c) result.meanRgb[c]/=count;
  const auto& rgb=result.meanRgb;
  const double l=std::cbrt(0.4122214708*rgb[0]+0.5363325363*rgb[1]+0.0514459929*rgb[2]);
  const double m=std::cbrt(0.2119034982*rgb[0]+0.6806995451*rgb[1]+0.1073969566*rgb[2]);
  const double s=std::cbrt(0.0883024619*rgb[0]+0.2817188376*rgb[1]+0.6299787005*rgb[2]);
  result.oklab={0.2104542553*l+0.7936177850*m-0.0040720468*s,
                1.9779984951*l-2.4285922050*m+0.4505937099*s,
                0.0259040371*l+0.7827717662*m-0.8086757660*s};
  // Box-average to a maximum of 128 samples per axis. Only a bounded in-memory
  // analysis grid is created; neither originals nor the grid are serialized.
  const int w=std::min(width,128),h=std::min(height,128);
  std::vector<double> image(w*h,0), weights(w*h,0);
  const double global=0.2126*result.meanRgb[0]+0.7152*result.meanRgb[1]+0.0722*result.meanRgb[2];
  for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
    const int i=y*width+x, j=(y*h/height)*w+x*w/width;
    if(rgba[4*i+3]<128) continue;
    image[j]+=0.2126*linear(rgba[4*i])+0.7152*linear(rgba[4*i+1])+0.0722*linear(rgba[4*i+2]);
    ++weights[j];
  }
  for(int i=0;i<w*h;++i) image[i]=weights[i]>0?image[i]/weights[i]:global;
  const auto pu=period(image,w,h,true),pv=period(image,w,h,false);
  result.periodU=pu.first; result.periodV=pv.first;
  result.periodConfidenceU=pu.second; result.periodConfidenceV=pv.second;
  // Structure tensor of central gradients, no wrap at a panel's boundary.
  double xx=0,yy=0,xy=0;
  for(int y=1;y<h-1;++y) for(int x=1;x<w-1;++x) {
    const double gx=(image[y*w+x+1]-image[y*w+x-1])*w;
    const double gy=(image[(y+1)*w+x]-image[(y-1)*w+x])*h;
    xx+=gx*gx; yy+=gy*gy; xy+=gx*gy;
  }
  const double energy=xx+yy;
  if(energy>1e-10) {
    const double coherence=std::hypot(xx-yy,2*xy)/energy;
    const double normal=0.5*std::atan2(2*xy,xx-yy);
    result.angle=std::fmod(normal*180/3.14159265358979323846+270,180);
    // Confidence includes axial alignment: 45-degree stripes are anisotropic
    // but cannot honestly be described as along U or along V.
    result.directionConfidence=std::abs(xx-yy)/energy;
    if(coherence>=0.20 && result.directionConfidence>=0.20)
      result.direction=yy>xx?1:2;
  }
  // Independent, fixed UV grid for log contrast/detail. Keep the legacy
  // 128-sample period/direction calculation above unchanged.
  constexpr int side=256;
  const auto bounds=[](int cell,int size) {
    if(size<side) {
      const int pixel=int((int64_t(2*cell+1)*size)/(2*side));
      return std::pair<int,int>{pixel,pixel+1};
    }
    // Pixel centres in [cell/256, (cell+1)/256).
    return std::pair<int,int>{int((int64_t(2*cell)*size+side-1)/(2*side)),
                             int((int64_t(2*(cell+1))*size+side-1)/(2*side))};
  };
  std::vector<double> logs(side*side,0), visible;
  std::vector<bool> included(side*side,false);
  double cellMean=0;
  for(int cy=0;cy<side;++cy) for(int cx=0;cx<side;++cx) {
    const auto [x0,x1]=bounds(cx,width);
    const auto [y0,y1]=bounds(cy,height);
    double sum=0;
    int samples=0;
    for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
      const auto* pixel=rgba+4*(size_t(y)*width+x);
      if(pixel[3]<128) continue;
      sum+=0.2126*linear(pixel[0])+0.7152*linear(pixel[1])+0.0722*linear(pixel[2]);
      ++samples;
    }
    if(samples) {
      const int i=cy*side+cx;
      logs[i]=sum/samples;
      cellMean+=logs[i];
      included[i]=true;
      visible.push_back(logs[i]);
    }
  }
  cellMean/=visible.size();
  visible.clear();
  for(int i=0;i<side*side;++i) {
    logs[i]=std::log((included[i]?logs[i]:cellMean)+0.001);
    if(included[i]) visible.push_back(logs[i]);
  }
  std::sort(visible.begin(),visible.end());
  const auto percentile=[&](double fraction) {
    const double index=fraction*(visible.size()-1);
    const size_t lower=size_t(index),upper=std::min(lower+1,visible.size()-1);
    return visible[lower]+(visible[upper]-visible[lower])*(index-lower);
  };
  result.logSpread=percentile(0.9)-percentile(0.1);
  const auto detail=[&](int kernel) {
    std::vector<double> horizontal(side*side);
    const int radius=kernel/2;
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
      const int i=y*side+x;
      double delta=0;
      for(int dx=-radius;dx<=radius;++dx)
        delta+=logs[y*side+(x+dx+side)%side]-logs[i];
      horizontal[i]=logs[i]+delta/kernel;
    }
    double total=0;
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
      const int i=y*side+x;
      if(!included[i]) continue;
      double delta=0;
      for(int dy=-radius;dy<=radius;++dy)
        delta+=horizontal[((y+dy+side)%side)*side+x]-horizontal[i];
      const double box=horizontal[i]+delta/kernel;
      total+=std::abs(logs[i]-box);
    }
    return total/visible.size();
  };
  result.detailK3=detail(3); result.detailK9=detail(9); result.detailK27=detail(27);
  return result;
}
}

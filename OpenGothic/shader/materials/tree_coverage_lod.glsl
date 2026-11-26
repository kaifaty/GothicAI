// The calibrated threshold is exactly zero. Any winning sample restores the fine cut.
const uint CoverageLowFrames = 2u;

uint coverageAge(uint previous, uint wins, float boxPixels, bool consecutive) {
  if(!consecutive || boxPixels<=0.0 || float(wins)/max(1.0,boxPixels)>0.0)
    return 0u;
  return min(previous+1u,CoverageLowFrames);
  }

BoughProjection coverageCut(BoughProjection projection, uint age) {
  if(age==CoverageLowFrames)
    projection.focalScale *= 0.25;
  return projection;
  }

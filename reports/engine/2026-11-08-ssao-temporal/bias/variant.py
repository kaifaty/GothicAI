# Writes ssao.comp for a bias probe: variant.py <mode>
# Run from the repository root; restore with git checkout. 0 main; 1 no early exit; 2 mean of 4 estimates of 8 samples (same rotation, the 4-frame cycle);
# 4 mean of 2 estimates of 16 samples (2-frame cycle); 5 no direction cells: H/(H+M) over all 32 samples, sums are linear
# 3 mean of 16 estimates of 8 samples (4 rotations x 4 subsets, a rotation that changes every cycle).
import sys
p='OpenGothic/shader/ssao/ssao.comp'
mode=int(sys.argv[1])
s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1,a
    s=s.replace(a,b)
rep('const uint  numSamples     = 32;', f'const int   PROBE          = {mode};\nconst uint  numSamples     = 32;')
rep('    if(isSolved(abs(occlusion-prev), samples))', '    if(PROBE==0 && isSolved(abs(occlusion-prev), samples))')
probe = '''
  if(PROBE>=2 && PROBE<=4 && valid && maxSteps==int(numIter)) {
    const int rotations = PROBE==3 ? 4 : 1;
    const uint subsets  = PROBE==4 ? 2u : 4u;
    float sum = 0, n = 0;
    for(int j=0; j<rotations; ++j) {
      const float fj = M_PI*2.0*wangHashNoise(uvec2(fragCoord), uint(j));
      for(uint k=0; k<subsets; ++k) {
        for(int c=0; c<(gridQSize+1)/2; ++c)
          hitV[c][gl_LocalInvocationIndex] = 0;
        hits = 0; anySmp = 0; occlusionAll = 0;
        for(uint it=0; it<numIter/subsets; ++it)
          calcOcclussion(at0, pos0, fj, k*(numIter/subsets)+it, numIter);
        if(anySmp!=0u) { sum += resolveOcclusion(); n += 1; } // a subset with no sample on screen says nothing
        }
      }
    imageStore(result, ivec2(gl_GlobalInvocationID.xy), vec4(1.0 - (n>0 ? sum/n : 0.0)));
    return;
    }

  if(valid) {
#if 1'''
rep('\n  if(valid) {\n#if 1', probe)
if mode==5:
    rep('void addHit(vec3 dir, float v) {\n', 'float sumH = 0, sumM = 0;\nvoid addHit(vec3 dir, float v) {\n  sumH += v; anySmp = 1u; return;\n')
    rep('void addMis(vec3 dir) {\n', 'void addMis(vec3 dir) {\n  sumM += 1.0; anySmp = 1u; return;\n')
    rep('float resolveOcclusion() {\n', 'float resolveOcclusion() {\n  return sumH+sumM>0.0 ? sumH/(sumH+sumM) : 0.0;\n')
open(p,'w').write(s)

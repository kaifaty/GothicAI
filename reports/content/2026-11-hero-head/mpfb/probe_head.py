# Probe: an MPFB male head for the hero pilot — what the base gives (counts, shape keys) and how it looks.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender blender -b -P probe_head.py -- <out dir>   (writes probe.blend)
import bpy, os, math, sys
from mathutils import Vector
from bl_ext.user_default.mpfb.services import HumanService, FaceService, LocationService, TargetService
OUT=sys.argv[sys.argv.index('--')+1]
D=LocationService.get_user_data()
for o in list(bpy.data.objects): bpy.data.objects.remove(o)
macro=TargetService.get_default_macro_info_dict()
macro.update(gender=1.0, age=0.577, muscle=0.6, weight=0.45, proportions=0.6)
macro['race']={'caucasian':1.0,'asian':0.0,'african':0.0}
bm=HumanService.create_human(macro_detail_dict=macro)
HumanService.set_character_skin(os.path.join(D,'skins/middleage_caucasian_male/middleage_caucasian_male.mhmat'),bm)
for f,t in [('eyes/high-poly/high-poly.mhclo','Eyes'),('eyebrows/eyebrow006/eyebrow006.mhclo','Eyebrows'),
            ('eyelashes/eyelashes01/eyelashes01.mhclo','Eyelashes'),('teeth/teeth_base/teeth_base.mhclo','Teeth'),
            ('tongue/tongue01/tongue01.mhclo','Tongue')]:
    p=os.path.join(D,f)
    if os.path.exists(p): HumanService.add_mhclo_asset(p,bm,asset_type=t)
    else: print('MISSING',p)
FaceService.load_targets(bm, load_microsoft_visemes=False, load_meta_visemes=True, load_arkit_faceunits=True)
keys=[k.name for k in bm.data.shape_keys.key_blocks] if bm.data.shape_keys else []
print('SHAPEKEYS',len(keys)); print('KEYS',[k for k in keys if k.startswith('viseme') or k[:3] in ('bro','eye','jaw','mou')][:80])
# head part: vertices whose weight in a head/neck group is set
vg={g.name:g.index for g in bm.vertex_groups}; print('GROUPS', [g for g in vg if 'head' in g.lower() or 'neck' in g.lower() or 'helper' in g.lower()][:20])
me=bm.data
print('BASEMESH verts',len(me.vertices),'faces',len(me.polygons))
for o in bpy.data.objects:
    if o.type=='MESH': print('OBJ',o.name,len(o.data.vertices),len(o.data.polygons))
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT,'probe.blend'))

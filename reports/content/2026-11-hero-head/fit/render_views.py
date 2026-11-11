# Clay renders of the human in a .blend with the fit's cameras: front, profile (ortho, on the concept's pupils) and
# three quarters; side light from the upper left so the brow, the orbits and the cheekbones read.
#   blender -b X.blend -P render_views.py -- params.json <out dir> <prefix> [views.json]
# With views.json the cameras stand on its eye point (the template's) and not on the eyes of this .blend, so a head
# whose eyes were moved by the wrap is still compared with the concept in the concept's frame.
import bpy, sys, os, json, math
import numpy as np
from mathutils import Vector
a=sys.argv[sys.argv.index('--')+1:]; P=json.load(open(a[0])); OUT=a[1]; pre=a[2]
sc=bpy.context.scene; bm=bpy.data.objects['Human']; cam=sc.camera
vg={g.name:g.index for g in bm.vertex_groups}
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update(); ev=bm.evaluated_get(bpy.context.evaluated_depsgraph_get()); me=ev.to_mesh()
def grp(nm):
    gi=vg[nm]; return [me.vertices[i].co.copy() for i,v in enumerate(bm.data.vertices) if any(g.group==gi and g.weight>0.5 for g in v.groups)]
e=grp('helper-l-eye')+grp('helper-r-eye'); eye=sum(e,Vector())/len(e)
ev.to_mesh_clear()
if len(a)>3: eye=Vector(json.load(open(a[3]))['eye'])
for m,v in mods: m.show_viewport=v
for o in bpy.data.objects:
    if o.type=='LIGHT': bpy.data.objects.remove(o)
for (rx,rz,en) in [(55,-40,3.0),(80,140,1.2)]:
    ld=bpy.data.lights.new('l','SUN'); ld.energy=en; lo=bpy.data.objects.new('l',ld); sc.collection.objects.link(lo); lo.rotation_euler=(math.radians(rx),0,math.radians(rz))
sc.world.node_tree.nodes['Background'].inputs[1].default_value=0.35
s=0.0615/P['ipd_px']; N=P['res'][0]; c=N/2; fx,fy=P['front_eye']; px,py=P['profile_eye']
views=[('front',Vector((-(fx-c)*s,-3,eye.z+(fy-c)*s)),(math.radians(90),0,0),s*N),
       ('profile',Vector((3,eye.y-(px-c)*s,eye.z+(py-c)*s)),(math.radians(90),0,math.radians(90)),s*N),
       ('quarter',Vector((math.sin(math.radians(35))*3,-math.cos(math.radians(35))*3,eye.z-0.03)),(math.radians(90),0,math.radians(35)),0.42)]
for name,loc,rot,osc in views:
    cam.location=loc; cam.rotation_euler=rot; cam.data.ortho_scale=osc
    sc.render.filepath=os.path.join(OUT,f'{pre}_{name}.png'); bpy.ops.render.render(write_still=True)

#!/usr/bin/env python3
"""Census connected city building shells; raw geometry stays outside the repository."""
import argparse
from collections import Counter, defaultdict
import csv
import itertools
import json
import math
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[2]
SHELL_CLASSES = frozenset((
    'masonry_rubble', 'masonry_dressed', 'framed_masonry_tile', 'planks_wall',
    'window_wall_tile', 'timber_strip', 'timber_frame_tile', 'roof_shingle',
    'roof_tiles', 'bound_timber_tile', 'masonry_panel', 'stone_strip', 'quay_wall_tile',
))
HARBOUR = ('NW_CITY_HAFEN_HOLZWAND_01.TGA', 'NW_CITY_HAFEN_HOLZWAND_02.TGA',
           'NW_CITY_HAFEN_GIEBEL_01.TGA')
VERTICAL_FOV = 125.  # Isolated harbour capture SystemPack.ini; narrow lanes need a wide lens.
MARKET = ('NW_CITY_WALL_03.TGA', 'NW_CITY_WINDOW_03.TGA',
          'NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA', 'NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA',
          'NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA', 'NW_CITY_WOODBEAM_02.TGA')


def shell_rows(mapping):
    """Use mapped surface roles, excluding interiors, quay kerbs and prop classes."""
    return {r['texture']: r for r in mapping if r['class'] in SHELL_CLASSES
            and r['decision'] != 'exclude' and not r['texture'].startswith('NW_CITY_INSIDEWALL_')
            and r['texture'] != 'NW_CITY_HAFENRANDSTEIN_01.TGA'}


def shell_triangle(triangle, rows):
    # Actual static building families in the city export; the same wall/beam
    # materials also occur on cannons, nets, shelves, signs, ships and loose boards.
    visual = triangle['visual']
    return (triangle['texture'] in rows and triangle['area_m2'] > 0 and
            (not visual or visual.startswith(('NW_CITY_BEAM', 'NW_MISC_POOR_HUT_'))))


class Dsu:
    def __init__(self, count):
        self.parent = list(range(count))

    def find(self, i):
        while i != self.parent[i]:
            self.parent[i] = self.parent[self.parent[i]]
            i = self.parent[i]
        return i

    def join(self, a, b):
        a, b = self.find(a), self.find(b)
        if a != b:
            self.parent[b] = a


def point_segment(p, a, b):
    edge = b - a
    t = np.sum((p-a)*edge, axis=-1) / np.maximum(np.sum(edge*edge, axis=-1), 1e-20)
    return np.linalg.norm(p - (a + np.clip(t, 0, 1)[..., None]*edge), axis=-1)


def point_triangle(p, t):
    a, b, c = t[:, 0], t[:, 1], t[:, 2]
    u, v, w = b-a, c-a, p-a
    uu, uv, vv = (np.sum(x*y, axis=1) for x, y in ((u,u),(u,v),(v,v)))
    wu, wv = np.sum(w*u, axis=1), np.sum(w*v, axis=1)
    det = uu*vv-uv*uv
    s = (wu*vv-wv*uv)/np.maximum(det, 1e-20)
    q = (wv*uu-wu*uv)/np.maximum(det, 1e-20)
    normal = np.cross(u, v)
    plane = np.abs(np.sum(w*normal, axis=1))/np.maximum(np.linalg.norm(normal, axis=1), 1e-20)
    edges = np.minimum.reduce([point_segment(p,a,b), point_segment(p,b,c), point_segment(p,c,a)])
    return np.where((det > 1e-12) & (s >= 0) & (q >= 0) & (s+q <= 1), plane, edges)


def edge_distance(a, b, c, d):
    u, v, w = b-a, d-c, a-c
    aa, bb, cc, dd, ee = (np.sum(x*y, axis=1) for x,y in ((u,u),(u,v),(v,v),(u,w),(v,w)))
    det = aa*cc-bb*bb
    s = (bb*ee-cc*dd)/np.maximum(det, 1e-20)
    t = (aa*ee-bb*dd)/np.maximum(det, 1e-20)
    interior = np.linalg.norm(w+s[:, None]*u-t[:, None]*v, axis=1)
    boundary = np.minimum.reduce([point_segment(a,c,d), point_segment(b,c,d),
                                  point_segment(c,a,b), point_segment(d,a,b)])
    return np.where((det > 1e-12) & (s >= 0) & (s <= 1) & (t >= 0) & (t <= 1), interior, boundary)


def edge_pierces(a, b, t):
    u, v = t[:,1]-t[:,0], t[:,2]-t[:,0]
    direction = b-a
    h = np.cross(direction, v)
    det = np.sum(u*h, axis=1)
    inv = np.divide(1., det, out=np.zeros_like(det), where=np.abs(det)>1e-12)
    s = a-t[:,0]
    q = np.cross(s,u)
    x, y, z = np.sum(s*h, axis=1)*inv, np.sum(direction*q, axis=1)*inv, np.sum(v*q, axis=1)*inv
    return (np.abs(det)>1e-12) & (x>=0) & (y>=0) & (x+y<=1) & (z>=0) & (z<=1)


def face_distance(a, b):
    """Exact triangle distance, including edge crossings and piercing faces."""
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    result = np.full(len(a), np.inf)
    for k in range(3):
        result = np.minimum(result, point_triangle(a[:,k], b))
        result = np.minimum(result, point_triangle(b[:,k], a))
        result[edge_pierces(a[:,k],a[:,(k+1)%3],b) | edge_pierces(b[:,k],b[:,(k+1)%3],a)] = 0
        for j in range(3):
            result = np.minimum(result, edge_distance(a[:,k],a[:,(k+1)%3],b[:,j],b[:,(j+1)%3]))
    return result


def cluster_triangles(triangles, gap_cm=50.):
    """Shared vertices rounded to 1 cm, then actual face separation <=50 cm."""
    if not triangles:
        return []
    points = np.asarray([t['points'] for t in triangles], dtype=float)
    dsu = Dsu(len(points))
    vertices = {}
    for i, triangle in enumerate(np.rint(points).astype(np.int64)):
        for vertex in triangle:
            key = tuple(vertex)
            if key in vertices:
                dsu.join(i, vertices[key])
            else:
                vertices[key] = i
    centers = points.mean(axis=1)
    radii = np.linalg.norm(points-centers[:,None], axis=2).max(axis=1)
    lo, hi = points.min(axis=1), points.max(axis=1)
    levels = np.ceil(np.log2(np.maximum(radii, 1))).astype(int)
    for level in np.unique(levels):
        indices = np.flatnonzero(levels == level)
        tree = cKDTree(centers[indices])
        for i, neighbors in enumerate(tree.query_ball_point(centers, radii + 2.**level + gap_cm)):
            js = np.asarray([indices[j] for j in neighbors if indices[j] > i
                             and dsu.find(i) != dsu.find(int(indices[j]))], dtype=int)
            if not len(js):
                continue
            box_gap = np.maximum(np.maximum(lo[i]-hi[js], lo[js]-hi[i]), 0)
            js = js[np.linalg.norm(box_gap, axis=1) <= gap_cm]
            if len(js):
                distances = face_distance(np.broadcast_to(points[i], (len(js),3,3)), points[js])
                for j in js[distances <= gap_cm + 1e-7]:
                    dsu.join(i, int(j))
    groups = defaultdict(list)
    for i in range(len(points)):
        groups[dsu.find(i)].append(i)
    return list(groups.values())


def describe(triangles, indices, landmarks):
    ts = [triangles[i] for i in indices]
    points = np.asarray([t['points'] for t in ts])
    lo, hi = points.min(axis=(0,1)), points.max(axis=(0,1))
    weights = np.asarray([t['area_m2'] for t in ts])
    center = np.average([t['center'] for t in ts], axis=0, weights=weights)
    textures = Counter()
    for t in ts:
        textures[t['texture']] += t['area_m2']
    nearest = min(landmarks, key=lambda w: np.linalg.norm(np.asarray(w['position'])-center))
    return dict(center_cm=center.tolist(), bounds_cm=[lo.tolist(),hi.tolist()],
                size_m=((hi-lo)/100).tolist(), area_m2=float(weights.sum()),
                textures_m2=dict(textures.most_common()), nearest=nearest['name'],
                nearest_distance_m=float(np.linalg.norm(np.asarray(nearest['position'])-center)/100),
                triangle_indices=indices)


def ground_height(ground, x, z, below):
    t = np.asarray(ground, dtype=float)
    a, u, v = t[:,0], t[:,1]-t[:,0], t[:,2]-t[:,0]
    det = u[:,0]*v[:,2]-u[:,2]*v[:,0]
    good = np.abs(det)>1e-8
    a,u,v,det = a[good],u[good],v[good],det[good]
    dx,dz = x-a[:,0],z-a[:,2]
    s,q = (dx*v[:,2]-dz*v[:,0])/det,(dz*u[:,0]-dx*u[:,2])/det
    y = a[:,1]+s*u[:,1]+q*v[:,1]
    y = y[(s>=-1e-6)&(q>=-1e-6)&(s+q<=1+1e-6)&(y<=below)]
    if not len(y):
        raise ValueError(f'No ground below camera at {x:.1f}, {z:.1f}')
    return float(y.max())


def project(points, eye, target):
    forward = target-eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0,1,0]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    delta = points-eye
    depth = delta@forward
    tan_v = math.tan(math.radians(VERTICAL_FOV/2))
    xy = np.stack((delta@right/(tan_v*1280/720), delta@up/tan_v),axis=1)/depth[:,None]
    return xy, depth


def facade(block, triangles):
    ts = [triangles[i] for i in block['triangle_indices'] if triangles[i]['texture']==HARBOUR[1]]
    # Keep connected coplanar fields together across their fan diagonals.
    faces = Dsu(len(ts))
    vertices = defaultdict(list)
    for i,t in enumerate(ts):
        for p in np.rint(t['points']).astype(int):
            key = tuple(p)
            for j in vertices[key]:
                if (np.dot(t['normal'],ts[j]['normal'])>.996 and
                    abs(np.dot(np.asarray(t['center'])-ts[j]['center'],t['normal']))<.5):
                    faces.join(i,j)
            vertices[key].append(i)
    patches = defaultdict(list)
    for i,t in enumerate(ts):
        patches[faces.find(i)].append(t)
    face = max(patches.values(),key=lambda patch:sum(t['area_m2'] for t in patch))
    normal = np.average([t['normal'] for t in face],axis=0,weights=[t['area_m2'] for t in face])
    normal /= np.linalg.norm(normal)
    anchor = np.average([t['center'] for t in face],axis=0,weights=[t['area_m2'] for t in face])
    indices = [i for i in block['triangle_indices']
               if np.abs((np.asarray(triangles[i]['points'])-anchor)@normal).max()<=50]
    points = np.asarray([triangles[i]['points'] for i in indices]).reshape(-1,3)
    outward = normal.copy(); outward[1]=0; outward/=np.linalg.norm(outward)
    if np.dot(anchor-np.asarray(block['center_cm']),outward)<0:
        outward *= -1
    side = np.cross([0,1,0],outward)
    along = (points-anchor)@side
    # Reach the wooden roof ridge, excluding unrelated stone towers in this block.
    roof = [t for i in block['triangle_indices'] for t in [triangles[i]]
            if t['texture'] in ('NW_CITY_ROOF_01.TGA','NW_CITY_ROOF_04.TGA',HARBOUR[2])]
    bottom = points[:,1].min()
    top = max([points[:,1].max()]+[p[1] for t in roof for p in t['points']])
    limits = [float(along.min()-300),float(along.max()+300)]
    skin_offset = max(0,float(((points-anchor)@outward).max()))
    exterior = anchor+outward*skin_offset
    target = exterior + side*sum(limits)/2
    target[1] = (bottom+top)/2
    frame = np.asarray([exterior+side*x for x in limits for _ in range(2)])
    frame[:,1] = [bottom,top,bottom,top]
    samples = np.asarray([t['center'] for t in face])+outward*(skin_offset+2)
    return dict(anchor=anchor,normal=normal,outward=outward,side=side,target=target,
                points=points,frame=frame,samples=samples,triangle_indices=indices,
                face_area_m2=sum(t['area_m2'] for t in face))


def walking_ground(raw, mapping):
    streets = {r['texture'] for r in mapping if r['class'] in (
        'mud','cobble','path_tile','ground_panel','paving_transition_tile','pavement_border_tile')}
    streets.add('NW_CITY_HAFENKAI_BODEN_01.TGA')
    terrain = ('NW_NATURE_WALDBODEN_', 'NW_NATURE_WBODEN_', 'NW_NATURE_STEIN_',
               'NW_NATURE_DARKWOOD_BODEN_', 'NW_NATURE_DARKWOOD_PATH_',
               'NW_NATURE_DARKWOOD_STEIN_', 'NW_NATURE_DARKWOOD_FELS_',
               'NW_NATURE_GREATSEA_SAND_', 'NW_NATURE_PATH_', 'NW_NATURE_ACKER_',
               'NW_NATURE_STAIRS_', 'OW_SURFACE_')
    return np.asarray([t for t,name in zip(raw['ground_triangles'],raw['ground_textures'],strict=True)
                       if name in streets or name.startswith(terrain)],dtype=float)


def camera(block, triangles, ground, angle=0, occluders=None):
    face = facade(block,triangles)
    a = math.radians(angle)
    direction = face['outward']*math.cos(a)+face['side']*math.sin(a)
    points = np.concatenate((face['points'],face['frame']))
    best = None
    # Keep a straight front / 45-degree bearing while moving along the facade
    # to find open street rather than placing the eye inside a neighbouring hut.
    for distance in np.arange(150.,2025.,25.):
        for offset in (0,150,-150,300,-300,450,-450,600,-600):
            target = face['target']+face['side']*offset
            eye = target + direction*distance
            try:
                ground_y = ground_height(ground,eye[0],eye[2],face['anchor'][1])
            except ValueError:
                continue
            eye[1] = ground_y+170
            xy,depth = project(points,eye,target)
            if depth.min()<=10 or np.abs(xy).max()>.96:
                continue
            visible = (sum(occluders.first_hit(eye,p,endpoint_margin=50) is None for p in face['samples'])/len(face['samples'])
                       if occluders is not None else 1.)
            result = dict(eye_cm=eye.tolist(),target_cm=target.tolist(),ground_y_cm=ground_y,
                          outward=face['outward'].tolist(),angle_degrees=angle,distance_cm=float(distance),
                          tangent_offset_cm=offset,frame_extent=float(np.abs(xy).max()),
                          facade_visible_fraction=visible,face_area_m2=face['face_area_m2'],
                          facade_triangle_indices=face['triangle_indices'])
            if best is None or visible>best['facade_visible_fraction']:
                best = result
            if visible==1:
                return result
    if best is not None:
        return best
    raise ValueError(f"{block['id']}: no ground-level facade camera within 20 m")


def write_scenario(path, candidates):
    lines = ['# Generated by building_survey.py census; original block facades at noon.',
             'cheat god','clear 1000000','set time 12 0','overlay off','view hide animated morph pfx','cam lift off','']
    for block in candidates:
        for view,cam in block['cameras'].items():
            target=np.asarray(cam['target_cm']); eye=np.asarray(cam['eye_cm'])
            # Hold before the hero falls from the collision-free teleport anchor.
            hero=target.copy(); hero[1]=block['bounds_cm'][1][1]+500
            delta=eye-hero
            distance=math.hypot(delta[0],delta[2]); yaw=math.degrees(math.atan2(delta[2],delta[0]))
            lines += [f"# {block['id']} {view}: eye = ground + 170 cm.",
                      f'cam look {target[1]-hero[1]:.3f}',
                      'goto pos '+' '.join(f'{v:.3f}' for v in hero),
                      f'cam world {distance:.3f} {delta[1]:.3f} {yaw:.6f}',
                      'wait 34','cam hold','wait 6000',f"shot {block['id']}-{view}",'wait 300','']
    path.write_text('\n'.join(lines))


def triangle_key(triangle):
    return (triangle['texture'],triangle['visual'],
            tuple(sorted(tuple(round(v,3) for v in p) for p in triangle['points'])))


def validate_market(blocks, triangles, measurement, landmarks):
    reference = json.loads(measurement.read_text())
    expected = Counter(triangle_key(t) for t in reference['candidate_house_triangles'] if t['texture'] in MARKET)
    overlaps = []
    for block in blocks:
        actual = Counter(triangle_key(triangles[i]) for i in block['triangle_indices'])
        overlaps.append(sum((expected & actual).values()))
    best = int(np.argmax(overlaps))
    result = dict(block_id=blocks[best]['id'],waypoint='NW_CITY_MERCHANT_HUT_02_ENTRY',
                  reference_triangles=sum(expected.values()),matched_triangles=overlaps[best],
                  missing_triangles=sum(expected.values())-overlaps[best])
    print(json.dumps(result),flush=True)
    if result['missing_triangles']:
        raise ValueError(f"Market validation failed: {result['matched_triangles']}/{result['reference_triangles']} triangles")
    return result


def select_candidates(blocks):
    return sorted((b for b in blocks if b['textures_m2'].get(HARBOUR[1],0)>=10
                   and b['size_m'][0]<=40 and b['size_m'][2]<=40),
                  key=lambda b:(-sum(b['textures_m2'].get(n,0) for n in HARBOUR),b['id']))[:6]


def census(args):
    raw=json.loads(args.input.read_text())
    with args.mapping.open() as stream:
        mapping=list(csv.DictReader(stream))
        rows=shell_rows(mapping)
    triangles=[t for t in raw['candidate_house_triangles'] if shell_triangle(t,rows)]
    landmarks=raw['waypoints']+raw.get('freepoints',[])
    groups=cluster_triangles(triangles)
    clusters=[describe(triangles,g,landmarks) for g in groups if sum(triangles[i]['area_m2'] for i in g)>=30]
    clusters.sort(key=lambda c:tuple(c['center_cm'][k] for k in (0,2,1)))
    for i,c in enumerate(clusters,1):
        c['id']=f'B{i:03d}'
    print(f'{len(triangles)} shell triangles; {len(groups)} components; {len(clusters)} >=30 m2',flush=True)
    validation=validate_market(clusters,triangles,args.market_measurement,landmarks) if args.market_measurement else None
    candidates=select_candidates(clusters)
    if len(candidates)!=6:
        raise ValueError(f'Expected six candidates, found {len(candidates)}')
    from intent_cameras import Occluders
    occluders=Occluders(raw['occluders'])
    ground=walking_ground(raw,mapping)
    for c in candidates:
        front=camera(c,triangles,ground,occluders=occluders)
        quarters=[camera(c,triangles,ground,angle=a,occluders=occluders) for a in (45,-45)]
        quarter=max(quarters,key=lambda cam:(cam['facade_visible_fraction'],-cam['distance_cm']))
        c['cameras']={'front':front,'three-quarter':quarter}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    fields=['id','center_x_m','center_y_m','center_z_m','size_x_m','size_y_m','size_z_m','area_m2','nearest','nearest_distance_m']
    fields += list(itertools.chain.from_iterable((f'texture_{i}',f'texture_{i}_m2') for i in range(1,7)))
    with args.output.open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=fields,lineterminator='\n'); writer.writeheader()
        for c in clusters:
            row=dict(id=c['id'],area_m2=f"{c['area_m2']:.6f}",nearest=c['nearest'],nearest_distance_m=f"{c['nearest_distance_m']:.3f}")
            row.update({f'center_{a}_m':f'{v/100:.3f}' for a,v in zip('xyz',c['center_cm'])})
            row.update({f'size_{a}_m':f'{v:.3f}' for a,v in zip('xyz',c['size_m'])})
            for i,(name,area) in enumerate(list(c['textures_m2'].items())[:6],1):
                row[f'texture_{i}']=name; row[f'texture_{i}_m2']=f'{area:.6f}'
            writer.writerow(row)
    metadata=dict(shell_textures=list(rows),shell_classes=sorted({r['class'] for r in rows.values()}),
                  triangle_count=len(triangles),component_count=len(groups),clusters=clusters,
                  candidates=candidates,market_validation=validation)
    args.metadata.parent.mkdir(parents=True,exist_ok=True)
    args.metadata.write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
    write_scenario(args.scenario,candidates)
    for c in candidates:
        print(c['id'],round(c['area_m2'],3),[round(c['textures_m2'].get(n,0),3) for n in HARBOUR],c['nearest'],{v:{k:cam[k] for k in ('distance_cm','tangent_offset_cm','angle_degrees','facade_visible_fraction')} for v,cam in c['cameras'].items()},flush=True)


def sheet(args):
    from PIL import Image, ImageDraw, ImageFont
    candidates=json.loads(args.metadata.read_text())['candidates']
    try:
        font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',20)
    except OSError:
        font=ImageFont.load_default(size=20)
    board=Image.new('RGB',(1280,6*(360+64)),'#17191b'); draw=ImageDraw.Draw(board)
    for i,block in enumerate(candidates):
        y=i*424
        draw.text((12,y+4),f"{block['id']} | {block['nearest']}",font=font,fill='white')
        areas=[block['textures_m2'].get(n,0) for n in HARBOUR]
        draw.text((12,y+32),f'W01 {areas[0]:.1f} | W02 {areas[1]:.1f} | gable {areas[2]:.1f} m2  |  front / three-quarter',font=font,fill='white')
        for x,view in ((0,'front'),(640,'three-quarter')):
            with Image.open(args.captures/'original'/f"{block['id']}-{view}.png") as source:
                if source.size!=(1280,720):
                    raise ValueError(f"{block['id']}-{view}: expected 1280x720, got {source.size}")
                panel=source.convert('RGB').resize((640,360),Image.Resampling.LANCZOS)
            board.paste(panel,(x,y+64))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    for quality in (90,85,80,75,70,60):
        board.save(args.output,quality=quality,optimize=True)
        if args.output.stat().st_size<=1_000_000:
            print(f'12 panels; {args.output.stat().st_size} bytes; JPEG quality {quality}')
            return
    raise ValueError('Contact sheet exceeds 1 MB')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('census')
    p.add_argument('--input',type=Path,required=True)
    p.add_argument('--mapping',type=Path,default=ROOT/'data/content/city-materials.csv')
    p.add_argument('--output',type=Path,default=ROOT/'reports/content/2026-10-harbour-house/census.csv')
    p.add_argument('--metadata',type=Path,required=True)
    p.add_argument('--scenario',type=Path,default=ROOT/'scripts/content/harbour-house.bench')
    p.add_argument('--market-measurement',type=Path)
    p.set_defaults(run=census)
    p=sub.add_parser('sheet')
    p.add_argument('--metadata',type=Path,required=True); p.add_argument('--captures',type=Path,required=True)
    p.add_argument('--output',type=Path,default=ROOT/'reports/content/2026-10-harbour-house/candidates.jpg')
    p.set_defaults(run=sheet)
    args=parser.parse_args(); args.run(args)


if __name__=='__main__':
    main()

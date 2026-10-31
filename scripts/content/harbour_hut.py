#!/usr/bin/env python3
"""Measure the selected harbour block from geometry and UV coordinates only."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path

import numpy as np

from building_survey import (Dsu, HARBOUR, ROOT, face_distance, ground_height,
                            shell_rows, shell_triangle, walking_ground)

LOCAL = Path.home() / '.local/share/gothic-ai'
BEAMS = ('NW_CITY_WOODBEAM_01.TGA', 'NW_CITY_WOODBEAM_02.TGA')
ROOF = 'NW_CITY_ROOF_01.TGA'


def coplanar_fields(triangles, vertical_only=True, angle_degrees=1., plane_cm=1., min_area_m2=5.):
    """Join touching faces with matching normals and planes (all units are cm)."""
    if not triangles:
        return []
    points = np.asarray([t['points'] for t in triangles], dtype=float)
    normals = np.asarray([t['normal'] for t in triangles], dtype=float)
    centers = points.mean(axis=1)
    lo, hi = points.min(axis=1), points.max(axis=1)
    eligible = np.abs(normals[:, 1]) <= .2 if vertical_only else np.ones(len(points), dtype=bool)
    dsu = Dsu(len(points))
    threshold = math.cos(math.radians(angle_degrees))
    for i in np.flatnonzero(eligible):
        candidates = np.flatnonzero(eligible & (np.arange(len(points)) > i)
                                    & (normals @ normals[i] >= threshold)
                                    & (np.abs((centers-centers[i]) @ normals[i]) <= plane_cm))
        if not len(candidates):
            continue
        # Check every vertex, rather than accepting two warped planes at their centres.
        distances = np.abs((points[candidates]-centers[i]) @ normals[i]).max(axis=1)
        box_gap = np.maximum(np.maximum(lo[i]-hi[candidates], lo[candidates]-hi[i]), 0)
        candidates = candidates[(distances <= plane_cm) & (np.linalg.norm(box_gap, axis=1) <= plane_cm)]
        if len(candidates):
            distances = face_distance(np.broadcast_to(points[i], (len(candidates), 3, 3)), points[candidates])
            for j in candidates[distances <= plane_cm + 1e-6]:
                dsu.join(int(i), int(j))
    groups = defaultdict(list)
    for i in np.flatnonzero(eligible):
        groups[dsu.find(int(i))].append(int(i))
    return sorted((g for g in groups.values() if sum(triangles[i]['area_m2'] for i in g) >= min_area_m2),
                  key=lambda g: (-sum(triangles[i]['area_m2'] for i in g), g[0]))


def uv_geometry(triangle, tangent):
    """UV extents and the affine world-space U/V vectors of one mesh triangle."""
    points = np.asarray(triangle['points'], dtype=float)
    uv = np.asarray(triangle['uv'], dtype=float)
    delta = uv[1:]-uv[0]
    span = uv.max(axis=0)-uv.min(axis=0)
    result = dict(uv_min=uv.min(axis=0).tolist(), uv_max=uv.max(axis=0).tolist(),
                  uv_span=span.tolist(), uv_area=float(abs(np.linalg.det(delta))/2))
    if abs(np.linalg.det(delta)) < 1e-10:
        result['degenerate_uv'] = True
        return result
    world_u, world_v = np.linalg.solve(delta, points[1:]-points[0])
    angle = math.degrees(math.atan2(world_u[1], np.dot(world_u, tangent)))
    result.update(cm_per_uv=[float(np.linalg.norm(world_u)), float(np.linalg.norm(world_v))],
                  world_u_cm=world_u.tolist(), world_v_cm=world_v.tolist(),
                  u_angle_degrees=angle)
    return result


def texture_areas(triangles):
    areas = Counter()
    for t in triangles:
        areas[t['texture']] += t['area_m2']
    return dict(areas.most_common())


def field_description(triangles, indices, number, hut_centers=()):
    ts = [triangles[i] for i in indices]
    weights = np.asarray([t['area_m2'] for t in ts])
    origin = np.average([t['center'] for t in ts], axis=0, weights=weights)
    normal = np.average([t['normal'] for t in ts], axis=0, weights=weights)
    normal /= np.linalg.norm(normal)
    outward = normal.copy()
    outward[1] = 0
    outward /= np.linalg.norm(outward)
    if len(hut_centers):
        center = min(hut_centers, key=lambda c: np.linalg.norm((origin-np.asarray(c))[[0, 2]]))
        if np.dot(origin-np.asarray(center), outward) < 0:
            outward *= -1
    tangent = np.cross(outward, [0., 1., 0.])
    points = np.asarray([t['points'] for t in ts]).reshape(-1, 3)
    horizontal = (points-origin) @ tangent
    faces = []
    for i in indices:
        face = dict(triangle_index=i, texture=triangles[i]['texture'], area_m2=triangles[i]['area_m2'])
        face.update(uv_geometry(triangles[i], tangent))
        faces.append(face)
    return dict(id=f'W{number:02d}', triangle_indices=indices, origin=origin.tolist(),
                outward=outward.tolist(), tangent=tangent.tolist(), plane_normal=normal.tolist(),
                width_cm=float(np.ptp(horizontal)), height_cm=float(np.ptp(points[:, 1])),
                horizontal_limits_cm=[float(horizontal.min()), float(horizontal.max())],
                height_limits_cm=[float(points[:, 1].min()), float(points[:, 1].max())],
                area_m2=float(weights.sum()), textures_m2=texture_areas(ts), faces=faces)


def material_scale(triangles, texture):
    values = [(t['area_m2'], uv_geometry(t, [1., 0., 0.])) for t in triangles if t['texture'] == texture]
    values = [(w, v) for w, v in values if not v.get('degenerate_uv')]
    if not values:
        return None
    scales = np.asarray([v['cm_per_uv'] for _, v in values])
    weights = np.asarray([w for w, _ in values])
    return dict(cm_per_uv_area_weighted=np.average(scales, axis=0, weights=weights).tolist(),
                cm_per_uv_min=scales.min(axis=0).tolist(), cm_per_uv_max=scales.max(axis=0).tolist(),
                triangle_count=len(values))


def module_summary(triangles, group):
    faces = [f for f in group['faces'] if f['texture'] == HARBOUR[1] and not f.get('degenerate_uv')]
    if not faces:
        return None
    scales = np.asarray([f['cm_per_uv'] for f in faces])
    weights = [f['area_m2'] for f in faces]
    # UV area measures module equivalents; partial and cut modules remain explicit.
    return dict(field_id=group['id'], wall_width_cm=group['width_cm'], wall_height_cm=group['height_cm'],
                texture_area_m2=sum(weights), module_equivalents_uv_area=sum(f['uv_area'] for f in faces),
                module_size_cm_area_weighted=np.average(scales, axis=0, weights=weights).tolist(),
                triangle_indices=[f['triangle_index'] for f in faces])


def area_signature_matches(reference, candidate, tolerance=.01):
    """Every shell texture must match; absent or extra materials are mismatches."""
    if reference.keys() != candidate.keys():
        return False
    return all(abs(candidate[name]-area) <= tolerance*area + 1e-8 for name, area in reference.items())


def repeated_blocks(block, blocks):
    return [dict(id=b['id'], center_cm=b['center_cm'], nearest=b['nearest'], area_m2=b['area_m2'],
                 textures_m2=b['textures_m2']) for b in blocks
            if b['id'] != block['id'] and area_signature_matches(block['textures_m2'], b['textures_m2'])]


def measure(raw, survey, mapping, block_id='B037'):
    rows = shell_rows(mapping)
    shell = [t for t in raw['candidate_house_triangles'] if shell_triangle(t, rows)]
    block = next(b for b in survey['clusters'] if b['id'] == block_id)
    triangles = [dict(shell[i]) for i in block['triangle_indices']]
    # These are the three existing beam-vob pairs in B037, not a house segmentation algorithm.
    hut_centers = []
    for sources in ((9906, 9907), (10074, 10075), (10226, 10227)):
        beam_points = np.asarray([t['points'] for t in triangles if t.get('source') in sources])
        if beam_points.size:
            hut_centers.append((beam_points.min(axis=(0, 1))+beam_points.max(axis=(0, 1)))/2)
    groups = [field_description(triangles, indices, n, hut_centers) for n, indices in
              enumerate(coplanar_fields(triangles), 1)]
    main = max((g for g in groups if HARBOUR[1] in g['textures_m2']),
               key=lambda g: g['textures_m2'][HARBOUR[1]])
    origin, outward, tangent = (np.asarray(main[k]) for k in ('origin', 'outward', 'tangent'))
    for t in triangles:
        p = np.asarray(t['points'])-origin
        t['local'] = np.stack((p@outward, p@tangent, np.asarray(t['points'])[:, 1]), axis=1).tolist()
        center = np.asarray(t['center'])-origin
        t['local_center'] = [float(center@outward), float(center@tangent), t['center'][1]]
    points = np.asarray([t['points'] for t in triangles]).reshape(-1, 3)
    roof = [t for t in triangles if t['texture'] == ROOF]
    roof_points = np.asarray([t['points'] for t in roof]).reshape(-1, 3)
    pitches = [math.degrees(math.acos(abs(t['normal'][1]))) for t in roof]
    dimensions = dict(axis_aligned_size_cm=np.ptp(points, axis=0).tolist(),
                      ground_bottom_y_cm=float(points[:, 1].min()),
                      roof_eave_y_range_cm=[float(roof_points[:, 1].min()),
                                            float(min(p[:, 1].max() for p in np.asarray([t['points'] for t in roof])))],
                      roof_ridge_y_cm=float(roof_points[:, 1].max()),
                      roof_pitch_degrees_range=[float(min(pitches)), float(max(pitches))],
                      roof_pitch_degrees_area_weighted=float(np.average(pitches, weights=[t['area_m2'] for t in roof])))
    return dict(block_id=block_id, waypoint=block['nearest'], coordinates='Gothic world centimetres (Y up)',
                origin=origin.tolist(), outward=outward.tolist(), tangent=tangent.tolist(),
                candidate_house_triangles=triangles, source_shell_triangle_indices=block['triangle_indices'],
                block=dict(center_cm=block['center_cm'], bounds_cm=block['bounds_cm'], area_m2=block['area_m2'],
                           textures_m2=block['textures_m2']),
                wall_group_rules=dict(min_area_m2=5, max_abs_normal_y=.2, normal_angle_degrees=1,
                                      plane_tolerance_cm=1, connection_gap_cm=1),
                wall_groups=groups, main_wall_group=main['id'], dimensions=dimensions,
                window_modules=[v for g in groups if (v := module_summary(triangles, g))],
                material_uv_scale={name: material_scale(triangles, name) for name in (HARBOUR[0], HARBOUR[2])},
                repeated_blocks=repeated_blocks(block, survey['clusters']),
                primary_hut=primary_hut(triangles, groups))


def primary_hut(triangles, groups):
    """The explicit manual frame around HUT_05 nearest the selected bed waypoint."""
    axis = np.asarray([.968147640, 0., -.250380004])
    depth = np.asarray([.250380004, 0., .968147640])
    axis /= np.linalg.norm(axis)
    depth /= np.linalg.norm(depth)
    bounds = np.asarray([[3800., -2950.], [5000., -1920.]])
    all_points = np.asarray([t['points'] for t in triangles])
    local = np.stack((all_points@axis, all_points@depth), axis=2)
    inside = ((local >= bounds[0]) & (local <= bounds[1])).all(axis=(1, 2))
    selected = []
    for i, t in enumerate(triangles):
        if t.get('source'):
            if t['source'] in (9906, 9907):
                selected.append(i)
            continue
        if not inside[i]:
            continue
        normal = np.asarray(t['normal'])
        if t['texture'] == ROOF and abs(normal@axis) > .12:
            continue
        horizontal = math.hypot(normal[0], normal[2])
        aligned = max(abs(normal@axis), abs(normal@depth))/max(horizontal, 1e-8)
        if t['texture'] in HARBOUR and aligned < .993:
            continue
        if t['texture'] == BEAMS[1] and abs(normal[1]) < .7 and aligned < .993:
            continue
        selected.append(i)
    chosen = [triangles[i] for i in selected]
    points = np.asarray([t['points'] for t in chosen]).reshape(-1, 3)
    roof = [t for t in chosen if t['texture'] == ROOF]
    roof_points = np.asarray([t['points'] for t in roof]).reshape(-1, 3)
    # The two 900 cm side fields bound the nominal 720 cm wide wall rectangle.
    side_groups = [g for g in groups if abs(np.dot(g['outward'], depth)) > .99
                   and 850 <= g['width_cm'] <= 950
                   and bounds[0, 0] <= np.dot(g['origin'], axis) <= bounds[1, 0]
                   and bounds[0, 1] <= np.dot(g['origin'], depth) <= bounds[1, 1]]
    front = min(side_groups, key=lambda g: np.dot(g['origin'], depth))
    back = max(side_groups, key=lambda g: np.dot(g['origin'], depth))
    origin = np.asarray(front['origin'])
    side_planes = [float(np.dot(g['origin'], depth)) for g in (front, back)]
    groups_inside = []
    for g in groups:
        relevant = sum(triangles[i]['area_m2'] for i in g['triangle_indices'] if i in selected)
        g['primary_hut_area_m2'] = relevant
        g['primary_hut'] = relevant >= .5*g['area_m2']
        g['primary_hut_fraction'] = relevant/g['area_m2']
        if g['primary_hut']:
            groups_inside.append(g['id'])
    pitches = [math.degrees(math.acos(abs(t['normal'][1]))) for t in roof]
    modules = []
    for group in side_groups:
        module = module_summary(triangles, group)
        if module is None:
            continue
        ids = module['triangle_indices']
        patches = coplanar_fields([triangles[i] for i in ids], min_area_m2=0)
        module['connected_patch_count'] = len(patches)
        module['patches'] = []
        for patch in patches:
            p = np.asarray([triangles[ids[i]]['points'] for i in patch]).reshape(-1, 3)
            uv = np.asarray([triangles[ids[i]]['uv'] for i in patch]).reshape(-1, 2)
            module['patches'].append(dict(triangle_indices=[ids[i] for i in patch],
                                         width_cm=float(np.ptp(p@axis)),
                                         height_cm=float(np.ptp(p[:, 1])),
                                         uv_span=np.ptp(uv, axis=0).tolist()))
        modules.append(module)
    return dict(selection=('Manual oriented XZ frame; uncut triangles with all three vertices inside it. '
                           'Only beam vobs 9906/9907; horizontal wall normals within 6.8 degrees of the manual axes; '
                           'roof normals within 6.9 degrees of perpendicular to the longitudinal axis.'),
                waypoint='NW_CITY_HABOUR_POOR_AREA_HUT_05_BED_02',
                origin=origin.tolist(), outward=(-depth).tolist(), tangent=axis.tolist(),
                manual_axes=dict(longitudinal=axis.tolist(), transverse=depth.tolist()),
                manual_bounds_cm=dict(longitudinal=bounds[:, 0].tolist(), transverse=bounds[:, 1].tolist()),
                triangle_indices=selected, wall_group_ids=groups_inside,
                front_wall_group=front['id'], back_wall_group=back['id'],
                nominal_plan_size_cm=[front['width_cm'], abs(side_planes[1]-side_planes[0])],
                nominal_wall_height_cm=front['height_cm'],
                wall_y_limits_cm=front['height_limits_cm'],
                roof_y_limits_cm=[float(roof_points[:, 1].min()), float(roof_points[:, 1].max())],
                bounds_cm=[points.min(axis=0).tolist(), points.max(axis=0).tolist()],
                roof_pitch_degrees_range=[float(min(pitches)), float(max(pitches))],
                roof_pitch_degrees_area_weighted=float(np.average(pitches, weights=[t['area_m2'] for t in roof])),
                textures_m2=texture_areas(chosen),
                material_uv_scale={name: material_scale(chosen, name) for name in (HARBOUR[0], HARBOUR[2])},
                window_modules=modules)


PALETTE = {
    HARBOUR[0]: '#d8b883', HARBOUR[1]: '#c3d9e8', HARBOUR[2]: '#d89464',
    ROOF: '#a8adb1', BEAMS[0]: '#242b32', BEAMS[1]: '#554333',
}


def cameras(measurement, ground, scenario=None):
    """Six measured anchors for HUT_05; the game keeps its installation FOV."""
    hut = measurement['primary_hut']
    a, b = np.asarray(hut['tangent']), -np.asarray(hut['outward'])
    anchors = (
        ('front', 4380, -2825, 150, -b, 600, 400),
        ('gable', 3860, -2427, 150, -a, 500, 170),
        ('three-quarter', 4380, -2427, 150, -(a+b)/math.sqrt(2), 1300, 170),
        ('window', 4060, -2820, 15, -b, 400, 170),
        ('planks', 4370, -2820, 30, -b, 300, 170),
        ('gable-close', 3860, -2427, 295, -a, 400, 170),
    )
    lines = ['# B037 HUT_05: original resources, default game FOV, noon.',
             '# Fixed cameras measured from the selected hut geometry; world units are cm.',
             'cheat god', 'clear 1000000', 'set time 12 0', 'overlay off',
             'view hide animated morph pfx', 'cam lift off', '']
    result = {}
    for name, longitudinal, transverse, height, direction, distance, eye_height in anchors:
        target = a*longitudinal+b*transverse+[0., height, 0.]
        eye = target+direction*distance
        floor = ground_height(ground, eye[0], eye[2], -50)
        eye[1] = floor+eye_height
        hero = target.copy()
        hero[1] = 950
        delta = eye-hero
        yaw = math.degrees(math.atan2(delta[2], delta[0]))
        result[name] = dict(eye_cm=eye.tolist(), target_cm=target.tolist(), ground_y_cm=floor,
                            eye_above_ground_cm=eye_height, distance_cm=distance)
        lines += [f'# {name}: {distance/100:g} m; eye = ground + {eye_height} cm.',
                  f'cam look {target[1]-hero[1]:.3f}',
                  'goto pos '+' '.join(f'{v:.3f}' for v in hero),
                  f'cam world {distance:.3f} {delta[1]:.3f} {yaw:.6f}',
                  'wait 34', 'cam hold', 'wait 6000', f'shot {name}', 'wait 300', '']
    if scenario:
        scenario.write_text('\n'.join(lines))
    return result


def diagram(measurement, output):
    """Orthographic geometry plates; colours identify materials, never texture pixels."""
    from PIL import Image, ImageDraw, ImageFont
    hut = measurement['primary_hut']
    triangles = [measurement['candidate_house_triangles'][i] for i in hut['triangle_indices']]
    a = np.asarray(hut['tangent'])
    b = -np.asarray(hut['outward'])
    points = np.asarray([t['points'] for t in triangles])
    floor = -66.220535278
    output.mkdir(parents=True, exist_ok=True)
    font_path = '/System/Library/Fonts/Supplemental/Arial.ttf'

    def font(size):
        try:
            return ImageFont.truetype(font_path, size)
        except OSError:
            return ImageFont.load_default(size=size)

    for name, horizontal, outward in (('front', a, -b), ('gable', -b, -a), ('rear', -a, b),
                                      ('plan', a, np.array([0., 1., 0.]))):
        image = Image.new('RGB', (1800, 1180), 'white')
        draw = ImageDraw.Draw(image)
        vertical = b if name == 'plan' else np.array([0., 1., 0.])
        xy = np.stack((points @ horizontal, points @ vertical), axis=2)
        lo, hi = xy.min(axis=(0, 1)), xy.max(axis=(0, 1))
        scale = min(1420/(hi[0]-lo[0]), 690/(hi[1]-lo[1]))
        offset = np.array([180+(1420-scale*(hi[0]-lo[0]))/2, 150.])

        def pixel(p):
            return tuple(offset + [scale*(p[0]-lo[0]), scale*(hi[1]-p[1])])

        near_plane = {'front': 2786.63, 'rear': -2066.64, 'gable': -3930.}[name] if name != 'plan' else None
        for index in np.argsort(points.mean(axis=1) @ outward):
            t = triangles[index]
            upper_timber = t['texture'] == BEAMS[1] and points[index, :, 1].max() > 250
            if (near_plane is not None and t['texture'] != ROOF and not upper_timber
                    and np.dot(t['center'], outward) < near_plane-150):
                continue
            polygon = [pixel(p) for p in xy[index]]
            draw.polygon(polygon, fill=PALETTE[t['texture']])
            draw.line(polygon+[polygon[0]], fill='#777777', width=1)
            if t['texture'] == HARBOUR[1] and name != 'plan':
                draw.line(polygon+[polygon[0]], fill='#285574', width=2)
                # Integer UV lines expose module boundaries even across triangulated faces.
                uv = np.asarray(t['uv'])
                for axis in (0, 1):
                    for level in range(math.ceil(uv[:, axis].min()), math.floor(uv[:, axis].max())+1):
                        hits = []
                        for j, k in ((0, 1), (1, 2), (2, 0)):
                            delta = uv[k, axis]-uv[j, axis]
                            if abs(delta) < 1e-8:
                                continue
                            q = (level-uv[j, axis])/delta
                            if 0 <= q <= 1:
                                hits.append(pixel(xy[index, j]+q*(xy[index, k]-xy[index, j])))
                        if len(hits) >= 2:
                            draw.line(hits[:2], fill='#285574', width=3)
        if name == 'plan':
            # The two roof skins stop at the thick ridge timber, leaving separate high edges.
            edges = set()
            for t in triangles:
                if t['texture'] != ROOF:
                    continue
                p = np.asarray(t['points'])
                for j, k in ((0, 1), (1, 2), (2, 0)):
                    key = tuple(sorted((tuple(np.rint(p[j]).astype(int)), tuple(np.rint(p[k]).astype(int)))))
                    edges.add(key)
            for edge in edges:
                p = np.asarray(edge)
                if (p[:, 1].min() > hut['roof_y_limits_cm'][0]+100
                        and np.ptp(p@a) > 300 and np.ptp(p@b) < 100):
                    draw.line([pixel([v@horizontal, v@vertical]) for v in p], fill='#b12525', width=5)
            a_mid = 4380.
            b_mid = -2426.63
            outline = [a*(a_mid+u)+b*(b_mid+v) for u, v in
                       ((-450, -360), (450, -360), (450, 360), (-450, 360), (-450, -360))]
            draw.line([pixel([v@horizontal, v@vertical]) for v in outline], fill='#285574', width=3)
            draw.text((180, 905), 'Red: roof edges at ridge timber. Blue: nominal wall plan; outer contour: roof overhang.',
                      fill='#333333', font=font(24))
        else:
            # The attic trusses sit behind the cladding; show their edges as hidden structure.
            for index, t in enumerate(triangles):
                if t['texture'] != BEAMS[0]:
                    continue
                polygon = np.asarray([pixel(p) for p in xy[index]])
                for j, k in ((0, 1), (1, 2), (2, 0)):
                    length = np.linalg.norm(polygon[k]-polygon[j])
                    for start in np.arange(0, length, 18):
                        segment = [tuple(polygon[j]+(polygon[k]-polygon[j])*q/max(length, 1))
                                   for q in (start, min(start+9, length))]
                        draw.line(segment, fill=PALETTE[BEAMS[0]], width=2)
            levels = [hut['wall_y_limits_cm'][0], floor, hut['wall_y_limits_cm'][1], hut['roof_y_limits_cm'][1]]
            last_y = -100.
            for y in sorted(levels, reverse=True):
                py = pixel([lo[0], y])[1]
                label_y = max(py-14, last_y+31)
                draw.line([(105, py), (165, py)], fill='#b12525', width=2)
                draw.text((12, label_y), f'{y-floor:+.0f} cm', fill='#b12525', font=font(25))
                last_y = label_y
            draw.text((180, 890), 'Heights: BED_02 floor = 0 cm. Blue edges: HOLZWAND_02 / integer UV lines.',
                      fill='#333333', font=font(24))
            draw.text((180, 925), 'Dashed dark edges: hidden WOODBEAM_01 attic trusses.', fill='#333333', font=font(24))
        draw.text((60, 35), f'B037 / HUT_05 — {name}: orthographic geometry', fill='#17191b', font=font(34))
        draw.text((180, 850), 'Wall plan: %.0f × %.0f cm; drawing extent: %.0f × %.0f cm' %
                  (*hut['nominal_plan_size_cm'], *(hi-lo)), fill='#333333', font=font(26))
        for i, (texture, colour) in enumerate(PALETTE.items()):
            x, y = 75+(i % 2)*850, 970+(i//2)*58
            draw.rectangle((x, y, x+38, y+30), fill=colour, outline='#555555')
            draw.text((x+50, y), texture.removeprefix('NW_CITY_').removesuffix('.TGA'), fill='#17191b', font=font(25))
        image.save(output/f'hut-{name}-geometry.png', optimize=True)


def sheet(captures, output):
    from PIL import Image, ImageDraw, ImageFont
    views = ('front', 'gable', 'three-quarter', 'window', 'planks', 'gable-close')
    image = Image.new('RGB', (1920, 800), '#17191b')
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=24)
    for i, name in enumerate(views):
        x, y = (i % 3)*640, (i//3)*400
        with Image.open(captures/'original'/f'{name}.png') as frame:
            if frame.size != (1280, 720):
                raise ValueError(f'{name}: expected 1280x720')
            image.paste(frame.convert('RGB').resize((640, 360), Image.Resampling.LANCZOS), (x, y+40))
        draw.text((x+12, y+7), f'B037 / HUT_05 | {name}', font=font, fill='white')
    for quality in (90, 85, 80, 75):
        image.save(output, quality=quality, optimize=True)
        if output.stat().st_size <= 1_000_000:
            print(f'{output}: 6 panels, {output.stat().st_size} bytes, JPEG quality {quality}')
            return
    raise ValueError('Contact sheet exceeds 1 MB')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=LOCAL/'harbour-house/triangles.json')
    parser.add_argument('--survey', type=Path, default=LOCAL/'harbour-house/survey.json')
    parser.add_argument('--mapping', type=Path, default=ROOT/'data/content/city-materials.csv')
    parser.add_argument('--output', type=Path, default=LOCAL/'texture-intent/harbour-hut-measurement.json')
    parser.add_argument('--draw', type=Path)
    parser.add_argument('--captures', type=Path)
    parser.add_argument('--scenario', type=Path)
    args = parser.parse_args()
    with args.mapping.open() as stream:
        mapping = list(csv.DictReader(stream))
    raw = json.loads(args.input.read_text())
    result = measure(raw, json.loads(args.survey.read_text()), mapping)
    result['cameras'] = cameras(result, walking_ground(raw, mapping), args.scenario)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    if args.draw:
        diagram(result, args.draw)
        if args.captures:
            sheet(args.captures, args.draw/'hut-original.jpg')
    print(json.dumps(dict(block=result['block_id'], triangle_count=len(result['candidate_house_triangles']),
                          wall_groups=len(result['wall_groups']), main_wall_group=result['main_wall_group'],
                          repeated_blocks=[b['id'] for b in result['repeated_blocks']]), indent=2))


if __name__ == '__main__':
    main()

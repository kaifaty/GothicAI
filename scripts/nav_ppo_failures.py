#!/usr/bin/env python3
"""Describe recorded final-drive failures, without running policy or physics.

NAVDEMO ``keys`` are teacher labels. Applied actions ALWAYS come from actions.txt.
The eye diagram reconstructs yaw/pitch, not the unrecorded head roll/eye origin;
cached far rays are excluded because their original pose was not serialized.
Memory connectivity describes recorded free cells, never physical traversability.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos

CONFIDENCE = .2
OBSTACLE = .2
BODY_RADIUS_CM = 30.
CHANNELS = nav_demos.CHANNELS


def read(path):
    return json.loads(Path(path).read_text())


def wrapped(degrees):
    return (np.asarray(degrees) + 180.) % 360. - 180.


def body_to_world(values, yaw_degrees):
    """Body [right,up,forward] -> world XYZ, matching navdemos.cpp::local."""
    values = np.asarray(values, dtype=float)
    yaw = np.deg2rad(np.asarray(yaw_degrees))
    x, z = values[..., 0], values[..., 2]
    return np.stack((-x*np.sin(yaw)+z*np.cos(yaw), values[..., 1],
                     x*np.cos(yaw)+z*np.sin(yaw)), axis=-1)


def ray_directions(layout, head_yaw_degrees, head_pitch_degrees):
    """Approximate [right,up,forward] ray direction, including posed head yaw.

    Exact only when head roll is zero: roll and eye origin are absent in NAVDEMO.
    """
    yaw, pitch = np.deg2rad(layout[:, 0]), np.deg2rad(layout[:, 1])
    head_yaw, head_pitch = np.deg2rad([head_yaw_degrees, head_pitch_degrees])
    forward = np.array([np.sin(head_yaw)*np.cos(head_pitch), np.sin(head_pitch),
                        np.cos(head_yaw)*np.cos(head_pitch)])
    right = np.array([np.cos(head_yaw), 0., -np.sin(head_yaw)])
    up = np.array([-np.sin(head_yaw)*np.sin(head_pitch), np.cos(head_pitch),
                   -np.cos(head_yaw)*np.sin(head_pitch)])
    return (np.cos(pitch)[:, None]*np.cos(yaw)[:, None]*forward
            + np.cos(pitch)[:, None]*np.sin(yaw)[:, None]*right
            + np.sin(pitch)[:, None]*up)


def align_actions(records, actions, trajectory, episode, retained):
    """Map absolute sensor timestamps to the exact applied-action rows."""
    if actions.ndim != 2 or actions.shape != (episode['decisions'] + 1, 9):
        raise ValueError('Invalid applied actions shape')
    if trajectory.ndim != 2 or trajectory.shape[1] != 5:
        raise ValueError('Invalid trajectory shape')
    origin = int(retained['terminal_time_ms']) - int(episode['elapsed_ms'])
    relative = records['time_ms'].astype(np.int64) - origin
    index = np.searchsorted(actions[:-1, 0], relative)
    if (np.any(index >= len(actions)-1)
            or not np.array_equal(actions[index, 0], relative)):
        raise ValueError('Sensor/applied action timestamps do not align')
    pose_index = np.searchsorted(trajectory[:, 0], relative)
    if (np.any(pose_index >= len(trajectory))
            or not np.array_equal(trajectory[pose_index, 0], relative)):
        raise ValueError('Sensor/trajectory timestamps do not align')
    pose_error = np.linalg.norm(trajectory[pose_index, 1:4] - records['position_cm'], axis=1)
    if np.max(pose_error, initial=0) > 1.:
        raise ValueError('Sensor/trajectory position mismatch >1 cm')
    return actions[index], relative, dict(action_source='actions.txt', teacher_keys_used=False,
               absolute_time_origin_ms=origin, matched_records=len(records),
               max_pose_error_cm=float(np.max(pose_error, initial=0)))


def channel_masks(memory, free_threshold=CONFIDENCE):
    channels = memory['channels'].astype(float) / 255.
    known = channels[..., 0] >= CONFIDENCE
    # Ledge, drop, water and movable are hazards too; no inference from confidence alone.
    obstacle = channels[..., 2:].max(axis=-1) >= OBSTACLE
    free = known & (channels[..., 1] >= free_threshold) & ~obstacle
    return known, free, obstacle


def pooled_memory(memory, factor):
    """Mean probability channels exactly as actor preprocessing (no Torch)."""
    n=len(memory)//factor
    result=np.zeros((n,n),dtype=[('channels','f4',(7,))])
    result['channels']=memory['channels'].astype(np.float32).reshape(n,factor,n,factor,7).mean(axis=(1,3))
    return result


def cell_index(point_right_forward, size, cell_cm):
    right, forward = point_right_forward
    return int(np.floor(forward / cell_cm + size/2)), int(np.floor(right / cell_cm + size/2))


def inside(index, size):
    return 0 <= index[0] < size and 0 <= index[1] < size


def dilate(mask, radius_cells):
    result = mask.copy()
    radius = int(np.ceil(radius_cells))
    n = len(mask)
    for dr in range(-radius, radius+1):
        for dc in range(-radius, radius+1):
            if dr*dr + dc*dc > radius_cells*radius_cells + 1.e-9:
                continue
            a, b = max(0, dr), min(n, n+dr)
            c, d = max(0, dc), min(n, n+dc)
            result[a:b, c:d] |= mask[a-dr:b-dr, c-dc:d-dc]
    return result


def connectivity(memory, cell_cm, target, free_threshold=CONFIDENCE, inflate_cm=0.):
    """4-connected chain of observed free cells; unknown is blocked.

    An outside target is clipped to a local proxy, explicitly marked as such.
    Inflation expands recorded hazards; missing height/clearance remains unknown.
    """
    known, free, obstacle = channel_masks(memory, free_threshold)
    n = len(memory)
    free = free & ~dilate(obstacle, inflate_cm / cell_cm)
    target = np.asarray(target, dtype=float)
    half = n*cell_cm/2
    outside = bool(np.any(np.abs(target) >= half))
    proxy = target.copy()
    if outside:
        proxy *= (half-cell_cm) / max(np.abs(proxy))
    start, end = (n//2, n//2), cell_index(proxy, n, cell_cm)
    result = dict(status=None, target_outside_grid=outside, local_proxy_cm=proxy.tolist(),
                  free_threshold=free_threshold, obstacle_threshold=OBSTACLE,
                  obstacle_inflation_cm=inflate_cm, clearance_not_proven=True,
                  reachable_cells=0, farthest_reachable_m=0., path_cells=0)
    if not free[start]:
        result['status'] = 'start_obstacle' if obstacle[start] else ('start_unknown' if not known[start] else 'start_not_known_free')
        return result
    visited = np.zeros_like(free)
    visited[start] = True
    queue = deque([(start, 0)])
    endpoint_steps = None
    farthest = 0.
    while queue:
        (r, c), steps = queue.popleft()
        farthest = max(farthest, np.hypot(r-start[0], c-start[1])*cell_cm/100)
        if (r, c) == end:
            endpoint_steps = steps
        for next_cell in ((r-1,c), (r+1,c), (r,c-1), (r,c+1)):
            if inside(next_cell, n) and free[next_cell] and not visited[next_cell]:
                visited[next_cell] = True
                queue.append((next_cell, steps+1))
    result.update(reachable_cells=int(visited.sum()), farthest_reachable_m=float(farthest))
    if endpoint_steps is not None:
        result.update(status='connected_to_local_proxy' if outside else 'connected', path_cells=endpoint_steps+1)
    elif obstacle[end]:
        result['status'] = 'local_proxy_obstacle' if outside else 'target_obstacle'
    elif not known[end]:
        result['status'] = 'local_proxy_unknown' if outside else 'target_unknown'
    elif not free[end]:
        result['status'] = 'local_proxy_not_known_free' if outside else 'target_not_known_free'
    else:
        result['status'] = 'not_connected'
    return result


def line_evidence(memory, cell_cm, target):
    known, free, obstacle = channel_masks(memory)
    n = len(memory)
    target = np.asarray(target, dtype=float)
    distance = float(np.linalg.norm(target))
    half=n*cell_cm/2
    scale=min(1.,half/max(np.max(np.abs(target)),1.e-9))
    clipped=target*scale
    points = np.linspace(np.zeros(2), clipped, max(2, int(distance*scale/(cell_cm/3))+1))
    indices = []
    for point in points:
        index = cell_index(point, n, cell_cm)
        if not inside(index, n):
            break
        if not indices or indices[-1] != index:
            indices.append(index)
    count = len(indices)
    cells = np.asarray(indices)
    masks = dict(known=known, known_free=free, obstacle=obstacle, unknown=~known)
    result = {name+'_cells': int(mask[cells[:,0], cells[:,1]].sum()) for name, mask in masks.items()}
    result.update(total_cells=count, target_outside_grid=not inside(cell_index(target, n, cell_cm), n))
    return result


def memory_view(memory, cell_cm, carrot, goal):
    known, free, obstacle = channel_masks(memory)
    probabilities = memory['channels'].astype(float)/255.
    result = dict(cell_cm=cell_cm, half_width_m=len(memory)*cell_cm/200,
                  known_cells=int(known.sum()), known_free_cells=int(free.sum()),
                  obstacle_cells=int(obstacle.sum()), unknown_cells=int((~known).sum()),
                  channel_cells_ge_0p2={name:int((probabilities[...,i]>=.2).sum()) for i,name in enumerate(CHANNELS)},
                  carrot_line=line_evidence(memory, cell_cm, carrot), goal_line=line_evidence(memory, cell_cm, goal),
                  carrot_connectivity=connectivity(memory, cell_cm, carrot),
                  goal_connectivity=connectivity(memory, cell_cm, goal))
    r,c=np.indices(known.shape)
    forward=(r+.5-len(memory)/2)*cell_cm
    right=(c+.5-len(memory)/2)*cell_cm
    zones=dict(front_2m=(forward>=0)&(forward<=200)&(np.abs(right)<=75),
               left_2m=(right<0)&(right>=-200)&(np.abs(forward)<=100),
               right_2m=(right>0)&(right<=200)&(np.abs(forward)<=100),
               central_1m=(right*right+forward*forward<=100**2))
    result['local_zones']={name:dict(cells=int(zone.sum()),known_free=int((zone&free).sum()),
                obstacle=int((zone&obstacle).sum()),unknown=int((zone&~known).sum()),
                channel_cells_ge_0p2={channel:int((zone&(probabilities[...,i]>=.2)).sum())
                                      for i,channel in enumerate(CHANNELS)})
                for name,zone in zones.items()}
    if cell_cm <= 50:
        middle=len(memory)//2
        result['center_four_cells']=[dict(row=row,column=column,
                 right_cm=float(right[row,column]),forward_cm=float(forward[row,column]),
                 channels={name:float(probabilities[row,column,i]) for i,name in enumerate(CHANNELS)})
                 for row in (middle-1,middle) for column in (middle-1,middle)]
        result['carrot_connectivity_inflated_30cm'] = connectivity(memory, cell_cm, carrot, inflate_cm=BODY_RADIUS_CM)
        result['carrot_free_threshold_sensitivity'] = {str(value):connectivity(memory, cell_cm, carrot, value) for value in (.1,.4)}
    else:
        result['coarse_only'] = f'{cell_cm/100:g} m cells do not resolve a body-width passage or its clearance'
    return result


def eye_view(record, layout):
    directions = ray_directions(layout, record['body']['head_yaw_deg'], record['body']['head_pitch_deg'])
    eye = record['eye']
    body_yaw = np.rad2deg(np.arctan2(directions[:,0], directions[:,2]))
    body_pitch = np.rad2deg(np.arctan2(directions[:,1], np.linalg.norm(directions[:,[0,2]],axis=1)))
    fresh = eye['age_ms'] <= 100
    wall = eye['hit'].astype(bool) & (np.abs(eye['normal_y'])<=.3) & fresh
    ground = eye['hit'].astype(bool) & (eye['normal_y']>=.7) & fresh
    # Near horizontal rays provide an approximate head-height obstacle view.
    horizontal = np.abs(body_pitch) <= 25
    result = dict(fresh_rays=int(fresh.sum()), stale_rays=int((~fresh).sum()),
                  fresh_wall_hits=int(wall.sum()), fresh_ground_hits=int(ground.sum()),
                  body_head_yaw_deg=float(record['body']['head_yaw_deg']),
                  head_pitch_deg=float(record['body']['head_pitch_deg']))
    front=np.abs(body_yaw)<=25
    front_hits=np.flatnonzero(front & fresh & eye['hit'].astype(bool))
    nearest=front_hits[np.argsort(eye['distance_cm'][front_hits],kind='stable')[:8]]
    result['front_anypitch']=dict(fresh_rays=int((front&fresh).sum()),
            vertical_wall_hits_within_2m=int((front&wall&(eye['distance_cm']<=200)).sum()),
            ground_hits_within_2m=int((front&ground&(eye['distance_cm']<=200)).sum()),
            closest_fresh_hits=[dict(ray_index=int(i),distance_m=float(eye['distance_cm'][i]/100),
                kind=int(eye['kind'][i]),normal_y=float(eye['normal_y'][i]),
                body_yaw_deg=float(body_yaw[i]),body_pitch_deg=float(body_pitch[i]),
                age_ms=int(eye['age_ms'][i])) for i in nearest])
    for name, sector in [('front', np.abs(body_yaw)<=25),
                         ('left', (body_yaw>=-100)&(body_yaw<-25)),
                         ('right',(body_yaw>25)&(body_yaw<=100))]:
        mask = sector & horizontal & fresh
        walls = mask & wall
        distances = eye['distance_cm'][walls]
        result[name] = dict(fresh_horizontal_rays=int(mask.sum()),wall_hits=int(walls.sum()),
                            wall_hits_within_1m=int((distances<=100).sum()),
                            wall_hits_within_2m=int((distances<=200).sum()),
                            nearest_wall_m=float(distances.min()/100) if len(distances) else None,
                            ground_hits=int((mask & ground).sum()),
                            other_surface_hits=int((mask & eye['hit'].astype(bool) & ~wall & ~ground).sum()))
    return result


def behaviour(trajectory, actions, target_position):
    elapsed = trajectory[-1,0]
    tail = trajectory[trajectory[:,0]>=elapsed-10_000]
    path = float(np.linalg.norm(np.diff(tail[:,[1,3]],axis=0),axis=1).sum())
    displacement = float(np.linalg.norm(tail[-1,[1,3]]-tail[0,[1,3]]))
    yaw_delta = wrapped(np.diff(tail[:,4]))
    yaw_travel = float(np.abs(yaw_delta).sum())
    yaw_span = float(np.ptp(np.rad2deg(np.unwrap(np.deg2rad(tail[:,4])))))
    goal_distances = np.linalg.norm(tail[:,1:4]-np.asarray(target_position),axis=1)
    actual = actions[(actions[:,0]>=tail[0,0])&(actions[:,0]<elapsed)]
    moving = (actual[:,1]!=0)|(actual[:,2]!=0)
    turning = actual[:,3]!=0
    def switches(values):
        nonzero = values[values!=0]
        return int((nonzero[1:]!=nonzero[:-1]).sum())
    reversals = {axis:switches(actual[:,col]) for axis,col in [('forward',1),('strafe',2),('turn',3)]}
    flags=[]
    if path<=50:
        flags.append('stationary')
    if yaw_travel>=90 and displacement<100:
        flags.append('turning_with_small_displacement')
    if path>=100 and displacement/max(path,1)<.3 and max(reversals.values())>=4:
        flags.append('local_oscillation')
    goal_change = float(goal_distances[-1]-goal_distances[0])
    if displacement>=100 and goal_change>50:
        flags.append('moving_away_in_euclidean_distance')
    if not flags:
        flags.append('local_motion' if displacement<100 else 'moving_at_timeout')
    return dict(labels=flags, elapsed_s=float((tail[-1,0]-tail[0,0])/1000),
                path_m=path/100, displacement_m=displacement/100,
                xz_span_m=float(np.linalg.norm(np.ptp(tail[:,[1,3]],axis=0))/100),
                yaw_travel_deg=yaw_travel, yaw_span_deg=yaw_span,
                yaw_net_deg=float(wrapped(tail[-1,4]-tail[0,4])),
                euclidean_goal_distance_change_m=goal_change/100,
                movement_action_fraction=float(moving.mean()),turn_action_fraction=float(turning.mean()),
                turn_only_action_fraction=float((turning & ~moving).mean()),
                forward_fraction=float((actual[:,1]>0).mean()),backward_fraction=float((actual[:,1]<0).mean()),
                jump_fraction=float((actual[:,5]!=0).mean()),climb_fraction=float((actual[:,6]!=0).mean()),
                sign_reversals=reversals,
                caveat='Euclidean motion away from goal does not prove a wrong route; heading/turn commands do not prove intent')


def summarize_episode(directory, measured):
    episode = read(directory/'episode.json')
    retained = read(directory/'retained-sensors.json')
    records = nav_demos.load_episode(directory/'tail-10s.bin.gz')
    if len(records)==0 or nav_demos.episode_version(directory/'tail-10s.bin.gz')!=2:
        raise ValueError(f'Expected nonempty v2 sensor tail: {directory}')
    actions = np.loadtxt(directory/'actions.txt',ndmin=2)
    trajectory = np.loadtxt(directory/'trajectory.txt',ndmin=2)
    _, relative, alignment = align_actions(records,actions,trajectory,episode,retained)
    layout = nav_demos.ray_layout(directory/'tail-10s.bin.gz')
    eye = [eye_view(record,layout) for record in records]
    final=records[-1]
    roads=records['roads']
    carrot=roads['carrot_direction']*roads['carrot_distance_cm'][:,None]
    goals=roads['goal_direction']*roads['goal_distance_cm'][:,None]
    carrot_world=records['position_cm']+body_to_world(carrot,records['body_yaw_deg'])
    angle=np.rad2deg(np.arctan2(carrot[:,0],carrot[:,2]))
    goal_angle=np.rad2deg(np.arctan2(goals[:,0],goals[:,2]))
    snapshots=[]
    for index in np.unique(np.linspace(0,len(records)-1,5).astype(int)):
        record=records[index]
        snapshots.append(dict(relative_ms=int(relative[index]),
            near=memory_view(record['memory_near'],25,carrot[index,[0,2]],goals[index,[0,2]]),
            far=memory_view(record['memory_far'],200,carrot[index,[0,2]],goals[index,[0,2]]),
            actor_near=memory_view(pooled_memory(record['memory_near'],2),50,carrot[index,[0,2]],goals[index,[0,2]]),
            actor_far=memory_view(pooled_memory(record['memory_far'],4),800,carrot[index,[0,2]],goals[index,[0,2]])))
    result=dict(index=episode.get('index',measured.get('index')), region=episode.get('region',measured.get('region')),
                start=episode.get('start',measured.get('start')),goal=episode.get('goal',measured.get('goal')),
                episode=str(directory), measured=measured, alignment=alignment,
                behaviour=behaviour(trajectory,actions,episode['target_position']),
                final_eye=eye[-1],
                eye_tail=dict(front_close_wall_frame_fraction=float(np.mean([x['front']['wall_hits_within_1m']>0 for x in eye])),
                              front_wall_2m_frame_fraction=float(np.mean([x['front']['wall_hits_within_2m']>0 for x in eye])),
                              front_wall_anypitch_2m_frame_fraction=float(np.mean([x['front_anypitch']['vertical_wall_hits_within_2m']>0 for x in eye])),
                              front_ground_anypitch_2m_frame_fraction=float(np.mean([x['front_anypitch']['ground_hits_within_2m']>0 for x in eye])),
                              median_front_nearest_wall_m=float(np.median([x['front']['nearest_wall_m'] for x in eye if x['front']['nearest_wall_m'] is not None])) if any(x['front']['nearest_wall_m'] is not None for x in eye) else None),
                carrot=dict(final_distance_m=float(roads['carrot_distance_cm'][-1]/100),
                    final_height_difference_m=float(carrot[-1,1]/100),
                    final_goal_height_difference_m=float(goals[-1,1]/100),
                    final_angle_deg=float(angle[-1]),final_goal_angle_deg=float(goal_angle[-1]),
                    final_goal_carrot_disagreement_deg=float(abs(wrapped(goal_angle[-1]-angle[-1]))),
                    angular_range_unwrapped_deg=float(np.ptp(np.rad2deg(np.unwrap(np.deg2rad(angle))))),
                    world_drift_m=float(np.linalg.norm(carrot_world[-1]-carrot_world[0])/100),
                    world_span_m=float(np.linalg.norm(np.ptp(carrot_world,axis=0))/100),
                    remaining_known_m=float(roads['remaining_known_cm'][-1]/100),
                    final_goal_distance_m=float(roads['goal_distance_cm'][-1]/100),
                    beyond_known=bool(roads['beyond_known'][-1])),
                memory_snapshots=snapshots,
                body_final=dict(yaw_deg=float(final['body_yaw_deg']),ground_slope_deg=float(final['body']['ground_slope_deg']),
                                flags={name:bool(final['body']['flags'][i]) for i,name in enumerate(nav_demos.BODY_FLAGS)}))
    return result, records, trajectory, layout


def draw_episode(result, records, trajectory, layout, destination):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.colors import ListedColormap
    final=records[-1]
    roads=final['roads']
    carrot=roads['carrot_direction'][[0,2]]*roads['carrot_distance_cm']/100
    goal=roads['goal_direction'][[0,2]]*roads['goal_distance_cm']/100
    delta=trajectory[trajectory[:,0]>=trajectory[-1,0]-10000,1:4]-final['position_cm']
    yaw=np.deg2rad(float(final['body_yaw_deg']))
    path=np.column_stack((-delta[:,0]*np.sin(yaw)+delta[:,2]*np.cos(yaw),delta[:,0]*np.cos(yaw)+delta[:,2]*np.sin(yaw)))/100
    fig,axes=plt.subplots(1,3,figsize=(14,4.8))
    cmap=ListedColormap(['#e3e5e8','#b1d5c5','#cb6767','#d5c7a2'])
    for ax,name,cell in zip(axes[:2],('near','far'),(.25,2.)):
        memory=final['memory_'+name]
        known,free,obstacle=channel_masks(memory)
        image=np.zeros(known.shape,dtype=int); image[known]=3;image[free]=1;image[obstacle]=2
        half=len(memory)*cell/2
        ax.imshow(image,origin='lower',extent=(-half,half,-half,half),cmap=cmap,vmin=0,vmax=3,interpolation='nearest')
        ax.plot(path[:,0],path[:,1],color='#2855a0',lw=1.2,label='last 10 s')
        ax.scatter([0],[0],c='black',s=18,zorder=5)
        targets = [(carrot,'#df8b00','carrot',float(roads['carrot_distance_cm'])/100),
                   (goal,'#7b3ba5','goal',float(roads['goal_distance_cm'])/100)]
        if np.allclose(carrot,goal,atol=.001):
            targets = [(carrot,'#df8b00','carrot=goal',float(roads['goal_distance_cm'])/100)]
        for vector,color,label,length in targets:
            shown=vector*min(1,half*.85/max(np.max(np.abs(vector)),1.e-6))
            ax.annotate('',xy=shown,xytext=(0,0),arrowprops=dict(arrowstyle='->',color=color,lw=1.8))
            ax.text(shown[0],shown[1],f'{label} {length:.1f}m (3D)',color=color,fontsize=7)
        if name=='near':
            directions=ray_directions(layout,final['body']['head_yaw_deg'],final['body']['head_pitch_deg'])
            fresh=final['eye']['age_ms']<=100
            horizontal=np.abs(directions[:,1])<=np.sin(np.deg2rad(25))
            for i in np.flatnonzero(fresh & horizontal):
                distance=min(float(final['eye']['distance_cm'][i])/100,6)
                point=directions[i,[0,2]]*distance
                wall=bool(final['eye']['hit'][i]) and abs(float(final['eye']['normal_y'][i]))<=.3
                ax.plot([0,point[0]],[0,point[1]],color='#b92323' if wall else '#888',alpha=.25,lw=.6)
        ax.set(xlim=(-half,half),ylim=(-half,half),aspect='equal',xlabel='right of final body (m)',ylabel='forward (m)',title=f'{name}: known free green; hazards red')
        ax.grid(alpha=.1)
    t=trajectory[trajectory[:,0]>=trajectory[-1,0]-10000]
    axes[2].plot((t[:,0]-trajectory[-1,0])/1000,np.rad2deg(np.unwrap(np.deg2rad(t[:,4])))-t[0,4],label='yaw change (deg)')
    axes[2].set(xlabel='seconds before end',ylabel='yaw from tail start (deg)',title='Recorded pose and actual behaviour')
    axes[2].grid(alpha=.2)
    b=result['behaviour'];e=result['eye_tail'];m=result['memory_snapshots'][-1]['near']
    axes[2].text(.02,.02,f"{' / '.join(b['labels'])}\npath {b['path_m']:.2f}m; net {b['displacement_m']:.2f}m\nturn-only actions {b['turn_only_action_fraction']:.0%}\nfront wall <=1m frames {e['front_close_wall_frame_fraction']:.0%}\nnear carrot: {m['carrot_connectivity']['status']}\n30cm inflation: {m['carrot_connectivity_inflated_30cm']['status']}",transform=axes[2].transAxes,fontsize=8,bbox=dict(facecolor='white',alpha=.85))
    raw_front=m['local_zones']['front_2m']['channel_cells_ge_0p2']
    actor_front=result['memory_snapshots'][-1]['actor_near']['local_zones']['front_2m']['channel_cells_ge_0p2']
    def hazard_text(values):
        return ', '.join(f'{name} {values[name]}' for name in ('wall','ledge','drop','water','movable'))
    axes[1].text(.02,.02,'Front 2m channel cells >=.2:\nraw: '+hazard_text(raw_front)
                 +'\nactor: '+hazard_text(actor_front),transform=axes[1].transAxes,fontsize=6.5,
                 bbox=dict(facecolor='white',alpha=.9))
    fig.suptitle(f"{result['index']} {result['region']}: {result['start']} -> {result['goal']}",fontsize=10)
    fig.text(.01,.005,'Eye is an approximate yaw/pitch projection: eye origin/head roll and cached-ray pose were not recorded. Memory connectivity is not physical clearance.',fontsize=7)
    fig.tight_layout(rect=(0,.035,1,.94))
    fig.savefig(destination,dpi=140)
    plt.close(fig)


def run(source, output):
    metrics=read(source/'metrics.json')
    failures=[episode for episode in metrics['episodes'] if not episode['success']]
    output.mkdir(parents=True,exist_ok=True)
    results=[]
    for measured in failures:
        directory=Path(measured['episode'])
        result,records,trajectory,layout=summarize_episode(directory,measured)
        figure=f"failure-{int(result['index']):03d}.png"
        draw_episode(result,records,trajectory,layout,output/figure)
        result['figure']=figure
        results.append(result)
        del records
    payload=dict(source=str(source), failures=len(results),
        behaviour_counts=dict(Counter(label for result in results for label in result['behaviour']['labels'])),
        assumptions=dict(confidence_threshold=CONFIDENCE,free_threshold=CONFIDENCE,obstacle_threshold=OBSTACLE,
            near_obstacle_inflation_cm=BODY_RADIUS_CM,unknown_cells_block_connectivity=True,
            fresh_eye_max_age_ms=100,eye_horizontal_pitch_degrees=25,front_cone_half_angle_degrees=25,
            near_range_m=8,far_half_width_m=100,
            eye_geometry='yaw/pitch approximation only; head roll and eye origin not recorded; old cached-far rays excluded',
            memory_connectivity='4-neighbour recorded free-cell chain, not a proof of body clearance; raw near/far cells .25/2 m; actor mean-pooled cells .5/8 m; no claim CNN decodes connectivity',
            action_provenance='actions.txt only; NAVDEMO keys are teacher labels'),episodes=results)
    (output/'failures.json').write_text(json.dumps(payload,indent=2,allow_nan=False)+'\n')
    print(json.dumps(dict(failures=len(results),behaviour_counts=payload['behaviour_counts'],output=str(output)),ensure_ascii=False))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    run(args.source.resolve(),args.output.resolve())


if __name__=='__main__':
    main()

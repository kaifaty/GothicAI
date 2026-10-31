"""Failure descriptions must preserve action provenance and sensor limitations."""
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import nav_demos
import nav_ppo_failures as failures


def test_alignment_uses_actual_actions_and_exact_absolute_clock():
    records=np.zeros(3,dtype=nav_demos.RECORD_V2_DTYPE)
    records['time_ms']=[10200,10300,10400]
    records['keys']['forward']=-1  # Deliberately opposite teacher label.
    actions=np.zeros((6,9));actions[:,0]=np.arange(6)*100;actions[:-1,1]=1
    trajectory=np.zeros((6,5));trajectory[:,0]=actions[:,0]
    episode=dict(decisions=5,elapsed_ms=500)
    applied,relative,diagnostic=failures.align_actions(records,actions,trajectory,episode,dict(terminal_time_ms=10500))
    assert relative.tolist()==[200,300,400]
    assert applied[:,1].tolist()==[1,1,1]
    assert diagnostic['action_source']=='actions.txt' and not diagnostic['teacher_keys_used']
    bad=records.copy();bad['time_ms'][1]+=1
    with pytest.raises(ValueError,match='timestamps'):
        failures.align_actions(bad,actions,trajectory,episode,dict(terminal_time_ms=10500))


def test_memory_side_opening_connects_without_treating_unknown_as_free():
    memory=np.zeros((8,8),dtype=nav_demos.CELL_DTYPE)
    for r,c in [(4,4),(5,4),(5,5),(5,6),(5,7),(4,7)]:
        memory['channels'][r,c,:2]=255
    memory['channels'][4,5,[0,2]]=255  # Wall blocks direct line, side opening remains.
    target=[75,0]
    assert failures.line_evidence(memory,25,target)['obstacle_cells']==1
    assert failures.connectivity(memory,25,target)['status']=='connected'
    memory['channels'][5,5]=0  # Unknown gap cannot be crossed.
    assert failures.connectivity(memory,25,target)['status']=='not_connected'
    memory['channels'][4,7]=0
    assert failures.connectivity(memory,25,target)['status']=='target_unknown'
    memory['channels'][4,4]=0
    assert failures.connectivity(memory,25,target)['status']=='start_unknown'


def test_eye_alignment_includes_head_yaw_and_does_not_call_ground_a_wall():
    layout=np.zeros((238,3));layout[:,1]=90;layout[:,2]=1000
    layout[:2,1]=0;layout[0,0]=0;layout[1,0]=-90
    record=np.zeros((),dtype=nav_demos.RECORD_V2_DTYPE)
    record['body']['head_yaw_deg']=90
    record['eye']['hit'][:2]=1;record['eye']['distance_cm'][:2]=50
    record['eye']['normal_y'][1]=.8
    directions=failures.ray_directions(layout,90,0)
    np.testing.assert_allclose(directions[0],[1,0,0],atol=1.e-6)
    np.testing.assert_allclose(directions[1],[0,0,1],atol=1.e-6)
    result=failures.eye_view(record,layout)
    assert result['front']['wall_hits_within_1m']==0
    assert result['front']['ground_hits']==1
    assert result['right']['wall_hits_within_1m']==1
    record['eye']['age_ms'][0]=500  # Old cached ray has no reconstructable pose.
    assert failures.eye_view(record,layout)['right']['wall_hits_within_1m']==0


def test_actor_pooling_averages_channels_without_promoting_unknown_to_free():
    memory=np.zeros((4,4),dtype=nav_demos.CELL_DTYPE)
    memory['channels'][0,0,2]=255
    memory['channels'][0,0,0]=255
    pooled=failures.pooled_memory(memory,2)
    assert pooled['channels'][0,0,2]==pytest.approx(255/4)
    known,free,obstacle=failures.channel_masks(pooled)
    assert known[0,0] and obstacle[0,0] and not free.any()


def test_low_front_surface_is_reported_without_calling_it_a_horizontal_wall():
    layout=np.zeros((238,3));layout[:,1]=90;layout[:,2]=1000
    layout[0,1]=-50
    record=np.zeros((),dtype=nav_demos.RECORD_V2_DTYPE)
    record['eye']['hit'][0]=1;record['eye']['distance_cm'][0]=80
    record['eye']['normal_y'][0]=.1;record['eye']['kind'][0]=1
    result=failures.eye_view(record,layout)
    assert result['front']['wall_hits_within_2m']==0
    assert result['front_anypitch']['vertical_wall_hits_within_2m']==1
    hit=result['front_anypitch']['closest_fresh_hits'][0]
    assert hit['distance_m']==pytest.approx(.8) and hit['normal_y']==pytest.approx(.1)
    assert hit['body_pitch_deg']==pytest.approx(-50) and hit['kind']==1
    record['eye']['normal_y'][0]=.8
    result=failures.eye_view(record,layout)
    assert result['front_anypitch']['vertical_wall_hits_within_2m']==0
    assert result['front_anypitch']['ground_hits_within_2m']==1


def test_local_memory_hazards_keep_channels_separate_and_expose_center_cells():
    memory=np.zeros((8,8),dtype=nav_demos.CELL_DTYPE)
    memory['channels'][4,4,[0,3]]=255  # Ledge, without wall.
    memory['channels'][4,5,[0,4]]=255  # Drop, without wall.
    result=failures.memory_view(memory,25,[25,0],[25,0])
    front=result['local_zones']['front_2m']['channel_cells_ge_0p2']
    assert front['wall']==0 and front['ledge']==1 and front['drop']==1
    center=result['center_four_cells']
    assert len(center)==4
    cell=next(cell for cell in center if cell['row']==4 and cell['column']==4)
    assert cell['right_cm']==12.5 and cell['forward_cm']==12.5
    assert cell['channels']['ledge']==1 and cell['channels']['wall']==0

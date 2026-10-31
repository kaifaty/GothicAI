"""Stage8 gates inspect immutable data without importing/running a model."""
import copy
import importlib.util
import json
from pathlib import Path

import pytest

spec=importlib.util.spec_from_file_location("body_combat_report",Path(__file__).parents[1]/"body_combat_report.py")
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)


def config():
    return json.loads((Path(__file__).parents[2]/"evals/npc-body/body-stage8-v1.json").read_text())


def service_measurement():
    comparisons={provider:{service:{"baseline_p95_ms":100.,"during_p95_ms":100.,"degradation_fraction":0.,"passed":True,"samples":20}
                            for service in ("asr","tts","decider","gemma")} for provider in ("native","gru","mlp")}
    for provider in ("native","gru"):
        comparisons[provider]["gemma"].update(during_p95_ms=125.,degradation_fraction=.25,passed=False)
    return {"comparisons":comparisons}


def test_standalone_service_budget_failure_is_reported_without_combat_credit_or_model_causation():
    value=report.service_gate_summary(config(),service_measurement(),["legacy GRU budget failure"])
    assert value["measurement_ready"] and not value["service_gate_ready"] and value["status"]=="failed"
    assert len(value["budget_issues"])==2 and value["combat_budget_issues"]==[]
    assert not value["physical_quality_credit"] and not value["model_causation_claim"]


def test_absent_service_role_preserves_legacy_candidate_coupling():
    cfg=config();cfg.pop("service_gate_role");cfg.pop("service_gate_protocol_version")
    value=report.service_gate_summary(cfg,service_measurement(),["legacy GRU budget failure"])
    assert value["service_gate_role"]=="combat_gate" and value["combat_budget_issues"]==["legacy GRU budget failure"]


def test_standalone_service_report_cannot_claim_a_missing_or_invalid_healthy_window():
    value=report.service_gate_summary(config(),service_measurement(),measurement_issues=["healthy window invalid"])
    assert not value["measurement_ready"] and not value["service_gate_ready"] and value["status"]=="incomplete"
    missing=report.service_gate_summary(config())
    assert not missing["measurement_ready"] and not missing["service_gate_ready"]


def test_service_separation_preserves_exact_twenty_sample_receipts():
    value=service_measurement();value["comparisons"]["native"]["tts"]["samples"]=19
    with pytest.raises(ValueError,match="sample count"):
        report.service_gate_summary(config(),value)


def test_declared_matrix_separates_exposed_data_from_twenty_fresh_screen_groups():
    value=report.validate_config(config())
    assert value["screening_cases"] == 80
    assert value["screening_selection_allowed"] is False and value["release_gate_claim"] is False


@pytest.mark.parametrize("failure",["exposed_seed","screen_selection","gate_degrade","virtual_actors","test_selection"])
def test_readiness_cannot_be_obtained_by_weakening_freshness_or_existing_gates(failure):
    value=config()
    if failure=="exposed_seed":value["screening"]["seeds"][0]=43
    elif failure=="screen_selection":value["screening"]["selection_allowed"]=True
    elif failure=="gate_degrade":value["quality"]["minimum_success_delta_to_native"]=-.25
    elif failure=="virtual_actors":value["load"]["duplicate_virtual_actors_allowed"]=True
    else:value["training"]["selection_split"]="test"
    with pytest.raises(ValueError):report.validate_config(value)


def visibility():
    ray={"range_pass":True,"fov_pass":False,"body_fov_pass":True,"ray_hit":False,"hit_vob":False,"query_pass":False,
         "distance_cm":221.,"legacy_angle_deg":0.,"cone_cos":1.,"body_cone_cos":-.5,"hit_fraction":0.}
    return {"phase":"visibility_diagnostic","diagnostic_schema_version":1,"oracle_debug_only":True,
            "clock_domain":"linux-clock-monotonic","source_phase":"controller_release_visibility",
            "session_epoch":1,"world_epoch":2,"entity_id":3,"target_entity_id":4,"diagnostic_generation":1,"diagnostic_seq":1,
            "snapshot_wall_monotonic_ns":123,"wall_monotonic_ns":123,"target_has_head":True,
            "body_yaw_deg":0.,"pose_view_deg":-72.7,
            "center_ray":ray,"head_ray":copy.deepcopy(ray),"actual_can_see":False,"query_matches_diagnostic":True,
            "senses_see":True,"target_terminal":False,"controller_visible":False,"reason":"outside_pose_cone"}


def test_native_pose_cone_loss_does_not_get_reinterpreted_as_body_yaw_visibility():
    initial={"session_epoch":1,"world_epoch":2,"actors":[{"entity_id":3},{"entity_id":4}]}
    value=report.visibility_diagnostics([visibility()],initial)
    assert value["causes"] == {"outside_pose_cone":1}


@pytest.mark.parametrize("failure",["false_native","wrong_reason","duplicate_sequence"])
def test_visibility_diagnostics_require_actual_query_and_causal_sequence(failure):
    row=visibility();rows=[row]
    if failure=="false_native":row["actual_can_see"]=True
    elif failure=="wrong_reason":row["reason"]="static_occlusion"
    else:rows.append(copy.deepcopy(row))
    with pytest.raises(ValueError):
        report.visibility_diagnostics(rows,{"session_epoch":1,"world_epoch":2,"actors":[{"entity_id":3},{"entity_id":4}]})


def test_los_oracle_is_prohibited_inside_the_numeric_observation_tree():
    with pytest.raises(ValueError,match="oracle"):
        report.no_oracle_features({"self":{"hp":300},"target":{"pose_view_deg":30.}})


def retarget_evidence():
    initial={"session_epoch":1,"world_epoch":2,"actors":[{"entity_id":3},{"entity_id":4}]}
    row=visibility();row.update(seq=10,t_game=200,actor_id=30,target_id=1,target="PC_HERO",target_entity_id=5,owner="native",control_generation=0)
    mappings=[{"phase":"arena_actor_mapping","observation_entity_id":3,"actor_id":30},
              {"phase":"arena_actor_mapping","observation_entity_id":4,"actor_id":40}]
    perception={"phase":"native_perception","actor_id":30,"target_id":1,"target":"PC_HERO","perception_kind":"dispatch",
                "native_callback_dispatched":True,"result":True,"t_game":200,"seq":9}
    restore={"phase":"ownership","reason":"native_restored","entity_id":3,"session_epoch":1,"world_epoch":2,
             "control_generation":7,"t_game_ms":180,"wall_monotonic_ns":120,"event_seq":6}
    numeric={"entity_id":3,"session_epoch":1,"world_epoch":2,"t_game_ms":200,"control_generation":0,"observation_seq":20,
             "target":{"entity_id":5,"present":True,"current_valid":True}}
    return initial,[*mappings,perception,row],[restore],[numeric]


def test_actual_post_handoff_retarget_is_retained_without_paired_los_or_quality_credit():
    initial,rows,controls,observations=retarget_evidence()
    value=report.visibility_diagnostics(rows,initial,controls,observations)
    assert value["rows"] == 1 and value["paired_rows"] == 0 and value["post_handoff_retarget_rows"] == 1
    assert value["post_handoff_targets"][0]["native_perception_seq"] == 9
    assert value["post_handoff_targets"][0]["quality_credit"] is False


@pytest.mark.parametrize("failure",["no_restore","no_perception","no_numeric","owned","wrong_epoch","restore_after","numeric_other_frame"])
def test_external_target_cannot_be_admitted_by_ignoring_unknown_ids(failure):
    initial,rows,controls,observations=retarget_evidence()
    if failure=="no_restore":controls=[]
    elif failure=="no_perception":rows.pop(-2)
    elif failure=="no_numeric":observations=[]
    elif failure=="owned":rows[-1].update(owner="scripted",control_generation=7)
    elif failure=="wrong_epoch":rows[-1]["world_epoch"]=1
    elif failure=="restore_after":controls[0]["wall_monotonic_ns"]=124
    else:observations[0]["t_game_ms"]=180
    with pytest.raises(ValueError):report.visibility_diagnostics(rows,initial,controls,observations)


def housekeeping_evidence():
    before=visibility();before.update(source_phase="owned_consumed_fai_or_script_wait_before",seq=1,t_game=200,sim_step=5,
        actor_id=30,owner="scripted",control_generation=7,command_id=2,execution_id=8,forced_edge=True,body_state_base=3,
        native_fai_wait=True,native_script_wait=False,native_fai_wait_remaining_ms=0,native_script_wait_remaining_ms=0)
    after=copy.deepcopy(before);after.update(source_phase="owned_consumed_fai_or_script_wait_after",seq=3,diagnostic_seq=2,
        snapshot_wall_monotonic_ns=125,wall_monotonic_ns=125,body_yaw_deg=3.)
    return before,after


def test_owned_native_wait_housekeeping_has_paired_real_command_boundary_without_new_choice():
    # >= now is a real native wait gate even when remaining_ms is zero.
    rows=housekeeping_evidence();value=report.owned_housekeeping(rows,[])
    assert value["branch_counts"] == {"fai_or_script_wait":1}
    assert value["pairs"][0]["body_yaw_delta_deg"] == 3.


@pytest.mark.parametrize("failure",["missing_exit","different_command","wrong_wait_branch","native_choice","new_command"])
def test_housekeeping_cannot_hide_a_new_brain_choice_or_broken_command_boundary(failure):
    before,after=housekeeping_evidence();rows=[before,after];controls=[]
    if failure=="missing_exit":rows.pop()
    elif failure=="different_command":after["command_id"]=3
    elif failure=="wrong_wait_branch":before["native_fai_wait"]=False
    elif failure=="native_choice":rows.insert(1,{"phase":"native_choice","actor_id":30,"seq":2})
    else:controls=[{"phase":"accepted","entity_id":3,"wall_monotonic_ns":124}]
    with pytest.raises(ValueError):report.owned_housekeeping(rows,controls)


def test_legacy_eval_cannot_be_renamed_into_the_new_selection_group():
    value=config();value["new_data"]["eval_seeds"]=[29,227];value["training"]["selection_seed_groups"]=[29,227]
    with pytest.raises(ValueError,match="exposed reference"):report.validate_config(value)


def combat_rows(candidate_wins=17):
    rows=[]
    for task in report.TASKS:
        for policy in ("native","gru","mlp"):
            for index in range(20):
                wins=20 if policy=="native" else candidate_wins if policy=="gru" else 0
                rows.append({"name":f"{task}-{policy}-{index}","task":task,"policy":policy,"credited_success":index<wins,
                             "runtime":{"neural_control_fraction":1.}})
    return rows


def test_exact_fifteen_percentage_points_is_allowed_and_mlp_is_comparison_only():
    evidence,issues=report.combat_comparisons(combat_rows(),config())
    assert issues==[] and evidence["comparisons"]["gru"]["native_duel"]["passed"] is True
    assert evidence["comparisons"]["mlp"]["native_duel"]["passed"] is False


@pytest.mark.parametrize("failure",["sixteen_wins","control_fraction","fallback_win"])
def test_combat_admission_never_trades_native_success_or_healthy_control_for_comparison_scores(failure):
    rows=combat_rows(16 if failure=="sixteen_wins" else 17)
    if failure=="control_fraction":next(row for row in rows if row["policy"]=="gru")["runtime"]["neural_control_fraction"]=.94
    elif failure=="fallback_win":next(row for row in rows if row["policy"]=="gru" and row["credited_success"])["credited_success"]=False
    _,issues=report.combat_comparisons(rows,config());assert issues


def test_latency_gate_uses_declared_nearest_rank_population_and_rejects_nan():
    assert report.percentile(list(range(20)))==18
    with pytest.raises(ValueError):report.percentile([1.,float("nan")])


def masked_refresh():
    command={"entity_id":4,"session_epoch":1,"world_epoch":2,"control_generation":8,"provider":"stage8-gru",
             "goal_version":1,"target_entity_id":3,"command_id":7,"action_id":8}
    source=[False]*12;source[8]=True
    row={"periodic":True,"can_decide":True,"action_mask_valid":True,"action_mask":source,"observation_seq":197,"t_game_ms":18461}
    current={**command,"observation_seq":197,"t_game_ms":18494,"owner":"scripted","release_pending":False,
             "control_allowed":True,"action_mask":[False]*12}
    receipt={"accepted":False,"reason":"masked","execution_id":0,"body_control":{"actors":[current]},
             "arena":{"outcome":"running","infrastructure":False,"actors":[{"entity_id":3,"hp":3000},{"entity_id":4,"hp":3000}]}}
    entry={"command_id":None,"stale_refresh_pending":True,"recovery_attempt":1}
    return entry,row,command,receipt,[]


def test_live_mask_change_is_still_a_rejected_opportunity_without_execution():
    value=report.masked_refresh_evidence(*masked_refresh())
    assert value["accepted"] is False and value["execution_id"]==0 and value["rejected_opportunity_retained"] is True
    assert value["current_observation_seq"]==value["source_observation_seq"] and value["current_game_ms"]>value["source_game_ms"]


@pytest.mark.parametrize("failure",["source_forbidden","ownership_lost","started","budget_exceeded"])
def test_mask_refusal_cannot_receive_healthy_refresh_by_waiving_actual_guards(failure):
    entry,row,command,receipt,controls=masked_refresh()
    if failure=="source_forbidden":row["action_mask"][8]=False
    elif failure=="ownership_lost":receipt["body_control"]["actors"][0]["release_pending"]=True
    elif failure=="budget_exceeded":entry["recovery_attempt"]=3
    else:controls.append({"phase":"started",**{key:command[key] for key in report.bc.canonical.IDENTITY},"command_id":7})
    with pytest.raises(ValueError):report.masked_refresh_evidence(entry,row,command,receipt,controls)


def native_guard_wait():
    entry,row,command,receipt,controls=masked_refresh();command["action_id"]=1
    row["action_mask"][1]=True;row["target"]={"visible":True}
    actor=receipt["body_control"]["actors"][0]
    actor.update(action_id=1,control_allowed=False,watchdog_timeout_ms=5000)
    receipt.update(id=17,reason="target_invalid",wall_monotonic_ns=100)
    wait={**{key:command[key] for key in (*report.bc.canonical.IDENTITY,"provider","goal_version","target_entity_id")},
          "status":"pending","refused_observation_seq":197,"native_current_observation_seq":197,
          "begin_game_ms":18494,"begin_wall_monotonic_ns":110,"wait_bound_ms":5000,"native_current_action_mask":[False]*12}
    entry.update(execution_id=0,native_guard_wait_pending=True,native_guard_wait=wait)
    return entry,row,command,receipt,controls


def test_actual_target_guard_classification_is_not_healthy_refresh_or_execution_credit():
    proof=report.target_guard_wait_evidence(*native_guard_wait())
    assert proof["accepted"] is False and proof["execution_id"]==0 and proof["healthy_refresh_credit"] is False
    assert proof["rejected_opportunity_retained"] is True


@pytest.mark.parametrize("mutation",["owned_changed","source_forbidden","new_watchdog","started"])
def test_native_guard_wait_cannot_hide_wrong_source_ownership_or_physical_execution(mutation):
    entry,row,command,receipt,controls=native_guard_wait()
    if mutation=="owned_changed":receipt["body_control"]["actors"][0]["release_pending"]=True
    elif mutation=="source_forbidden":row["action_mask"][1]=False
    elif mutation=="new_watchdog":entry["native_guard_wait"]["wait_bound_ms"]=5001
    else:controls.append({"phase":"started",**{key:command[key] for key in report.bc.canonical.IDENTITY},"command_id":command["command_id"]})
    with pytest.raises(ValueError):report.target_guard_wait_evidence(entry,row,command,receipt,controls)


def recovered_guard():
    entry,row,command,receipt,controls=native_guard_wait();guard=report.target_guard_wait_evidence(entry,row,command,receipt,controls);wait=guard["wait"]
    fresh={**{key:command[key] for key in report.bc.canonical.IDENTITY},"periodic":True,"observation_seq":198,"t_game_ms":18527,
           "control_allowed":True,"action_mask_valid":True,"target":{"visible":True,"present":True,"current_valid":True,"entity_id":3},"goal":{"version":1}}
    actor={**receipt["body_control"]["actors"][0],"control_allowed":True}
    actual={"id":18,"wall_monotonic_ns":190,"body_control":{"actors":[actor]},"observations":[fresh]}
    records=[{**wait,"phase":"begin","native_response_id":17},{**wait,"phase":"recovered","game_ms":18527,
             "wall_monotonic_ns":200,"observation_seq":198,"native_response_id":18}]
    return {"native_guard_waits":{"records":records,"pending":[]}},[({},actual,0,0)],controls,[guard]


def test_native_guard_recovery_requires_fresh_periodic_and_preserves_memory_only_history():
    record,pairs,controls,guards=recovered_guard()
    memory={"kind":"model_input","request":{"command_enabled":False,"memory_only":True},
            "response":{"model_processed":True,"action_id":None},"timing":{"process_begin_wall_monotonic_ns":150}}
    proof=report.target_guard_history(record,[memory],pairs,controls,guards)
    assert proof["waits"][0]["healthy_refresh_credit"] is False and proof["periodic_history_skipped"] is False
    memory["request"]["command_enabled"]=True
    with pytest.raises(ValueError,match="skipped history or proposed a command"):
        report.target_guard_history(record,[memory],pairs,controls,guards)
    pairs[0][1]["observations"][0]["periodic"]=False
    with pytest.raises(ValueError,match="fresh same-lease visible periodic"):
        report.target_guard_history(record,[],pairs,controls,guards)


def softlock_evidence():
    mapping={"phase":"arena_actor_mapping","observation_entity_id":3,"actor_id":30}
    first={"phase":"phase_snapshot","actor_id":30,"t_game":20,"layers":[{"name":"S_1HRUN","start":0,"clock":20,"combination":0,"speed":1.,"duration_ms":2100}],
           "fai_wait_until":30,"ani_wait_until":0,"wait_until":0,"output_wait_until":0,"x":1.,"y":2.,"z":3.,"yaw":0.,"hp":300}
    late=copy.deepcopy(first);late.update(t_game=100);late["layers"][0]["clock"]=100
    control={"phase":"started","entity_id":3,"command_id":2,"execution_id":8,"action_id":10,"t_game_ms":10}
    return [mapping,first,late],[control],{"game_ms":200}


def test_native_deadline_plus_two_ticks_detects_an_unchanged_finite_wait_lock():
    evidence=report.finite_softlocks(*softlock_evidence())
    assert len(evidence["violations"])==1 and evidence["violations"][0]["native_deadline_game_ms"]==30


@pytest.mark.parametrize("boundary",["completed","cancelled","new_hit","movement"])
def test_terminal_progress_and_new_native_layers_are_not_laundered_into_finite_softlocks(boundary):
    mechanics,controls,final=softlock_evidence()
    if boundary in ("completed","cancelled"):controls.append({**controls[0],"phase":boundary,"t_game_ms":50})
    elif boundary=="new_hit":mechanics[-1]["layers"][0].update(name="T_1H_HIT",start=70)
    else:controls[0]["action_id"]=1
    assert report.finite_softlocks(mechanics,controls,final)["violations"]==[]


def test_final_visibility_integrity_binds_actual_native_response_and_global_record_count():
    final={"game_ms":200};status={"arena":final,"infrastructure_failure":False,"visibility_diagnostics":
        {"enabled":True,"capture_failed":False,"dropped_total":0,"records_total":1,"clock_domain":"linux-clock-monotonic","diagnostic_generation":1}}
    record={"final":final,"perception_evidence":{"final_native_status":status}}
    pairs=[({"cmd":"body_control_status"},status,0.,1.)]
    assert report.capture_integrity(record,[visibility()],pairs)["dropped"]==0
    status["visibility_diagnostics"]["capture_failed"]=True
    with pytest.raises(ValueError,match="dropped/failed"):report.capture_integrity(record,[visibility()],pairs)


def service_records():
    fixtures={"fixture_binding_sha256":"fixture","request_sha256":{name:name for name in ("asr","tts","decider","gemma")},
              "asr":{"request":{}},"tts":{"request":{"id":"r"}},
              "decider":{"request":{"id":"r","questions":{"react":{"criteria":{"greet":"nod"}}}}},"gemma":{"request":{"model":"m"}}}
    rows=[]
    for service in fixtures["request_sha256"]:
        row={"service":service,"phase":"before","status":"completed","warmup":False,"iteration":0,"request_sha256":service,"fixture_binding_sha256":"fixture",
             "request_begin_wall_monotonic_ns":100,"request_submitted_wall_monotonic_ns":200,"first_result_wall_monotonic_ns":300,
             "completed_wall_monotonic_ns":500,"metric_origin_wall_monotonic_ns":200,"latency_ms":.0001}
        if service=="asr":
            row.update(speech_submitted_wall_monotonic_ns=180,metric_origin_wall_monotonic_ns=180,latency_ms=.00012,
                       response_events=[{"wall_monotonic_ns":299,"value":{"type":"transcript.text.done","text":"готов"}}],result={"transcript":"готов"})
        elif service=="tts":
            done={"type":"done","id":"r","sample_rate":48000}
            row.update(response_events=[{"wall_monotonic_ns":300,"type":"pcm","bytes":4,"sha256":"a"*64},{"wall_monotonic_ns":400,"value":done}],
                       result={"pcm_bytes":4,"sample_rate":48000,"done":done})
        elif service=="decider":
            value={"type":"answers","id":"r","answers":{"react":{"choice":"greet"}}}
            row.update(response_events=[{"wall_monotonic_ns":300,"value":value}],result=value)
        else:
            row.update(response_events=[{"wall_monotonic_ns":300,"value":{"model":"m","choices":[{"delta":{"content":"готов"},"finish_reason":None}]}},
                                        {"wall_monotonic_ns":400,"value":{"model":"m","choices":[{"delta":{},"finish_reason":"stop"}]}},
                                        {"wall_monotonic_ns":450,"type":"sse_done"}],result={"text":"готов","model":"m","done_received":True})
        rows.append(row)
    return fixtures,rows


def test_protected_service_metrics_derive_from_real_semantic_results_and_monotonic_receipts():
    fixtures,rows=service_records();metrics=report.service_receipts(rows,fixtures,"before",1,0)
    assert metrics["asr"]["p95_ms"]==.00012 and metrics["tts"]["samples"]==1


@pytest.mark.parametrize("failure",["fake_latency","no_pcm","wrong_decision","missing_sse_done","changed_fixture"])
def test_service_summary_cannot_replace_missing_or_changed_actual_results(failure):
    fixtures,rows=service_records()
    if failure=="fake_latency":rows[0]["latency_ms"]=0.
    elif failure=="no_pcm":rows[1]["response_events"].pop(0)
    elif failure=="wrong_decision":rows[2]["result"]["answers"]["react"]["choice"]="attack"
    elif failure=="missing_sse_done":rows[3]["response_events"].pop()
    else:rows[0]["fixture_binding_sha256"]="other"
    with pytest.raises(ValueError):report.service_receipts(rows,fixtures,"before",1,0)


def combat_collection_config():
    return json.loads((Path(__file__).parents[2]/"evals/npc-body/body-combat-demonstrations-v1.json").read_text())


def test_production_matrix_keeps_calibration_out_and_varies_real_train_clearance():
    value=combat_collection_config();cases=report.demonstration_cases(value)
    assert len(cases)==84 and len({case["name"] for case in cases})==84
    assert sum(case["task"]=="native_duel" for case in cases)==36
    assert all(not case["name"].startswith("calibration-") for case in cases)
    assert not {case["seed"] for case in cases}&set(value["reserved_test_seeds"])
    for seed,requested in value["microcurriculum"]["wall_clearance_by_seed_cm"].items():
        selected=[case for case in cases if case["seed"]==int(seed) and "wall_clearance_cm" in case]
        assert len(selected)==8 and all(case["task"]=="reset_stress" and case["wall_clearance_cm"]==requested for case in selected)
    assert all("wall_clearance_cm" not in case for case in cases if case["task"]=="native_duel")


def clearance():
    geometry={"actual_wall_clearance_cm":72.,"requested_wall_clearance_cm":70,"wall_clearance_error_cm":2.,
              "wall_clearance_tolerance_cm":20,"obstacle_distance_cm":72.,"safe_height_delta_cm":0.,"placement_height_delta_cm":0.}
    for key in ("wall_ray_hit","capsule_move_blocked","capsule_block_is_land","initial_capsules_free","safe_outward_move",
                "grounded_placements","target_visible","melee_line_clear"):geometry[key]=True
    geometry.update(placement_basis="wall_normal_xz",search_ray_direction_world=[-1.,0.,0.],placement_wall_anchor_cm=[2.,85.,0.],
                    placement_wall_normal_xz_world=[1.,0.,0.],placement_wall_tangent_world=[0.,0.,-1.],primary_collision_center_cm=[72.,85.,0.],
                    actual_wall_normal_xz_world=[1.,0.,0.],wall_normal=[1.,0.,0.],wall_point_cm=[0.,85.,0.],wall_direction_world=[-1.,0.,0.],
                    requested_wall_normal_clearance_cm=70,actual_wall_normal_clearance_cm=72.,wall_normal_clearance_error_cm=2.,wall_normal_clearance_tolerance_cm=20)
    initial={"initial_state":{"setup":{"wall_clearance_cm":70,"geometry_proof":geometry}}}
    evidence={"requested_cm":70,"actual_cm":72.,"error_cm":2.,"tolerance_cm":20,"geometry_proof":geometry,
              "training_admission":"requires_actual_command_outcome"}
    evidence.update(normal_requested_cm=70,normal_actual_cm=72.,normal_error_cm=2.,normal_tolerance_cm=20,placement_basis="wall_normal_xz")
    return initial,{"geometry":"near_wall","wall_clearance_cm":70},evidence


def test_requested_clearance_does_not_imply_physical_action_success():
    initial,case,evidence=clearance()
    report.clearance_admission(initial,case,evidence)
    assert evidence["training_admission"]=="requires_actual_command_outcome"


@pytest.mark.parametrize("failure",["height","collision","requested","fake_admission","physical_outside_bound"])
def test_clearance_admission_requires_bounded_real_geometry_not_setup_metadata(failure):
    initial,case,evidence=clearance();geometry=initial["initial_state"]["setup"]["geometry_proof"]
    if failure=="height":geometry["placement_height_delta_cm"]=51.
    elif failure=="collision":geometry["capsule_move_blocked"]=False
    elif failure=="requested":case["wall_clearance_cm"]=90
    elif failure=="fake_admission":evidence["training_admission"]="guaranteed_success"
    else:geometry["actual_wall_clearance_cm"]=181.
    with pytest.raises(ValueError):report.clearance_admission(initial,case,evidence)


@pytest.mark.parametrize("failure",["duplicate","outside_bound","too_few","boolean"])
def test_preregistered_micro_clearances_cannot_be_invented_after_measurement(failure):
    value={"new_data":{"micro_wall_clearances_cm":[120,130,140,150,160,170]}}
    if failure=="duplicate":value["new_data"]["micro_wall_clearances_cm"][0]=130
    elif failure=="outside_bound":value["new_data"]["micro_wall_clearances_cm"][0]=181
    elif failure=="too_few":value["new_data"]["micro_wall_clearances_cm"].pop()
    else:value["new_data"]["micro_wall_clearances_cm"][0]=True
    with pytest.raises(ValueError):report.declared_clearances(value)


def test_full_bouts_do_not_silently_receive_micro_clearance_geometry():
    with pytest.raises(ValueError):report.clearance_admission({"initial_state":{"setup":{"wall_clearance_cm":120}}},{"geometry":"near_wall"},None)
    report.clearance_admission({"initial_state":{"setup":{"geometry_proof":{"obstacle_distance_cm":122.23}}}},{"geometry":"near_wall"},None)


@pytest.mark.parametrize("failure",["fake_projection","fake_normal","nonunit","ray_bound","tangent","missing_normal"])
def test_optional_normal_clearance_is_independent_of_ray_metadata(failure):
    initial,case,evidence=clearance();geometry=initial["initial_state"]["setup"]["geometry_proof"]
    if failure=="fake_projection":geometry["primary_collision_center_cm"][0]=75.
    elif failure=="fake_normal":geometry["wall_normal"]=[0.,0.,1.]
    elif failure=="nonunit":geometry["actual_wall_normal_xz_world"]=[2.,0.,0.]
    elif failure=="ray_bound":geometry["actual_wall_clearance_cm"]=181.
    elif failure=="tangent":geometry["placement_wall_tangent_world"]=[1.,0.,0.]
    else:geometry.pop("actual_wall_normal_xz_world")
    with pytest.raises(ValueError):report.normal_clearance_geometry(geometry,70)



def tangent_case(side=0,sign=1):
    tangent=[0.,0.,-1.];delta=350*sign*(1 if side==0 else -1)
    initial={"side":side,"primary_entity_id":1,"opponent_entity_id":2,"actors":[{"entity_id":1,"pos":[0.,0.,0.]},{"entity_id":2,"pos":[0.,0.,-delta]}],
             "initial_state":{"setup":{"wall_tangent_sign":sign,"geometry_proof":{"requested_wall_tangent_sign":sign,"placement_tangent_sign":sign,
                             "placement_wall_tangent_world":tangent,"placement_wall_normal_xz_world":[1.,0.,0.]}}}}
    for actor,role,physical in zip(initial["actors"],("primary","opponent"),(side,1-side)):actor.update(role=role,physical_side=physical)
    initial["initial_state"]["actors"]=initial["actors"]
    case={"side":side,"geometry":"near_wall","wall_clearance_cm":120,"wall_tangent_sign":sign,"distance_cm":350}
    return initial,case


@pytest.mark.parametrize("side,sign",[(0,1),(0,-1),(1,1),(1,-1)])
def test_optional_tangent_sign_is_proved_by_actual_actor_positions_with_side_swap(side,sign):
    initial,case=tangent_case(side,sign);proof=report.tangent_placement_proof(initial,case)
    assert proof["pair_tangent_projection_cm"]==350*sign*(1 if side==0 else -1)


@pytest.mark.parametrize("failure",["wrong_actor_position","wrong_actual_sign","wrong_side","bool_sign","missing_clearance","unrequested_proof"])
def test_tangent_sign_metadata_cannot_replace_actual_mirrored_pair_geometry(failure):
    initial,case=tangent_case()
    if failure=="wrong_actor_position":initial["actors"][1]["pos"][2]*=-1
    elif failure=="wrong_actual_sign":initial["initial_state"]["setup"]["geometry_proof"]["placement_tangent_sign"]=-1
    elif failure=="wrong_side":case["side"]=1
    elif failure=="bool_sign":case["wall_tangent_sign"]=True
    elif failure=="missing_clearance":case.pop("wall_clearance_cm")
    else:case.pop("wall_tangent_sign")
    with pytest.raises(ValueError):report.tangent_placement_proof(initial,case)



def decider_receipt():
    identity={"MainPID":"42","ActiveState":"active","SubState":"running","InvocationID":"fixed","ExecMainStartTimestampMonotonic":"100"}
    fixtures={"decider":{"expected_greeting":{"type":"hello","service":"gothic-decider","model":"gothic-g9e"},
                         "deployed_binding":{"unit":"gothic-decider","unit_identity":identity,"pid":42}}}
    result={"services_before":{"decider":identity},"connection_handshakes":{"decider":{**fixtures["decider"]["expected_greeting"],"protocol_version":1}}}
    return fixtures,result


def test_actual_decider_greeting_binds_exact_tuned_model_and_same_process_not_engine_substring():
    fixtures,result=decider_receipt();proof=report.decider_handshake(fixtures,result)
    assert proof["actual_greeting"]["model"]=="gothic-g9e" and proof["pid"]==42


@pytest.mark.parametrize("failure",["old_substring","wrong_service","missing_hello","spoofed_fixture","changed_pid"])
def test_protected_decider_handshake_cannot_credit_wrong_model_or_process(failure):
    fixtures,result=decider_receipt()
    if failure=="old_substring":result["connection_handshakes"]["decider"]["model"]="other-strands-model"
    elif failure=="wrong_service":result["connection_handshakes"]["decider"]["service"]="another-decider"
    elif failure=="missing_hello":result.pop("connection_handshakes")
    elif failure=="spoofed_fixture":fixtures["decider"]["expected_greeting"]["model"]="other-model"
    else:fixtures["decider"]["deployed_binding"]["pid"]=43
    with pytest.raises(ValueError):report.decider_handshake(fixtures,result)

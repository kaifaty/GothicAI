"""Preregistration matrix/threshold bindings; all inputs are temporary files."""
import copy
import importlib.util
import json
from pathlib import Path
import sys

import pytest

SCRIPTS=Path(__file__).parents[1];sys.path.insert(0,str(SCRIPTS))
spec=importlib.util.spec_from_file_location("final_declaration",SCRIPTS/"body_final_evaluation.py")
final=importlib.util.module_from_spec(spec);spec.loader.exec_module(final)


def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value))


def protocol(tmp_path):
    root,reference,child=(tmp_path/name for name in ("gate","v6","child"))
    original=json.loads((SCRIPTS.parent/"evals/npc-body/body-stage8-v1.json").read_text())
    config=json.loads((SCRIPTS.parent/"evals/npc-body/body-stage8-evaluation-v2.json").read_text())
    original["runtime"]["native_serial_workers"]=False;config["runtime"]["native_serial_workers"]=False
    candidate={"run":str(child),"checkpoint_sha256":{"gru":"gru","mlp":"mlp"},"normalizer_sha256":"norm"}
    config["reference"]={"run_name":child.name,"policy_code_run_name":reference.name,
                         "gru_checkpoint_sha256":"gru","mlp_checkpoint_sha256":"mlp","normalizer_sha256":"norm"}
    original_file=reference/"code/evals/npc-body/body-stage8-v1.json";write(original_file,original)
    write(root/"code/evals/npc-body/body-stage8-v1.json",original)
    helper=root/"code/scripts/body_block_reaction.py";helper.parent.mkdir(parents=True);helper.write_text("frozen preparation")
    helper_config=root/"code/evals/npc-body/body-block-reaction-v1.json";write(helper_config,{"preparation":config["preparation"]})
    config["bindings"]["original_stage8_config_sha256"]=final.sha(original_file)
    config["bindings"]["original_stage8_config_run_name"]=reference.name
    config["bindings"]["preparation_helper_sha256"]=final.sha(helper)
    config["bindings"]["preparation_config_sha256"]=final.sha(helper_config)
    frozen={}
    for name in config["bindings"]["frozen_policy_modules"]:
        relative="code/body-service/src/"+name.replace(".","/")+".py" if name.startswith("gothic_body.") else "code/scripts/"+name+".py"
        path=reference/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(name)
        frozen[relative]=final.sha(path);config["bindings"]["frozen_policy_modules"][name]=final.sha(path)
    write(reference/"environment.json",{"frozen_files":frozen})
    write(child/"dataset/dataset.json",{"groups":{"[\"arena-native-melee-v1\",181]":"train","[\"arena-native-melee-v1\",223]":"eval"}})
    write(root/final.CONFIG,config)
    return root,reference,child,candidate,config


def test_final_protocol_keeps_original_80_case_order_and_quality(tmp_path):
    root,reference,_,candidate,_=protocol(tmp_path)
    assert len(final.declared_protocol(root,reference,candidate)["screening"]["cases"])==80


def test_reordered_fresh_cases_are_not_admitted(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["screening"]["cases"][0],config["screening"]["cases"][1]=config["screening"]["cases"][1],config["screening"]["cases"][0]
    write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="matrix/order changed"):final.declared_protocol(root,reference,candidate)


def test_quality_threshold_cannot_be_relaxed(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["quality"]["minimum_success_delta_to_native"]=-.3;write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="thresholds changed"):final.declared_protocol(root,reference,candidate)


def test_collector_gc_profile_cannot_use_a_truthy_nonboolean(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["runtime"]["collector_static_gc_freeze"]="true";write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="explicit boolean"):
        final.declared_protocol(root,reference,candidate)


def test_native_deadline_polling_requires_boolean_and_matching_ordinary_profile(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["runtime"]["native_deadline_polling"]="true";write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="explicit boolean"):
        final.declared_protocol(root,reference,candidate)
    config["runtime"]["native_deadline_polling"]=True;write(root/final.CONFIG,config)
    current=final.read(root/"code/evals/npc-body/body-stage8-v1.json")
    current["runtime"]["native_deadline_polling"]=False;write(root/"code/evals/npc-body/body-stage8-v1.json",current)
    with pytest.raises(ValueError,match="ordinary/load native deadline polling differs"):
        final.declared_protocol(root,reference,candidate)


def test_native_serial_worker_profile_requires_boolean_and_same_evaluator_config(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["runtime"]["native_serial_workers"]="true";write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="explicit boolean"):
        final.declared_protocol(root,reference,candidate)
    config["runtime"]["native_serial_workers"]=True;write(root/final.CONFIG,config)
    current=final.read(root/"code/evals/npc-body/body-stage8-v1.json")
    current["runtime"]["native_serial_workers"]=False;write(root/"code/evals/npc-body/body-stage8-v1.json",current)
    with pytest.raises(ValueError,match="ordinary/load native worker profile differs"):
        final.declared_protocol(root,reference,candidate)


def test_native_worker_launch_flag_is_explicit_and_default_absent():
    from types import SimpleNamespace
    from body_observe_suite import native_worker_arguments
    assert native_worker_arguments(SimpleNamespace())==[]
    assert native_worker_arguments(SimpleNamespace(native_serial_workers=False))==[]
    assert native_worker_arguments(SimpleNamespace(native_serial_workers=True))==["--serial-workers"]
    with pytest.raises(ValueError,match="explicit boolean"):
        native_worker_arguments(SimpleNamespace(native_serial_workers="true"))


def test_serial_workers_cannot_silently_use_immutable_legacy_launcher(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["runtime"]["native_serial_workers"]=True;write(root/final.CONFIG,config)
    current=final.read(root/"code/evals/npc-body/body-stage8-v1.json")
    current["runtime"]["native_serial_workers"]=True;write(root/"code/evals/npc-body/body-stage8-v1.json",current)
    with pytest.raises(ValueError,match="current explicitly frozen native launcher"):
        final.declared_protocol(root,reference,candidate)


def test_bootstrap_gc_reset_requires_an_explicit_static_scope(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["runtime"].update(collector_bootstrap_gc_reset=True,collector_static_gc_freeze=False);write(root/final.CONFIG,config)
    with pytest.raises(ValueError,match="explicit boolean/static scope"):
        final.declared_protocol(root,reference,candidate)


def test_fresh_overlap_with_any_train_eval_test_or_ood_group_fails(tmp_path):
    root,reference,child,candidate,_=protocol(tmp_path)
    write(child/"dataset/dataset.json",{"groups":{"[\"arena-native-melee-v1\",251]":"ood"}})
    with pytest.raises(ValueError,match="overlaps any"):final.declared_protocol(root,reference,candidate)


def test_selected_weights_must_match_frozen_evaluator_config(tmp_path):
    root,reference,_,candidate,_=protocol(tmp_path);candidate["checkpoint_sha256"]["gru"]="new unregistered weights"
    with pytest.raises(ValueError,match="fixed selected child"):final.declared_protocol(root,reference,candidate)


def test_current_runtime_imports_require_exact_current_gate_file_hashes(tmp_path):
    root,reference,_,candidate,config=protocol(tmp_path)
    config["reference"]["policy_code_run_name"]=root.name
    frozen={}
    for name,digest in config["bindings"]["frozen_policy_modules"].items():
        relative="code/body-service/src/"+name.replace(".","/")+".py" if name.startswith("gothic_body.") else "code/scripts/"+name+".py"
        file=root/relative;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes((reference/relative).read_bytes())
        frozen[relative]=digest
    write(root/"environment.json",{"frozen_files":frozen});write(root/final.CONFIG,config)
    assert final.declared_protocol(root,reference,candidate)["reference"]["policy_code_run_name"]==root.name
    (root/"code/scripts/body_policy_diagnostics.py").write_text("unbound delivery change")
    with pytest.raises(ValueError,match="actual policy/runtime/packer"):
        final.declared_protocol(root,reference,candidate)


def test_screening_execution_resource_alias_is_explicit():
    assert final.EXECUTION_PHASES["screening"]=="screening-reaction"
    assert set(final.EXECUTION_PHASES)=={"shadow","realtime","realtime-reaction","screening","load"}


def selected_child(tmp_path,monkeypatch):
    root=tmp_path/"child"
    write(root/"code/evals/npc-body/body-defence-bc-v1.json",{"train":{"seeds":[11,29,43]}})
    write(root/"dataset/dataset.json",{"episode_count":207,"episodes":[
        {"episode_id":"fresh223","group":'["arena-native-melee-v1",223]',"split":"eval"},
        {"episode_id":"fresh227","group":'["arena-native-melee-v1",227]',"split":"eval"}]})
    write(root/"dataset/normalization.json",{"mean":[0],"std":[1]})
    profile={"kind":"body_defence_child_selection_view","test_and_ood_used_for_selection":False,
        "dataset_sha256":final.sha(root/"dataset/dataset.json"),"normalization_sha256":final.sha(root/"dataset/normalization.json"),
        "selection_evaluation_episode_ids":["fresh223","fresh227"]}
    write(root/"dataset/selection-profile.json",profile)
    selection={**profile,"selection_split":"eval","selection_view":"full_episode","evaluation_mode":"continuous",
        "selection_profile_sha256":final.sha(root/"dataset/selection-profile.json"),
        "config_sha256":final.sha(root/"code/evals/npc-body/body-defence-bc-v1.json"),
        "weights_fixed_at_utc":"2026-10-14T11:00:00+00:00","training_device":"cuda","models":{}}
    for architecture in ("gru","mlp"):
        folder=root/"models"/architecture;folder.mkdir(parents=True)
        (folder/"best.pt").write_bytes(architecture.encode())
        choice={"selected":{"checkpoint_sha256":final.sha(folder/"best.pt")}}
        selection["models"][architecture]=choice;write(folder/"selection.json",choice)
    write(root/"selection.json",selection)
    monkeypatch.setattr(final.repair,"frozen_environment",lambda path:{"binary_sha256":"same-real-engine"})
    monkeypatch.setattr(final.repair,"inventory",lambda path,names:{name:"bound" for name in names})
    return root


def test_selected_child_continuous_cuda_schema_is_bound_without_model_load(tmp_path,monkeypatch):
    root=selected_child(tmp_path,monkeypatch);value=final.fixed_child(root)
    assert value["training_device"]=="cuda" and value["selection_allowed_during_evaluation"] is False
    assert value["selection_evaluation_episode_ids"]==["fresh223","fresh227"]


def test_selected_child_legacy_eval_reopening_is_rejected(tmp_path,monkeypatch):
    root=selected_child(tmp_path,monkeypatch);index=final.read(root/"dataset/dataset.json")
    index["episodes"][0]["group"]='["arena-native-melee-v1",29]';write(root/"dataset/dataset.json",index)
    with pytest.raises(ValueError,match="reopened excluded"):
        final.fixed_child(root)


def compact_failure(tmp_path,monkeypatch):
    prior=tmp_path/"gate-v2";candidate={"checkpoint_sha256":{},"normalizer_sha256":""}
    for name in ("environment.json","source.tar.gz","reference/gothic-body-runner","dataset/normalization.json"):
        path=prior/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(name)
    candidate["normalizer_sha256"]=final.sha(prior/"dataset/normalization.json")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";file.parent.mkdir(parents=True);file.write_bytes(architecture.encode())
        candidate["checkpoint_sha256"][architecture]=final.sha(file);write(file.parent/"best.pt.json",{})
    write(prior/"final-evaluation.json",{"kind":final.KIND,"candidate":candidate})
    for phase in ("shadow","realtime","realtime-reaction"):
        write(prior/phase/"suite.json",{"status":"collected"})
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            (prior/(stem+"-"+phase+suffix)).write_text("preserved old phase")
    write(prior/"realtime-reaction/suite.json",{"status":"infrastructure_failure","cases":[],"error":"'rng_state'"})
    monkeypatch.setattr(final.repair,"frozen_environment",lambda root:{})
    return prior,candidate


def test_additional_zero_case_compact_failure_is_bound_without_quality_credit(tmp_path,monkeypatch):
    prior,candidate=compact_failure(tmp_path,monkeypatch);value=final.preparation_failure_snapshot(prior,candidate)
    assert value["completed_reaction_cases"]==0 and value["model_quality_evaluated"] is False
    assert "resources-realtime-reaction.txt" in value["files"]
    write(prior/"realtime-reaction/suite.json",{"status":"infrastructure_failure","cases":[],"error":"different physics failure"})
    with pytest.raises(ValueError,match="exact zero-case compact"):
        final.preparation_failure_snapshot(prior,candidate)


def test_compact_preparation_failure_cannot_hide_a_model_decision(tmp_path,monkeypatch):
    prior,candidate=compact_failure(tmp_path,monkeypatch)
    path=prior/"realtime-reaction/partial/policy-inputs.jsonl";path.parent.mkdir()
    path.write_text(json.dumps({"kind":"model_input"})+"\n")
    with pytest.raises(ValueError,match="already ran a model decision"):
        final.preparation_failure_snapshot(prior,candidate)


def test_prior_known_run_cannot_hide_any_load_or_fresh_exposure(tmp_path,monkeypatch):
    prior=tmp_path/"gate3";prior.mkdir();(prior/"load").mkdir()
    monkeypatch.setattr(final.repair,"frozen_environment",lambda root:{})
    with pytest.raises(ValueError,match="exposed load or fresh"):
        final.prior_known_snapshot(prior,{})


def delivery_failure(tmp_path,monkeypatch):
    prior,candidate=compact_failure(tmp_path,monkeypatch);name="realtime-distance-223-1-gru"
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6)):
        write(prior/phase/"suite.json",{"status":"collected","cases":[{}]*count})
    request={"entity_id":1,"session_epoch":7,"world_epoch":8,"control_generation":9,"observation_seq":5}
    refusal={"request":request,"response":{"accepted":False,"reason":"stale_observation","clock_domain":"linux-clock-monotonic"},
             "native_current_observation_seq":7,"sent_wall_monotonic_ns":100,"received_wall_monotonic_ns":300}
    write(prior/"realtime"/name/"episode.json",{"policy":"gru","neural_control_fraction":2/3,"fallback_events":[],
          "command_responses":[refusal,{"response":{"accepted":True}},{"response":{"accepted":True}}]})
    row={**request,"observation_seq":7,"snapshot_wall_monotonic_ns":200,"clock_domain":"linux-clock-monotonic"}
    (prior/"realtime"/name/"observations.jsonl").write_text(json.dumps(row)+"\n")
    write(prior/"validation/known-audit-v1/report.json",{"kind":"body_stage8_known_saved_runtime_report","passed":False,
          "issues":[name+": healthy control below .95",name+": healthy neural control below95percent"],"load_read":False,
          "fresh_screening_read":False,"stage8_quality_credit":False,"candidate_checkpoint_sha256":candidate["checkpoint_sha256"],
          "normalization_sha256":candidate["normalizer_sha256"],"environment_sha256":final.sha(prior/"environment.json"),
          "declaration_sha256":final.sha(prior/"final-evaluation.json"),
          "raw_sha256":{"realtime/"+name+"/episode.json":final.sha(prior/"realtime"/name/"episode.json")}})
    return prior,candidate


def test_closed_known_delivery_failure_retains_failed_control_and_future_mint_proof(tmp_path,monkeypatch):
    prior,candidate=delivery_failure(tmp_path,monkeypatch)
    value=final.additional_failure_snapshot(prior,candidate)
    assert value["kind"]=="closed_known_delivery_failure" and value["quality_credit"] is False
    assert value["accepted_model_commands"]==2 and value["command_opportunities"]==3
    assert value["command_sent_wall_monotonic_ns"]<value["future_observation_wall_monotonic_ns"]


def test_delivery_retry_refuses_other_health_failure_or_preexisting_observation(tmp_path,monkeypatch):
    prior,candidate=delivery_failure(tmp_path,monkeypatch);path=prior/"validation/known-audit-v1/report.json"
    report=final.read(path);report["issues"].append("another actual model failure");write(path,report)
    with pytest.raises(ValueError,match="exact closed known delivery"):
        final.additional_failure_snapshot(prior,candidate)
    report["issues"].pop();write(path,report)
    path=prior/"realtime/realtime-distance-223-1-gru/observations.jsonl"
    row=json.loads(path.read_text());row["snapshot_wall_monotonic_ns"]=99;path.write_text(json.dumps(row)+"\n")
    with pytest.raises(ValueError,match="future observation clock"):
        final.additional_failure_snapshot(prior,candidate)


def test_delivery_failure_cannot_hide_load_or_fresh_exposure(tmp_path,monkeypatch):
    prior,candidate=delivery_failure(tmp_path,monkeypatch);(prior/"screening").mkdir()
    with pytest.raises(ValueError,match="exposed load or fresh"):
        final.additional_failure_snapshot(prior,candidate)


def partial_age_failure(tmp_path):
    folder=tmp_path/"partial";folder.mkdir();identity={"entity_id":1,"session_epoch":7,"world_epoch":8,"control_generation":9}
    row={**identity,"observation_seq":337,"t_game_ms":23561};old={**row,"observation_seq":335,"t_game_ms":23527}
    models=[]
    for index in range(145):
        models.append({"kind":"model_input","request":{"observation":old},"response":{"model_processed":True},
                       "timing":{"process_begin_wall_monotonic_ns":10+index,"process_end_wall_monotonic_ns":11+index}})
    models.append({"kind":"model_input","request":{"observation":row,"now_game_ms":23727},
                   "response":{"status":"fallback","reason":"observation_too_old","model_processed":False,"hidden_reset":True},
                   "timing":{"collector_received_wall_monotonic_ns":400,"process_begin_wall_monotonic_ns":401,"process_end_wall_monotonic_ns":402}})
    ipc=[];decisions=[]
    def pair(request,response):
        ipc.extend(({"direction":"request","value":request},{"direction":"response","value":response}))
    for index in range(1,14):
        identifier=5000+index if index<13 else 6042
        command={"cmd":"body_action",**identity,"command_id":index,"observation_seq":335}
        reply={"id":identifier,"accepted":index<13,"reason":"accepted" if index<13 else "stale_observation"}
        pair({"id":identifier,**command},reply)
        decisions.append({"kind":"arbitration","observation":old,"command_request":command,"command_response":reply,
                          "command_id":index if index<13 else None,"execution_id":index if index<13 else 0,
                          "recovery_attempt":1,"stale_refresh_pending":True})
    pair({"id":6041,"cmd":"body_observations"},{"id":6041})
    pair({"id":6044,"cmd":"body_events"},{"id":6044})
    pair({"id":6045,"cmd":"body_observations"},{"id":6045,"game_ms":23727,"observations":[row]})
    pair({"id":6046,"cmd":"body_release",**identity},{"id":6046,"accepted":True,"reason":"release_pending","wall_monotonic_ns":450})
    transport=[]
    for identifier,sent,received in ((6041,200,300),(6042,301,350),(6044,351,390),(6045,391,400),(6046,403,460)):
        transport.extend(({"direction":"request","id":identifier,"wall_monotonic_ns":sent},
                          {"direction":"response","id":identifier,"wall_monotonic_ns":received}))
    control={"phase":"ownership","reason":"native_restored",**identity,"release_reason":"released","t_game_ms":23761,"wall_monotonic_ns":440}
    terminal={"game_ms":23761,"outcome":"infrastructure","infrastructure":True,"reason":"unexpected_control_loss:released"}
    for name,values in (("policy-inputs.jsonl",models+decisions),("ipc.jsonl",ipc),("collector-transport.jsonl",transport),
                        ("body-control-events.jsonl",[control]),("arena-events.jsonl",[{"kind":"terminal","arena":terminal}])):
        (folder/name).write_text("".join(json.dumps(value)+"\n" for value in values))
    (folder/"observer-timing.jsonl").write_text('{"kind":"sample_step"}\n{"sim_step":231,"game_ms":23461,"kind":"sample_step","actors":')
    return folder


def test_partial_known_age_failure_keeps_145_forwards_refusal_release_and_truncated_bytes(tmp_path):
    folder=partial_age_failure(tmp_path);before=(folder/"observer-timing.jsonl").read_bytes()
    proof=final.partial_known_age_evidence(folder)
    assert proof["real_model_forwards"]==145 and proof["command_opportunities"]==13 and proof["accepted_model_commands"]==12
    assert proof["fallback_age_game_ms"]==166 and proof["timing_trace_complete"] is False and proof["quality_credit"] is False
    assert (folder/"observer-timing.jsonl").read_bytes()==before


@pytest.mark.parametrize("mutation",["promoted_episode","wrong_age","different_release","fabricated_acceptance"])
def test_partial_known_failure_rejects_promotion_or_changed_causal_chain(tmp_path,mutation):
    folder=partial_age_failure(tmp_path)
    if mutation=="promoted_episode":write(folder/"episode.json",{})
    elif mutation=="wrong_age":
        path=folder/"policy-inputs.jsonl";rows=[json.loads(s) for s in path.read_text().splitlines()]
        rows[145]["request"]["now_game_ms"]=23627;path.write_text("".join(json.dumps(row)+"\n" for row in rows))
    elif mutation=="different_release":
        path=folder/"body-control-events.jsonl";row=json.loads(path.read_text());row["release_reason"]="target_visibility_lost";write(path,row)
    else:
        path=folder/"ipc.jsonl";rows=[json.loads(s) for s in path.read_text().splitlines()]
        rows[1]["value"]["accepted"]=False;path.write_text("".join(json.dumps(row)+"\n" for row in rows))
    with pytest.raises(ValueError,match="partial known"):
        final.partial_known_age_evidence(folder)


def test_runtime_scheduling_transition_requires_native_pending_input_proof(tmp_path):
    with pytest.raises(ValueError,match="native pending-input proof"):
        final.pending_input_binding(tmp_path,"binary","source","library",{"frozen_files":{}})


def test_separate_queue_proof_cannot_change_native_binary_or_source(tmp_path,monkeypatch):
    monkeypatch.setattr(final.repair,"frozen_environment",lambda root:{"binary_sha256":"different-engine"})
    with pytest.raises(ValueError,match="different binary or source"):
        final.queue_proof_binding(tmp_path/"queue",tmp_path/"native",{"binary_sha256":"physical-engine"},{})


def failed_queue_trace():
    rows=[];requests={};responses={}
    for index,identifier in enumerate((8,81,153)):
        base=index*100;step=index*2;game=16000+step*33
        common={"clock_domain":"linux-clock-monotonic","oracle_only":True,"runner_generation":1}
        for phase,clock,sim,stamp,game_ms,tick in (
                ("tick_begin",base+10,step,base+10,game,step+1),
                ("tick_end",base+30,step+1,base+10,game+33,step+1),
                ("input_read",base+31,step+1,base+20,game+33,step+1),
                ("tick_begin",base+33,step+1,base+20,game+33,step+2),
                ("tick_end",base+34,step+2,base+20,game+66,step+2)):
            row={**common,"seq":len(rows)+1,"phase":phase,"wall_monotonic_ns":clock,"sim_step":sim,
                 "tick_due_wall_monotonic_ns":stamp,"game_ms":game_ms,"tick_id":tick}
            if phase=="input_read":row.update(request_id=identifier,command="body_control_status",admitted_before_due_tick=False,max_ready_commands_per_due_tick=1)
            rows.append(row)
        requests[identifier]=({"cmd":"body_control_status"},base+29)
        responses[identifier]=({"wall_monotonic_ns":base+32,"sim_step":step+1,"game_ms":game+33,
                               "mode":"realtime","paused":False,"arena":{"started":True,"outcome":"running"}},base+35)
    return rows,requests,responses


def test_prior_queue_failure_preserves_exact_three_real_post_overrun_queries():
    rows,requests,responses=failed_queue_trace();proof=final.failed_queue_chains(rows,requests,responses)
    assert [row["request_id"] for row in proof["failed_query_chains"]]==[8,81,153]
    assert proof["quality_credit"] is False
    assert all(row["native_reply_wall_monotonic_ns"]<row["following_tick_begin_wall_monotonic_ns"] for row in proof["failed_query_chains"])


@pytest.mark.parametrize("mutation",["prestart","paused","not_overrun"])
def test_prior_queue_failure_cannot_reinterpret_prestart_or_another_boundary(mutation):
    rows,requests,responses=failed_queue_trace()
    if mutation=="prestart":responses[8][0]["arena"]["started"]=False
    elif mutation=="paused":responses[8][0]["paused"]=True
    else:rows[1]["wall_monotonic_ns"]=19
    with pytest.raises(ValueError,match="actual started post-overrun"):
        final.failed_queue_chains(rows,requests,responses)


def latency_failure(tmp_path,monkeypatch):
    prior,candidate=delivery_failure(tmp_path,monkeypatch);name="block-reaction-223-0-gru";folder=prior/"realtime-reaction"/name;folder.mkdir()
    declaration=final.read(prior/"final-evaluation.json");declaration["quality"]={"model_step_p95_ms":5};write(prior/"final-evaluation.json",declaration)
    inputs=[];latencies=[]
    for index,duration in enumerate((1243341,18962995,792226,1248120),1):
        row={"entity_id":1,"session_epoch":7,"world_epoch":8,"control_generation":9,"observation_seq":3+2*index}
        timing={"process_begin_wall_monotonic_ns":index*100_000_000,"process_end_wall_monotonic_ns":index*100_000_000+duration}
        inputs.append({"kind":"model_input","request":{"stream_seq":index,"observation":row},"response":{"model_processed":True},"timing":timing})
        latencies.append({"kind":"model_step","observation_key":[row[key] for key in ("entity_id","session_epoch","world_epoch","control_generation","observation_seq")],**timing})
    for name_file,rows in (("policy-inputs.jsonl",inputs),("latency.jsonl",latencies)):
        (folder/name_file).write_text("".join(json.dumps(row)+"\n" for row in rows))
    record={"policy":"gru","final":{"outcome":"task_success","reason":"melee_hit_blocked","primary_entity_id":1,
             "actors":[{"entity_id":1,"hp":300,"received_damage":0,"blocked_hits":1}]},"neural_control_fraction":1,
             "fallback_events":[],"no_fallback_credit":True,"model_stats":{"runtime_fallbacks":0},
             "latency_sha256":final.sha(folder/"latency.jsonl"),"model_journal_sha256":final.sha(folder/"policy-inputs.jsonl")}
    write(folder/"episode.json",record)
    report=final.read(prior/"validation/known-audit-v1/report.json");report["issues"]=[name+": model_step_ms missing/exceeds budget"]
    report["declaration_sha256"]=final.sha(prior/"final-evaluation.json")
    report["raw_sha256"]={"realtime-reaction/"+name+"/episode.json":final.sha(folder/"episode.json")}
    write(prior/"validation/known-audit-v1/report.json",report)
    return prior,candidate


def test_actual_model_latency_failure_preserves_all_four_wall_samples_without_stall_claim(tmp_path,monkeypatch):
    prior,candidate=latency_failure(tmp_path,monkeypatch);value=final.additional_failure_snapshot(prior,candidate)
    assert value["kind"]=="model_latency_failure" and value["real_model_samples"]==4 and value["model_step_p95_ms"]==18.962995
    assert value["cause"]=="not_established" and value["quality_credit"] is False and value["actual_block_preserved"] is True


def test_model_latency_failure_cannot_drop_locked_sample_or_add_other_failure(tmp_path,monkeypatch):
    prior,candidate=latency_failure(tmp_path,monkeypatch);folder=prior/"realtime-reaction/block-reaction-223-0-gru"
    path=folder/"policy-inputs.jsonl";rows=path.read_text().splitlines();path.write_text("\n".join(rows[:1]+rows[2:])+"\n")
    record=final.read(folder/"episode.json");record["model_journal_sha256"]=final.sha(path);write(folder/"episode.json",record)
    report_path=prior/"validation/known-audit-v1/report.json";report=final.read(report_path)
    report["raw_sha256"]["realtime-reaction/block-reaction-223-0-gru/episode.json"]=final.sha(folder/"episode.json");write(report_path,report)
    with pytest.raises(ValueError,match="population omitted real inputs"):
        final.additional_failure_snapshot(prior,candidate)


def visibility_failed_cohort():
    final_state={"outcome":"control_lost","reason":"target_visibility_lost","infrastructure":False,"elapsed_game_ms":2067,
                 "game_ms":18361,"session_epoch":7,"world_epoch":8,
                 "actors":[{"hp":3000,"owner":"native"}]+[{"hp":3000,"owner":"scripted"}]*3}
    record={"name":"four-actor-gru-0","cohort":0,"policy":"gru","final":final_state,"neural_control_fraction":2/3,
            "fallback_events":[],"model_stats":{"runtime_fallbacks":0}}
    decisions=[{"command_response":{"accepted":True}},{"command_response":{"accepted":True}},{"command_response":{"accepted":False}}]
    controls=[{"phase":"ownership","reason":"native_restored","release_reason":"target_visibility_lost","t_game_ms":18361,
               "session_epoch":7,"world_epoch":8}]
    window={"native_final":final_state,"healthy_physical_ownership":False,"all_requests_inside_window":False}
    return record,decisions,controls,window


def test_closed_gru_visibility_failure_keeps_short_window_and_rejected_opportunity():
    proof=final.failed_visibility_load_cohort(*visibility_failed_cohort())
    assert proof["elapsed_game_ms"]==2067 and proof["neural_control_fraction"]==2/3
    assert proof["service_window_valid"] is False and proof["quality_credit"] is False


@pytest.mark.parametrize("mutation",["promoted_window","changed_outcome","no_restore","changed_fraction"])
def test_visibility_load_failure_cannot_gain_credit_by_changing_window_or_handoff(mutation):
    record,decisions,controls,window=visibility_failed_cohort()
    if mutation=="promoted_window":window["healthy_physical_ownership"]=True
    elif mutation=="changed_outcome":record["final"]["outcome"]="time_limit"
    elif mutation=="no_restore":controls=[]
    else:record["neural_control_fraction"]=1
    with pytest.raises(ValueError):final.failed_visibility_load_cohort(record,decisions,controls,window)


def incomplete_load_failure(tmp_path,monkeypatch):
    prior,candidate=latency_failure(tmp_path,monkeypatch)
    report_path=prior/"validation/known-audit-v1/report.json";report=final.read(report_path)
    report.update(passed=True,issues=[]);write(report_path,report)
    cases=[]
    for policy,count in (("native",4),("gru",4),("mlp",2)):
        for index in range(count):
            folder=prior/"load"/f"four-actor-{policy}-{index}";folder.mkdir(parents=True)
            record={"name":folder.name,"policy":policy,"physical_actor_ids":[1,2,3,4],
                    "final":{"outcome":"time_limit","infrastructure":False},"fallback_events":[],
                    "model_stats":{"runtime_fallbacks":0},"neural_control_fraction":None if policy=="native" else 1}
            if policy!="native":
                journal=folder/"policy-inputs.jsonl"
                journal.write_text(json.dumps({"kind":"model_input","timing":{"process_begin_wall_monotonic_ns":100,
                    "process_end_wall_monotonic_ns":200}})+"\n")
                record["model_journal_sha256"]=final.sha(journal)
            write(folder/"episode.json",record);cases.append(record)
    write(prior/"load/suite.json",{"kind":"body_policy_stage8_four_actor_load","status":"infrastructure_failure",
          "error":"native arena infrastructure failure: unexpected_control_loss:released","behavioral_quality_evaluated":False,"cases":cases})
    partial=prior/"load/four-actor-mlp-2";partial.mkdir()
    for name in ("ipc.jsonl","collector-transport.jsonl","policy-inputs.jsonl","latency.jsonl","observations.jsonl","events.jsonl","body-control-events.jsonl"):
        (partial/name).write_text("")
    (partial/"arena-events.jsonl").write_text(json.dumps({"kind":"terminal","arena":{"outcome":"infrastructure","infrastructure":True,
           "reason":"unexpected_control_loss:released","real_actor_count":4}})+"\n")
    for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
        (prior/(stem+"-load"+suffix)).write_text("preserved actual failure")
    return prior,candidate


def test_known_pass_and_partial_load_failure_remain_bound_without_load_credit(tmp_path,monkeypatch):
    prior,candidate=incomplete_load_failure(tmp_path,monkeypatch);value=final.additional_failure_snapshot(prior,candidate)
    assert value["known_passed"] is True and value["load_quality_credit"] is False and len(value["completed_cohorts"])==10
    assert value["partial_cohort"]=="four-actor-mlp-2" and "load/four-actor-mlp-2/ipc.jsonl" in value["files"]


def test_incomplete_load_guard_cannot_omit_closed_cohort_or_promote_partial(tmp_path,monkeypatch):
    prior,candidate=incomplete_load_failure(tmp_path,monkeypatch);path=prior/"load/suite.json";suite=final.read(path)
    removed=suite["cases"].pop();write(path,suite)
    with pytest.raises(ValueError,match="exact ten-cohort incomplete"):
        final.additional_failure_snapshot(prior,candidate)
    suite["cases"].append(removed);write(path,suite);write(prior/"load/four-actor-mlp-2/episode.json",{})
    with pytest.raises(ValueError,match="discarded or promoted"):
        final.additional_failure_snapshot(prior,candidate)


def test_zero_case_gc_startup_refusal_cannot_hide_a_runner(tmp_path,monkeypatch):
    prior,candidate=compact_failure(tmp_path,monkeypatch)
    for name in ("realtime","realtime-reaction","load","screening"):
        if (prior/name).exists():
            with pytest.raises(ValueError,match="progressed to runtime/fresh"):
                final.collector_startup_failure_snapshot(prior,candidate)
            break


def masked_load_failure(tmp_path,monkeypatch):
    prior,candidate=incomplete_load_failure(tmp_path,monkeypatch);path=prior/"load/suite.json";suite=final.read(path)
    suite["cases"]=suite["cases"][:5];suite["error"]="unexpected realtime decision refusal: masked";write(path,suite)
    folder=prior/"load/four-actor-gru-1"
    for name in ("episode.json","policy-inputs.jsonl"):
        (folder/name).unlink(missing_ok=True)
    for name in ("ipc.jsonl","collector-transport.jsonl","policy-inputs.jsonl","latency.jsonl","arena-events.jsonl",
                 "observations.jsonl","events.jsonl","body-control-events.jsonl"):
        (folder/name).write_text("")
    request={"id":7,"cmd":"body_action","entity_id":4,"session_epoch":1,"world_epoch":2,"control_generation":8,"goal_version":1,
             "target_entity_id":3,"provider":"stage8-gru","observation_seq":197,"issued_game_ms":18461,"action_id":8,"ttl_ms":100,"command_id":7}
    allowed=[False]*12;allowed[8]=True
    source={key:request[key] for key in ("entity_id","session_epoch","world_epoch","control_generation","observation_seq")}
    source.update(periodic=True,can_decide=True,action_mask_valid=True,action_mask=allowed,t_game_ms=18461,goal={"version":1},target={"entity_id":3})
    current={**request,"owner":"scripted","release_pending":False,"control_allowed":True,"action_mask":[False]*12}
    response={"id":7,"accepted":False,"reason":"masked","execution_id":0,"game_ms":18494,"body_control":{"actors":[current]},
              "arena":{"outcome":"running","infrastructure":False,"actors":[{"entity_id":3,"hp":3000,"down":False},{"entity_id":4,"hp":3000,"down":False}]}}
    rows=[{"direction":"response","value":{"observations":[source]}},{"direction":"request","value":request},{"direction":"response","value":response}]
    (folder/"ipc.jsonl").write_text("".join(json.dumps(row)+"\n" for row in rows))
    return prior,candidate,folder


def test_five_closed_cohorts_and_live_mask_refusal_receive_no_load_credit(tmp_path,monkeypatch):
    prior,candidate,_=masked_load_failure(tmp_path,monkeypatch);value=final.additional_failure_snapshot(prior,candidate)
    assert value["kind"]=="closed_known_load_mask_refusal" and len(value["completed_cohorts"])==5
    assert value["partial_cohort"]=="four-actor-gru-1" and value["native_terminal"] is None and value["load_quality_credit"] is False
    assert value["masked_refusal"]["accepted"] is False and value["masked_refusal"]["rejected_opportunity_retained"] is True


def test_mask_failure_guard_cannot_hide_lost_ownership_as_a_healthy_race(tmp_path,monkeypatch):
    prior,candidate,folder=masked_load_failure(tmp_path,monkeypatch)
    rows=[json.loads(line) for line in (folder/"ipc.jsonl").read_text().splitlines()]
    rows[-1]["value"]["body_control"]["actors"][0]["release_pending"]=True
    (folder/"ipc.jsonl").write_text("".join(json.dumps(row)+"\n" for row in rows))
    with pytest.raises(ValueError,match="healthy same-token provenance"):
        final.additional_failure_snapshot(prior,candidate)

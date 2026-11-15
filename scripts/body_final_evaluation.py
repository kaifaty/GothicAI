#!/usr/bin/env python3
"""Freeze the repaired held-out evaluation against already selected child weights.

This artifact tool never starts a model/engine or reads a screening outcome
during declaration. The original v6 failures remain explicitly bound evidence.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parent))
import body_evaluation_repair as repair

KIND="body_stage8_final_evaluation_preregistration"
CONFIG="code/evals/npc-body/body-stage8-evaluation-v2.json"
EXECUTION_PHASES={"shadow":"shadow","realtime":"realtime","realtime-reaction":"realtime-reaction","load":"load","screening":"screening-reaction"}
PROTOCOL=("code/scripts/body_final_evaluation.py","code/scripts/body_combat_evaluation.py",CONFIG,
          "code/scripts/body_block_reaction.py","code/evals/npc-body/body-block-reaction-v1.json",
          "code/scripts/body_service_load.py","code/scripts/body_policy_load.py","code/evals/npc-body/body-stage8-v1.json")
BOOTSTRAP_INTERPRETER={"python_version":[3,12,13],"python_implementation":"cpython",
    "executable":str(Path.home()/"gothic-ai/body/.venv/bin/python"),
    "resolved_executable":str(Path.home()/".local/share/uv/python/cpython-3.12.13-linux-x86_64-gnu/bin/python3.12"),
    "executable_sha256":"021044895e95be79dc2f110367607e684119afbc8ce75f6f0eec94844e0acec7"}


def require(condition,message):
    if not condition:raise ValueError(message)


def read(path):return json.loads(Path(path).read_text())
def sha(path):return repair.sha(Path(path))


def service_gate_role(config):
    role=config.get("service_gate_role","combat_gate")
    version=config.get("service_gate_protocol_version",0)
    # remote_host_not_measured (v2): the gate runs on a host without the protected
    # services, so it shares no CPU with them and measures none; their report stays
    # a separate artefact of the service host.
    require(role in ("combat_gate","standalone_report","remote_host_not_measured") and type(version) is int and version in (0,1,2) and
            (role!="standalone_report" or version==1) and (role!="remote_host_not_measured" or version==2),
            "invalid declared service gate role/version")
    return role


def fixed_child(root):
    environment=repair.frozen_environment(root);selected=read(root/"selection.json")
    config=read(root/"code/evals/npc-body/body-defence-bc-v1.json")
    profile=read(root/"dataset/selection-profile.json")
    require(selected.get("test_and_ood_used_for_selection") is False and selected.get("selection_split")=="eval" and
            selected.get("selection_view")=="full_episode" and selected.get("evaluation_mode")=="continuous", "child weights selected using another context")
    require(profile.get("kind")=="body_defence_child_selection_view" and profile.get("test_and_ood_used_for_selection") is False and
            selected.get("selection_profile_sha256")==sha(root/"dataset/selection-profile.json") and
            selected.get("config_sha256")==sha(root/"code/evals/npc-body/body-defence-bc-v1.json"), "child selection declaration changed")
    index=read(root/"dataset/dataset.json")
    require(index["episode_count"]==207 and {json.loads(row["group"])[1] for row in index["episodes"] if row["split"]=="eval"}=={223,227},
            "child selection reopened excluded legacy/fresh groups")
    for key,name in (("dataset_sha256","dataset/dataset.json"),("normalization_sha256","dataset/normalization.json")):
        require(selected.get(key)==profile.get(key)==sha(root/name),"child data/normalization changed")
    require(profile["selection_evaluation_episode_ids"]==selected["selection_evaluation_episode_ids"]==
            [row["episode_id"] for row in index["episodes"] if row["split"]=="eval"],"child selected undeclared eval episodes")
    files=["environment.json","source.tar.gz","reference/gothic-body-runner","selection.json","dataset/dataset.json",
           "dataset/normalization.json","dataset/selection-profile.json","code/evals/npc-body/body-defence-bc-v1.json"]
    for architecture in ("gru","mlp"):
        chosen=selected["models"][architecture]["selected"]
        require(chosen["checkpoint_sha256"]==sha(root/"models"/architecture/"best.pt"),"selected child checkpoint changed")
        require(read(root/"models"/architecture/"selection.json")==selected["models"][architecture],"child global/model selection differs")
        files.extend("models/"+architecture+"/"+name for name in ("best.pt","best.pt.json","selection.json","training.json","history.json"))
    return {"run":str(root),"files":repair.inventory(root,files),"checkpoint_sha256":{
        name:selected["models"][name]["selected"]["checkpoint_sha256"] for name in ("gru","mlp")},
        "normalizer_sha256":selected["normalization_sha256"],"weights_fixed_at_utc":selected["weights_fixed_at_utc"],
        "training_device":selected["training_device"],"binary_sha256":environment["binary_sha256"],
        "selection_evaluation_episode_ids":selected["selection_evaluation_episode_ids"],"selection_allowed_during_evaluation":False}


def declared_protocol(root,reference,candidate):
    config=read(root/CONFIG);old=read(reference/"code/evals/npc-body/body-stage8-v1.json")
    require(type(config["runtime"].get("native_deadline_polling",False)) is bool,"native deadline polling must be an explicit boolean")
    require(type(config["runtime"].get("native_serial_workers",False)) is bool,"native serial workers must be an explicit boolean")
    require(type(config["runtime"].get("collector_static_gc_freeze",False)) is bool,"collector static GC freeze must be an explicit boolean")
    reset=config["runtime"].get("collector_bootstrap_gc_reset",False)
    require(type(reset) is bool and (not reset or config["runtime"].get("collector_static_gc_freeze") is True),
            "collector bootstrap GC reset requires explicit boolean/static scope")
    require(config.get("kind")=="body_stage8_evaluation_protocol_repair" and config.get("schema_version")==2 and
            config.get("original_v6_credit") is False and config.get("selection_allowed") is False and config.get("retraining_allowed") is False,
            "evaluation protocol replaced old failures or selects weights")
    require(config["screening"]["cases"]==old["screening"]["cases"] and config["screening"]["seeds"]==old["screening"]["seeds"] and
            config["screening"]["policies"]==["native","gru","mlp"] and len(config["screening"]["cases"])==80,
            "fresh case matrix/order changed")
    require(sum(row["task"]=="block" for row in config["screening"]["cases"])==20 and
            config["quality"]==old["quality"],"task coverage or quality/performance thresholds changed")
    current=read(root/"code/evals/npc-body/body-stage8-v1.json")
    require(service_gate_role(current)==service_gate_role(config) and
            current.get("service_gate_protocol_version",0)==config.get("service_gate_protocol_version",0),
            "ordinary/load service gate role differs from declared evaluator")
    require(type(current["runtime"].get("native_deadline_polling",False)) is bool and
            current["runtime"].get("native_deadline_polling",False)==config["runtime"].get("native_deadline_polling",False),
            "ordinary/load native deadline polling differs from declared evaluator")
    require(type(current["runtime"].get("native_serial_workers",False)) is bool and
            current["runtime"].get("native_serial_workers",False)==config["runtime"].get("native_serial_workers",False),
            "ordinary/load native worker profile differs from declared evaluator")
    require(current["quality"]==old["quality"] and current["screening"]==old["screening"] and
            all(current["services"][key]==old["services"][key] for key in ("measured_requests","warmup_requests","during_cohorts","measured_requests_per_cohort","warmup_cohort","concurrency_per_service")),
            "current service protocol altered original quality/held-out/count gates")
    code_run=config["reference"]["policy_code_run_name"]
    require(not config["runtime"].get("native_serial_workers",False) or code_run==root.name,
            "serial worker evaluation requires the current explicitly frozen native launcher")
    require(code_run in (reference.name,root.name) and
            config["bindings"].get("original_stage8_config_run_name",reference.name)==reference.name,
            "runtime code or original case declaration belongs to an undeclared run")
    if code_run==root.name:
        require(all(config["runtime"].get(key)==value for key,value in
                    (("compact_replies",True),("heartbeat_interval_ms",250),("max_stale_refresh_attempts",2),("startup_warmup_steps",2))),
                "current delivery/startup profile differs from the bounded declared repair")
        threads=config["runtime"].get("inference_cpu_threads",2)
        require(type(threads) is int and threads in (1,2),"declared inference CPU threads outside bounded 1..2 profile")
    require(config["reference"]["run_name"]==Path(candidate["run"]).name and
            config["reference"]["normalizer_sha256"]==candidate["normalizer_sha256"] and
            all(config["reference"][name+"_checkpoint_sha256"]==candidate["checkpoint_sha256"][name] for name in ("gru","mlp")),
            "evaluation config does not bind the fixed selected child")
    require(config["bindings"]["original_stage8_config_sha256"]==sha(reference/"code/evals/npc-body/body-stage8-v1.json") and
            config["bindings"]["preparation_helper_sha256"]==sha(root/"code/scripts/body_block_reaction.py") and
            config["bindings"]["preparation_config_sha256"]==sha(root/"code/evals/npc-body/body-block-reaction-v1.json"),
            "repaired evaluator preparation/source contract changed")
    require(config["preparation"]==read(root/"code/evals/npc-body/body-block-reaction-v1.json")["preparation"],
            "final reaction physical preparation changed")
    policy_root=reference if code_run==reference.name else root
    original_files=read(policy_root/"environment.json")["frozen_files"]
    for name,digest in config["bindings"]["frozen_policy_modules"].items():
        relative=("code/body-service/src/"+name.replace(".","/")+".py") if name.startswith("gothic_body.") else "code/scripts/"+name+".py"
        require(original_files.get(relative)==digest==sha(policy_root/relative),"declared actual policy/runtime/packer import binding changed")
    groups=set(read(Path(candidate["run"])/"dataset/dataset.json")["groups"])
    seeds={json.loads(group)[1] for group in groups}
    require(not seeds.intersection(config["screening"]["seeds"]),"fresh screening overlaps any train/eval/test/OOD group")
    require(config["screening"].get("selection_allowed") is False,"fresh screening allowed adaptive selection")
    return config


def prior_failure_snapshot(prior,candidate):
    """Bind a failed known run before retry; never inspect a fresh screening case."""
    repair.frozen_environment(prior)
    require(not (prior/"screening").exists(),"prior fresh screening was exposed; a new held-out declaration is required")
    declaration=read(prior/"final-evaluation.json")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"],"retry changed selected weights/normalization")
    reaction=read(prior/"realtime-reaction/suite.json");load=read(prior/"load/suite.json")
    require(reaction.get("status")=="collected" and len(reaction.get("cases",[]))==6 and not reaction.get("error"),
            "prior known reaction evidence is missing")
    failures=[]
    for record in reaction["cases"]:
        if record["policy"]!="gru":continue
        primary=next(actor for actor in record["final"]["actors"] if actor["entity_id"]==record["final"]["primary_entity_id"])
        if record["final"]["outcome"]!="task_success" or primary["blocked_hits"]==0 or primary["hp"]!=300 or record.get("neural_control_fraction",0)<.95 or record.get("fallback_events"):
            failures.append({"name":record["name"],"outcome":record["final"]["outcome"],"hp":primary["hp"],
                "blocked_hits":primary["blocked_hits"],"neural_control_fraction":record.get("neural_control_fraction"),"quality_credit":False})
    require(failures and load.get("status")=="infrastructure_failure" and load.get("error"),"prior failed known/load result is not preserved")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json"]
    for phase in ("shadow","realtime","realtime-reaction","load"):
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix)
            require(file.is_file(),"prior failed phase resource/service/log binding missing")
            names.append(str(file.relative_to(prior)))
    audit=prior/"validation/known-reaction-audit"
    require((audit/"report.json").is_file() and (audit/"diagnosis.json").is_file(),"independent prior failure audit/diagnosis missing")
    names.extend(str(file.relative_to(prior)) for file in audit.rglob("*") if file.is_file())
    return {"run":str(prior),"files":repair.inventory(prior,names),"known_gru_failures":failures,"load_error":load["error"],
            "fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def preparation_failure_snapshot(prior,candidate):
    """The specific closed compact-purity failure has no model quality result."""
    repair.frozen_environment(prior)
    require(not (prior/"screening").exists() and not (prior/"load").exists(),"preparation failure progressed to another quality phase")
    declaration=read(prior/"final-evaluation.json");suite=read(prior/"realtime-reaction/suite.json")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"],"additional failed run changed selected models")
    require(suite.get("status")=="infrastructure_failure" and suite.get("cases")==[] and suite.get("error") in ("'rng_state'","rng_state"),
            "additional prior is not the exact zero-case compact preparation failure")
    for path in (prior/"realtime-reaction").glob("*/policy-inputs.jsonl"):
        for line in path.read_text().splitlines():
            require(json.loads(line).get("kind") not in ("model_input","arbitration"),"failed preparation already ran a model decision")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json"]
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt"
        require(sha(file)==candidate["checkpoint_sha256"][architecture],"additional failed runtime checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"additional failed runtime normalizer changed")
    names.append("dataset/normalization.json")
    for phase in ("shadow","realtime","realtime-reaction"):
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"closed compact preparation resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    return {"kind":"compact_preparation_purity_infrastructure_failure","run":str(prior),"files":repair.inventory(prior,names),
            "error":suite["error"],"completed_reaction_cases":0,"model_quality_evaluated":False,"fresh_screening_started":False,
            "selected_weights_changed":False,"prior_failures_replaced":False}


def known_delivery_failure_snapshot(prior,candidate):
    """Preserve the exact closed known control failure before a native retry."""
    repair.frozen_environment(prior)
    require(not (prior/"load").exists() and not (prior/"screening").exists(),"known delivery failure exposed load or fresh screening")
    declaration=read(prior/"final-evaluation.json");report_file=prior/"validation/known-audit-v1/report.json";report=read(report_file)
    name="realtime-distance-223-1-gru"
    require(report.get("kind")=="body_stage8_known_saved_runtime_report" and report.get("passed") is False and
            report.get("issues")==[name+": healthy control below .95",name+": healthy neural control below95percent"] and
            report.get("load_read") is False and report.get("fresh_screening_read") is False and report.get("stage8_quality_credit") is False,
            "additional prior is not the exact closed known delivery failure")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==report.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==report.get("normalization_sha256")==candidate["normalizer_sha256"] and
            report.get("environment_sha256")==sha(prior/"environment.json") and report.get("declaration_sha256")==sha(prior/"final-evaluation.json"),
            "known delivery retry changed selected models or original audit binding")
    repair.verify_inventory(prior,report["raw_sha256"])
    record=read(prior/"realtime"/name/"episode.json");receipts=record.get("command_responses",[])
    rejected=[row for row in receipts if row["response"].get("accepted") is False]
    accepted=[row for row in receipts if row["response"].get("accepted") is True]
    require(record.get("policy")=="gru" and record.get("neural_control_fraction")==2/3 and len(receipts)==3 and len(accepted)==2 and
            len(rejected)==1 and rejected[0]["response"].get("reason")=="stale_observation" and record.get("fallback_events")==[],
            "known delivery failure command counts/refusal changed")
    refusal=rejected[0];request=refusal["request"];response=refusal["response"]
    with (prior/"realtime"/name/"observations.jsonl").open() as stream:
        rows=[row for line in stream if (row:=json.loads(line)).get("entity_id")==request["entity_id"] and
              row.get("observation_seq")==refusal.get("native_current_observation_seq")]
    require(len(rows)==1 and all(rows[0].get(key)==request.get(key) for key in ("entity_id","session_epoch","world_epoch","control_generation")) and
            request["observation_seq"]<rows[0]["observation_seq"] and response.get("clock_domain")==rows[0].get("clock_domain")=="linux-clock-monotonic" and
            refusal["sent_wall_monotonic_ns"]<rows[0]["snapshot_wall_monotonic_ns"]<refusal["received_wall_monotonic_ns"],
            "known stale refusal lacks actual future observation clock evidence")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"failed delivery normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt"
        require(sha(file)==candidate["checkpoint_sha256"][architecture],"failed delivery runtime checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6)):
        suite=read(prior/phase/"suite.json")
        require(suite.get("status")=="collected" and len(suite.get("cases",[]))==count and not suite.get("error"),"known delivery stages not closed")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"closed failed-known resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"closed_known_delivery_failure","run":str(prior),"files":repair.inventory(prior,names),"known_audit_sha256":sha(report_file),
            "failed_case":name,"accepted_model_commands":2,"command_opportunities":3,"healthy_neural_control_fraction":2/3,
            "rejected_reason":"stale_observation","command_sent_wall_monotonic_ns":refusal["sent_wall_monotonic_ns"],
            "future_observation_wall_monotonic_ns":rows[0]["snapshot_wall_monotonic_ns"],"quality_credit":False,
            "load_started":False,"fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def model_latency_failure_snapshot(prior,candidate):
    """Retain a real four-input latency failure without inventing its cause."""
    repair.frozen_environment(prior)
    require(not (prior/"load").exists() and not (prior/"screening").exists(),"model latency failure exposed load or fresh screening")
    name="block-reaction-223-0-gru";report_file=prior/"validation/known-audit-v1/report.json"
    report=read(report_file);declaration=read(prior/"final-evaluation.json")
    require(report.get("kind")=="body_stage8_known_saved_runtime_report" and report.get("passed") is False and
            report.get("issues")==[name+": model_step_ms missing/exceeds budget"] and report.get("load_read") is False and
            report.get("fresh_screening_read") is False and report.get("stage8_quality_credit") is False,
            "additional prior is not the exact closed known model latency failure")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==report.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==report.get("normalization_sha256")==candidate["normalizer_sha256"] and
            report.get("environment_sha256")==sha(prior/"environment.json") and report.get("declaration_sha256")==sha(prior/"final-evaluation.json"),
            "model latency retry changed selected models or original audit binding")
    repair.verify_inventory(prior,report["raw_sha256"])
    path=prior/"realtime-reaction"/name;record=read(path/"episode.json");final=record["final"]
    primary=next(actor for actor in final["actors"] if actor["entity_id"]==final["primary_entity_id"])
    require(record.get("policy")=="gru" and final.get("outcome")=="task_success" and final.get("reason")=="melee_hit_blocked" and
            primary.get("hp")==300 and primary.get("blocked_hits")>=1 and primary.get("received_damage")==0 and
            record.get("neural_control_fraction")==1 and record.get("fallback_events")==[] and record.get("no_fallback_credit") is True and
            record.get("model_stats",{}).get("runtime_fallbacks")==0,"latency failure did not preserve actual healthy model block")
    require(record.get("latency_sha256")==sha(path/"latency.jsonl") and record.get("model_journal_sha256")==sha(path/"policy-inputs.jsonl"),
            "failed model latency journals changed")
    inputs=[json.loads(line) for line in (path/"policy-inputs.jsonl").read_text().splitlines() if line]
    inputs=[row for row in inputs if row.get("kind")=="model_input"]
    latencies=[json.loads(line) for line in (path/"latency.jsonl").read_text().splitlines() if line]
    latencies=[row for row in latencies if row.get("kind")=="model_step"]
    require(len(inputs)==len(latencies)==4 and all(row.get("response",{}).get("model_processed") is True for row in inputs),
            "failed model latency population omitted real inputs")
    samples=[]
    for index,(entry,receipt) in enumerate(zip(inputs,latencies),1):
        timing=entry["timing"];row=entry["request"]["observation"]
        begin,end=timing.get("process_begin_wall_monotonic_ns"),timing.get("process_end_wall_monotonic_ns")
        require(entry["request"].get("stream_seq")==index and type(begin) is int and type(end) is int and 0<begin<end and
                receipt.get("observation_key")==[row[key] for key in ("entity_id","session_epoch","world_epoch","control_generation","observation_seq")] and
                all(receipt.get(key)==value for key,value in timing.items()),"failed model latency source clocks/identity differ")
        samples.append((end-begin)/1e6)
    p95=sorted(samples)[-1]
    require(p95==18.962995 and p95>declaration["quality"]["model_step_p95_ms"]==5,"recorded model p95 failure was weakened or reclassified")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"failed latency normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"failed latency checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6)):
        suite=read(prior/phase/"suite.json")
        require(suite.get("status")=="collected" and len(suite.get("cases",[]))==count and not suite.get("error"),"known latency stages not closed")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"closed failed-latency resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"model_latency_failure","run":str(prior),"files":repair.inventory(prior,names),"known_audit_sha256":sha(report_file),
            "failed_case":name,"real_model_samples":4,"model_process_wall_ms":samples,"model_step_p95_ms":p95,"model_step_budget_ms":5,
            "cause":"not_established","actual_block_preserved":True,"healthy_neural_control_fraction":1,"quality_credit":False,
            "load_started":False,"fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def known_load_failure_snapshot(prior,candidate):
    """Bind the complete known pass and the subsequent incomplete load matrix."""
    repair.frozen_environment(prior)
    require(not (prior/"screening").exists(),"failed known/load run exposed fresh screening")
    declaration=read(prior/"final-evaluation.json");report_file=prior/"validation/known-audit-v1/report.json";report=read(report_file)
    require(report.get("kind")=="body_stage8_known_saved_runtime_report" and report.get("passed") is True and report.get("issues")==[] and
            report.get("load_read") is False and report.get("fresh_screening_read") is False and report.get("stage8_quality_credit") is False and
            declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==report.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==report.get("normalization_sha256")==candidate["normalizer_sha256"] and
            report.get("environment_sha256")==sha(prior/"environment.json") and report.get("declaration_sha256")==sha(prior/"final-evaluation.json"),
            "failed load dropped its known pass or changed selected models")
    repair.verify_inventory(prior,report["raw_sha256"])
    suite=read(prior/"load/suite.json")
    masked=suite.get("error")=="unexpected realtime decision refusal: masked"
    counts=(("native",4),("gru",1)) if masked else (("native",4),("gru",4),("mlp",2))
    names_expected=[f"four-actor-{policy}-{index}" for policy,count in counts for index in range(count)]
    require(suite.get("kind")=="body_policy_stage8_four_actor_load" and suite.get("status")=="infrastructure_failure" and
            (masked or suite.get("error")=="native arena infrastructure failure: unexpected_control_loss:released") and
            suite.get("behavioral_quality_evaluated") is False and [row.get("name") for row in suite.get("cases",[])]==names_expected,
            "additional prior is not the exact "+("five-cohort masked" if masked else "ten-cohort incomplete")+" load failure")
    completed=[]
    for record in suite["cases"]:
        folder=prior/"load"/record["name"];final=record["final"]
        require(read(folder/"episode.json")==record and final.get("outcome")=="time_limit" and final.get("infrastructure") is False and
                len(set(record.get("physical_actor_ids",[])))==4,"failed load changed its completed physical cohorts")
        wall=[]
        if record["policy"]!="native":
            require(record.get("fallback_events")==[] and record.get("model_stats",{}).get("runtime_fallbacks")==0 and
                    record.get("neural_control_fraction",0)>=.95,"completed failed-load cohort lost healthy neural control")
            journal=folder/"policy-inputs.jsonl";require(record.get("model_journal_sha256")==sha(journal),"completed load timing journal changed")
            with journal.open() as stream:
                for line in stream:
                    entry=json.loads(line)
                    if entry.get("kind")!="model_input":continue
                    timing=entry["timing"];begin,end=timing["process_begin_wall_monotonic_ns"],timing["process_end_wall_monotonic_ns"]
                    require(type(begin) is int and type(end) is int and 0<begin<=end,"completed load model wall interval reversed")
                    wall.append((end-begin)/1e6)
            require(wall,"completed neural load has no actual forward population")
        completed.append({"name":record["name"],"policy":record["policy"],"healthy_neural_control_fraction":record.get("neural_control_fraction"),
                          "actual_model_calls":len(wall),"model_step_p95_ms":sorted(wall)[math.ceil(len(wall)*.95)-1] if wall else None})
    partial_name="four-actor-gru-1" if masked else "four-actor-mlp-2";partial=prior/"load"/partial_name
    require(all((partial/name).is_file() for name in ("ipc.jsonl","collector-transport.jsonl","policy-inputs.jsonl","latency.jsonl",
                "arena-events.jsonl","observations.jsonl","events.jsonl","body-control-events.jsonl")) and not (partial/"episode.json").exists(),
            "partial failed cohort was discarded or promoted to complete")
    with (partial/"arena-events.jsonl").open() as stream:
        terminals=[row["arena"] for line in stream if (row:=json.loads(line)).get("kind")=="terminal"]
    masked_proof=None
    if masked:
        requests={};refusals=[];sources=[]
        with (partial/"ipc.jsonl").open() as stream:
            for line in stream:
                entry=json.loads(line);value=entry["value"]
                if entry["direction"]=="request" and value.get("cmd")=="body_action":requests[value["id"]]=value
                if entry["direction"]=="response":
                    sources.extend(value.get("observations",[]))
                    if value.get("accepted") is False and value.get("reason")=="masked":refusals.append(value)
        require(len(refusals)==1 and not terminals,"masked collector abort was fabricated as a native terminal")
        refusal=refusals[0];request=requests[refusal["id"]]
        source=[row for row in sources if row.get("entity_id")==request["entity_id"] and row.get("observation_seq")==request["observation_seq"]]
        current=[row for row in refusal["body_control"]["actors"] if row["entity_id"]==request["entity_id"]]
        hp=[row for row in refusal["arena"]["actors"] if row["entity_id"] in (request["entity_id"],request["target_entity_id"])]
        fields=("entity_id","session_epoch","world_epoch","control_generation","goal_version","target_entity_id","provider")
        require(len(source)==len(current)==1 and source[0].get("periodic") is True and source[0].get("can_decide") is True and
                source[0].get("action_mask_valid") is True and source[0]["action_mask"][request["action_id"]] is True and
                all(source[0].get(key)==request.get(key) for key in ("entity_id","session_epoch","world_epoch","control_generation")) and
                source[0]["goal"]["version"]==request["goal_version"] and source[0]["target"]["entity_id"]==request["target_entity_id"] and
                request.get("action_id")==8 and request.get("ttl_ms")==100 and current[0].get("observation_seq")==request["observation_seq"] and
                all(current[0].get(key)==request.get(key) for key in fields) and current[0].get("owner")=="scripted" and
                current[0].get("release_pending") is False and current[0].get("control_allowed") is True and
                current[0]["action_mask"][request["action_id"]] is False and refusal.get("execution_id")==0 and
                refusal["game_ms"]>request["issued_game_ms"]==source[0]["t_game_ms"] and refusal["arena"].get("outcome")=="running" and
                refusal["arena"].get("infrastructure") is False and len(hp)==2 and all(row["hp"]>0 and row.get("down") is False for row in hp),
                "masked refusal lacks actual allowed-source/forbidden-live-mask healthy same-token provenance")
        with (partial/"body-control-events.jsonl").open() as stream:
            require(not any((event:=json.loads(line)).get("phase")=="started" and event.get("entity_id")==request["entity_id"] and
                            event.get("command_id")==request["command_id"] for line in stream),"masked refused command was physically started")
        masked_proof={"request":request,"source_game_ms":source[0]["t_game_ms"],"source_action_mask":source[0]["action_mask"],
                      "actual_current_actor":current[0],"actual_refusal_game_ms":refusal["game_ms"],"accepted":False,"execution_id":0,
                      "rejected_opportunity_retained":True}
    else:
        require(len(terminals)==1 and terminals[0].get("outcome")=="infrastructure" and terminals[0].get("infrastructure") is True and
                terminals[0].get("reason")=="unexpected_control_loss:released" and terminals[0].get("real_actor_count")==4,
                "partial load error lacks its actual native four-actor terminal")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"failed load normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"failed load checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6),("load",len(names_expected))):
        phase_suite=read(prior/phase/"suite.json")
        require(len(phase_suite.get("cases",[]))==count and (phase=="load" or phase_suite.get("status")=="collected" and not phase_suite.get("error")),
                "failed load lost its closed known matrix")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"failed known/load resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"closed_known_load_mask_refusal" if masked else "closed_known_load_infrastructure_failure","run":str(prior),"files":repair.inventory(prior,names),"known_audit_sha256":sha(report_file),
            "known_passed":True,"completed_cohorts":completed,"partial_cohort":partial_name,"native_terminal":terminals[0] if terminals else None,
            "masked_refusal":masked_proof,
            "load_error":suite["error"],"load_quality_credit":False,"fresh_screening_started":False,
            "selected_weights_changed":False,"prior_failures_replaced":False}


def failed_visibility_load_cohort(record,decisions,controls,window):
    final=record["final"];index=record["cohort"];lost=index in (0,2,3)
    accepted=sum(row.get("command_response",{}).get("accepted") is True for row in decisions)
    require(record.get("policy")=="gru" and index in (0,1,2,3) and decisions and
            accepted/len(decisions)==record.get("neural_control_fraction") and record.get("fallback_events")==[] and
            record.get("model_stats",{}).get("runtime_fallbacks")==0,"failed GRU cohort actual opportunity/fallback population differs")
    require(final.get("outcome")==("control_lost" if lost else "time_limit") and
            final.get("reason")==("target_visibility_lost" if lost else "time_limit") and final.get("infrastructure") is False and
            len(final.get("actors",[]))==4 and all(actor["hp"]>0 for actor in final["actors"]),"failed load GRU visibility outcome changed")
    require(window.get("native_final")==final and window.get("healthy_physical_ownership") is (not lost) and
            window.get("all_requests_inside_window") is (not lost),"failed GRU service window was promoted or changed")
    if lost:
        require(final["elapsed_game_ms"]<20000 and any(actor.get("owner")=="native" for actor in final["actors"]) and
                any(event.get("phase")=="ownership" and event.get("reason")=="native_restored" and
                    event.get("release_reason")=="target_visibility_lost" and event.get("t_game_ms")<=final["game_ms"] and
                    event.get("session_epoch")==final["session_epoch"] and event.get("world_epoch")==final["world_epoch"] for event in controls),
                "failed GRU visibility handoff lacks actual native restoration")
    else:require(final["elapsed_game_ms"]==20000 and record["neural_control_fraction"]>=.95,"sole complete GRU load changed")
    return {"name":record["name"],"outcome":final["outcome"],"elapsed_game_ms":final["elapsed_game_ms"],
            "accepted_commands":accepted,"opportunities":len(decisions),"neural_control_fraction":record["neural_control_fraction"],
            "service_window_valid":not lost,"quality_credit":False}


def visibility_load_failure_snapshot(prior,candidate):
    repair.frozen_environment(prior)
    require(not (prior/"screening").exists(),"visibility load failure exposed fresh screening")
    declaration=read(prior/"final-evaluation.json");known_path=prior/"validation/known-audit-v1/report.json";known=read(known_path)
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==known.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==known.get("normalization_sha256")==candidate["normalizer_sha256"] and
            known.get("kind")=="body_stage8_known_saved_runtime_report" and known.get("environment_sha256")==sha(prior/"environment.json") and
            known.get("declaration_sha256")==sha(prior/"final-evaluation.json") and known.get("passed") is True and known.get("issues")==[] and known.get("load_read") is False and
            known.get("fresh_screening_read") is False and known.get("stage8_quality_credit") is False,
            "visibility load retry dropped its known pass or changed fixed models")
    repair.verify_inventory(prior,known["raw_sha256"])
    suite=read(prior/"load/suite.json");expected=[f"four-actor-{policy}-{index}" for policy in ("native","gru") for index in range(4)]
    require(suite.get("status")=="infrastructure_failure" and suite.get("error")=="native arena infrastructure failure: unexpected_control_loss:released" and
            [record.get("name") for record in suite.get("cases",[])]==expected,"prior is not exact eight-cohort visibility load failure")
    completed=[]
    for record in suite["cases"]:
        folder=prior/"load"/record["name"]
        require(read(folder/"episode.json")==record and len(set(record.get("physical_actor_ids",[])))==4,"visibility failure completed cohort changed")
        if record["policy"]=="native":
            require(record["final"].get("outcome")=="time_limit" and record["final"]["elapsed_game_ms"]==20000,"failed load native cohort changed")
            continue
        entries=[json.loads(line) for line in (folder/"policy-inputs.jsonl").read_text().splitlines()]
        decisions=[entry for entry in entries if entry.get("kind")=="arbitration"]
        require(record.get("model_journal_sha256")==sha(folder/"policy-inputs.jsonl"),"failed GRU model journal changed")
        ipc=[json.loads(line) for line in (folder/"ipc.jsonl").read_text().splitlines()]
        raw_requests={entry["value"]["id"]:entry["value"] for entry in ipc if entry["direction"]=="request"}
        raw_responses={entry["value"]["id"]:entry["value"] for entry in ipc if entry["direction"]=="response"}
        for decision in decisions:
            response=decision["command_response"];identifier=response["id"]
            require(response==raw_responses.get(identifier) and {key:value for key,value in raw_requests.get(identifier,{}).items() if key!="id"}==decision["command_request"],
                    "failed GRU accepted/refused opportunity differs from actual IPC")
        controls=[json.loads(line) for line in (folder/"body-control-events.jsonl").read_text().splitlines()]
        window=read(prior/"load/services"/f"during-gru-{record['cohort']}"/"window-binding.json")
        require(record["service_workload"]["window_binding"]==window,"failed GRU window sidecar differs")
        completed.append(failed_visibility_load_cohort(record,decisions,controls,window))
    partial=prior/"load/four-actor-mlp-0"
    require(not (partial/"episode.json").exists(),"failed MLP cohort promoted")
    entries=[json.loads(line) for line in (partial/"policy-inputs.jsonl").read_text().splitlines()]
    rejected=[entry for entry in entries if entry.get("kind")=="arbitration" and entry.get("command_response",{}).get("reason")=="target_invalid"]
    require(len(rejected)==1 and not any(entry.get("kind")=="model_input" and entry.get("response",{}).get("status")=="fallback" for entry in entries),
            "partial MLP failure was changed to a model age/budget fallback")
    entry=rejected[0];row=entry["observation"];request=entry["command_request"];response=entry["command_response"]
    actors=[actor for actor in response["body_control"]["actors"] if actor["entity_id"]==request["entity_id"]]
    require(request.get("entity_id")==2 and request.get("command_id")==6 and request.get("action_id")==1 and
            request.get("observation_seq")==row.get("observation_seq")==274 and request.get("issued_game_ms")==row.get("t_game_ms")==19261 and
            row.get("periodic") is True and row.get("can_decide") is True and row["action_mask"][1] is True and row["target"].get("visible") is True and
            response.get("accepted") is False and response.get("execution_id")==0 and response.get("game_ms")==19294 and len(actors)==1 and
            actors[0].get("owner")=="scripted" and actors[0].get("release_pending") is False and actors[0].get("control_allowed") is False and
            actors[0].get("can_decide") is False and actors[0].get("action_mask")==[False]*12 and actors[0].get("observation_seq")==277,
            "partial MLP refusal lacks genuine allowed-source/current-target guard evidence")
    requests={};responses={}
    for line in (partial/"ipc.jsonl").read_text().splitlines():
        value=json.loads(line);table=requests if value["direction"]=="request" else responses;table[value["value"]["id"]]=value["value"]
    fields=(*("entity_id","session_epoch","world_epoch","control_generation"),"provider","goal_version","target_entity_id")
    require(all(actors[0].get(key)==request.get(key) for key in fields) and
            response==responses[7906] and requests[7906]=={"id":7906,**request} and requests[7907].get("cmd")=="body_release" and
            all(requests[7907].get(key)==request.get(key) for key in ("entity_id","session_epoch","world_epoch","control_generation","provider")) and
            responses[7907].get("accepted") is True and responses[7907].get("reason")=="release_pending",
            "partial MLP refusal/release differs from actual IPC")
    controls=[json.loads(line) for line in (partial/"body-control-events.jsonl").read_text().splitlines()]
    require(any(event.get("phase")=="ownership" and event.get("reason")=="native_restored" and event.get("entity_id")==2 and
                event.get("t_game_ms")==19294 and event.get("release_reason")=="released" for event in controls),"partial MLP explicit release lacks native restore")
    terminals=[entry["arena"] for line in (partial/"arena-events.jsonl").read_text().splitlines() if (entry:=json.loads(line)).get("kind")=="terminal"]
    require(len(terminals)==1 and terminals[0].get("outcome")=="infrastructure" and terminals[0].get("reason")=="unexpected_control_loss:released" and
            terminals[0].get("game_ms")==19294,"partial MLP actual infrastructure terminal changed")
    service_root=prior/"load/services"
    def service_rows(path):return [json.loads(line) for line in path.read_text().splitlines()]
    before=service_rows(service_root/"before/requests.jsonl")
    native=[row for index in range(4) for row in service_rows(service_root/f"during-native-{index}/requests.jsonl")]
    service_metrics={}
    for service in ("asr","tts","decider","gemma"):
        samples=[]
        for population in (before,native):
            selected=[row for row in population if row["service"]==service and row.get("warmup") is False]
            require(len(selected)==20 and all(row.get("status")=="completed" and math.isfinite(row["latency_ms"]) for row in selected),"failed load native/before service receipts changed")
            require(all(abs(row["latency_ms"]-(row["first_result_wall_monotonic_ns"]-row["metric_origin_wall_monotonic_ns"])/1e6)<=1e-9 for row in selected),
                    "failed load service diagnostic latency differs from raw clocks")
            samples.append(sorted(row["latency_ms"] for row in selected)[18])
        service_metrics[service]={"baseline_p95_ms":samples[0],"native_p95_ms":samples[1],"native_degradation_fraction":samples[1]/samples[0]-1}
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"visibility failure normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"visibility failure selected weights changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6),("load",8)):
        closed=read(prior/phase/"suite.json")
        require(len(closed.get("cases",[]))==count and (phase=="load" or closed.get("status")=="collected" and not closed.get("error")),"visibility load failed to preserve closed known matrix")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"visibility load failure resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"known_pass_eight_cohort_visibility_load_failure","run":str(prior),"files":repair.inventory(prior,names),
            "known_passed":True,"completed_native_cohorts":4,"completed_gru_cohorts":completed,"partial_case":"four-actor-mlp-0",
            "partial_refusal":"target_invalid","partial_accepted":False,"partial_execution_id":0,"partial_native_terminal":terminals[0],
            "native_service_diagnostic":service_metrics,"native_serial_workers":declaration.get("native_serial_workers"),
            "native_deadline_polling":declaration.get("native_deadline_polling"),"gru_service_comparison_valid":False,"load_quality_credit":False,
            "fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def start_age_load_failure_snapshot(prior,candidate):
    """Known pass, four closed native cohorts, then the first GRU cohort's age-guard fallback.

    Gate v12: right after arena start four back-to-back heartbeats each cost a catch-up
    tick, the first numeric rows reached the model past the 100 ms age guard, and the
    resulting release aborted the suite. Bound as evidence, never re-evaluated.
    """
    repair.frozen_environment(prior)
    require(not (prior/"screening").exists(),"start-age load failure exposed fresh screening")
    declaration=read(prior/"final-evaluation.json");known_path=prior/"validation/known-audit-v1/report.json";known=read(known_path)
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==known.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==known.get("normalization_sha256")==candidate["normalizer_sha256"] and
            known.get("kind")=="body_stage8_known_saved_runtime_report" and known.get("environment_sha256")==sha(prior/"environment.json") and
            known.get("declaration_sha256")==sha(prior/"final-evaluation.json") and known.get("passed") is True and known.get("issues")==[] and known.get("load_read") is False and
            known.get("fresh_screening_read") is False and known.get("stage8_quality_credit") is False,
            "start-age load retry dropped its known pass or changed fixed models")
    repair.verify_inventory(prior,known["raw_sha256"])
    suite=read(prior/"load/suite.json");expected=[f"four-actor-native-{index}" for index in range(4)]
    require(suite.get("status")=="infrastructure_failure" and suite.get("error")=="native arena infrastructure failure: unexpected_control_loss:released" and
            suite.get("behavioral_quality_evaluated") is False and [record.get("name") for record in suite.get("cases",[])]==expected,
            "prior is not the exact four-native start-age load failure")
    for record in suite["cases"]:
        folder=prior/"load"/record["name"]
        require(read(folder/"episode.json")==record and len(set(record.get("physical_actor_ids",[])))==4 and
                record["final"].get("outcome")=="time_limit" and record["final"].get("elapsed_game_ms")==20000,"start-age failure native cohort changed")
    partial=prior/"load/four-actor-gru-0"
    require(not (partial/"episode.json").exists(),"failed GRU start cohort promoted")
    entries=[json.loads(line) for line in (partial/"policy-inputs.jsonl").read_text().splitlines()]
    fallbacks=[entry for entry in entries if entry.get("kind")=="model_input" and entry.get("response",{}).get("status")=="fallback"]
    require(fallbacks and all(entry["response"].get("reason")=="observation_too_old" for entry in fallbacks) and
            not any(entry.get("kind")=="arbitration" for entry in entries),"partial GRU start failure is not a pre-decision age fallback")
    first=fallbacks[0]["request"];age=first["now_game_ms"]-first["observation"]["t_game_ms"]
    require(age>100,"partial GRU start fallback is not past the 100 ms age guard")
    terminals=[entry["arena"] for line in (partial/"arena-events.jsonl").read_text().splitlines() if (entry:=json.loads(line)).get("kind")=="terminal"]
    require(len(terminals)==1 and terminals[0].get("outcome")=="infrastructure" and terminals[0].get("reason")=="unexpected_control_loss:released",
            "partial GRU start infrastructure terminal changed")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"start-age failure normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"start-age failure selected weights changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6),("load",4)):
        closed=read(prior/phase/"suite.json")
        require(len(closed.get("cases",[]))==count and (phase=="load" or closed.get("status")=="collected" and not closed.get("error")),"start-age failure lost its closed known matrix")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"start-age load failure resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"known_pass_gru_start_observation_age_load_failure","run":str(prior),"files":repair.inventory(prior,names),
            "known_passed":True,"completed_native_cohorts":4,"partial_case":"four-actor-gru-0","partial_fallback":"observation_too_old",
            "partial_first_row_age_game_ms":age,"partial_native_terminal":terminals[0],"model_decisions_before_abort":0,
            "load_quality_credit":False,"fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def collector_startup_failure_snapshot(prior,candidate):
    """A refused static scope followed only synthetic warmup, with no runner."""
    repair.frozen_environment(prior)
    require(not any((prior/phase).exists() for phase in ("realtime","realtime-reaction","load","screening")),"collector startup failure progressed to runtime/fresh")
    declaration=read(prior/"final-evaluation.json");suite=read(prior/"shadow/suite.json");manifest=read(prior/"shadow/manifest.json")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"] and
            suite.get("kind")=="body_policy_stage8_diagnostics" and suite.get("phase")=="shadow" and suite.get("status")=="infrastructure_failure" and
            suite.get("cases")==[] and suite.get("error")=="static GC scope requires enabled dynamic GC and no preexisting freeze ownership",
            "additional prior is not the exact zero-case collector startup refusal")
    scope=read(prior/"shadow/collector-gc.json")
    require(manifest.get("collector_gc")==scope and scope.get("enabled") is True and "startup" not in scope and "restore" not in scope and
            scope.get("dynamic_gc_events")==[] and scope.get("dropped_total")==0,"failed collector startup was promoted or altered")
    require(not any((prior/"shadow").glob("*/ipc.jsonl")) and not any((prior/"shadow").glob("*/policy-inputs.jsonl")),"failed collector startup created a runner/real model history")
    require(set(manifest.get("startup_warmup",{}))=={"gru","mlp"} and
            all(proof.get("steps")==2 and proof.get("model_process_calls")==proof.get("commands")==0 and proof.get("runtime_before")==proof.get("runtime_after") and
                proof.get("runtime_after",{}).get("actors")==0 for proof in manifest["startup_warmup"].values()),"collector failure lost its synthetic-only warmup")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"collector failure normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"collector failure checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
        file=prior/(stem+"-shadow"+suffix);require(file.is_file(),"collector startup refusal resource/service/log missing")
        names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for directory in (prior/"shadow",prior/"validation") for file in directory.rglob("*") if file.is_file())
    return {"kind":"collector_static_gc_startup_failure","run":str(prior),"files":repair.inventory(prior,names),"error":suite["error"],
            "completed_cases":0,"engine_started":False,"real_policy_observations":0,"synthetic_warmup_steps_per_architecture":2,
            "quality_credit":False,"fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def partial_known_age_evidence(folder):
    def lines(name):return [json.loads(line) for line in (folder/name).read_text().splitlines() if line]
    journal=lines("policy-inputs.jsonl");models=[row for row in journal if row.get("kind")=="model_input"]
    decisions=[row for row in journal if row.get("kind")=="arbitration"]
    failures=[row for row in models if row.get("response",{}).get("status")=="fallback"]
    require(len(models)==146 and sum(row.get("response",{}).get("model_processed") is True for row in models)==145 and len(failures)==1 and
            len(decisions)==13 and sum(row.get("command_response",{}).get("accepted") is True for row in decisions)==12,
            "partial known failure actual forward/opportunity population changed")
    failure=failures[0];row=failure["request"]["observation"];token=("entity_id","session_epoch","world_epoch","control_generation")
    identity={key:row[key] for key in token};response=failure["response"]
    require(row.get("observation_seq")==337 and row.get("t_game_ms")==23561 and failure["request"].get("now_game_ms")==23727 and
            response.get("reason")=="observation_too_old" and response.get("model_processed") is False and response.get("hidden_reset") is True,
            "partial known failure is not the exact genuine age166 fallback")
    rejected=[entry for entry in decisions if entry.get("command_response",{}).get("accepted") is False]
    require(len(rejected)==1 and rejected[0]["observation"]["observation_seq"]==335 and
            rejected[0]["command_response"].get("reason")=="stale_observation" and rejected[0].get("recovery_attempt")==1 and
            rejected[0].get("stale_refresh_pending") is True and rejected[0].get("command_id") is None and
            rejected[0].get("execution_id")==0,"partial known failure strict stale refusal changed")
    requests={};responses={};wire={}
    for entry in lines("ipc.jsonl"):
        value=entry["value"];table=requests if entry["direction"]=="request" else responses
        require(value["id"] not in table,"partial known duplicate IPC ID")
        table[value["id"]]=value
        for observation in value.get("observations",[]):wire[tuple(observation.get(key) for key in token)+(observation["observation_seq"],)]=observation
    require(set(requests)==set(responses) and requests[6042]["cmd"]=="body_action" and requests[6042]["observation_seq"]==335 and
            responses[6042].get("accepted") is False and responses[6042].get("reason")=="stale_observation" and
            requests[6045]["cmd"]=="body_observations" and responses[6045].get("game_ms")==23727 and
            wire.get(tuple(identity.values())+(337,))==row and
            responses[6042]==rejected[0]["command_response"],"partial known refusal/oldest model row differs from exact received IPC")
    actual_actions=[value for value in requests.values() if value.get("cmd")=="body_action"]
    require(len(actual_actions)==13,"partial known actual primitive request count differs")
    for entry in decisions:
        command=entry["command_request"]
        matched=[value for value in actual_actions if all(value.get(key)==command.get(key) for key in (*token,"command_id"))]
        require(len(matched)==1 and {key:value for key,value in matched[0].items() if key!="id"}==command and
                responses[matched[0]["id"]]==entry["command_response"],"partial known accepted/rejected opportunity differs from actual IPC")
    clocks={(entry["direction"],entry["id"]):entry["wall_monotonic_ns"] for entry in lines("collector-transport.jsonl")}
    require(failure["timing"]["collector_received_wall_monotonic_ns"]==clocks[("response",6045)] and
            clocks[("response",6045)]<=failure["timing"]["process_begin_wall_monotonic_ns"]<=failure["timing"]["process_end_wall_monotonic_ns"]<=clocks[("request",6046)] and
            requests[6046].get("cmd")=="body_release" and all(requests[6046].get(key)==value for key,value in identity.items()) and
            responses[6046].get("accepted") is True and responses[6046].get("reason")=="release_pending",
            "partial known age fallback lacks its exact subsequent explicit release receipt")
    controls=lines("body-control-events.jsonl")
    restored=[event for event in controls if event.get("phase")=="ownership" and event.get("reason")=="native_restored" and
              all(event.get(key)==value for key,value in identity.items())]
    require(len(restored)==1 and restored[0].get("release_reason")=="released" and restored[0].get("t_game_ms")==23761 and
            clocks[("request",6046)]<=restored[0]["wall_monotonic_ns"]<=responses[6046]["wall_monotonic_ns"]<=clocks[("response",6046)],
            "partial known failure native restoration was not caused by the recorded release")
    terminals=[entry["arena"] for entry in lines("arena-events.jsonl") if entry.get("kind")=="terminal"]
    require(len(terminals)==1 and terminals[0].get("game_ms")==23761 and terminals[0].get("outcome")=="infrastructure" and
            terminals[0].get("infrastructure") is True and terminals[0].get("reason")=="unexpected_control_loss:released",
            "partial known failure lacks its authoritative native infrastructure terminal")
    timing_lines=(folder/"observer-timing.jsonl").read_text().splitlines();parsed=0;tail=None
    for index,line in enumerate(timing_lines):
        try:json.loads(line);parsed+=1
        except ValueError:
            require(index==len(timing_lines)-1 and line.startswith('{"sim_step":231,"game_ms":23461,') and
                    '"kind":"sample_step"' in line and line.endswith('"actors":'),"partial timing journal has another malformed row")
            tail=line
    require(tail is not None and not (folder/"episode.json").exists(),"partial known timing tail/unfinished episode was promoted")
    wall=[]
    for entry in models:
        begin,end=(entry["timing"][key] for key in ("process_begin_wall_monotonic_ns","process_end_wall_monotonic_ns"))
        require(type(begin) is int and type(end) is int and 0<begin<=end,"partial known model clock reversed")
        wall.append((end-begin)/1e6)
    return {"real_model_forwards":145,"real_model_inputs":146,"accepted_model_commands":12,"command_opportunities":13,
            "healthy_neural_control_before_fallback":12/13,"rejected_reason":"stale_observation","fallback_observation_seq":337,
            "fallback_observed_game_ms":23561,"fallback_now_game_ms":23727,"fallback_age_game_ms":166,
            "fallback_reason":"observation_too_old","actual_native_terminal":terminals[0],"explicit_release_request_id":6046,
            "model_process_wall_p95_ms":sorted(wall)[math.ceil(len(wall)*.95)-1],
            "rpc_roundtrip_ms":{str(identifier):(clocks[("response",identifier)]-clocks[("request",identifier)])/1e6 for identifier in (6041,6042,6044,6045)},
            "timing_trace_complete":False,"complete_timing_lines":parsed,"truncated_timing_tail_preserved":True,
            "native_rpc_delay_cause":"not_established","quality_credit":False}


def partial_known_failure_snapshot(prior,candidate):
    repair.frozen_environment(prior)
    require(not any((prior/phase).exists() for phase in ("realtime-reaction","load","screening")),"partial known failure exposed later stages")
    declaration=read(prior/"final-evaluation.json");suite=read(prior/"realtime/suite.json");shadow=read(prior/"shadow/suite.json")
    name="realtime-native_duel-223-1-gru";folder=prior/"realtime"/name
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"],"partial known retry changed selected models")
    config=read(prior/"code/evals/npc-body/body-stage8-v1.json")
    expected=[case["name"]+"-"+policy for case in config["realtime"]["cases"] for policy in config["realtime"]["policies"]]
    require(shadow.get("status")=="collected" and len(shadow.get("cases",[]))==3 and not shadow.get("error") and
            suite.get("status")=="infrastructure_failure" and suite.get("error")=="native arena infrastructure failure: unexpected_control_loss:released" and
            len(suite.get("cases",[]))==22 and [record.get("name") for record in suite["cases"]]==expected[:22] and expected[22]==name,
            "additional prior is not the exact closed shadow3/ordinary22 partial known failure")
    for phase in ("shadow","realtime"):
        for record in read(prior/phase/"suite.json")["cases"]:
            require(read(prior/phase/record["name"]/"episode.json")==record,"partial known completed episode differs from suite")
    evidence=partial_known_age_evidence(folder)
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"partial known normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"partial known checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase in ("shadow","realtime"):
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"partial known closed resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"partial_known_observation_age_infrastructure_failure","run":str(prior),"files":repair.inventory(prior,names),
            "error":suite["error"],"closed_shadow_cases":3,"closed_ordinary_cases":22,"partial_case":name,**evidence,
            "reaction_started":False,"load_started":False,"fresh_screening_started":False,"selected_weights_changed":False,"prior_failures_replaced":False}


def realtime_age_failure_snapshot(prior,candidate):
    """Gate v14: ordinary realtime aborted by the 100 ms age guard in its first GRU duel.

    Shadow 3 and ordinary 10 closed, the partial native_duel GRU case fell back with
    observation_too_old after runner ticks stretched to 30-75 ms on the shared host;
    the reaction phase was still collected. No load or fresh screening was started.
    """
    repair.frozen_environment(prior)
    require(not any((prior/phase).exists() for phase in ("load","screening")),"realtime age failure exposed load or fresh screening")
    declaration=read(prior/"final-evaluation.json");suite=read(prior/"realtime/suite.json");shadow=read(prior/"shadow/suite.json")
    reaction=read(prior/"realtime-reaction/suite.json")
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"],"realtime age retry changed selected models")
    config=read(prior/"code/evals/npc-body/body-stage8-v1.json")
    expected=[case["name"]+"-"+policy for case in config["realtime"]["cases"] for policy in config["realtime"]["policies"]]
    name=expected[10];folder=prior/"realtime"/name
    require(shadow.get("status")=="collected" and len(shadow.get("cases",[]))==3 and not shadow.get("error") and
            suite.get("status")=="infrastructure_failure" and suite.get("error")=="native arena infrastructure failure: unexpected_control_loss:released" and
            [record.get("name") for record in suite.get("cases",[])]==expected[:10] and name.endswith("-gru") and
            reaction.get("status")=="collected" and len(reaction.get("cases",[]))==6 and not reaction.get("error"),
            "additional prior is not the exact shadow3/ordinary10 realtime age failure")
    for phase in ("shadow","realtime","realtime-reaction"):
        for record in read(prior/phase/"suite.json")["cases"]:
            require(read(prior/phase/record["name"]/"episode.json")==record,"realtime age failure completed episode differs from suite")
    require(not (folder/"episode.json").exists(),"partial realtime GRU case promoted")
    entries=[json.loads(line) for line in (folder/"policy-inputs.jsonl").read_text().splitlines() if line]
    fallbacks=[entry for entry in entries if entry.get("kind")=="model_input" and entry.get("response",{}).get("status")=="fallback"]
    require(len(fallbacks)==1 and fallbacks[0]["response"].get("reason")=="observation_too_old","partial realtime case is not one age fallback")
    first=fallbacks[0]["request"];age=first["now_game_ms"]-first["observation"]["t_game_ms"]
    require(age>100,"partial realtime fallback is not past the 100 ms age guard")
    terminals=[entry["arena"] for line in (folder/"arena-events.jsonl").read_text().splitlines() if (entry:=json.loads(line)).get("kind")=="terminal"]
    require(len(terminals)==1 and terminals[0].get("outcome")=="infrastructure" and terminals[0].get("reason")=="unexpected_control_loss:released",
            "partial realtime infrastructure terminal changed")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json","dataset/normalization.json"]
    require(sha(prior/"dataset/normalization.json")==candidate["normalizer_sha256"],"realtime age failure normalizer changed")
    for architecture in ("gru","mlp"):
        file=prior/"models"/architecture/"best.pt";require(sha(file)==candidate["checkpoint_sha256"][architecture],"realtime age failure checkpoint changed")
        names.extend((str(file.relative_to(prior)),str((file.parent/"best.pt.json").relative_to(prior))))
    for phase in ("shadow","realtime","realtime-reaction"):
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"realtime age failure resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"known_realtime_observation_age_infrastructure_failure","run":str(prior),"files":repair.inventory(prior,names),
            "error":suite["error"],"closed_shadow_cases":3,"closed_ordinary_cases":10,"partial_case":name,
            "partial_fallback":"observation_too_old","partial_first_row_age_game_ms":age,"partial_native_terminal":terminals[0],
            "reaction_collected":True,"known_audit_started":False,"load_started":False,"fresh_screening_started":False,
            "selected_weights_changed":False,"prior_failures_replaced":False}


def additional_failure_snapshot(prior,candidate):
    if (prior/"realtime/suite.json").is_file() and read(prior/"realtime/suite.json").get("status")=="infrastructure_failure":
        if (prior/"realtime-reaction/suite.json").is_file():return realtime_age_failure_snapshot(prior,candidate)
        return partial_known_failure_snapshot(prior,candidate)
    if not (prior/"realtime-reaction/suite.json").is_file():
        return collector_startup_failure_snapshot(prior,candidate)
    suite=read(prior/"realtime-reaction/suite.json")
    if suite.get("status")=="infrastructure_failure" and suite.get("cases")==[]:
        return preparation_failure_snapshot(prior,candidate)
    if (prior/"load/suite.json").is_file() and read(prior/"validation/known-audit-v1/report.json").get("passed") is True:
        if len(read(prior/"load/suite.json").get("cases",[]))==8:return visibility_load_failure_snapshot(prior,candidate)
        if ([case.get("name") for case in read(prior/"load/suite.json").get("cases",[])]==[f"four-actor-native-{index}" for index in range(4)] and
                not (prior/"load/four-actor-gru-0/episode.json").exists()):
            return start_age_load_failure_snapshot(prior,candidate)
        return known_load_failure_snapshot(prior,candidate)
    if read(prior/"validation/known-audit-v1/report.json").get("issues")==["block-reaction-223-0-gru: model_step_ms missing/exceeds budget"]:
        return model_latency_failure_snapshot(prior,candidate)
    return known_delivery_failure_snapshot(prior,candidate)


def prior_known_snapshot(prior,candidate):
    """A completed known pass precedes an unmeasured load delivery replacement."""
    repair.frozen_environment(prior)
    require(not (prior/"load").exists() and not (prior/"screening").exists(),"prior known run exposed load or fresh screening")
    declaration=read(prior/"final-evaluation.json");report_file=prior/"validation/known-audit-v2/report.json";report=read(report_file)
    require(declaration.get("kind")==KIND and declaration["candidate"]["checkpoint_sha256"]==candidate["checkpoint_sha256"] and
            declaration["candidate"]["normalizer_sha256"]==candidate["normalizer_sha256"],"pre-load replacement selected different models")
    require(report.get("kind")=="body_stage8_known_saved_runtime_report" and report.get("passed") is True and report.get("issues")==[] and
            report.get("load_read") is False and report.get("fresh_screening_read") is False and report.get("stage8_quality_credit") is False and
            report.get("candidate_checkpoint_sha256")==candidate["checkpoint_sha256"] and
            report.get("normalization_sha256")==candidate["normalizer_sha256"] and
            report.get("environment_sha256")==sha(prior/"environment.json") and report.get("declaration_sha256")==sha(prior/"final-evaluation.json"),
            "prior closed-known audit is missing or promoted to a full/fresh result")
    repair.verify_inventory(prior,report["raw_sha256"])
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","final-evaluation.json"]
    for phase,count in (("shadow",3),("realtime",24),("realtime-reaction",6)):
        suite=read(prior/phase/"suite.json")
        require(suite.get("status")=="collected" and len(suite.get("cases",[]))==count and not suite.get("error"),"prior known stages not closed")
        names.extend(str(file.relative_to(prior)) for file in (prior/phase).rglob("*") if file.is_file())
        for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
            file=prior/(stem+"-"+phase+suffix);require(file.is_file(),"closed prior-known phase resource/service/log missing")
            names.append(str(file.relative_to(prior)))
    names.extend(str(file.relative_to(prior)) for file in (prior/"validation").rglob("*") if file.is_file())
    return {"kind":"closed_known_preload_delivery_replacement","run":str(prior),"files":repair.inventory(prior,names),
            "known_audit_sha256":sha(report_file),"known_passed":True,"model_failure":False,"load_started":False,
            "fresh_screening_started":False,"selected_weights_changed":False,"full_stage8_credit":False}


def pending_input_binding(proof,binary_sha256,source_sha256,library_sha256,environment):
    folder=proof/"pending-input-probe";helper=proof/"code/scripts/body_pending_input_probe.py"
    require((folder/"manifest.json").is_file() and (folder/"probe.json").is_file() and helper.is_file(),
            "runtime scheduling transition lacks its native pending-input proof")
    manifest=read(folder/"manifest.json");result=read(folder/"probe.json")
    require({"libOpenGothic.a","libengine-platform.a"} <= set(manifest.get("engine_libraries",{})),
            "native scheduling proof lacks engine library set provenance; legacy single-library hashes cannot be compared")
    require(manifest.get("binary_sha256")==binary_sha256 and manifest.get("source_sha256")==source_sha256 and
            manifest.get("engine_library_sha256")==library_sha256 and manifest.get("driver_sha256")==sha(helper)==
            environment["frozen_files"].get(str(helper.relative_to(proof))) and manifest.get("scheduling_diagnostics") is True and
            manifest.get("model") is None and result.get("kind")=="body_pending_input_probe" and result.get("status")=="collected" and
            result.get("model") is None and result.get("quality_gate_evaluated") is False and
            result.get("profile",{}).get("quality_credit") is False,
            "native scheduling probe binary/source/helper/model binding differs")
    inventory=manifest.get("helper_inventory",{})
    require(set(inventory)=={"body_arena_env","body_headless_suite","body_observe_suite"},"pending-input helper inventory differs")
    for name,entry in inventory.items():
        file=proof/"code/scripts"/(name+".py")
        require(Path(entry["path"]).resolve()==file.resolve() and entry.get("sha256")==sha(file)==
                environment["frozen_files"].get(str(file.relative_to(proof))),"pending-input imported changed source helper")
    sys.path.insert(0,str(proof/"code/scripts"))
    spec=importlib.util.spec_from_file_location("saved_pending_input_transition",helper)
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module
    previous={name:sys.modules.pop(name,None) for name in inventory}
    try:
        spec.loader.exec_module(module)
        require(module.helper_inventory()==inventory,"pending-input actual imports differ from declared queue source paths")
        saved=module.audit_saved_output(folder)
    finally:
        for name,original in previous.items():
            if original is None:sys.modules.pop(name,None)
            else:sys.modules[name]=original
    expected={"native_binary_source_helper_binding","strict_native_clock_and_ready_admission_cap_one",
              "actual_periodic_action_mask_and_execution","healthy_real_time_actors_without_model_or_clock_pause"}
    checks=saved.get("checks",[])
    require(saved.get("kind")=="body_pending_input_saved_audit" and saved.get("schema_version")==1 and saved.get("passed") is True and
            saved.get("model") is None and isinstance(checks,list) and len(checks)==4 and {row.get("name") for row in checks}==expected and
            all(row.get("passed") is True for row in checks) and saved.get("chains") and saved.get("query_chains"),
            "actual native ready-write/fresh-query clock chains or physical wait are missing")
    repair.verify_inventory(folder,saved["raw_sha256"])
    return saved


def queue_proof_binding(queue,proof,setup,environment):
    queue_environment=repair.frozen_environment(queue)
    require(queue_environment["binary_sha256"]==setup["binary_sha256"] and
            repair.archive_source_identity(queue/"source.tar.gz")["source_sha256"]==setup["source_sha256"],
            "queue/native physical proof use different binary or source")
    for name in ("body_arena_env","body_headless_suite","body_observe_suite"):
        relative="code/scripts/"+name+".py"
        require(queue_environment["frozen_files"].get(relative)==environment["frozen_files"].get(relative),
                "queue proof changed native physical harness helpers")
    saved=pending_input_binding(queue,setup["binary_sha256"],setup["source_sha256"],setup["engine_library_sha256"],queue_environment)
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","code/scripts/body_pending_input_probe.py"]
    names.extend("code/scripts/"+name+".py" for name in ("body_arena_env","body_headless_suite","body_observe_suite"))
    names.extend(str(file.relative_to(queue)) for file in (queue/"pending-input-probe").rglob("*") if file.is_file())
    value={"run":str(queue),"files":repair.inventory(queue,names),"binary_sha256":setup["binary_sha256"],
           "source_sha256":setup["source_sha256"],"engine_library_sha256":setup["engine_library_sha256"],"saved_audit":saved,
           "physical_proof_run":str(proof),"physical_proofs_repeated":False}
    if queue!=proof and (proof/"pending-input-probe/probe.json").is_file():
        previous=read(proof/"pending-input-probe/probe.json")
        old_names=[str(file.relative_to(proof)) for file in (proof/"pending-input-probe").rglob("*") if file.is_file()]
        old_names.append("code/scripts/body_pending_input_probe.py")
        value["prior_queue_attempt"]={"run":str(proof),"files":repair.inventory(proof,old_names),
                                      "status":previous.get("status"),"error":previous.get("error"),"quality_credit":False}
    return value


def failed_queue_chains(scheduling,requests,responses):
    """Identify the three observed post-overrun query replies, without credit."""
    require(scheduling and [row.get("seq") for row in scheduling]==list(range(1,len(scheduling)+1)),
            "failed queue scheduling trace dropped/duplicated rows")
    ticks={};active=None;previous=0
    for row in scheduling:
        stamp=row.get("wall_monotonic_ns")
        require(type(stamp) is int and previous<=stamp and row.get("clock_domain")=="linux-clock-monotonic" and
                row.get("oracle_only") is True,"failed queue native clock/domain invalid")
        previous=stamp
        if row["phase"]=="tick_begin":
            require(active is None and row["tick_id"] not in ticks,"failed queue tick overlaps")
            active=row;ticks[row["tick_id"]]=[row,None]
        elif row["phase"]=="tick_end":
            require(active is not None and active["tick_id"]==row["tick_id"] and row["sim_step"]==active["sim_step"]+1 and
                    row["tick_due_wall_monotonic_ns"]==active["tick_due_wall_monotonic_ns"],"failed queue physical tick end differs")
            ticks[row["tick_id"]][1]=row;active=None
        else:require(row["phase"]=="input_read" and active is None,"failed queue read inside physical tick")
    require(active is None and all(end is not None for _,end in ticks.values()),"failed queue physical tick incomplete")
    failures=[];fresh=0
    for row in scheduling:
        if row["phase"]!="input_read":continue
        identifier=row["request_id"];request,sent=requests[identifier];response,received=responses[identifier]
        require(row["command"]==request["cmd"] and row.get("max_ready_commands_per_due_tick")==1 and
                sent<=row["wall_monotonic_ns"]<=response["wall_monotonic_ns"]<=received,"failed queue READ/receipt clock differs")
        if row.get("admitted_before_due_tick") is True or row["command"] not in ("body_observations","status","body_control_status","body_events","arena_status"):
            continue
        following=[pair for pair in ticks.values() if pair[0]["seq"]>row["seq"]]
        if not following or row["wall_monotonic_ns"]<row["tick_due_wall_monotonic_ns"]:continue
        begin,end=min(following,key=lambda pair:pair[0]["seq"])
        if begin["tick_due_wall_monotonic_ns"]!=row["tick_due_wall_monotonic_ns"]:continue
        if end["wall_monotonic_ns"]<=response["wall_monotonic_ns"] and response["sim_step"]==end["sim_step"] and response["game_ms"]==end["game_ms"]:
            fresh+=1;continue
        preceding=[pair[1] for pair in ticks.values() if pair[1]["seq"]<row["seq"]]
        prior=max(preceding,key=lambda value:value["seq"]) if preceding else {}
        arena=response.get("arena",{})
        require(row["command"]=="body_control_status" and row.get("admitted_before_due_tick") is False and
                response.get("mode")=="realtime" and response.get("paused") is False and arena.get("started") is True and
                arena.get("outcome")=="running" and not arena.get("terminated") and not arena.get("truncated") and
                row["tick_due_wall_monotonic_ns"]<prior.get("wall_monotonic_ns",0)<=row["wall_monotonic_ns"]<=
                response["wall_monotonic_ns"]<begin["wall_monotonic_ns"]<=end["wall_monotonic_ns"] and
                prior.get("sim_step")==row["sim_step"]==response["sim_step"]==begin["sim_step"] and
                prior.get("game_ms")==row["game_ms"]==response["game_ms"]==begin["game_ms"] and
                end["sim_step"]==response["sim_step"]+1 and end["game_ms"]>response["game_ms"],
                "prior queue failure is not the actual started post-overrun query defect")
        failures.append({"request_id":identifier,"command":row["command"],"tick_due_wall_monotonic_ns":row["tick_due_wall_monotonic_ns"],
            "preceding_tick_end_wall_monotonic_ns":prior["wall_monotonic_ns"],"input_read_wall_monotonic_ns":row["wall_monotonic_ns"],
            "native_reply_wall_monotonic_ns":response["wall_monotonic_ns"],"following_tick_begin_wall_monotonic_ns":begin["wall_monotonic_ns"],
            "following_tick_end_wall_monotonic_ns":end["wall_monotonic_ns"],"reply_sim_step":response["sim_step"],
            "reply_game_ms":response["game_ms"],"required_sim_step":end["sim_step"],"required_game_ms":end["game_ms"]})
    require([row["request_id"] for row in failures]==[8,81,153],"prior queue failure differs from exact three saved query chains")
    return {"failed_query_chains":failures,"valid_fresh_query_chains":fresh,"quality_credit":False}


def prior_queue_failure_snapshot(prior):
    environment=repair.frozen_environment(prior);folder=prior/"pending-input-probe";result=read(folder/"probe.json")
    manifest=read(folder/"manifest.json");helper=prior/"code/scripts/body_pending_input_probe.py"
    require(result.get("status")=="collected" and result.get("error")=="queued numeric/status query replied before its due physical tick" and
            result.get("kind")=="body_pending_input_probe" and result.get("model") is None and result.get("quality_gate_evaluated") is False and
            manifest.get("model") is None and manifest.get("scheduling_diagnostics") is True and
            result.get("profile")==manifest.get("profile") and result["profile"].get("mode")=="realtime" and
            result["profile"].get("quality_credit") is False,"prior queue is not the exact closed no-model query ordering failure")
    require(manifest.get("binary_sha256")==environment["binary_sha256"] and
            manifest.get("source_sha256")==repair.archive_source_identity(prior/"source.tar.gz")["source_sha256"] and
            manifest.get("driver_sha256")==sha(helper)==environment["frozen_files"].get(str(helper.relative_to(prior))),
            "prior queue binary/source/driver changed")
    inventory=manifest.get("helper_inventory",{})
    require(set(inventory)=={"body_arena_env","body_headless_suite","body_observe_suite"},"prior queue helper inventory differs")
    for name,entry in inventory.items():
        file=prior/"code/scripts"/(name+".py")
        require(host_path(entry["path"]).resolve()==file.resolve() and entry.get("sha256")==sha(file)==environment["frozen_files"].get(str(file.relative_to(prior))),
                "prior queue imported changed helper")
    requests={};responses={}
    for line in (folder/"episode/ipc.jsonl").read_text().splitlines():
        row=json.loads(line);value=row["value"];table=requests if row["direction"]=="request" else responses
        require(value["id"] not in table and type(row.get("wall_monotonic_ns")) is int,"prior queue duplicate IPC/clock")
        table[value["id"]]=(value,row["wall_monotonic_ns"])
    require(set(requests)==set(responses) and len(requests)<=2200,"prior queue IPC incomplete/unbounded")
    for identifier,(request,sent) in requests.items():
        response,received=responses[identifier]
        require(request.get("cmd") not in ("pause","resume","advance","arena_step","body_fixture") and
                response.get("clock_domain")=="linux-clock-monotonic" and sent<=response["wall_monotonic_ns"]<=received and
                all(response.get(key) is False for key in ("graphics_device_created","window_created","audio_device_created")),
                "prior queue used a clock/device fixture or invalid native clock")
    scheduling=[json.loads(line) for line in (folder/"episode/observer-timing.jsonl").read_text().splitlines() if line]
    scheduling=[row for row in scheduling if row.get("kind")=="runner_scheduling"]
    closed=result["close_response"]["scheduling_diagnostics"]
    require(result["close_response"].get("done") is True and closed.get("rows_emitted")==len(scheduling) and
            closed.get("dropped_total")==0 and closed.get("capacity")==65536,"prior queue native close/capture incomplete")
    evidence=failed_queue_chains(scheduling,requests,responses)
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","code/scripts/body_pending_input_probe.py"]
    names.extend("code/scripts/"+name+".py" for name in inventory)
    names.extend(str(file.relative_to(prior)) for file in folder.rglob("*") if file.is_file())
    for stem,suffix in (("services","-before.json"),("services","-after.json"),("resources",".txt"),("launcher",".log")):
        file=prior/(stem+"-pending-input-probe"+suffix);require(file.is_file(),"prior queue resource/service/log missing")
        names.append(str(file.relative_to(prior)))
    return {"kind":"closed_native_queued_query_ordering_failure","run":str(prior),"files":repair.inventory(prior,names),
            "error":result["error"],"binary_sha256":manifest["binary_sha256"],"source_sha256":manifest["source_sha256"],
            "engine_library_sha256":manifest["engine_library_sha256"],"helper_sha256":manifest["driver_sha256"],**evidence,
            "model_started":False,"prior_failure_replaced":False,"strict_query_gate_waived":False}


def serial_worker_proof(proof,folders):
    results={}
    for name in folders:
        directory=proof/name;manifest=read(directory/"manifest.json")
        require(manifest.get("native_serial_workers") is True,"native physical proof did not use declared serial worker CLI")
        paths=sorted(directory.rglob("ipc.jsonl"));count=0
        require(paths,"serial worker physical proof lacks raw IPC")
        for path in paths:
            for line in path.read_text().splitlines():
                entry=json.loads(line)
                if entry.get("direction")!="response":continue
                value=entry["value"];profile=value.get("worker_execution")
                require(profile=={"serial":True,"mode":"caller_serial","worker_pool_created":False,"native_task_parallelism_cap":1} and
                        type(profile.get("serial")) is bool and type(profile.get("worker_pool_created")) is bool and
                        type(profile.get("native_task_parallelism_cap")) is int,"native physical proof actual serial worker profile differs")
                count+=1
        require(count,"serial worker physical proof lacks actual native responses")
        results[name]={"response_count":count,"native_task_parallelism_cap":1}
    return {"native_serial_workers":True,"actual_response_profiles":results,"total_process_thread_cap_claim":False,
            "native_rpc_delay_cause_claim":False}


def motor_binding(proof,root,candidate,queue_proof=None):
    environment=repair.frozen_environment(proof)
    runtime_binary=read(root/"environment.json")["binary_sha256"]
    require(environment["binary_sha256"]==runtime_binary,"held-out runtime/native proof use different engines")
    setup=read(proof/"setup-probe/manifest.json");parity=read(proof/"setup-probe/parity/headless-parity.json")
    realtime=read(proof/"realtime-probe/probe.json")
    require(setup["binary_sha256"]==runtime_binary and parity.get("status")=="matched" and
            parity.get("profile")=="full" and {row["seed"] for row in parity["episodes"]}=={17,29,43} and
            all(row["status"]=="matched" and row["difference_count"]==0 for row in parity["episodes"]), "updated engine full native parity missing")
    require(realtime.get("status")=="passed" and read(proof/"realtime-probe/manifest.json")["binary_sha256"]==runtime_binary,
            "updated engine realtime proof incomplete")
    require(repair.archive_source_identity(root/"source.tar.gz")["source_sha256"]==setup["source_sha256"],"final engine source differs from native motor proof")
    names=["environment.json","source.tar.gz","reference/gothic-body-runner","setup-probe/manifest.json",
           "setup-probe/parity/headless-parity.json","realtime-probe/probe.json","realtime-probe/manifest.json"]
    transition=None;queue_binding=None
    if runtime_binary!=candidate["binary_sha256"]:
        folder=proof/"visibility-boundary-probe";manifest=read(folder/"manifest.json");result=read(folder/"probe.json")
        helper=proof/"code/scripts/body_visibility_boundary_probe.py"
        require(manifest.get("binary_sha256")==runtime_binary and manifest.get("source_sha256")==setup["source_sha256"] and
                manifest.get("probe_sha256")==sha(helper)==environment["frozen_files"].get(str(helper.relative_to(proof))) and
                result.get("kind")=="body_visibility_boundary_probe" and result.get("status")=="passed" and result.get("quality_credit") is False and
                result.get("model") is None and result.get("imported_model_modules")==[],
                "runtime motor transition lacks exact frozen physical boundary proof")
        require(set(manifest["helper_sha256"])=={"body_arena_env.py","body_observe_suite.py","body_headless_suite.py"},
                "physical boundary proof helper inventory differs")
        for name,digest in manifest["helper_sha256"].items():
            relative="code/scripts/"+name if name.endswith(".py") else "code/scripts/"+name+".py"
            require(sha(proof/relative)==digest==environment["frozen_files"].get(relative),"motor proof imported changed helper")
        sys.path.insert(0,str(proof/"code/scripts"))
        spec=importlib.util.spec_from_file_location("saved_owned_motor_transition",helper)
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        physical=module.audit_saved_output(folder)
        require(len(physical.get("checks",{}))==8 and all(value is True for value in physical["checks"].values()) and
                physical.get("four_actors_retained") is True and physical.get("replayed_accepted_commands")==64,
                "saved owned motor regression lacks real hit/lock/visibility recovery")
        compact_folder=proof/"compact-reply-probe";compact_manifest=read(compact_folder/"manifest.json")
        compact_result=read(compact_folder/"probe.json");compact_helper=proof/"code/scripts/body_compact_reply_probe.py"
        require(compact_manifest.get("binary_sha256")==runtime_binary and compact_manifest.get("source_sha256")==setup["source_sha256"] and
                compact_manifest.get("probe_sha256")==sha(compact_helper)==environment["frozen_files"].get(str(compact_helper.relative_to(proof))) and
                compact_result.get("kind")=="body_compact_reply_probe" and compact_result.get("status")=="passed" and
                compact_result.get("quality_credit") is False and compact_result.get("model") is None and
                compact_result.get("imported_model_modules")==[],"compact transport lacks actual held/full preparation proof")
        require(set(compact_manifest["helper_sha256"])=={"body_block_reaction.py","body_arena_env.py","body_observe_suite.py","body_headless_suite.py"},
                "compact preparation helper inventory differs")
        for name,digest in compact_manifest["helper_sha256"].items():
            require(sha(proof/"code/scripts"/name)==digest==environment["frozen_files"].get("code/scripts/"+name),
                    "compact preparation imported changed helper")
        spec=importlib.util.spec_from_file_location("saved_compact_motor_transition",compact_helper)
        compact_module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=compact_module;spec.loader.exec_module(compact_module)
        compact=compact_module.audit_saved_output(compact_folder)
        require(len(compact.get("checks",{}))==10 and all(value is True for value in compact["checks"].values()),
                "saved compact preparation/active numeric invariants incomplete")
        queue_binding=queue_proof_binding(queue_proof or proof,proof,setup,environment)
        pending=queue_binding["saved_audit"]
        control=read(proof/"faults/validated/body-api.json");control_manifest=read(proof/"faults/manifest.json")
        require(control_manifest["binary_sha256"]==runtime_binary and control_manifest["source_sha256"]==setup["source_sha256"] and
                control.get("status")=="complete" and control.get("full_stage4_evidence") is True and not control.get("issues"),
                "runtime transition actual native control/mask fault proof incomplete")
        names.extend(str(file.relative_to(proof)) for directory in (folder,compact_folder,proof/"faults") for file in directory.rglob("*") if file.is_file())
        names.extend((str(helper.relative_to(proof)),str(compact_helper.relative_to(proof))))
        transition={"kind":"proved_owned_motor_and_transport_transition","training_binary_sha256":candidate["binary_sha256"],
                    "runtime_binary_sha256":runtime_binary,"physical_owned_regression":physical,"compact_preparation_proof":compact,
                    "pending_input_scheduling_proof":pending,
                    "native_control_report_sha256":sha(proof/"faults/validated/body-api.json"),
                    "weights_retrained":False,"historical_owned_trajectory_equivalence":False}
    workers=None
    if read(root/CONFIG)["runtime"].get("native_serial_workers",False):
        workers=serial_worker_proof(proof,("setup-probe","realtime-probe","visibility-boundary-probe","compact-reply-probe","faults"))
        workers["queue"]=serial_worker_proof(queue_proof or proof,("pending-input-probe",))
    return {"run":str(proof),"files":repair.inventory(proof,names),"binary_sha256":runtime_binary,
            "training_binary_sha256":candidate["binary_sha256"],"source_sha256":setup["source_sha256"],
            "motor_transition":transition,"queue_proof":queue_binding,"serial_worker_proof":workers,"historical_owned_trajectory_equivalence":False}


host_path=repair.host_path


def numerical_report(path,models):
    report=read(path)
    require(report.get("kind")=="body_defence_child_saved_data_audit" and report.get("schema_version")==1 and
            report.get("status")=="passed" and report.get("passed") is True and report.get("closed_loop_quality_credit") is False,
            "independent child data/numerical audit incomplete")
    files=report.get("file_inventory")
    require(isinstance(files,dict) and files,"numerical audit lacks source/raw dependency inventory")
    for name,expected in files.items():
        require(Path(name).is_absolute() and host_path(name).is_file() and sha(host_path(name))==expected,"independent numerical audit dependency changed")
    local={str(host_path(name)):expected for name,expected in files.items()}
    for name in ("environment.json","selection.json","dataset/dataset.json","dataset/normalization.json"):
        require(local.get(str(models/name))==sha(models/name),"numerical audit did not bind selected child artifact")
    return {"path":str(path),"sha256":sha(path),"file_inventory":files,"closed_loop_quality_credit":False}


def declare(root,reference,models,proof,child_report,prior_failed_run=None,additional_failed_runs=(),prior_known_run=None,queue_proof=None,prior_queue_failed_run=None):
    require(root!=reference and root!=models,"final evaluation needs its own immutable output run")
    require(not (root/"final-evaluation.json").exists() and not any((root/name).exists() for name in ("screening","realtime","realtime-reaction","load","shadow")),
            "preregister before any selected-model quality phase; preserve existing evidence")
    repair.frozen_environment(root);repair.frozen_environment(reference)
    require(Path(__file__).resolve()==(root/"code/scripts/body_final_evaluation.py").resolve(),"execute separately frozen declaration source")
    candidate=fixed_child(models);config=declared_protocol(root,reference,candidate)
    require(config["reference"]["policy_code_run_name"]==reference.name or prior_failed_run is not None,
            "current runtime retry requires preserved prior failed evaluation")
    native=motor_binding(proof,root,candidate,queue_proof)
    stamp=dt.datetime.now(dt.timezone.utc).isoformat()
    require(dt.datetime.fromisoformat(candidate["weights_fixed_at_utc"])<=dt.datetime.fromisoformat(stamp),"child weights not fixed before evaluation")
    value={"schema_version":1,"kind":KIND,"created_at_utc":stamp,"new_run":str(root),"reference_run":str(reference),
           "run_environment_sha256":sha(root/"environment.json"),"modules":repair.inventory(root,PROTOCOL),"candidate":candidate,
           "child_independent_numerical_audit":numerical_report(child_report,models),
           "original_files":repair.inventory(reference,["environment.json","source.tar.gz","reference/gothic-body-runner","selection.json",
               "code/evals/npc-body/body-stage8-v1.json"]),"original_failures":repair.original_failures(reference),"updated_engine":native,
           "candidate_policy":"gru","mlp_role":"comparison_only","repaired_task":"block",
           "original_neutral_block_results_replaced":False,"screening_case_count":80,"screening_seeds":config["screening"]["seeds"],
           "quality":config["quality"],"runtime_execution_phases":EXECUTION_PHASES,
           "service_gate_role":service_gate_role(config),"service_gate_protocol_version":config.get("service_gate_protocol_version",0),
           "inference_cpu_threads":config["runtime"].get("inference_cpu_threads",2),
           "collector_static_gc_freeze":config["runtime"].get("collector_static_gc_freeze",False),
           "collector_bootstrap_gc_reset":config["runtime"].get("collector_bootstrap_gc_reset",False),
           "native_deadline_polling":config["runtime"].get("native_deadline_polling",False),
           "native_serial_workers":config["runtime"].get("native_serial_workers",False),
           "launcher_sha256":sha(root/"code/scripts/body-bc-remote.sh"),"screening_selection_allowed":False,"retraining_allowed":False,
           "real_defense_requires_action9_blocked_hit_hp300":True,"jump_or_kill_is_block_success":False,
           "timeout_semantics":"preparation included in native held-start timeout; active duration reported separately"}
    if prior_failed_run is not None:value["prior_failed_evaluation"]=prior_failure_snapshot(prior_failed_run,candidate)
    if additional_failed_runs:
        require(len(set(additional_failed_runs))==len(additional_failed_runs) and prior_failed_run not in additional_failed_runs,
                "failed evaluations repeated")
        value["additional_failed_evaluations"]=[additional_failure_snapshot(prior,candidate) for prior in additional_failed_runs]
    if prior_known_run is not None:value["prior_known_evaluation"]=prior_known_snapshot(prior_known_run,candidate)
    if prior_queue_failed_run is not None:value["prior_failed_queue_proof"]=prior_queue_failure_snapshot(prior_queue_failed_run)
    if value["collector_bootstrap_gc_reset"]:
        require(list(sys.version_info[:3])==BOOTSTRAP_INTERPRETER["python_version"] and sys.implementation.name=="cpython" and
                str(Path(sys.executable).absolute())==BOOTSTRAP_INTERPRETER["executable"] and
                str(Path(sys.executable).resolve())==BOOTSTRAP_INTERPRETER["resolved_executable"] and sha(sys.executable)==BOOTSTRAP_INTERPRETER["executable_sha256"],
                "bootstrap GC declaration uses another interpreter")
        value["collector_bootstrap_interpreter"]=BOOTSTRAP_INTERPRETER
    (root/"final-evaluation.json").write_text(json.dumps(value,indent=2,allow_nan=False)+"\n")
    return value


def validate_declaration(root):
    value=read(root/"final-evaluation.json")
    require(value.get("kind")==KIND and value.get("schema_version")==1 and value.get("new_run")==str(root),"invalid final evaluation declaration")
    reference=Path(value["reference_run"]);models=Path(value["candidate"]["run"]);proof=Path(value["updated_engine"]["run"])
    for path in (root,reference,models,proof):repair.frozen_environment(path)
    require(value["run_environment_sha256"]==sha(root/"environment.json"),"final frozen environment changed")
    repair.verify_inventory(root,value["modules"]);repair.verify_inventory(reference,value["original_files"])
    repair.verify_inventory(reference,value["original_failures"]["files"])
    require(value["candidate"]==fixed_child(models),"selected child changed after preregistration")
    require(value["child_independent_numerical_audit"]==numerical_report(Path(value["child_independent_numerical_audit"]["path"]),models),
            "child numerical audit changed after preregistration")
    config=declared_protocol(root,reference,value["candidate"])
    require(value.get("service_gate_role","combat_gate")==service_gate_role(config) and
            value.get("service_gate_protocol_version",0)==config.get("service_gate_protocol_version",0),
            "service gate separation changed after preregistration")
    if "prior_failed_evaluation" in value:
        require(value["prior_failed_evaluation"]==prior_failure_snapshot(Path(value["prior_failed_evaluation"]["run"]),value["candidate"]),
                "prior failed evaluation changed or screening became exposed")
    for prior in value.get("additional_failed_evaluations",[]):
        require(prior==additional_failure_snapshot(Path(prior["run"]),value["candidate"]),"additional failed evaluation evidence changed")
    if "prior_known_evaluation" in value:
        require(value["prior_known_evaluation"]==prior_known_snapshot(Path(value["prior_known_evaluation"]["run"]),value["candidate"]),
                "prior completed known evidence changed or progressed to load/fresh")
    if "prior_failed_queue_proof" in value:
        require(value["prior_failed_queue_proof"]==prior_queue_failure_snapshot(Path(value["prior_failed_queue_proof"]["run"])),
                "prior failed queue proof changed")
    require(config["reference"]["policy_code_run_name"]==reference.name or "prior_failed_evaluation" in value,
            "current runtime retry dropped its prior failures")
    queue=value["updated_engine"].get("queue_proof")
    require(value["updated_engine"]==motor_binding(proof,root,value["candidate"],Path(queue["run"]) if queue else None),"updated native compatibility changed")
    require(value["candidate_policy"]=="gru" and value["mlp_role"]=="comparison_only" and value["quality"]==config["quality"] and
            value["screening_selection_allowed"] is False and value["retraining_allowed"] is False and
            value["original_neutral_block_results_replaced"] is False,"final evaluation weakened declared selection/task gates")
    require(value.get("runtime_execution_phases")==EXECUTION_PHASES and value.get("launcher_sha256")==sha(root/"code/scripts/body-bc-remote.sh"),
            "runtime output/execution phase alias or frozen launcher changed")
    require(value.get("inference_cpu_threads")==config["runtime"].get("inference_cpu_threads",2),"declared inference CPU thread profile changed")
    require(value.get("collector_static_gc_freeze",False)==config["runtime"].get("collector_static_gc_freeze",False),"declared collector GC profile changed")
    require(value.get("collector_bootstrap_gc_reset",False)==config["runtime"].get("collector_bootstrap_gc_reset",False),"declared bootstrap GC reset changed")
    require(value.get("native_deadline_polling",False)==config["runtime"].get("native_deadline_polling",False),"declared native deadline polling changed")
    require(value.get("native_serial_workers",False)==config["runtime"].get("native_serial_workers",False),"declared native worker profile changed")
    if value.get("collector_bootstrap_gc_reset"):
        require(value.get("collector_bootstrap_interpreter")==BOOTSTRAP_INTERPRETER and
                sha(BOOTSTRAP_INTERPRETER["resolved_executable"])==BOOTSTRAP_INTERPRETER["executable_sha256"],"declared bootstrap interpreter changed")
    return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase",choices=("declare","bindings"))
    parser.add_argument("--run",type=Path,required=True)
    parser.add_argument("--reference-run",type=Path)
    parser.add_argument("--models-run",type=Path)
    parser.add_argument("--proof-run",type=Path)
    parser.add_argument("--queue-proof-run",type=Path)
    parser.add_argument("--prior-queue-failed-run",type=Path)
    parser.add_argument("--child-report",type=Path)
    parser.add_argument("--prior-failed-run",type=Path)
    parser.add_argument("--additional-failed-run",type=Path,action="append",default=[])
    parser.add_argument("--prior-known-run",type=Path)
    args=parser.parse_args()
    try:
        root=args.run.resolve()
        if args.phase=="declare":
            require(all(path is not None for path in (args.reference_run,args.models_run,args.proof_run,args.child_report)),"declare requires reference/models/proof runs and independent child report")
            value=declare(root,args.reference_run.resolve(),args.models_run.resolve(),args.proof_run.resolve(),args.child_report.resolve(),
                          args.prior_failed_run.resolve() if args.prior_failed_run else None,[path.resolve() for path in args.additional_failed_run],
                          args.prior_known_run.resolve() if args.prior_known_run else None,
                          args.queue_proof_run.resolve() if args.queue_proof_run else None,
                          args.prior_queue_failed_run.resolve() if args.prior_queue_failed_run else None)
        else:value=validate_declaration(root)
        print(json.dumps({"declaration":str(root/"final-evaluation.json"),"candidate_checkpoint_sha256":value["candidate"]["checkpoint_sha256"],
                          "fresh_seeds":value["screening_seeds"],"quality_evaluated":False}))
        return 0
    except (OSError,ValueError,KeyError,TypeError) as error:parser.exit(2,"final evaluation refused: "+str(error)+"\n")


if __name__=="__main__":raise SystemExit(main())

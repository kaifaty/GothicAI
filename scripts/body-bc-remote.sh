#!/usr/bin/env bash
# Stage 7: all model forwards, training and arena episodes run on dev-ubuntu.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/ryzen-venv.sh"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
REMOTE="gothic-ai/body"
# Another engine workspace (source, build, provenance under ~/), e.g. a merged-main runner.
ENGINE="${GOTHIC_BODY_ENGINE:-gothic-ai/body-api}"
[[ "$ENGINE" =~ ^gothic-ai/[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Invalid engine workspace' >&2; exit 2; }
# prepare: freeze this code root (scripts, evals, body-service under ~/) instead of the synced source,
# e.g. the gate's frozen code with a new run name.
PREPARE_CODE="${GOTHIC_BODY_PREPARE_CODE:-$REMOTE/source}"
[[ "$PREPARE_CODE" =~ ^gothic-ai/[a-zA-Z0-9/_.-]+$ && "$PREPARE_CODE" != *..* ]] || { echo 'Invalid prepare code root' >&2; exit 2; }
# Phases: the archived native-off episode the setup parity replays (the stage 3 one belongs to the pre-Jolt engine).
NATIVE_REFERENCE="${GOTHIC_BODY_NATIVE_REFERENCE:-gothic-ai/body-headless/runs/stage3-v1/native-off}"
[[ "$NATIVE_REFERENCE" =~ ^gothic-ai/[a-zA-Z0-9/_.-]+$ && "$NATIVE_REFERENCE" != *..* ]] || { echo 'Invalid native reference' >&2; exit 2; }
ACTION="${1:-preflight}"
RUN_NAME="${2:-stage7-bc-$(date -u +%Y%m%dT%H%M%SZ)}"
CAPTURE_RUN="${3:-}"
LOCK_WAIT_SECONDS="${GOTHIC_BODY_LOCK_WAIT_SECONDS:-0}"
[[ "$RUN_NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Invalid run name' >&2; exit 2; }
[[ -z "$CAPTURE_RUN" || "$CAPTURE_RUN" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Invalid capture run name' >&2; exit 2; }
# Other sessions queue on the same Ryzen lock with long blocking waits; a phase may wait up to 10 h.
[[ "$LOCK_WAIT_SECONDS" =~ ^[0-9]{1,5}$ ]] && ((LOCK_WAIT_SECONDS <= 36000)) || { echo 'Lock wait must be an integer from 0 to 36000 seconds' >&2; exit 2; }

freeze_engine_reference() {
  local out="$1" engine_root="$2" lock_held="${3:-false}" snapshot_tmp
  if [[ "$lock_held" != true ]]; then
    exec 8>"$engine_root/build.lock"
    flock -n 8 || { echo 'Build owns the engine snapshot; wait for its owner' >&2; exit 1; }
  fi
  [[ ! -e "$engine_root/build-active" ]] || { echo 'Wait for the private build' >&2; exit 1; }
  # Reuse this run's prepared source and retain the first library snapshot.
  # A later sync/build must not change the inputs while a phase is queued.
  engine_source="$out/reference/source/OpenGothic"
  engine_library="$out/reference/libOpenGothic.a"
  if [[ ! -d "$engine_source" ]]; then
    snapshot_tmp=$(mktemp -d "$out/reference/.source.XXXXXX")
    if ! tar -xzf "$out/source.tar.gz" -C "$snapshot_tmp" OpenGothic; then
      rm -rf "$snapshot_tmp"
      return 1
    fi
    mkdir -p "$out/reference/source"
    mv "$snapshot_tmp/OpenGothic" "$engine_source"
    rmdir "$snapshot_tmp"
  fi
  if [[ ! -f "$engine_library" ]]; then
    snapshot_tmp=$(mktemp "$out/reference/.library.XXXXXX")
    if ! cp "$engine_root/build/libOpenGothic.a" "$snapshot_tmp"; then
      rm -f "$snapshot_tmp"
      return 1
    fi
    mv "$snapshot_tmp" "$engine_library"
  fi
  if [[ "$lock_held" != true ]]; then
    flock -u 8
    exec 8>&-
  fi
}

remote_script() {
  local remote_args="" arg variable
  for arg in "$@"; do printf -v remote_args '%s %q' "$remote_args" "$arg"; done
  {
    for variable in GQ_BIN GQ_OWNER GQ_HOME GQ_LEGACY_LOCK GQ_PROFILE; do
      if [[ -n "${!variable+x}" ]]; then printf 'export %s=%q\n' "$variable" "${!variable}"; fi
    done
    cat "$ROOT/scripts/gq-job.sh"
    declare -f ryzen_task_venv freeze_engine_reference
    cat
  } | ssh -o BatchMode=yes "$HOST" "bash -s --$remote_args"
}

preflight() {
  "$ROOT/scripts/body-trajectories-remote.sh" preflight
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
[[ -x "$HOME/.local/bin/uv" ]] || { echo 'User-local uv is missing' >&2; exit 1; }
[[ -f "$HOME/$1/data/stage6-v1/dataset/dataset.json" ]] || { echo 'Immutable stage 6 corpus is missing' >&2; exit 1; }
systemctl --user show gothic-voxtral gothic-voxcpm gothic-decider gothic-gemma \
  -p Id -p ActiveState -p SubState -p MainPID
echo 'Stage 7 uses a separate venv and CPU-only model execution by default.'
REMOTE_SCRIPT
}

sync_sources() {
  # Code-only synchronization leaves the private engine and its provenance
  # untouched; use it when the native source/binary have already been built.
  [[ "$ACTION" == sync-code ]] || "$ROOT/scripts/body-trajectories-remote.sh" sync
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
mkdir -p "$HOME/$1/source/body-service" "$HOME/$1/source/scripts/tests" "$HOME/$1/source/scripts/bench"
REMOTE_SCRIPT
  rsync -a --delete --exclude=.venv/ --exclude=__pycache__/ --exclude=.pytest_cache/ \
    "$ROOT/body-service/" "$HOST:$REMOTE/source/body-service/"
  rsync -a "$ROOT/scripts/body-bc-remote.sh" "$ROOT/scripts/gq-job.sh" "$ROOT/scripts/ryzen-venv.sh" "$ROOT/scripts/body_demonstration_suite.py" \
    "$ROOT/scripts/body_demonstration_teacher.py" "$ROOT/scripts/body_demonstration_recover.py" \
    "$ROOT/scripts/body_bc_suite.py" "$ROOT/scripts/body_setup_probe.py" \
    "$ROOT/scripts/body_policy_suite.py" "$ROOT/scripts/body_bc_report.py" \
    "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/body_trajectory_dataset.py" "$ROOT/scripts/body_trajectory_recorder.py" "$ROOT/scripts/body_observe_suite.py" \
    "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/bench/body-api.bench" "$HOST:$REMOTE/source/scripts/bench/"
  local stage8_file
  for stage8_file in body_policy_diagnostics.py body_policy_load.py body_combat_teacher.py body_combat_demonstrations.py body_combat_report.py body_combat_suite.py body_service_load.py body_realtime_probe.py body_visibility_compare.py body_visibility_boundary_probe.py body_compact_reply_probe.py body_pending_input_probe.py body_block_reaction.py body_block_reaction_report.py body_evaluation_repair.py body_combat_evaluation.py body_defence_teacher.py body_defence_demonstrations.py body_defence_suite.py body_training_gpu_watch.py body_defence_report.py body_final_evaluation.py body_stage8_final_report.py body_stage8_known_report.py body_collector_gc.py body_safe_peer_teacher.py body_safe_peer_demonstrations.py body_safe_peer_suite.py body_safe_peer_report.py; do
    [[ ! -f "$ROOT/scripts/$stage8_file" ]] || rsync -a "$ROOT/scripts/$stage8_file" "$HOST:$REMOTE/source/scripts/"
  done
  rsync -a "$ROOT/evals/npc-body/body-bc-v1.json" "$ROOT/evals/npc-body/body-demonstrations-v1.json" \
    "$HOST:$REMOTE/source/evals/npc-body/"
  for stage8_file in body-stage8-v1.json body-combat-demonstrations-v1.json body-block-reaction-v1.json body-stage8-evaluation-v2.json body-defence-demonstrations-v1.json body-defence-bc-v1.json body-safe-peer-demonstrations-v1.json body-safe-peer-bc-v1.json; do
    [[ ! -f "$ROOT/evals/npc-body/$stage8_file" ]] || rsync -a "$ROOT/evals/npc-body/$stage8_file" "$HOST:$REMOTE/source/evals/npc-body/"
  done
  rsync -a --exclude=__pycache__/ "$ROOT/scripts/tests/" "$HOST:$REMOTE/source/scripts/tests/"
}

install_env() {
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
[[ -d "$task_root/.venv" ]] || "$HOME/.local/bin/uv" venv --python 3.12 "$task_root/.venv"
# A private environment; shared cached wheels may be hard-linked without changing
# any service environment. CUDA stays disabled in all default stage-7 commands.
export PATH="$HOME/.local/bin:$PATH"
UV_OFFLINE=1 ryzen_task_venv "$task_root/.venv"
"$HOME/.local/bin/uv" pip freeze --python "$task_root/.venv/bin/python" > "$task_root/provenance/body-environment.txt"
REMOTE_SCRIPT
}

prepare_run() {
  remote_script "$REMOTE" "$ENGINE" "$RUN_NAME" "$PREPARE_CODE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
engine_root="$HOME/$2"
out="$task_root/data/$3"
code_root="$HOME/$4"
# The default engine's provenance is mirrored into the task root; another engine keeps its own.
provenance="$task_root/provenance"
[[ "$2" == gothic-ai/body-api ]] || provenance="$engine_root/provenance"
[[ -d "$code_root/scripts" && -d "$code_root/evals" && -d "$code_root/body-service" ]] || { echo 'Prepare code root is incomplete' >&2; exit 1; }
[[ ! -e "$out" ]] || { echo 'Run exists; choose a fresh name' >&2; exit 1; }
[[ -x "$engine_root/build/opengothic/gothic-body-runner" ]] || { echo 'Build runner first' >&2; exit 1; }
if gq_available; then
  exec 8>"$engine_root/build.lock"
  flock -n 8 || { echo 'Build owns the engine snapshot; wait for its owner' >&2; exit 1; }
fi
[[ ! -e "$engine_root/build-active" ]] || { echo 'Wait for the private build' >&2; exit 1; }
mkdir -p "$out/code" "$out/reference" "$out/models" "$out/validation"
rsync -a --exclude=__pycache__/ --exclude=.pytest_cache/ \
  "$code_root/scripts" "$code_root/evals" "$code_root/body-service" "$out/code/"
cp "$engine_root/build/opengothic/gothic-body-runner" \
   "$engine_root/build/opengothic/gothic-body-resolver-test" "$out/reference/"
cp "$provenance/"{source-head.txt,source.patch,configure.log,build.log,resolver-tests.json} "$task_root/provenance/body-environment.txt" "$out/"
nice -n 10 tar --exclude=__pycache__ --exclude=.pytest_cache -czf "$out/source.tar.gz" \
  -C "$engine_root/source" OpenGothic -C "$out/code" scripts evals body-service
if gq_available; then
  freeze_engine_reference "$out" "$engine_root" true
  flock -u 8
  exec 8>&-
fi
CUDA_VISIBLE_DEVICES='' "$task_root/.venv/bin/python" - "$out" <<'PY'
import datetime,hashlib,json,os,subprocess,sys
from importlib.metadata import version
from pathlib import Path
out=Path(sys.argv[1])
def read(args):return subprocess.check_output(args,text=True).strip()
config=json.loads((out/'code/evals/npc-body/body-bc-v1.json').read_text())
services={name:read(['systemctl','--user','show',name,'-p','ActiveState','-p','SubState','-p','MainPID']) for name in config['resources']['required_services']}
preflight={'schema_version':1,'host':os.uname().nodename,'recorded_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'device_requested':'cpu','cuda_visible_devices':os.environ.get('CUDA_VISIBLE_DEVICES'),'gpu':subprocess.run(['nvidia-smi','--query-gpu=memory.used,memory.free,utilization.gpu','--format=csv,noheader'],capture_output=True,text=True).stdout.strip() or 'unavailable','ram':read(['free','-b']),'disk':read(['df','-B1',str(out)]),'services':services,'model_forward_performed':False}
(out/'preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
environment={'python':sys.version,'packages':{name:version(name) for name in ['torch','numpy','pytest']},'frozen_files':{str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((out/'code').rglob('*')) if p.is_file() and not {'__pycache__','.pytest_cache'}.intersection(p.parts)},'binary_sha256':hashlib.sha256((out/'reference/gothic-body-runner').read_bytes()).hexdigest(),'source_archive_sha256':hashlib.sha256((out/'source.tar.gz').read_bytes()).hexdigest()}
(out/'environment.json').write_text(json.dumps(environment,indent=2)+'\n')
PY
echo "Frozen stage 7 run: $out"
REMOTE_SCRIPT
}

prepare_audit() {
  # A repaired read-only reporter has its own frozen source, rather than
  # replacing any executable or failed report from the original experiment.
  remote_script "$REMOTE" "$RUN_NAME" <<'REMOTE_SCRIPT'
set -euo pipefail
audit="$HOME/$1/data/$2/validation/audit-code"
[[ -f "$HOME/$1/data/$2/environment.json" && ! -e "$audit" ]] || {
  echo 'Audit requires an existing frozen run and a new audit snapshot' >&2; exit 1;
}
mkdir -p "$audit/scripts/tests"
REMOTE_SCRIPT
  rsync -a "$ROOT/scripts/body_bc_report.py" "$HOST:$REMOTE/data/$RUN_NAME/validation/audit-code/scripts/"
  rsync -a "$ROOT/scripts/body-bc-remote.sh" "$ROOT/scripts/gq-job.sh" "$ROOT/scripts/ryzen-venv.sh" "$HOST:$REMOTE/data/$RUN_NAME/validation/audit-code/scripts/"
  rsync -a "$ROOT/scripts/tests/test_body_bc_report.py" "$HOST:$REMOTE/data/$RUN_NAME/validation/audit-code/scripts/tests/"
  remote_script "$REMOTE" "$RUN_NAME" <<'REMOTE_SCRIPT'
set -euo pipefail
"$HOME/$1/.venv/bin/python" - "$HOME/$1/data/$2" <<'PY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]);audit=root/'validation/audit-code'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
value={'kind':'independent_reporter_repair_snapshot','frozen_run_environment_sha256':sha(root/'environment.json'),
       'original_report_sha256':sha(root/'validation/body-bc.json'),
       'config_sha256':sha(root/'code/evals/npc-body/body-bc-v1.json'),
       'dataset_helper_sha256':sha(root/'code/scripts/body_trajectory_dataset.py'),
       'files':{str(p.relative_to(audit)):sha(p) for p in sorted(audit.rglob('*')) if p.is_file()}}
(audit/'audit.json').write_text(json.dumps(value,indent=2)+'\n')
PY
REMOTE_SCRIPT
}

prepare_faults_retry() {
  # Supply the omitted fixture in a separate declared retry, leaving the
  # original frozen source, failed log and partial outputs intact.
  remote_script "$REMOTE" "$RUN_NAME" <<'REMOTE_SCRIPT'
set -euo pipefail
out="$HOME/$1/data/$2"
retry="$out/faults-retry-v1"
[[ -f "$out/environment.json" && -f "$out/launcher-stage8-faults.log" && ! -e "$retry" ]] || {
  echo 'Retry requires a frozen failed fault run and a fresh retry directory' >&2; exit 1;
}
mkdir -p "$retry/code/scripts/bench"
REMOTE_SCRIPT
  rsync -a "$ROOT/scripts/bench/body-api.bench" "$HOST:$REMOTE/data/$RUN_NAME/faults-retry-v1/code/scripts/bench/"
  rsync -a "$ROOT/scripts/body-bc-remote.sh" "$ROOT/scripts/gq-job.sh" "$ROOT/scripts/ryzen-venv.sh" "$HOST:$REMOTE/data/$RUN_NAME/faults-retry-v1/code/scripts/"
  remote_script "$REMOTE" "$ENGINE" "$RUN_NAME" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
engine_root="$HOME/$2"
out="$task_root/data/$3"
engine_source="$engine_root/source/OpenGothic"
engine_library="$engine_root/build/libOpenGothic.a"
if gq_available; then freeze_engine_reference "$out" "$engine_root"; fi
CUDA_VISIBLE_DEVICES='' "$task_root/.venv/bin/python" - "$out" "$engine_source" "$engine_library" "$task_root/.venv/bin/python" <<'PY'
import datetime,hashlib,json,sys
from pathlib import Path
out,engine_source,engine_library,python=Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4]
retry=out/'faults-retry-v1';code=out/'code';home=Path.home()
def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()
fixture=retry/'code/scripts/bench/body-api.bench'
assert sha(fixture)=='9ed2f6bba58f474a7d7ab1daab8e72fcc48bf11a64f27c1ff15ad46e0f3f6456','Retry fixture differs from declared canonical bench'
environment=json.loads((out/'environment.json').read_text())
for name,expected in environment['frozen_files'].items():assert sha(out/name)==expected,'Original frozen code changed: '+name
assert sha(out/'reference/gothic-body-runner')==environment['binary_sha256'],'Original runner changed'
assert sha(out/'source.tar.gz')==environment['source_archive_sha256'],'Original archive changed'
assert not (code/'scripts/bench/body-api.bench').exists(),'This retry is only for the missing bench failure'
originals=['environment.json','source.tar.gz','reference/gothic-body-runner','code/scripts/body_api_suite.py',
           'code/evals/npc-body/body-api-v1.json','code/evals/npc-body/body-bc-v1.json','launcher-stage8-faults.log']
originals += [name for name in ['resources-stage8-faults.txt','services-stage8-faults-before.json','services-stage8-faults-after.json'] if (out/name).is_file()]
originals += [str(path.relative_to(out)) for path in sorted((out/'faults').rglob('*')) if path.is_file()]
argv=[python,str(code/'scripts/body_api_suite.py'),'--runner',str(out/'reference/gothic-body-runner'),
      '--assets',str(home/'gothic-ai/body-baseline/assets'),'--save',str(home/'gothic-ai/body-baseline/runs/melee-reference-stage1-v4/run/save_slot_1.sav'),
      '--reference',str(home/'gothic-ai/body-baseline/runs/melee-reference-stage1-v4'),'--source',str(engine_source),
      '--engine-library',str(engine_library),'--scenario',str(code/'scripts/bench/body-arena.bench'),
      '--melee-scenario',str(code/'scripts/bench/body-melee.bench'),'--native-reference',str(home/'gothic-ai/body-headless/runs/stage3-v1/native-off'),
      '--config',str(code/'evals/npc-body/body-api-v1.json'),'--scenario',str(fixture),'--output',str(retry/'run')]
declaration={'schema_version':1,'kind':'body_stage8_missing_bench_retry','recorded_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'original_run':str(out),'phase':'stage8-faults-retry','allowed_changes':['scenario_fixture_path','output_path'],
    'original_files':{name:sha(out/name) for name in originals},
    'retry_files':{str(path.relative_to(retry)):sha(path) for path in sorted((retry/'code').rglob('*')) if path.is_file()},
    'engine_library':{'path':str(engine_library),'sha256':sha(engine_library)},
    'argv':argv,'resources':{'cpu_quota_percent':200,'memory_max':'6G','runtime_max_seconds':2400,'nice':10,'tasks_max':128,
        'working_directory':str(out),'simulation_lock':str(home/'gothic-ai/body-simulation.lock')},
    'execution_environment':{'PYTHONPATH':str(code/'body-service/src')+':'+str(code/'scripts'),
        'CUDA_VISIBLE_DEVICES':'','OMP_NUM_THREADS':'2','MKL_NUM_THREADS':'2','OPENBLAS_NUM_THREADS':'2',
        'DISPLAY':'','WAYLAND_DISPLAY':'','GOTHIC_AI':'0','VK_DRIVER_FILES':'/nonexistent/body-bc-no-vulkan.json',
        'VK_ICD_FILENAMES':'/nonexistent/body-bc-no-vulkan.json','OPENGOTHIC_TEXTURES':'','OPENGOTHIC_BODY_TRACE':'1','OPENGOTHIC_BODY_ANIMATION_RNG':'isolated'},
    'original_frozen_files_changed':False,'execution_started':False}
with (retry/'declaration.json').open('x') as stream:stream.write(json.dumps(declaration,indent=2)+'\n')
print('Frozen missing-bench retry: '+str(retry))
PY
REMOTE_SCRIPT
}

run_faults_retry() {
  remote_script "$REMOTE" "$RUN_NAME" <<'REMOTE_SCRIPT'
set -euo pipefail
retry="$HOME/$1/data/$2/faults-retry-v1"
[[ -f "$retry/declaration.json" ]] || { echo 'Prepare the independent fault retry first' >&2; exit 1; }
exec bash "$retry/code/scripts/body-bc-remote.sh" _faults-retry-local "$2"
REMOTE_SCRIPT
}

prepare_model_reference() {
  # Reuse selected weights for a new runtime experiment without performing
  # training or attributing the original checkpoint selection to this run.
  remote_script "$REMOTE" "$RUN_NAME" "${CAPTURE_RUN:-stage8-combat-v6}" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
out="$task_root/data/$2"
reference="$task_root/data/$3"
[[ "$out" != "$reference" && -f "$out/environment.json" && -f "$reference/selection.json" && ! -e "$out/model-reference" ]] || {
  echo 'Model reference requires a fresh frozen run and an independent selected source' >&2; exit 1;
}
CUDA_VISIBLE_DEVICES='' "$task_root/.venv/bin/python" - "$out" "$reference" <<'PY'
import datetime,hashlib,json,shutil,sys
from pathlib import Path
out,reference=map(Path,sys.argv[1:]);destination=out/'model-reference'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
selection=json.loads((reference/'selection.json').read_text())
originals=['environment.json','selection.json','dataset/normalization.json']
for architecture in ('gru','mlp'):
    checkpoint=reference/'models'/architecture/'best.pt'
    assert sha(checkpoint)==selection['models'][architecture]['selected']['checkpoint_sha256'],'Selected reference weights changed'
    originals.extend(['models/'+architecture+'/best.pt','models/'+architecture+'/best.pt.json'])
    assert not (out/'models'/architecture).exists(),'Destination already has model artifacts'
source_hashes={name:sha(reference/name) for name in originals}
destination.mkdir()
for name in originals:
    target=(out/name) if name.startswith('models/') else (destination/name)
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(reference/name,target)
    assert sha(target)==source_hashes[name],'Copied reference bytes differ'
runtime_normalizer=out/'dataset/normalization.json'
assert not runtime_normalizer.exists(),'Destination runtime normalizer already exists'
runtime_normalizer.parent.mkdir(parents=True,exist_ok=True)
shutil.copyfile(reference/'dataset/normalization.json',runtime_normalizer)
assert sha(runtime_normalizer)==source_hashes['dataset/normalization.json'],'Copied runtime normalizer differs'
declaration={'schema_version':1,'kind':'body_frozen_selected_model_reference',
    'recorded_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'reference_run':str(reference),
    'source_files':source_hashes,'new_run_environment_sha256':sha(out/'environment.json'),
    'policy_code_root':str(out/'code'),'reference_policy_code_root':str(reference/'code'),
    'checkpoint_paths':{name:str(out/'models'/name/'best.pt') for name in ('gru','mlp')},
    'normalizer_sha256':source_hashes['dataset/normalization.json'],
    'runtime_normalizer':{'path':'dataset/normalization.json','sha256':sha(runtime_normalizer)},
    'training_performed':False,'checkpoint_selection_performed':False,'original_run_modified':False}
(destination/'declaration.json').write_text(json.dumps(declaration,indent=2)+'\n')
PY
REMOTE_SCRIPT
}

run_phase() {
  remote_script "$REMOTE" "$ENGINE" "$RUN_NAME" "$1" "$CAPTURE_RUN" "$LOCK_WAIT_SECONDS" "$NATIVE_REFERENCE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
engine_root="$HOME/$2"
out="$task_root/data/$3"
phase="$4"
capture_run="$5"
lock_wait_seconds="$6"
native_reference="$HOME/${7:-gothic-ai/body-headless/runs/stage3-v1/native-off}"
[[ "$lock_wait_seconds" =~ ^[0-9]{1,5}$ ]] && ((lock_wait_seconds <= 36000)) || { echo 'Invalid remote lock wait' >&2; exit 2; }
code="$out/code"
[[ -f "$out/environment.json" && -x "$out/reference/gothic-body-runner" ]] || {
  echo 'Prepare a frozen run first' >&2; exit 1;
}
simulation_phase=false
case "$phase" in
  setup|tests|audit-tests|combat-tests|stage8-tests|stage8-faults|stage8-faults-retry|runtime-cpu-profile-tests|runtime-delivery-tests|load-delivery-tests|service-cadence-tests|visibility-boundary-tests|visibility-boundary-probe|compact-reply-tests|compact-reply-probe|pending-input-tests|pending-input-probe|block-reaction-tests|block-reaction|realtime-reference|realtime-reaction|screening-reaction|final-evaluation-tests|safe-peer-teacher-tests|safe-peer-tests|safe-peer-calibration|safe-peer-smoke|safe-peer-demos|defence-tests|defence-smoke|defence-demos|realtime-probe|probe-tests|probe|shadow|realtime|screening|load|load-reference|combat-geometry-preflight|combat-calibration|combat-smoke|combat-demos|smoke|demos|arena|arena-smoke) simulation_phase=true ;;
esac
use_gq=false
if gq_available; then
  use_gq=true
  # Reserve this run/phase before touching its evidence or submitting a job.
  # The ID does not exist while submit is in flight. Keep this local lock
  # through wait and cleanup; other runs/phases still use the queue normally.
  # Aliases of the same suite command share both reservation and saved ID.
  queue_phase="$phase"
  case "$queue_phase" in
    train-cuda) queue_phase=defence-train-cuda ;;
  esac
  gq_job_id_file="$out/gq-$queue_phase.id"
  exec 7>"$out/gq-$queue_phase.lock"
  flock -n 7 || { echo "Another launcher owns this run/phase: $queue_phase" >&2; exit 1; }
  [[ ! -e "$gq_job_id_file" ]] || { echo 'Queue id evidence exists; preserve it and use a fresh run' >&2; exit 1; }
  echo 'gq: lock-wait is accepted for compatibility; batch queue waiting has no timeout.'
else
  lock_path="$HOME/gothic-ai/body-training.lock"
  [[ "$simulation_phase" != true ]] || lock_path="$HOME/gothic-ai/body-simulation.lock"
  exec 9>"$lock_path"
  flock -w "$lock_wait_seconds" 9 || { echo 'Another phase owns the required body lock' >&2; exit 1; }
  if [[ "$phase" == defence-train-cuda || "$phase" == safe-peer-train-cuda || "$phase" == train-cuda ]]; then
    exec 8>"$HOME/gothic-ai/body-simulation.lock"
    flock -w "$lock_wait_seconds" 8 || { echo 'CUDA training waits for the simulation owner' >&2; exit 1; }
  fi
fi
[[ ! -e "$engine_root/build-active" ]] || { echo 'Wait for the private build' >&2; exit 1; }
if [[ "$use_gq" != true && "$simulation_phase" == true ]] && pgrep -x 'gothic-body-run' >/dev/null; then
  echo 'Another runner is active; wait for its owner' >&2; exit 1
fi
python="$task_root/.venv/bin/python"
engine_source="$engine_root/source/OpenGothic"
engine_library="$engine_root/build/libOpenGothic.a"
if [[ "$use_gq" == true ]]; then freeze_engine_reference "$out" "$engine_root"; fi
common=(--runner "$out/reference/gothic-body-runner" --assets "$HOME/gothic-ai/body-baseline/assets"
 --save "$HOME/gothic-ai/body-baseline/runs/melee-reference-stage1-v4/run/save_slot_1.sav"
 --reference "$HOME/gothic-ai/body-baseline/runs/melee-reference-stage1-v4"
 --source "$engine_source" --engine-library "$engine_library"
 --scenario "$code/scripts/bench/body-arena.bench" --melee-scenario "$code/scripts/bench/body-melee.bench"
 --native-reference "$native_reference")
budget=2400
case "$phase" in
  runtime-cpu-profile-tests)
    budget=180
    argv=("$python" -m pytest "$code/body-service/tests/test_runtime_cpu_profile.py"
      "$code/scripts/tests/test_body_runtime_delivery.py" "$code/scripts/tests/test_body_policy_diagnostics.py"
      "$code/scripts/tests/test_body_load_delivery.py" "$code/scripts/tests/test_body_policy_load.py"
      "$code/scripts/tests/test_body_collector_gc.py"
      -q --junitxml="$out/runtime-cpu-profile-tests.xml") ;;
  pending-input-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_pending_input_probe.py"
      -q --junitxml="$out/pending-input-tests.xml") ;;
  pending-input-probe)
    [[ ! -e "$out/pending-input-probe" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Pending input evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_pending_input_probe.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json" --output "$out/pending-input-probe") ;;
  load-delivery-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_policy_load.py" "$code/scripts/tests/test_body_load_delivery.py"
      -q --junitxml="$out/load-delivery-tests.xml") ;;
  compact-reply-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_compact_reply_probe.py"
      -q --junitxml="$out/compact-reply-tests.xml") ;;
  compact-reply-probe)
    [[ ! -e "$out/compact-reply-probe" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Compact reply evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_compact_reply_probe.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json" --output "$out/compact-reply-probe") ;;
  runtime-delivery-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_runtime_delivery.py" "$code/scripts/tests/test_body_policy_diagnostics.py"
      "$code/scripts/tests/test_body_policy_load.py" "$code/scripts/tests/test_body_service_load.py"
      -q --junitxml="$out/runtime-delivery-tests.xml") ;;
  service-cadence-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_service_load.py"
      -q --junitxml="$out/service-cadence-tests.xml") ;;
  visibility-boundary-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_visibility_boundary_probe.py"
      -q --junitxml="$out/visibility-boundary-tests.xml") ;;
  visibility-boundary-probe)
    [[ ! -e "$out/visibility-boundary-probe" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Visibility boundary evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_visibility_boundary_probe.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json"
      --reference-episode "$task_root/data/stage8-final-gate-v1/load/four-actor-gru-0" --output "$out/visibility-boundary-probe") ;;
  final-evaluation-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_final_evaluation.py" "$code/scripts/tests/test_body_stage8_final_report.py"
      "$code/scripts/tests/test_body_block_reaction_report.py"
      -q --junitxml="$out/final-evaluation-tests.xml") ;;
  block-reaction-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_block_reaction.py"
      -q --junitxml="$out/block-reaction-tests.xml") ;;
  block-reaction)
    [[ ! -e "$out/block-reaction" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Reaction evidence already exists; preserve it' >&2; exit 1; }
    reaction_reference="$task_root/data/stage8-combat-v6"
    if [[ -f "$out/evaluation-repair.json" ]]; then
      "$python" "$code/scripts/body_evaluation_repair.py" "$out" --reference-run "$reaction_reference" --audit
    else
      "$python" "$code/scripts/body_evaluation_repair.py" "$out" --reference-run "$reaction_reference"
    fi
    argv=("$python" "$code/scripts/body_block_reaction.py" "${common[@]}"
      --reference-run "$reaction_reference" --policy-code-root "$reaction_reference/code"
      --gru-checkpoint "$reaction_reference/models/gru/best.pt" --mlp-checkpoint "$reaction_reference/models/mlp/best.pt"
      --config "$code/evals/npc-body/body-block-reaction-v1.json" --output "$out/block-reaction") ;;
  realtime-reference)
    [[ ! -e "$out/realtime-reference" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Reference realtime evidence already exists; preserve it' >&2; exit 1; }
    realtime_reference="$task_root/data/stage8-combat-v6"
    argv=("$python" "$realtime_reference/code/scripts/body_policy_diagnostics.py" "${common[@]}"
      --config "$realtime_reference/code/evals/npc-body/body-stage8-v1.json" --phase realtime
      --policy-code-root "$realtime_reference/code"
      --gru-checkpoint "$realtime_reference/models/gru/best.pt" --mlp-checkpoint "$realtime_reference/models/mlp/best.pt"
      --device cpu --output "$out/realtime-reference") ;;
  realtime-reaction|screening-reaction)
    evaluation_phase="$phase"
    [[ "$phase" != screening-reaction ]] || evaluation_phase=screening
    [[ ! -e "$out/$evaluation_phase" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Repaired evaluation evidence already exists; preserve it' >&2; exit 1; }
    mapfile -t evaluation_references < <("$python" - "$code/evals/npc-body/body-stage8-evaluation-v2.json" <<'PY'
import json,re,sys
config=json.load(open(sys.argv[1]));reference=config['reference']
for name in (reference['run_name'],reference.get('policy_code_run_name','stage8-combat-v6')):
    assert re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_-]*',name),'Invalid declared evaluation reference run'
    print(name)
PY
    )
    [[ "${#evaluation_references[@]}" -eq 2 ]] || { echo 'Invalid repaired evaluation references' >&2; exit 1; }
    evaluation_weights="$task_root/data/${evaluation_references[0]}"
    evaluation_policy_code="$task_root/data/${evaluation_references[1]}/code"
    argv=("$python" "$code/scripts/body_combat_evaluation.py" "${common[@]}" --phase "$evaluation_phase"
      --reference-run "$evaluation_weights" --policy-code-root "$evaluation_policy_code"
      --gru-checkpoint "$evaluation_weights/models/gru/best.pt" --mlp-checkpoint "$evaluation_weights/models/mlp/best.pt"
      --config "$code/evals/npc-body/body-stage8-evaluation-v2.json" --output "$out/$evaluation_phase") ;;
  safe-peer-teacher-tests)
    budget=240
    argv=("$python" -m pytest "$code/scripts/tests/test_body_safe_peer_teacher.py"
      "$code/scripts/tests/test_body_trajectory_dataset.py" -q --junitxml="$out/safe-peer-teacher-tests.xml") ;;
  safe-peer-tests)
    budget=240
    argv=("$python" -m pytest "$code/scripts/tests/test_body_safe_peer_teacher.py" "$code/scripts/tests/test_body_safe_peer_suite.py"
      "$code/scripts/tests/test_body_trajectory_dataset.py" -q --junitxml="$out/safe-peer-tests.xml") ;;
  safe-peer-calibration|safe-peer-smoke|safe-peer-demos)
    mode=()
    destination="$out/safe-peer-demonstrations"
    [[ "$phase" != safe-peer-smoke ]] || { mode=(--smoke); destination="$out/safe-peer-demonstrations-smoke"; }
    [[ "$phase" != safe-peer-calibration ]] || { mode=(--calibration); destination="$out/safe-peer-calibration"; }
    [[ ! -e "$destination" ]] || { echo 'Safe peer evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_safe_peer_demonstrations.py" "${common[@]}"
      --config "$code/evals/npc-body/body-safe-peer-demonstrations-v1.json"
      --existing-dataset "$task_root/data/stage8-defence-bc-v2/dataset/dataset.json"
      --output "$destination")
    argv+=("${mode[@]}") ;;
  safe-peer-dataset|safe-peer-verify|safe-peer-overfit|safe-peer-train|safe-peer-train-cuda|safe-peer-evaluate)
    safe_peer_phase="${phase#safe-peer-}"
    [[ "$safe_peer_phase" != train && "$safe_peer_phase" != train-cuda ]] || budget=14400
    argv=("$python" "$code/scripts/body_safe_peer_suite.py" "$safe_peer_phase" --run "$out") ;;
  defence-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_trajectory_dataset.py" "$code/scripts/tests/test_body_defence_teacher.py"
      -q --junitxml="$out/defence-tests.xml") ;;
  defence-smoke|defence-demos)
    mode=()
    destination="$out/defence-demonstrations"
    [[ "$phase" != defence-smoke ]] || { mode=(--smoke); destination="$out/defence-demonstrations-smoke"; }
    [[ ! -e "$destination" ]] || { echo 'Defence demonstration evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_defence_demonstrations.py" "${common[@]}"
      --config "$code/evals/npc-body/body-defence-demonstrations-v1.json"
      --reaction-helper "$task_root/data/stage8-reaction-v3/code/scripts/body_block_reaction.py"
      --reaction-config "$task_root/data/stage8-reaction-v3/code/evals/npc-body/body-block-reaction-v1.json"
      --normalizer "$task_root/data/stage8-combat-v6/dataset/normalization.json" --output "$destination" "${mode[@]}") ;;
  defence-dataset|defence-verify|defence-overfit|defence-train|defence-train-cuda|defence-evaluate|train-cuda)
    defence_phase="${phase#defence-}"
    [[ "$defence_phase" != train && "$defence_phase" != train-cuda ]] || budget=14400
    argv=("$python" "$code/scripts/body_defence_suite.py" "$defence_phase" --run "$out") ;;
  probe-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_policy_diagnostics.py"
      -q --junitxml="$out/probe-tests.xml") ;;
  combat-tests)
    budget=180
    argv=("$python" -m pytest "$code/scripts/tests/test_body_trajectory_dataset.py"
      "$code/scripts/tests/test_body_combat_teacher.py" -q --junitxml="$out/combat-tests.xml") ;;
  stage8-tests)
    budget=180
    argv=("$python" -m pytest "$code/body-service/tests/test_continuous_evaluation.py"
      "$code/scripts/tests/test_body_trajectory_dataset.py" "$code/scripts/tests/test_body_combat_teacher.py"
      "$code/scripts/tests/test_body_policy_diagnostics.py" "$code/scripts/tests/test_body_policy_load.py"
      "$code/scripts/tests/test_body_service_load.py" "$code/scripts/tests/test_body_combat_report.py"
      -q --junitxml="$out/stage8-tests.xml") ;;
  stage8-faults)
    [[ ! -e "$out/faults" ]] || { echo 'Native fault evidence already exists; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_api_suite.py" "${common[@]}"
      --config "$code/evals/npc-body/body-api-v1.json" --scenario "$code/scripts/bench/body-api.bench"
      --output "$out/faults") ;;
  stage8-faults-retry)
    retry="$out/faults-retry-v1"
    [[ ! -e "$retry/run" && ! -e "$out/resources-$phase.txt" ]] || { echo 'Retry evidence already exists; preserve it' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_api_suite.py" "${common[@]}"
      --config "$code/evals/npc-body/body-api-v1.json" --scenario "$retry/code/scripts/bench/body-api.bench"
      --output "$retry/run")
    "$python" - "$out" "$retry" "${argv[@]}" <<'PY'
import hashlib,json,sys
from pathlib import Path
out,retry=Path(sys.argv[1]),Path(sys.argv[2]);declaration=json.loads((retry/'declaration.json').read_text())
def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()
assert declaration['kind']=='body_stage8_missing_bench_retry' and declaration['phase']=='stage8-faults-retry'
assert declaration['argv']==sys.argv[3:],'Retry command differs from frozen declaration'
for name,expected in declaration['original_files'].items():assert sha(out/name)==expected,'Original failed run changed: '+name
for name,expected in declaration['retry_files'].items():assert sha(retry/name)==expected,'Retry fixture/launcher changed: '+name
environment=json.loads((out/'environment.json').read_text())
for name,expected in environment['frozen_files'].items():assert sha(out/name)==expected,'Original frozen code changed: '+name
assert sha(Path(declaration['engine_library']['path']))==declaration['engine_library']['sha256'],'Private engine library changed before retry'
PY
    ;;
  realtime-probe)
    [[ ! -e "$out/realtime-probe" ]] || { echo 'Realtime probe already exists; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_realtime_probe.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json" --output "$out/realtime-probe") ;;
  probe|shadow|realtime|screening)
    [[ ! -e "$out/$phase" ]] || { echo 'Stage 8 phase already exists; use a fresh run' >&2; exit 1; }
    stage8_models="$out"
    stage8_policy_code="$code"
    if [[ "$phase" == probe ]]; then
      stage8_models="$task_root/data/stage7-bc-v9"
      stage8_policy_code="$stage8_models/code"
    fi
    argv=("$python" "$code/scripts/body_policy_diagnostics.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json" --phase "$phase"
      --policy-code-root "$stage8_policy_code"
      --gru-checkpoint "$stage8_models/models/gru/best.pt" --mlp-checkpoint "$stage8_models/models/mlp/best.pt"
      --device cpu --output "$out/$phase") ;;
  load|load-reference)
    [[ ! -e "$out/$phase" ]] || { echo 'Stage 8 load already exists; use a fresh run' >&2; exit 1; }
    stage8_models="$out"
    [[ "$phase" != load-reference ]] || stage8_models="$task_root/data/stage8-combat-v6"
    argv=("$python" "$code/scripts/body_policy_load.py" "${common[@]}"
      --config "$code/evals/npc-body/body-stage8-v1.json"
      --gru-checkpoint "$stage8_models/models/gru/best.pt" --mlp-checkpoint "$stage8_models/models/mlp/best.pt"
      --device cpu --output "$out/$phase") ;;
  combat-geometry-preflight|combat-calibration|combat-smoke|combat-demos)
    budget=2400
    mode=()
    destination="$out/combat-demonstrations"
    [[ "$phase" != combat-smoke ]] || { mode=(--smoke); destination="$out/combat-demonstrations-smoke"; }
    [[ "$phase" != combat-calibration ]] || { mode=(--calibration); destination="$out/combat-calibration"; }
    [[ "$phase" != combat-geometry-preflight ]] || { mode=(--geometry-preflight); destination="$out/combat-geometry-preflight"; }
    [[ ! -e "$destination" ]] || { echo 'Combat demonstrations already exist; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_combat_demonstrations.py" "${common[@]}"
      --config "$code/evals/npc-body/body-combat-demonstrations-v1.json"
      --existing-dataset "$task_root/data/stage7-bc-v9/dataset/dataset.json" --output "$destination" "${mode[@]}") ;;
  stage8-dataset|stage8-verify|stage8-overfit|stage8-train|stage8-evaluate)
    [[ "$phase" != stage8-train ]] || budget=14400
    argv=("$python" "$code/scripts/body_combat_suite.py" "${phase#stage8-}" --run "$out") ;;
  stage8-report|probe-report)
    report_phase=full
    [[ "$phase" != probe-report ]] || report_phase=probe
    argv=("$python" "$code/scripts/body_combat_report.py" "$out" --phase "$report_phase"
      --config "$code/evals/npc-body/body-stage8-v1.json" --output-dir "$out/validation/$report_phase") ;;
  audit-tests|audit-report)
    [[ -f "$out/validation/audit-code/audit.json" ]] || { echo 'Freeze the independent audit first' >&2; exit 1; }
    "$python" - "$out" <<'PY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]);audit=root/'validation/audit-code';m=json.loads((audit/'audit.json').read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for name,expected in m['files'].items():
    assert sha(audit/name)==expected,'Independent audit source changed'
for key,p in [('frozen_run_environment_sha256',root/'environment.json'),('original_report_sha256',root/'validation/body-bc.json'),
              ('config_sha256',root/'code/evals/npc-body/body-bc-v1.json'),('dataset_helper_sha256',root/'code/scripts/body_trajectory_dataset.py')]:
    assert sha(p)==m[key],'Original experiment evidence changed'
PY
    if [[ "$phase" == audit-tests ]]; then
      budget=180
      argv=("$python" -m pytest "$out/validation/audit-code/scripts/tests/test_body_bc_report.py"
        -q --junitxml="$out/validation/audit-tests.xml")
    else
      argv=("$python" "$out/validation/audit-code/scripts/body_bc_report.py" "$out"
        --config "$code/evals/npc-body/body-bc-v1.json" --output-dir "$out/validation/audit-v1")
    fi ;;
  recover)
    [[ -n "$capture_run" ]] || { echo 'Recovery requires the captured run name as third argument' >&2; exit 1; }
    [[ ! -e "$out/demonstrations" ]] || { echo 'Recovered collection already exists; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_demonstration_recover.py"
      --source "$task_root/data/$capture_run/demonstrations" --output "$out/demonstrations"
      --config "$code/evals/npc-body/body-demonstrations-v1.json" --captured-code "$task_root/data/$capture_run/code") ;;
  setup)
    budget=300
    [[ ! -e "$out/setup-probe" ]] || { echo 'Setup proof already exists; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_setup_probe.py" "${common[@]}"
      --config "$code/evals/npc-body/body-bc-v1.json" --output "$out/setup-probe") ;;
  tests)
    budget=180
    argv=("$python" -m pytest "$code/body-service/tests" "$code/scripts/tests/test_body_trajectory_dataset.py"
      "$code/scripts/tests/test_body_trajectory_recorder.py" "$code/scripts/tests/test_body_demonstration_teacher.py"
      "$code/scripts/tests/test_body_demonstration_recover.py"
      "$code/scripts/tests/test_body_bc_report.py" "$code/scripts/tests/test_body_policy_suite.py"
      -q --junitxml="$out/tests.xml") ;;
  smoke|demos)
    budget=900
    mode=()
    destination="$out/demonstrations"
    [[ "$phase" != smoke ]] || { mode=(--smoke); destination="$out/demonstrations-smoke"; }
    [[ ! -e "$destination" ]] || { echo 'Demonstrations already exist; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_demonstration_suite.py" "${common[@]}"
      --config "$code/evals/npc-body/body-demonstrations-v1.json" --existing-dataset "$task_root/data/stage6-v1/dataset/dataset.json"
      --output "$destination" "${mode[@]}") ;;
  dataset|verify|overfit|train|evaluate)
    [[ "$phase" != train ]] || budget=11000
    argv=("$python" "$code/scripts/body_bc_suite.py" "$phase" --run "$out") ;;
  arena|arena-smoke)
    mode=()
    destination="$out/arena"
    [[ "$phase" != arena-smoke ]] || { mode=(--smoke); destination="$out/arena-smoke"; }
    [[ ! -e "$destination" ]] || { echo 'Arena evidence already exists; use a fresh run' >&2; exit 1; }
    argv=("$python" "$code/scripts/body_policy_suite.py" "${common[@]}" --config "$code/evals/npc-body/body-bc-v1.json"
      --gru-checkpoint "$out/models/gru/best.pt" --mlp-checkpoint "$out/models/mlp/best.pt" --device cpu
      --output "$destination" "${mode[@]}") ;;
  report)
    argv=("$python" "$code/scripts/body_bc_report.py" "$out" --config "$code/evals/npc-body/body-bc-v1.json"
      --output-dir "$out/validation") ;;
  *) echo 'Unknown stage 7 phase' >&2; exit 2 ;;
esac
case "$phase" in
  block-reaction|probe|shadow|realtime|screening|load|load-reference|realtime-reference|realtime-reaction|screening-reaction)
    if [[ -f "$out/model-reference/declaration.json" ]]; then
      "$python" - "$out" <<'PY'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1]);declaration=json.loads((out/'model-reference/declaration.json').read_text());reference=Path(declaration['reference_run'])
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
assert declaration['kind']=='body_frozen_selected_model_reference' and declaration['new_run_environment_sha256']==sha(out/'environment.json')
for name,expected in declaration['source_files'].items():
    assert sha(reference/name)==expected,'Selected source reference changed: '+name
    copied=out/name if name.startswith('models/') else out/'model-reference'/name
    assert sha(copied)==expected,'Frozen selected model reference changed: '+name
runtime=declaration['runtime_normalizer']
assert runtime['path']=='dataset/normalization.json' and runtime['sha256']==declaration['normalizer_sha256']
assert sha(out/runtime['path'])==runtime['sha256'],'Canonical runtime normalizer changed'
PY
    fi ;;
esac
unit="gothic-body-bc-$3-$phase"
case "$phase" in
  safe-peer-calibration|safe-peer-smoke|safe-peer-demos|setup|realtime-probe|visibility-boundary-probe|compact-reply-probe|pending-input-probe|stage8-faults|probe|shadow|realtime|screening|load|load-reference|realtime-reaction|screening-reaction)
    native_serial_workers=$("$python" - "$code/evals/npc-body/body-stage8-v1.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))['runtime'].get('native_serial_workers',False)
assert type(value) is bool,'native_serial_workers must be an explicit boolean'
print('true' if value else 'false')
PY
)
    [[ "$native_serial_workers" != true ]] || argv+=(--serial-workers)
    ;;
esac
cuda_visible_devices=''
[[ "$phase" != defence-train-cuda && "$phase" != safe-peer-train-cuda && "$phase" != train-cuda ]] || cuda_visible_devices=0
"$python" - "$out" "$phase" before <<'PY'
import datetime,json,subprocess,sys
from pathlib import Path
def gpu():
    # The gate runs models on CPU; a host without a working NVIDIA driver records that fact.
    run=subprocess.run(['nvidia-smi','--query-gpu=memory.used,memory.free,utilization.gpu','--format=csv,noheader'],capture_output=True,text=True)
    return run.stdout.strip() if run.returncode==0 and run.stdout.strip() else 'unavailable: '+(run.stdout+run.stderr).strip().splitlines()[0][:120]
out=Path(sys.argv[1]);config=json.loads((out/'code/evals/npc-body/body-bc-v1.json').read_text())
snapshot={'recorded_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'services':{name:subprocess.check_output(['systemctl','--user','show',name,'-p','ActiveState','-p','SubState','-p','MainPID'],text=True).strip() for name in config['resources']['required_services']},'gpu':gpu()}
(out/f'services-{sys.argv[2]}-{sys.argv[3]}.json').write_text(json.dumps(snapshot,indent=2)+'\n')
PY
gpu_watch_pid=''
gpu_watch_done="$out/gpu-watch-$phase.done"
if [[ "$use_gq" == true ]]; then
  queue_class=batch
  case "$phase" in
    tests|*-tests) queue_class=quick ;;
    realtime|realtime-reference|realtime-reaction|realtime-probe|load|load-reference|screening-reaction|shadow|probe|screening|block-reaction|pending-input-probe|stage8-faults|stage8-faults-retry|verify|stage8-verify|defence-verify|safe-peer-verify)
      queue_class=quiet ;;
  esac
  queue_budget="$budget"
  if ((queue_budget > 7200)); then
    # These suites skip completed seeds but cannot resume a partial seed from
    # its checkpoint. Do not restart a partially written frozen experiment.
    queue_budget=7200
    echo "gq: warning: $phase cannot resume a partial checkpoint; --max is capped at 2h (legacy budget ${budget}s)."
  fi
  queue_args=(--class "$queue_class" --threads 2 --mem 6G --max "${queue_budget}s" --name "body-bc $3 $phase")
  [[ "$queue_class" != quiet ]] || queue_args+=(--cpu-quota 200%)
  [[ "$cuda_visible_devices" != 0 ]] || queue_args+=(--gpu-mem 1536M)
fi
if [[ "$cuda_visible_devices" == 0 ]]; then
  [[ ! -e "$out/gpu-watch-$phase.jsonl" && ! -e "$out/gpu-watch-$phase.json" && ! -e "$gpu_watch_done" ]] || {
    echo 'CUDA monitor evidence exists; preserve it and use a fresh run' >&2; exit 1;
  }
  (
    # The monitor lives outside the training unit and must not keep the
    # phase or legacy coordination locks alive after the launcher finishes.
    exec 7>&- 8>&- 9>&-
    watcher_script="$code/scripts/body_training_gpu_watch.py"
    watcher_queue_args=()
    if [[ "$use_gq" == true ]]; then
      # Queue admission may take hours. The monitor follows this exact job's
      # state and starts sampling only after the command has been admitted.
      while [[ ! -s "$gq_job_id_file" ]]; do
        [[ ! -e "$gpu_watch_done" ]] || exit 0
        sleep 0.2
      done
      unit="gq-$(cat "$gq_job_id_file")"
      watcher_queue_args=(--gq-home "${GQ_HOME:-$HOME/gothic-ai/queue}")
      # The current monitor understands gq units, including for a previously
      # frozen training run whose original code remains immutable.
      watcher_script="$task_root/source/scripts/body_training_gpu_watch.py"
    fi
    exec "$python" "$watcher_script" --unit "$unit.service" "${watcher_queue_args[@]}" \
      --output "$out/gpu-watch-$phase.jsonl" --summary "$out/gpu-watch-$phase.json" --job-done "$gpu_watch_done"
  ) > "$out/gpu-watch-$phase.log" 2>&1 &
  gpu_watch_pid=$!
fi
if [[ "$use_gq" != true ]]; then
  trap 'systemctl --user stop "$unit.service" >/dev/null 2>&1 || true; if [[ -n "$gpu_watch_pid" ]]; then touch "$gpu_watch_done"; wait "$gpu_watch_pid" || true; fi' INT TERM
fi
set +e
if [[ "$use_gq" == true ]]; then
  cd "$out"
  # Keep the helper in this shell so its protected, retryable cancellation
  # handles launcher signals; the GPU watcher already runs in the background.
  GQ_JOB_ID_FILE="$gq_job_id_file" gq_job "${queue_args[@]}" -- \
    env "PYTHONPATH=$code/body-service/src:$code/scripts" \
    "CUDA_VISIBLE_DEVICES=$cuda_visible_devices" OMP_NUM_THREADS=2 MKL_NUM_THREADS=2 OPENBLAS_NUM_THREADS=2 \
    DISPLAY= WAYLAND_DISPLAY= GOTHIC_AI=0 \
    VK_DRIVER_FILES=/nonexistent/body-bc-no-vulkan.json VK_ICD_FILENAMES=/nonexistent/body-bc-no-vulkan.json \
    OPENGOTHIC_TEXTURES= OPENGOTHIC_BODY_TRACE=1 OPENGOTHIC_BODY_ANIMATION_RNG=isolated \
    /usr/bin/time -v -o "$out/resources-$phase.txt" "${argv[@]}" > "$out/launcher-$phase.log" 2>&1
  result=$?
else
  systemd-run --user --wait --pipe --collect --unit="$unit" \
  --property=Nice=10 --property=CPUQuota=200% --property=MemoryMax=6G \
  --property=TasksMax=128 --property="RuntimeMaxSec=$budget" --working-directory="$out" \
  --setenv="PYTHONPATH=$code/body-service/src:$code/scripts" \
  --setenv="CUDA_VISIBLE_DEVICES=$cuda_visible_devices" --setenv="OMP_NUM_THREADS=2" --setenv="MKL_NUM_THREADS=2" --setenv="OPENBLAS_NUM_THREADS=2" \
  --setenv="DISPLAY=" --setenv="WAYLAND_DISPLAY=" --setenv="GOTHIC_AI=0" \
  --setenv="VK_DRIVER_FILES=/nonexistent/body-bc-no-vulkan.json" --setenv="VK_ICD_FILENAMES=/nonexistent/body-bc-no-vulkan.json" \
  --setenv="OPENGOTHIC_TEXTURES=" --setenv="OPENGOTHIC_BODY_TRACE=1" --setenv="OPENGOTHIC_BODY_ANIMATION_RNG=isolated" \
  /usr/bin/time -v -o "$out/resources-$phase.txt" "${argv[@]}" > "$out/launcher-$phase.log" 2>&1
  result=$?
fi
if [[ -n "$gpu_watch_pid" ]]; then
  touch "$gpu_watch_done"
  wait "$gpu_watch_pid"
  gpu_watch_result=$?
  [[ "$gpu_watch_result" -eq 0 ]] || result="$gpu_watch_result"
fi
set -e
trap - INT TERM HUP
"$python" - "$out" "$phase" after <<'PY'
import datetime,json,subprocess,sys
from pathlib import Path
def gpu():
    # The gate runs models on CPU; a host without a working NVIDIA driver records that fact.
    run=subprocess.run(['nvidia-smi','--query-gpu=memory.used,memory.free,utilization.gpu','--format=csv,noheader'],capture_output=True,text=True)
    return run.stdout.strip() if run.returncode==0 and run.stdout.strip() else 'unavailable: '+(run.stdout+run.stderr).strip().splitlines()[0][:120]
out=Path(sys.argv[1]);config=json.loads((out/'code/evals/npc-body/body-bc-v1.json').read_text())
snapshot={'recorded_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'services':{name:subprocess.check_output(['systemctl','--user','show',name,'-p','ActiveState','-p','SubState','-p','MainPID'],text=True).strip() for name in config['resources']['required_services']},'gpu':gpu()}
(out/f'services-{sys.argv[2]}-{sys.argv[3]}.json').write_text(json.dumps(snapshot,indent=2)+'\n')
PY
tail -12 "$out/launcher-$phase.log"
[[ "$result" -eq 0 ]] || { echo "Phase $phase failed ($result); frozen evidence remains in $out" >&2; exit "$result"; }
echo "Stage 7 phase $phase complete: $out"
REMOTE_SCRIPT
}

collect_run() {
  local local_out="$ROOT/data/npc-body/bc/$RUN_NAME"
  mkdir -p "$local_out"
  # Whitelist compact artifacts. No raw game journals, weights, assets or
  # duplicated training trajectories are downloaded to the development Mac.
  rsync -a --prune-empty-dirs --exclude=code/ --exclude=reference/ --include='*/' \
    --include='*.json' --include='*.md' --include='resources*.txt' \
    --include='body-environment.txt' --exclude='*' "$HOST:$REMOTE/data/$RUN_NAME/" "$local_out/"
  echo "Collected compact stage 7 results: $local_out"
}

case "$ACTION" in
  preflight) preflight ;;
  sync) sync_sources ;;
  sync-code) sync_sources ;;
  env) install_env ;;
  build) "$ROOT/scripts/body-trajectories-remote.sh" build ;;
  prepare) prepare_run ;;
  prepare-audit) prepare_audit ;;
  prepare-model-reference) prepare_model_reference ;;
  prepare-faults-retry) prepare_faults_retry ;;
  stage8-faults-retry) run_faults_retry ;;
  _faults-retry-local)
    # Only the frozen server copy enters this branch; run_phase's same heredoc
    # executes locally, retaining its resource bounds and snapshots.
    remote_script() { { cat "$ROOT/scripts/gq-job.sh"; declare -f ryzen_task_venv freeze_engine_reference; cat; } | bash -s -- "$@"; }
    run_phase stage8-faults-retry ;;
  setup|tests|audit-tests|audit-report|combat-tests|probe-tests|stage8-tests|stage8-faults|runtime-cpu-profile-tests|runtime-delivery-tests|load-delivery-tests|service-cadence-tests|visibility-boundary-tests|visibility-boundary-probe|compact-reply-tests|compact-reply-probe|pending-input-tests|pending-input-probe|block-reaction-tests|block-reaction|realtime-reference|realtime-reaction|screening-reaction|final-evaluation-tests|safe-peer-teacher-tests|safe-peer-tests|safe-peer-calibration|safe-peer-smoke|safe-peer-demos|safe-peer-dataset|safe-peer-verify|safe-peer-overfit|safe-peer-train|safe-peer-train-cuda|safe-peer-evaluate|defence-tests|defence-smoke|defence-demos|defence-dataset|defence-verify|defence-overfit|defence-train|defence-train-cuda|defence-evaluate|train-cuda|realtime-probe|probe|shadow|realtime|screening|load|load-reference|combat-geometry-preflight|combat-calibration|combat-smoke|combat-demos|stage8-dataset|stage8-verify|stage8-overfit|stage8-train|stage8-evaluate|stage8-report|probe-report|smoke|demos|recover|dataset|verify|overfit|train|evaluate|arena|arena-smoke|report) run_phase "$ACTION" ;;
  collect) collect_run ;;
  *) echo 'Usage: body-bc-remote.sh preflight|sync|sync-code|env|build|prepare|prepare-audit|prepare-model-reference|prepare-faults-retry|stage8-faults-retry|block-reaction-tests|block-reaction|realtime-reference|load-reference|setup|tests|audit-tests|audit-report|smoke|demos|recover|dataset|verify|overfit|train|evaluate|arena|arena-smoke|report|collect [run-name] [reference/capture-run]' >&2; exit 2 ;;
esac

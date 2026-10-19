"""Queued captures keep their own save, engine and runner without SSH or a game."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


SCRIPT = Path(__file__).with_name("capture-materials.sh")


def executable(path, source):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source)
    path.chmod(0o755)


def wait_queued(queue, count, processes):
    deadline = time.monotonic() + 10
    while len(list(queue.glob("*.json"))) < count:
        assert all(process.poll() is None for process in processes), "capture exited before queue admission"
        assert time.monotonic() < deadline, "capture did not reach the fake queue"
        time.sleep(0.02)


def test_waiting_captures_do_not_share_runtime(tmp_path):
    repo = tmp_path / "repo"
    script = repo / "scripts/content/capture-materials.sh"
    script.parent.mkdir(parents=True)
    shutil.copy2(SCRIPT, script)
    engine_source = repo / "OpenGothic/source.cpp"
    engine_source.parent.mkdir()
    engine_source.write_bytes(b"fixture engine source")
    digest = hashlib.sha256(b"source.cpp\0" + hashlib.sha256(engine_source.read_bytes()).digest()).hexdigest()
    home = tmp_path / "remote-home"
    binary = home / f"gothic-ai/engine-cache/{digest}/Gothic2Notr"
    (home / "gothic-ai/game/Data").mkdir(parents=True)
    queue = tmp_path / "queued"
    queue.mkdir()
    release = tmp_path / "release"
    fake_bin = tmp_path / "bin"
    python = f"#!{sys.executable}\n"
    executable(fake_bin / "uv", python + '''
from pathlib import Path
import sys
out = Path(sys.argv[sys.argv.index('--output') + 1])
index = out / 'material-a/materials/materials.json'
index.parent.mkdir(parents=True)
index.write_text('{}')
''')
    executable(fake_bin / "ssh", python + '''
import subprocess
import sys
args = sys.argv[1:]
while args[0].startswith('-'):
    option = args.pop(0)
    if option == '-o': args.pop(0)
args.pop(0)  # host
raise SystemExit(subprocess.call(['bash', '-c', args[0]]))
''')
    executable(fake_bin / "rsync", python + '''
from pathlib import Path
import shutil
import sys
args = [arg for arg in sys.argv[1:] if arg not in ('-a', '--')]
target = Path(args[-1].split(':', 1)[-1])
for source in args[:-1]:
    path = Path(source.split(':', 1)[-1])
    if source.endswith('/'):
        shutil.copytree(path, target, dirs_exist_ok=True)
    else:
        shutil.copy2(path, target)
''')
    executable(repo / "scripts/gq", python + '''
import json
import os
from pathlib import Path
import subprocess
import sys
import time
command = sys.argv[sys.argv.index('--') + 1:]
state = Path(os.environ['CAPTURE_TEST_STATE'])
(state / 'queued' / (command[-1] + '.json')).write_text(json.dumps(command))
deadline = time.monotonic() + 15
while not (state / 'release').exists():
    if time.monotonic() > deadline: raise SystemExit('fake queue admission timed out')
    time.sleep(0.02)
raise SystemExit(subprocess.call(command))
''')
    tools = repo / "reports/render-features/2026-10-18/tools"
    tools.mkdir(parents=True)
    (tools / "rf.bench").write_text("goto NW_CITY_MERCHANT_SHOP01_FRONT_03\nshot city\n")
    output = tmp_path / "captures"
    processes = []
    try:
        for variant in ("A", "B"):
            # B changes every previously shared input while A waits for admission.
            executable(binary, f"OPENGOTHIC_CONTENT engine-{variant}\n")
            (tools / "gpu-nvidia.py").write_text(f"gpu-{variant}")
            (tools / "discard-ffmpeg.sh").write_text(f"ffmpeg-{variant}")
            save = tmp_path / f"save-{variant}.sav"
            save.write_text(f"save-{variant}")
            (tools / "run-ryzen.sh").write_text('''#!/usr/bin/env bash
set -euo pipefail
R="$HOME/gothic-ai/render-features"
D="$1/material-a"
mkdir -p "$D"
python3 - "$R" "$D/inputs.json" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
files = ('build/opengothic/Gothic2Notr', 'save_slot_1.sav', 'gpu-nvidia.py', 'discard-ffmpeg.sh')
data = {name: (root / name).read_text() for name in files}
data['runner'] = 'VARIANT'
Path(sys.argv[2]).write_text(json.dumps(data))
PY
printf '0\n' >"$D/exit.txt"
printf 'fake screenshot\n' >"$D/city.png"
printf 'materials: 1 entries from fixture\n[bench] goto NW_CITY_MERCHANT_SHOP01_FRONT_03\n[bench] shot city\n' >"$D/game.log"
'''.replace("VARIANT", variant))
            env = dict(os.environ, HOME=str(home), PATH=f"{fake_bin}:{os.environ['PATH']}",
                       SAVE=str(save), CAPTURE_TEST_STATE=str(tmp_path))
            processes.append(subprocess.Popen(
                ["bash", str(script), "--content-root", str(tmp_path / "content"), "--output", str(output)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            ))
            wait_queued(queue, len(processes), processes)
        release.touch()
        for process in processes:
            stdout, stderr = process.communicate(timeout=10)
            assert process.returncode == 0, stdout + stderr
    finally:
        release.touch()
        for process in processes:
            if process.poll() is None:
                process.communicate(timeout=20)

    requests = [json.loads(path.read_text()) for path in queue.glob("*.json")]
    assert len(requests) == 2
    assert len({request[-1] for request in requests}) == 2
    captures = sorted(output.glob("*/captures/material-a/inputs.json"))
    assert len(captures) == 2
    seen = set()
    for capture in captures:
        data = json.loads(capture.read_text())
        variant = data["runner"]
        seen.add(variant)
        assert data == {
            "build/opengothic/Gothic2Notr": f"OPENGOTHIC_CONTENT engine-{variant}\n",
            "save_slot_1.sav": f"save-{variant}",
            "gpu-nvidia.py": f"gpu-{variant}",
            "discard-ffmpeg.sh": f"ffmpeg-{variant}",
            "runner": variant,
        }
    assert seen == {"A", "B"}

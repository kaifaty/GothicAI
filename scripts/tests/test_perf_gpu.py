"""GPU clock journal and warm-up of scripts/perf.sh, with nvidia-smi and the game replaced by stubs."""
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import pytest


SCRIPTS = Path(__file__).parents[1]
sys.path.insert(0, str(SCRIPTS))
import perf_gpu_log as gpu


def rows(temps, step_s=1):
    return [{"mono_ns": int(index * step_s * 1e9), "temp_c": temp} for index, temp in enumerate(temps)]


def test_warm_reason_needs_ninety_seconds_and_a_still_minute():
    second = int(1e9)
    rising = rows([50 + index * 0.1 for index in range(400)])
    assert gpu.warm_reason(rising, 0, 150 * second, 300) is None
    # Flat from the start: warm at 90 s of load, not before.
    flat = rows([70.0] * 200)
    assert gpu.warm_reason(flat, 0, 90 * second - 1, 300) is None
    assert gpu.warm_reason(flat, 0, 90 * second, 300) == "stable"
    # A 1 °C wobble is still, 1.5 °C is not; a missing reading does not count.
    wobble = rows([70 + (index % 2) for index in range(200)])
    wobble[120]["temp_c"] = None
    assert gpu.warm_reason(wobble, 0, 150 * second, 300) == "stable"
    assert gpu.warm_reason(rows([70 + (index % 2) * 1.5 for index in range(200)]), 0, 150 * second, 300) is None
    # The window is the last 60 s: a 2 °C step 59 s back counts, 61 s back does not.
    for step_at, warm in ((150 - 59, None), (150 - 61, "stable")):
        stepped = rows([68.0 if index < step_at else 70.0 for index in range(151)])
        assert gpu.warm_reason(stepped, 0, 150 * second, 300) == warm
    assert gpu.warm_reason(rising, 0, 300 * second - 1, 300) is None
    assert gpu.warm_reason(rising, 0, 300 * second, 300) == "max"
    # A journal that stopped long ago does not cover the minute, nor do two fresh samples after the gap.
    assert gpu.warm_reason(rows([70.0] * 40), 0, 120 * second, 300) is None
    resumed = rows([70.0] * 40) + [{"mono_ns": 118 * second, "temp_c": 70.0}, {"mono_ns": 119 * second, "temp_c": 70.0}]
    assert gpu.warm_reason(resumed, 0, 120 * second, 300) is None


SMI_LINES = r'''#!/usr/bin/env bash
[[ "${1:-}" != -L ]] || { echo 'GPU 0: Fake'; exit 0; }
[[ "$*" == "--query-gpu=temperature.gpu,clocks.sm,clocks.mem,power.draw,pstate,fan.speed,utilization.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits -lms 1000" ]] || exit 9
echo $$ >"$FAKE_SMI_PID"
echo '61, 1905, 9501, 301.25, P2, 45, 99, 0x0000000000000000'
sleep 0.5
echo '[N/A], [N/A], 9501, [N/A], P2, [N/A], 99, 0x0000000000000004'
echo 'Unable to determine the device handle'
exec sleep 600
'''

SMI_LOOP = r'''#!/usr/bin/env bash
[[ "${1:-}" != -L ]] || { echo 'GPU 0: Fake'; exit 0; }
echo $$ >"$FAKE_SMI_PID"
while :; do echo '66, 1890, 9501, 310.5, P2, 50, 98, 0x0000000000000000'; sleep 0.2; done
'''

SMI_RISING = r'''#!/usr/bin/env bash
[[ "${1:-}" != -L ]] || { echo 'GPU 0: Fake'; exit 0; }
for ((temp = 40; ; temp++)); do echo "$temp, 1890, 9501, 310.5, P2, 50, 98, 0x0"; sleep 0.2; done
'''

SMI_SILENT = '#!/usr/bin/env bash\n[[ "${1:-}" != -L ]] || exit 0\nexec sleep 600\n'

SMI_SLOW = SMI_LOOP.replace("while :;", "sleep 3; while :;")

SMI_ONE_LINE = r'''#!/usr/bin/env bash
[[ "${1:-}" != -L ]] || { echo 'GPU 0: Fake'; exit 0; }
echo '66, 1890, 9501, 310.5, P2, 50, 98, 0x0000000000000000'
exit 9
'''

SMI_NO_QUERY = '#!/usr/bin/env bash\n[[ "${1:-}" != -L ]] || exit 0\nexit 9\n'

SMI_BROKEN = '#!/usr/bin/env bash\necho "NVIDIA-SMI has failed" >&2\nexit 9\n'


def stub(directory, name, text):
    directory.mkdir(exist_ok=True)
    (directory / name).write_text(text)
    (directory / name).chmod(0o755)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_journal_stamps_rows_survives_na_and_stops_on_sigterm(tmp_path):
    stub(tmp_path / "bin", "nvidia-smi", SMI_LINES)
    out = tmp_path / "gpu.csv"
    pid = tmp_path / "smi.pid"
    environment = dict(os.environ, FAKE_SMI_PID=str(pid), PATH=f"{tmp_path / 'bin'}{os.pathsep}{os.environ['PATH']}")
    before = time.monotonic_ns()
    process = subprocess.Popen([sys.executable, str(SCRIPTS / "perf_gpu_log.py"), str(out)], env=environment)
    deadline = time.monotonic() + 10
    while len(gpu.read_log(out)) < 2:
        assert time.monotonic() < deadline and process.poll() is None
        time.sleep(0.05)
    process.send_signal(signal.SIGTERM)
    assert process.wait(timeout=10) == 0
    assert not alive(int(pid.read_text()))
    assert out.read_text().splitlines()[0] == "mono_ns,temp_c,sm_mhz,mem_mhz,power_w,pstate,fan_pct,util_pct,reasons"
    first, second = gpu.read_log(out)
    assert before <= first["mono_ns"] and first["mono_ns"] + 0.4e9 <= second["mono_ns"] <= time.monotonic_ns()
    assert (first["temp_c"], first["sm_mhz"], first["mem_mhz"], first["power_w"]) == (61, 1905, 9501, 301.25)
    assert (second["temp_c"], second["sm_mhz"], second["power_w"], second["mem_mhz"]) == (None, None, None, 9501)
    assert (first["fan_pct"], first["util_pct"], second["fan_pct"], second["util_pct"]) == (45, 99, None, 99)
    assert second["reasons"] == "0x0000000000000004"


GAME = r'''#!/usr/bin/env python3
import json, os, signal, time
from pathlib import Path
out = Path(os.environ["OPENGOTHIC_BENCH_OUT"])
profile = os.environ.get("OPENGOTHIC_FRAME_PROFILE", "")
with open(os.environ["FAKE_GAME_LOG"], "a") as log:
    log.write(f"{out}\t{profile}\n")
if not profile:
    if sum(1 for line in open(os.environ["FAKE_GAME_LOG"]) if line.endswith("\t\n")) == 1:
        time.sleep(1)  # a warm-up scenario shorter than the warm-up: restarted
        raise SystemExit(0)
    def stopped(*_):
        Path(os.environ["FAKE_GAME_LOG"] + ".stopped").touch()
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stopped)
    time.sleep(60)
    raise SystemExit(0)
start = time.monotonic_ns()
lines = ["attempt,segment,begin_ns,end_ns,outcome,overflow"]
for view in range(1, 5):
    for frame in range(18):
        begin = start + int(((view - 1) * 10 + frame * 0.5) * 1e9)
        lines.append(f"{len(lines)},perf-market/view-{view},{begin},{begin + 1000000},1,0")
Path(profile).write_text("\n".join(lines) + "\n")
(out / "appcadence.csv.json").write_text(json.dumps({"complete": True, "frames": 72, "overflow": 0}))
(out / "events.jsonl").write_text(json.dumps({"kind": "perf_segment", "segment": "perf-market/view-1"}) + "\n")
print("[bench] drawable 1728x1117")
'''


def stand(tmp_path, smi, stamp_age=None, warmup=None, slow_checks=False, **extra):
    root = tmp_path / "gothic"
    (root / "scripts/bench").mkdir(parents=True)
    (root / "OpenGothic/build-perf").mkdir(parents=True)
    (root / "OpenGothic/build-perf/CMakeCache.txt").write_text(
        "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\nOPENGOTHIC_ENABLE_FRAME_PROFILE:BOOL=ON\n"
        "OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON\n")
    for name in ("perf.sh", "perf_report.py", "perf_gpu_log.py", "base-ensure.sh"):
        shutil.copyfile(SCRIPTS / name, root / "scripts" / name)
    (root / "scripts/bench/perf-market.bench").write_text("")
    stub(tmp_path / "bin", "game", GAME)
    stub(tmp_path / "bin", "pgrep", "#!/usr/bin/env bash\nexit 1\n")
    path = f"{tmp_path / 'bin'}{os.pathsep}{os.environ['PATH']}"
    if smi is not None:
        stub(tmp_path / "bin", "nvidia-smi", smi)
    if slow_checks:
        # The build checks before the base, display and journal take 3 s.
        stub(tmp_path / "bin", "grep", f"#!/usr/bin/env bash\nsleep 1\nexec {shutil.which('grep')} \"$@\"\n")
    stamp = tmp_path / "perf-last-game"
    if stamp_age is not None:
        stamp.touch()
        os.utime(stamp, (time.time() - stamp_age,) * 2)
    environment = dict(os.environ, PATH=path, NO_BUILD="1", GAME_BIN=str(tmp_path / "bin/game"),
                       GOTHIC_DIR=str(tmp_path), OPENGOTHIC_BASE="test-base", PERF_SOURCE_FINGERPRINT="f" * 64,
                       PERF_GPU_STAMP=str(stamp), PERF_WARMUP_MAX="3", PERF_WAIT="0",
                       FAKE_GAME_LOG=str(tmp_path / "games"), FAKE_SMI_PID=str(tmp_path / "smi.pid"))
    for name in ("BENCH_HOST", "PERF_WARMUP", "PERF_ENV", "PERF_ARGS", "PERF_INI", "PERF_REPEAT"):
        environment.pop(name, None)
    if warmup is not None:
        environment["PERF_WARMUP"] = warmup
    environment.update(extra)
    result = subprocess.run(["bash", str(root / "scripts/perf.sh"), "market"], env=environment,
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    out = Path(result.stdout.splitlines()[-1])
    games = [line.split("\t") for line in (tmp_path / "games").read_text().splitlines()]
    assert "No such file" not in result.stderr
    return out, games, result.stdout + result.stderr, stamp


def test_fresh_stamp_skips_the_warm_up_and_journals_the_run(tmp_path):
    # The stamp is 118 s old when the run phase begins; its checks take 3 s, the journal's first sample 3 s more.
    out, games, stdout, stamp = stand(tmp_path, SMI_SLOW, stamp_age=118, slow_checks=True)
    assert re.search(r"^perf: GPU warm: last stand game ended 11[0-9] s ago, no warm-up$", stdout, re.M)
    assert [Path(leaf).relative_to(out).as_posix() for leaf, _ in games] == ["market/repeat-1"]
    assert not (out / "warmup").exists()
    warmup = json.loads((out / "warmup.json").read_text())
    assert warmup["reason"] == "skipped-recent-game" and 117 <= warmup["last_game_age_s"] < 120
    assert gpu.read_log(out / "gpu.csv")
    assert not alive(int((tmp_path / "smi.pid").read_text()))
    assert time.time() - stamp.stat().st_mtime < 30
    clock = json.loads((out / "report.json").read_text())["places"]["market"]["views"]["view-1"]["gpu_clock"]
    assert set(clock) == {"sm_mhz", "mem_mhz", "temp_c", "power_w", "samples"}


def test_old_stamp_warms_up_with_restarts_until_the_ceiling(tmp_path):
    out, games, stdout, stamp = stand(tmp_path, SMI_LOOP, stamp_age=122)
    warmup = json.loads((out / "warmup.json").read_text())
    assert warmup["reason"] == "max" and warmup["mode"] == "auto"
    # The first game ends by itself and is restarted; the second is stopped at the ceiling.
    assert warmup["runs"] == 2 and 3 <= warmup["seconds"] < 6
    assert (tmp_path / "games.stopped").exists()
    assert (warmup["temp_end_c"], warmup["sm_end_mhz"]) == (66, 1890)
    leaves = [Path(leaf).relative_to(out).as_posix() for leaf, _ in games]
    assert leaves == [f"warmup/run-{n}" for n in range(1, warmup["runs"] + 1)] + ["market/repeat-1"]
    assert [profile for _, profile in games][:-1] == [""] * warmup["runs"]
    assert not list((out / "warmup").rglob("frameprofile.csv"))
    assert (out / "warmup/run-1/run/Gothic.ini").read_text() == (out / "market/repeat-1/run/Gothic.ini").read_text()
    report = json.loads((out / "report.json").read_text())
    assert [source["directory"] for source in report["sources"]] == ["market/repeat-1"]
    assert "Прогрев GPU (auto): max" in (out / "report.md").read_text()
    assert not alive(int((tmp_path / "smi.pid").read_text()))


@pytest.mark.parametrize("smi,ceiling,reason", ((SMI_LOOP, "60", "stable"), (SMI_RISING, "4", "max")))
def test_still_temperature_stops_the_warm_up_before_the_ceiling(tmp_path, smi, ceiling, reason):
    # A still card is warm after the minimum; a heating one past the minimum is not, until the ceiling.
    out, games, _, _ = stand(tmp_path, smi, stamp_age=130, PERF_WARMUP_MAX=ceiling,
                             PERF_WARMUP_MIN="2", PERF_WARMUP_WINDOW="1")
    warmup = json.loads((out / "warmup.json").read_text())
    assert warmup["reason"] == reason and warmup["runs"] == 2 and warmup["seconds"] < 10
    assert reason == "stable" or warmup["seconds"] >= 4
    assert (tmp_path / "games.stopped").exists()


@pytest.mark.parametrize("warmup", (None, "auto", "3"))
def test_without_nvidia_smi_there_is_no_journal(tmp_path, warmup):
    out, games, _, _ = stand(tmp_path, SMI_BROKEN if shutil.which("nvidia-smi") else None, stamp_age=1000,
                             warmup=warmup)
    files = ["market", "metadata.json", "report.json", "report.md"]
    if warmup == "3":
        # A fixed warm-up needs no journal.
        assert json.loads((out / "warmup.json").read_text())["reason"] == "fixed"
        assert 3 <= json.loads((out / "warmup.json").read_text())["seconds"] < 3.6
        assert len(games) == json.loads((out / "warmup.json").read_text())["runs"] + 1
        files += ["warmup", "warmup.json"]
    else:
        assert len(games) == 1
        files += ["warmup.json"] * (warmup is not None)
    assert sorted(path.name for path in out.iterdir()) == sorted(files)
    if warmup == "auto":
        assert json.loads((out / "warmup.json").read_text())["reason"] == "no-gpu-log"
    assert "Прогрев" not in (out / "report.md").read_text()
    assert "warmup" not in json.loads((out / "report.json").read_text())


@pytest.mark.parametrize("smi,early", ((SMI_NO_QUERY, True), (SMI_SILENT, False)))
def test_query_without_samples_goes_on_without_a_journal(tmp_path, smi, early):
    started = time.monotonic()
    out, games, output, _ = stand(tmp_path, smi, stamp_age=1000)
    assert time.monotonic() - started < 20  # a silent query is given up after 5 s
    assert "perf: warning: no GPU clock sample in 5 s; no journal" in output
    assert ("perf: warning: GPU clock journal stopped early (exit 9)" in output) == early
    assert not (out / "gpu.csv").exists() and len(games) == 1
    assert json.loads((out / "warmup.json").read_text())["reason"] == "no-gpu-log"


def test_journal_ending_early_warns_and_reports_views_without_samples(tmp_path):
    out, games, output, _ = stand(tmp_path, SMI_ONE_LINE, stamp_age=110)
    assert "perf: warning: GPU clock journal stopped early (exit 9)" in output
    warnings = json.loads((out / "report.json").read_text())["warnings"]
    assert [f"журнал GPU не покрывает market/view-{view}" for view in range(1, 5)] == warnings

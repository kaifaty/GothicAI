"""City-4 tour coverage and census requirements, without a game or original pixels."""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("capture_city4", HERE / "capture-city-4.py")
CAPTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAPTURE)


def prepared_tour(tmp_path):
    names = re.findall(r"^# ([A-Z0-9_]+\.TGA) \|", (HERE / "city-intent.bench").read_text(), re.MULTILINE)[:58]
    misc = [f"NW_MISC_TEST_{number}.TGA" for number in range(7)]
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({name: {"albedo": f"tile-{i}/albedo.dds"}
                                 for i, name in enumerate(names + misc)}))
    inspection = tmp_path / "misc.bench"
    inspection.write_text("\n".join(
        f"# {name} | MARKT | synthetic test camera\ncam look 45\ngoto pos 1 2 3\n"
        f"cam world 4 5 6\nwait 34\ncam hold\nwait 600\nshot {Path(name).stem.lower()}\nwait 300\n"
        for name in misc))
    scenario = tmp_path / "city-4.bench"
    CAPTURE.prepare(index, scenario, inspection)
    return index, scenario, inspection


def test_tour_contains_all_comparisons_and_a_census_camera_for_every_target(tmp_path):
    index, scenario, _ = prepared_tour(tmp_path)
    text = scenario.read_text()
    shots = [line[5:] for line in CAPTURE.commands(scenario) if line.startswith("shot ")]
    assert len(shots) == len(set(shots)) == 35
    comparisons = set(shots) - {"city_a", "city_night"}
    assert len(comparisons) == 33
    assert {"pilot_" + name for name in ("roof", "upper", "house", "interior", "facade", "quay", "stairs")} <= comparisons
    assert len([name for name in comparisons if name.startswith("nw_misc_")]) == 7
    tail = text.split("# Census-only surface tour:", 1)[1]
    assert tail.count("cam hold") == len(json.loads(index.read_text())) == 65
    assert "shot " not in tail


def test_missing_misc_camera_stops_before_writing_a_tour(tmp_path):
    index, scenario, inspection = prepared_tour(tmp_path)
    scenario.unlink()
    inspection.write_text(inspection.read_text().split("# NW_MISC_TEST_6.TGA", 1)[0])
    with pytest.raises(SystemExit, match="seven NW_MISC"):
        CAPTURE.prepare(index, scenario, inspection)
    assert not scenario.exists()


def census_capture(tmp_path, variant="city-4"):
    directory = tmp_path / variant
    directory.mkdir()
    scenario = tmp_path / "capture.bench"
    scenario.write_text("goto MARKT\nshot market\n")
    (directory / "market.png").write_bytes(b"synthetic screenshot")
    (directory / "exit.txt").write_text("0\n")
    count = {"city-4": 65, "city-3": 58, "original": 0}[variant]
    (directory / "game.log").write_text(f"materials: {count} entries from fixture\n"
                                       "[bench] goto MARKT\n[bench] shot market\n[bench] done\n")
    city = [f"NW_CITY_TEST_{i}.TGA" for i in range(58)]
    misc = [f"NW_MISC_TEST_{i}.TGA" for i in range(7)]
    names = city if variant == "city-3" else city + misc
    scenario.write_text(scenario.read_text() + "# Census-only surface tour:\n"
                        + "\n".join("# " + name for name in city + misc) + "\n")
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({name: {"albedo": f"tile-{i}/albedo.dds"} for i, name in enumerate(names)}))
    originals = [{"name": name[:-4] + "-C.TEX", "kind": "texture", "opens": 1, "source": "fixture.vdf"}
                 for name in names]
    replacements = [{"name": name, "kind": "texture", "opens": 1, "replacement": f"materials/tile-{i}/albedo.dds"}
                    for i, name in enumerate(names)]
    census = {"files": originals if variant == "original" else [],
              "replacements": [] if variant == "original" else replacements}
    (directory / "census.json").write_text(json.dumps(census))
    return directory, scenario, index, census, originals


@pytest.mark.parametrize("variant", ("original", "city-3", "city-4"))
def test_capture_accepts_required_variant_counts(tmp_path, variant):
    directory, scenario, index, _, _ = census_capture(tmp_path, variant)
    CAPTURE.check(directory, scenario, index, variant)


@pytest.mark.parametrize("variant", ("city-3", "city-4"))
@pytest.mark.parametrize("failure", ("missing_replacement", "original_alias", "no_replacements",
                                    "wrong_albedo_path", "missing_census"))
def test_replacement_variants_require_complete_census(tmp_path, variant, failure):
    directory, scenario, index, census, originals = census_capture(tmp_path, variant)
    if failure == "missing_replacement":
        census["replacements"].pop()
    elif failure == "original_alias":
        census["files"].append(originals[0])
    elif failure == "no_replacements":
        census["replacements"] = []
    elif failure == "wrong_albedo_path":
        census["replacements"][0]["replacement"] = "materials/unrelated/albedo.dds"
    (directory / "census.json").write_text(json.dumps(census))
    if failure == "missing_census":
        (directory / "census.json").unlink()
    with pytest.raises(subprocess.CalledProcessError):
        CAPTURE.check(directory, scenario, index, variant)


@pytest.mark.parametrize("replaced", (False, True))
@pytest.mark.parametrize("variant", ("original", "city-3", "city-4"))
def test_capture_rejects_unrelated_materials(tmp_path, replaced, variant):
    directory, scenario, index, census, originals = census_capture(tmp_path, variant)
    count = 58 if variant == "city-3" else 65
    names = [f"UNRELATED_{i}.TGA" for i in range(count)]
    index.write_text(json.dumps({name: {"albedo": f"unrelated-{i}/albedo.dds"}
                                 for i, name in enumerate(names)}))
    census["files"] = originals
    census["replacements"] = ([{"name": name, "kind": "texture", "opens": 1,
                                "replacement": f"materials/unrelated-{i}/albedo.dds"}
                               for i, name in enumerate(names)] if replaced else [])
    (directory / "census.json").write_text(json.dumps(census))
    with pytest.raises(SystemExit, match=f"{count} city census targets"):
        CAPTURE.check(directory, scenario, index, variant)


@pytest.mark.parametrize("variant", ("original", "city-3", "city-4"))
def test_capture_requires_census_target_list_in_scenario(tmp_path, variant):
    directory, scenario, index, _, _ = census_capture(tmp_path, variant)
    scenario.write_text("goto MARKT\nshot market\n")
    count = 58 if variant == "city-3" else 65
    with pytest.raises(SystemExit, match=f"{count} city census targets"):
        CAPTURE.check(directory, scenario, index, variant)


def test_original_baseline_rejects_even_an_unrelated_replacement(tmp_path):
    directory, scenario, index, census, _ = census_capture(tmp_path, "original")
    census["replacements"].append({"name": "UNRELATED.TGA", "kind": "texture", "opens": 1,
                                   "replacement": "materials/unrelated.dds"})
    (directory / "census.json").write_text(json.dumps(census))
    with pytest.raises(SystemExit, match="zero replacements"):
        CAPTURE.check(directory, scenario, index, "original")


def test_compose_writes_one_jpeg_triple_per_comparison_point(tmp_path):
    _, scenario, _ = prepared_tour(tmp_path)
    captures = tmp_path / "captures"
    for variant in CAPTURE.VARIANTS:
        (captures / variant).mkdir(parents=True)
        for command in CAPTURE.commands(scenario):
            if command.startswith("shot "):
                Image.new("RGB", (20, 10), "#ab8050").save(captures / variant / (command[5:] + ".png"))
    output = tmp_path / "triples"
    CAPTURE.compose(captures, scenario, output)
    assert len(list(output.glob("*.jpg"))) == 33
    with Image.open(output / "pilot_roof.jpg") as image:
        assert image.format == "JPEG" and image.size == (60, 54)


@pytest.mark.parametrize("variant", ("original", "city-3", "city-4"))
def test_queued_launchers_keep_their_own_inputs_and_outputs(tmp_path, variant):
    repo = tmp_path / "repo"
    scripts = repo / "scripts/content"
    scripts.mkdir(parents=True)
    for name in ("capture-city-4.sh", "capture-city-4.py", "capture-city.sh", "city-intent.bench",
                 "city-3.bench", "quality-pilot.bench"):
        shutil.copy2(HERE / name, scripts / name)
    home = tmp_path / "remote-home"
    home.mkdir()
    queue = tmp_path / "queued"
    queue.mkdir()
    release = tmp_path / "release"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()

    def executable(path, source):
        path.write_text(f"#!{sys.executable}\n" + source)
        path.chmod(0o755)

    executable(fake_bin / "ssh", '''
import os
import subprocess
import sys
args = sys.argv[1:]
assert args.pop(0) == '-T'
args.pop(0)  # host
raise SystemExit(subprocess.call(['bash', '-c', args[0]], cwd=os.environ['HOME']))
''')
    executable(fake_bin / "rsync", '''
import os
from pathlib import Path
import shutil
import sys
args = [arg for arg in sys.argv[1:] if arg not in ('-a', '--delete')]
def resolve(value):
    remote = ':' in value
    path = Path(value.split(':', 1)[-1])
    return Path(os.environ['HOME']) / path if remote and not path.is_absolute() else path
target = resolve(args[-1])
if '--delete' in sys.argv and target.exists():
    shutil.rmtree(target)
target.mkdir(parents=True, exist_ok=True)
for source in args[:-1]:
    path = resolve(source)
    if source.endswith('/'):
        shutil.copytree(path, target, dirs_exist_ok=True)
    else:
        shutil.copy2(path, target)
''')
    executable(repo / "scripts/gq", '''
import json
import os
from pathlib import Path
import sys
import time
command = sys.argv[sys.argv.index('--') + 1:]
root = Path(os.environ['HOME']) / Path(command[1]).parent.parent
variant = command[3]
state = Path(os.environ['CITY4_CAPTURE_TEST_STATE'])
(state / 'queued' / (str(os.getpid()) + '.json')).write_text(json.dumps(command))
deadline = time.monotonic() + 15
while not (state / 'release').exists():
    if time.monotonic() > deadline:
        raise SystemExit('fake queue admission timed out')
    time.sleep(0.02)
out = root / 'runs' / variant
out.mkdir(parents=True, exist_ok=True)
data = {'marker': (root / 'content/city-4/marker').read_text(),
        'scenario': (root / 'scripts/city-4.bench').read_text(),
        'helper': (root / 'scripts/capture-city.sh').read_text(),
        'index': json.loads((root / 'content' / variant / 'materials/materials.json').read_text())}
if variant == 'city-3':
    data['city3_marker'] = (root / 'content/city-3/marker').read_text()
(out / 'inputs.json').write_text(json.dumps(data))
''')
    processes = []
    outputs = []
    try:
        for marker in ("A", "B"):
            inputs = tmp_path / f"inputs-{marker}"
            inputs.mkdir()
            index, _, inspection = prepared_tour(inputs)
            inspection.write_text(inspection.read_text() + f"\n# capture-test {marker}\n")
            staging = inputs / "staging"
            names = json.loads(index.read_text())
            for name in ("city-4", "city-3"):
                packed = staging / name
                (packed / "materials").mkdir(parents=True)
                (packed / "marker").write_text(marker)
                (packed / "materials/materials.json").write_text(json.dumps(
                    names if name == "city-4" else {k: v for k, v in names.items()
                                                   if not k.startswith("NW_MISC_")}))
            (scripts / "capture-city.sh").write_text(
                (HERE / "capture-city.sh").read_text() + f"\n# runner {marker}\n")
            output = tmp_path / f"output-{marker}"
            outputs.append(output)
            env = dict(os.environ, HOME=str(home), PATH=f"{fake_bin}:{os.environ['PATH']}",
                       CONTENT_STAGING_ROOT=str(staging), CITY4_CAPTURE_TEST_STATE=str(tmp_path))
            processes.append(subprocess.Popen(
                ["bash", str(scripts / "capture-city-4.sh"), "--capture", variant, str(output),
                 "--inspection", str(inspection)], env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE))
            deadline = time.monotonic() + 10
            while len(list(queue.glob("*.json"))) < len(processes):
                assert all(p.poll() is None for p in processes), "launcher exited before queue admission"
                assert time.monotonic() < deadline, "launcher did not reach the fake queue"
                time.sleep(0.02)
        # Both launchers have uploaded; A has not been admitted while B changed every input.
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
    assert len({request[1] for request in requests}) == 2
    for request in requests:
        assert request[:1] == ["bash"] and request[2:4] == ["--remote-capture", variant]
        assert len(request) == 5 and Path(request[4]) == Path(request[1]).parent.parent
    for marker, output in zip(("A", "B"), outputs):
        data = json.loads((output / variant / "inputs.json").read_text())
        assert data["marker"] == marker
        assert f"# capture-test {marker}\n" in data["scenario"]
        assert data["helper"].endswith(f"# runner {marker}\n")
        assert len(data["index"]) == {"original": 0, "city-3": 58, "city-4": 65}[variant]
        if variant == "city-3":
            assert data["city3_marker"] == marker


def test_remote_capture_rejects_a_shared_runtime_root(tmp_path):
    home = tmp_path / "home"
    shared = home / "gothic-ai/city-4"
    shared.mkdir(parents=True)
    result = subprocess.run(
        ["bash", str(HERE / "capture-city-4.sh"), "--remote-capture", "city-4", str(shared)],
        check=False, env=dict(os.environ, HOME=str(home)), capture_output=True, text=True)
    assert result.returncode == 2
    assert "Invalid city-4 capture root" in result.stderr
